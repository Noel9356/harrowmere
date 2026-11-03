// enforcement.cpp — task 4.6's implementation (see enforcement.hpp for the
// pinned reading). The manhunt spawn ENQUEUES into the next tick's step 5
// (the §4.4 reentrancy rule) through the IEventSpawner seam — this file
// never includes events/ (D-SINK-1; the Done-when).

#include "justice/enforcement.hpp"

#include <vector>

#include "character/character.hpp"
#include "core/constants.hpp"        // A-95/A-96, DAY_LENGTH_SECONDS
#include "core/constants_phase2.hpp" // WITNESS_RADIUS_M (the garrison reads)
#include "core/game_events.hpp"      // PlayerSignal (the Investigated/Wanted/Poster facts)
#include "core/world_context.hpp"
#include "items/inventory.hpp"    // ItemInstanceRegistry (the power read)
#include "justice/cases.hpp"      // findEvidence (the warrant's case)
#include "justice/conviction.hpp" // convict (the NPC auto-submit path)
#include "justice/evidence.hpp"   // Evidence
#include "justice/justice_store.hpp"
#include "justice/warrants.hpp" // purgeExpiredWarrants (the lazy read rule)
#include "npc/state.hpp"
#include "progression/power.hpp" // getCurrentPower (the odds comparison)
#include "world/world.hpp"

namespace harrowmere::justice
{

namespace
{

bool isEnforcementRole(
    const std::string &roleKey
)
{
    return roleKey == "guard" || roleKey == "captain" || roleKey == "militia_veteran";
}

bool isGarrisonRole(
    const std::string &roleKey
)
{
    // The garrison answers the alarm; the militia is a citizen posse (4.7).
    return roleKey == "guard" || roleKey == "captain";
}

void publishSignal(
    WorldContext &ctx, PlayerSignal::Kind kind, const char *text
)
{
    if (ctx.simEvents != nullptr)
    {
        ctx.simEvents->onPlayerSignal(PlayerSignal{kind, text});
    }
}

} // namespace

std::uint64_t localGuardPower(
    territoryId t, WorldContext &ctx
)
{
    if (ctx.npcs == nullptr || ctx.characters == nullptr || ctx.data == nullptr || ctx.itemInstances == nullptr)
    {
        return 0;
    }
    std::uint64_t sum = 0;
    for (const auto &[id, npc] : ctx.npcs->all())
    {
        if (npc.homeTerritory != t || !isEnforcementRole(npc.roleKey) || !ctx.characters->exists(id))
        {
            continue;
        }
        sum += getCurrentPower(ctx.characters->get(id), *ctx.data, *ctx.itemInstances);
    }
    return sum;
}

std::uint64_t culpritPower(
    characterId culprit, WorldContext &ctx
)
{
    if (ctx.characters == nullptr || !ctx.characters->exists(culprit) || ctx.data == nullptr
        || ctx.itemInstances == nullptr)
    {
        return 0;
    }
    return getCurrentPower(ctx.characters->get(culprit), *ctx.data, *ctx.itemInstances);
}

EnforcementDecision respondToWarrant(
    const Warrant &w, WorldContext &ctx
)
{
    assert(ctx.worldStore != nullptr && "respondToWarrant: unbound context");
    if (ctx.worldStore == nullptr)
    {
        return EnforcementDecision::LawlessRefused;
    }
    // Lawless territories have no one to enforce (the doc rule): no arrest,
    // no evacuation, no manhunt.
    if (ctx.worldStore->isLawless(w.territory))
    {
        return EnforcementDecision::LawlessRefused;
    }

    // respondToWarrant, design reference verbatim: the ratio multiplies the
    // GUARD side (A-95 = 1.5x — guards engage only at advantage; the card
    // pitfall pins the direction).
    const std::uint64_t guards  = localGuardPower(w.territory, ctx);
    const std::uint64_t culprit = culpritPower(w.culpritId, ctx);
    if (guards * ENGAGE_RATIO >= culprit * SCALE)
    {
        return attemptArrest(w, ctx);
    }

    // Overmatched: evacuate the civilians, then call the manhunt — ENQUEUED
    // into the next tick's step 5 (the §4.4 reentrancy rule; spawning
    // synchronously inside step 6 would re-enter the event engine mid-tick).
    evacuateTerritoryCivilians(w.territory, w.expiresTime, ctx);
    if (ctx.scheduler != nullptr)
    {
        ManhuntSeed seed;
        seed.warrantId = w.id;
        seed.culprit   = w.culpritId;
        seed.territory = w.territory;
        // The manhunt plays out where the culprit is (the hunt follows the
        // hunted; the issuing territory stands in when the position has none).
        seed.region = 0;
        if (ctx.characters != nullptr && ctx.characters->exists(w.culpritId))
        {
            seed.region = world::regionIdAt(ctx.characters->get(w.culpritId).body.position);
        }
        if (seed.region == 0)
        {
            const Position anchor = ctx.world->settlementCenter(w.territory);
            seed.region           = world::regionIdAt(anchor);
        }
        ctx.scheduler->enqueueForNextTick(TickStep::Events, [seed](WorldContext &c) {
            if (c.eventSpawner != nullptr)
            {
                c.eventSpawner->spawnEvent("MANHUNT", seed);
            }
        });
    }
    return EnforcementDecision::Evacuated;
}

EnforcementDecision attemptArrest(
    const Warrant &w, WorldContext &ctx
)
{
    // The doc invariant ("never arrest with zero prior signals"): a
    // deterministic REFUSAL (the return value names it) — stronger than the
    // debug assert, the invariant holds in release too.
    if (ctx.justiceStore == nullptr || ctx.justiceStore->signalledCulprits.count(w.culpritId) == 0)
    {
        return EnforcementDecision::NoSignal;
    }
    if (ctx.characters == nullptr || !ctx.characters->exists(w.culpritId))
    {
        return EnforcementDecision::NoSignal;
    }

    if (!ctx.characters->isPlayer(w.culpritId))
    {
        // NPC culprits auto-submit (the card's arrest resolution): the
        // warrant's evidence convicts with the standard sentence.
        Evidence *e = ctx.justiceStore != nullptr ? findEvidence(*ctx.justiceStore, w.evidenceId) : nullptr;
        if (e != nullptr)
        {
            convict(*e, ctx, WarrantResolved::How::Convicted);
        }
    }
    // A player culprit stands their engagement: the guard's damage is lawful
    // through the warrant (damage.cpp's warrantCovers), the culprit's
    // defense damage vs guards is assault unless a lethal warrant exists
    // (isCriminalAct) — and the player ends it by surrender (4.3), escape,
    // or capture (4.7). The arrest attempt itself is the signal that the
    // engagement stands.
    return EnforcementDecision::Arrested;
}

void notifyCaseStatusChanged(
    const Evidence &e, int oldStatusOrdinal, WorldContext &ctx
)
{
    if (ctx.justiceStore == nullptr)
    {
        return;
    }
    const EvidenceStatus oldStatus = static_cast<EvidenceStatus>(oldStatusOrdinal);
    // The player-facing publications (the UI signal queue is the local
    // player's; an NPC's case is nobody's screen).
    const bool forThePlayer = ctx.characters != nullptr && ctx.characters->isPlayer(e.culpritId);

    // First reach of INVESTIGATING (from UNKNOWN — the WANTED->INVESTIGATING
    // expiry downgrade re-signals nothing): guards shadow, the greeting tone
    // cools (4.5's read). The FIRST thing the player can observe (§4.6).
    if (oldStatus == EvidenceStatus::UNKNOWN && e.status == EvidenceStatus::INVESTIGATING)
    {
        // The signal-before-force memory covers every culprit (the doc's
        // invariant binds the enforcement, not the UI); the PUBLICATION is
        // the player's alone.
        ctx.justiceStore->signalledCulprits.insert(e.culpritId);
        if (forThePlayer)
        {
            publishSignal(ctx, PlayerSignal::Kind::Investigated, "You are being watched.");
        }
    }

    // WANTED: the signal + the poster world object at the territory center
    // (7.3b reads the poster map; the UI notice is the signal).
    if (oldStatus != EvidenceStatus::WANTED && e.status == EvidenceStatus::WANTED)
    {
        ctx.justiceStore->signalledCulprits.insert(e.culpritId);
        ctx.justiceStore->posters[e.territory] = ctx.now;
        if (forThePlayer)
        {
            publishSignal(ctx, PlayerSignal::Kind::Wanted, "A warrant is out for you.");
            publishSignal(ctx, PlayerSignal::Kind::Poster, "Wanted posters went up in the territory.");
        }
    }
}

void evacuateTerritoryCivilians(
    territoryId territory, std::uint64_t untilWorldTime, WorldContext &ctx
)
{
    if (ctx.npcs == nullptr)
    {
        return;
    }
    // Civilians (non-guard roles) take the fleeing intent toward home/safe
    // zone for the manhunt duration; MID/FAR civilians are abstractly marked
    // by the same flag (the doc rule — the intent is the deliverable).
    for (auto &[id, npc] : ctx.npcs->all())
    {
        if (npc.homeTerritory != territory || isEnforcementRole(npc.roleKey))
        {
            continue;
        }
        npc.fleeing              = true;
        npc.fleeingUntil         = untilWorldTime;
        npc.currentIntent.kind   = NpcIntentKind::Flee;
        npc.currentIntent.target = 0; // home/safe zone (placeless)
    }
}

void updateLawlessness(
    WorldContext &ctx
)
{
    if (ctx.worldStore == nullptr || ctx.npcs == nullptr)
    {
        return;
    }
    // Per territory (id ascending, R7): the garrison watch promotes to
    // lawless after LAWLESS_AFTER_DAYS (A-96); guards existing clears both.
    for (const auto &[tid, terr] : ctx.worldStore->territories())
    {
        (void)terr;
        if (ctx.worldStore->isSafeTerritory(tid))
        {
            continue; // safe zones never watch (the safe flag is the exemption)
        }
        std::size_t garrison = 0;
        for (const auto &[id, npc] : ctx.npcs->all())
        {
            if (npc.homeTerritory == tid && isGarrisonRole(npc.roleKey) && ctx.characters->exists(id))
            {
                ++garrison;
            }
        }
        if (garrison > 0)
        {
            ctx.worldStore->clearGarrisonLessWatch(tid);
            ctx.worldStore->clearLawless(tid);
            continue;
        }
        // The watched/unwatched decision is PRESENCE-based (the stamp value
        // can legitimately be 0 — worldTime zero — and must not re-stamp
        // forever); the promotion itself reads the stamp value.
        if (!ctx.worldStore->isGarrisonLessWatched(tid))
        {
            ctx.worldStore->watchGarrisonLess(tid, ctx.now);
        } else if (ctx.now - ctx.worldStore->lawlessWatchStart(tid) >= LAWLESS_AFTER_DAYS * core::DAY_LENGTH_SECONDS)
        {
            ctx.worldStore->markLawless(tid);
        }
    }
    // The fleeing flags self-clear when their window passes (lazy, the
    // isArrested pattern — no global timer).
    for (auto &[id, npc] : ctx.npcs->all())
    {
        if (npc.fleeing && npc.fleeingUntil <= ctx.now)
        {
            npc.fleeing            = false;
            npc.currentIntent.kind = NpcIntentKind::None;
        }
    }
}

void EnforcementStepSystem::run(
    WorldContext &ctx
)
{
    if (ctx.justiceStore == nullptr || ctx.worldStore == nullptr)
    {
        return;
    }
    // The lawlessness driver first (the case ticks next tick see the flag) —
    // day-gated: the promotion threshold is DAY-granular (LAWLESS_AFTER_DAYS,
    // A-96), so the per-territory garrison scan (territories x population)
    // runs once per game-day and on the armed first call. Per-tick it was
    // 14,400x the rule's own cadence and 87% of the whole tick budget (the
    // 8.3 soak's step-cost profile; the §4.5 posture's "simulate-less" arm).
    const std::uint64_t lawlessDay = ctx.now / core::DAY_LENGTH_SECONDS;
    if (!lawlessArmed_ || lawlessDay != lastLawlessDay_)
    {
        lawlessArmed_   = true;
        lastLawlessDay_ = lawlessDay;
        updateLawlessness(ctx);
    }

    // Warrant response: every NEW warrant (id ascending, R7) responds once.
    // The lazy purge first — the doc's read rule (reads are mutating).
    purgeExpiredWarrants(ctx);
    for (auto &[warrantId, w] : ctx.justiceStore->warrants)
    {
        if (responded_.count(warrantId) != 0)
        {
            continue;
        }
        responded_.insert(warrantId);
        respondToWarrant(w, ctx);
    }
}

} // namespace harrowmere::justice
