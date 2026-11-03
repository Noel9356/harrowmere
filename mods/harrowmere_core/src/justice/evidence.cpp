// evidence.cpp — task 4.1's commit pipeline: recordCrime, the physical scene
// score, the assault aggregation and the real JusticeSink. Every rule is
// card-verbatim (design reference §Crime and Evidence); see evidence.hpp.

#include "justice/evidence.hpp"
#include "justice/citizen_justice.hpp" // 4.7: the massacre tally + the loot-currency rule

#include <algorithm>
#include <cassert>

#include "character/character.hpp" // Character (the culprit/victim records)
#include "character/species.hpp"   // isSapient (the witness-qualification callers' data read)
#include "core/clock.hpp"          // DAY_LENGTH_SECONDS / GAME_HOUR_SECONDS (A-3)
#include "core/constants.hpp"      // A-59/A-62 + the marker weights, ASSAULT_THRESHOLD, the night band
#include "core/game_events.hpp"    // CrimeRecorded (0.7's payload)
#include "core/world_context.hpp"  // WorldContext + ISimEventSink
#include "data/laws.hpp"           // the complete LawSet (the exemption gate)
#include "justice/conviction.hpp"  // isArrested (the sink's jail read)
#include "justice/reputation.hpp"  // (warrant reads route through warrants.hpp)
#include "justice/warrants.hpp"    // hasWarrantIn / hasLethalWarrantIn (the sink's reads)
#include "npc/relationships.hpp"   // 3.9's API — the ONLY door to qualification + the weight formula
#include "player/controller.hpp"   // PlayerActions (the BLOODSTAINED act surface 4.1 reads)
#include "world/world.hpp"         // WorldStore: territoryAt / lawsAt / isSafeTerritory

namespace harrowmere::justice
{

namespace
{} // namespace

Evidence commitEvidence(
    CrimeType t,
    characterId culpritId,
    characterId victimId,
    territoryId terr,
    WorldContext &ctx,
    std::vector<Report> reports,
    std::uint32_t physicalScore
)
{
    JusticeStore &js = *ctx.justiceStore;
    Evidence e{};
    e.id            = js.nextEvidenceId();
    e.type          = t;
    e.culpritId     = culpritId;
    e.victimId      = victimId;
    e.territory     = terr; // the snapshot taken at commit; NEVER recomputed
    e.worldTime     = ctx.now;
    e.status        = EvidenceStatus::UNKNOWN;
    e.reports       = std::move(reports);
    e.physicalScore = physicalScore;
    capReports(e);

    auto &cases = js.evidenceStore[terr]; // insertion-ordered per territory (R7)
    cases.push_back(e);

    // 0.7 rule 1: publish the fact after the mutation is complete. Nothing
    // about the crime is visible to the player at the act (the doc's rule) —
    // the event is the cross-system notification, not a UI surface.
    if (ctx.simEvents != nullptr)
    {
        ctx.simEvents->onCrimeRecorded(CrimeRecorded{e.id, e.culpritId, e.type, e.territory});
    }
    return e;
}

void recordCrime(
    CrimeType t, const Character &culprit, const Character &victim, const Position &p, WorldContext &ctx
)
{
    assert(
        ctx.justiceStore != nullptr && ctx.worldStore != nullptr && ctx.characters != nullptr
        && "recordCrime: unbound context"
    );
    if (ctx.justiceStore == nullptr || ctx.worldStore == nullptr || ctx.characters == nullptr)
    {
        return;
    }
    world::WorldStore &world = *ctx.worldStore;

    // Gate 1 — only players generate evidence (the doc's rule; an NPC brawl
    // is the world's business, never a case).
    if (!ctx.characters->isPlayer(culprit.id))
    {
        return;
    }

    // Gate 2 — the territory snapshot, taken at commit and NEVER recomputed.
    const territoryId terr   = world.territoryAt(p);
    const data::LawSet &laws = world.lawsAt(terr);
    const bool safe          = world.isSafeTerritory(terr);

    // Gate 3 — the species-exemption HARD gate: it returns BEFORE constructing
    // the Evidence (order matters for exploits — evidence-then-exempt would
    // still leak into archives). Outside safe zones only: the doc's own gate
    // shape (a safe zone's ward watches everything).
    if (!safe)
    {
        const auto exemptIt = laws.exemptions.find(victim.species);
        if (exemptIt != laws.exemptions.end() && exemptIt->second[static_cast<std::size_t>(t)])
        {
            return; // no evidence object ever exists
        }
    }

    // The reports: live witnesses, then the safe-zone ward's maximal testimony
    // (A-59 — the doc's `Report{0, WARD_WEIGHT, true}`: the ward IS an
    // identity source in safe zones; do not "fix" it to false).
    std::vector<Report> reports = witnessReports(culprit.id, p, ctx);
    if (safe)
    {
        reports.push_back(Report{0, WARD_WEIGHT, true, false});
    }
    const std::uint32_t phys = scorePhysical(p, t, ctx);

    commitEvidence(t, culprit.id, victim.id, terr, ctx, std::move(reports), phys);
}

void noteAssault(
    characterId culprit, characterId victim, std::uint64_t appliedDamage, WorldContext &ctx
)
{
    assert(ctx.justiceStore != nullptr && ctx.characters != nullptr && "noteAssault: unbound context");
    if (ctx.justiceStore == nullptr || ctx.characters == nullptr)
    {
        return;
    }
    JusticeStore &js = *ctx.justiceStore;

    // The per-pair accumulator (3.2's hook): unconsented damage to a sapient
    // victim piles up until it crosses ASSAULT_THRESHOLD, then the ASSAULT
    // evidence records atomically at the victim's position and the pair
    // resets (the next beating builds its own case).
    std::uint64_t &accumulated = js.assaultDamage[culprit][victim];
    accumulated += appliedDamage;
    if (accumulated < ASSAULT_THRESHOLD)
    {
        return;
    }
    accumulated = 0;

    if (!ctx.characters->exists(victim) || !ctx.characters->exists(culprit))
    {
        return; // the fight outlived its fighters — nothing to record against
    }
    recordCrime(
        CrimeType::ASSAULT,
        ctx.characters->get(culprit),
        ctx.characters->get(victim),
        ctx.characters->get(victim).body.position,
        ctx
    );
}

std::uint32_t scorePhysical(
    const Position &p, CrimeType type, WorldContext &ctx, bool guardMarkedTheft
)
{
    // D-PHYS-2 (pinned v1): PHYS_BASE + the markers with live state sources,
    // clamped to [0, SCALE]. The dormant markers (their state owners are later
    // cards) carry their weights in constants.hpp and stay inert until those
    // sources land — the DEVIATIONS row lists them.
    (void)p; // the position feeds the FORCED_ENTRY/BURNING contexts (4.5/6.3/5.x data — dormant in v1)

    std::uint64_t score = PHYS_BASE;

    // Night (the +500 marker): harder to see the culprit flee. Integer clock
    // math on clock.hpp's documented anchors (R5: no float in the rule).
    const std::uint64_t dayTime = ctx.now % core::DAY_LENGTH_SECONDS;
    if (dayTime >= NIGHT_START_GAME_S || dayTime < NIGHT_END_GAME_S)
    {
        score += PHYS_MARKER_NIGHT;
    }

    // Bloodstained attacker (+2000): the suspicion act active on the culprit.
    // Only players commit crimes, so the player's act surface is the read; no
    // writer exists in v1 (the act owners are 3.2's flee/4.5's aftermath —
    // DEVIATIONS row), so the marker reads false until one lands.
    if (ctx.playerActions != nullptr && ctx.playerActions->exposes(SuspicionAct::BLOODSTAINED))
    {
        score += PHYS_MARKER_BLOODSTAINED;
    }

    // Theft of a guardMarked item (+3000): fired by holdsGuardGearTheft (the
    // one marker with a live v1 source — 2.4's guard-gear flag).
    if (type == CrimeType::THEFT && guardMarkedTheft)
    {
        score += PHYS_MARKER_GUARD_MARKED;
    }

    return static_cast<std::uint32_t>(clampScalar<std::uint64_t>(score, 0, SCALE));
}

// ---- the real JusticeSink (the plugin swaps this in for the null default) ----

void JusticeSystem::recordCrime(
    const CrimeContext &crime
)
{
    assert(ctx_ != nullptr && "JusticeSystem: bind() before use");
    if (ctx_ == nullptr)
    {
        return;
    }
    assert(
        ctx_->characters != nullptr && ctx_->characters->exists(crime.culprit) && ctx_->characters->exists(crime.victim)
        && "JusticeSystem::recordCrime: unknown culprit/victim records"
    );
    if (ctx_->characters == nullptr || !ctx_->characters->exists(crime.culprit)
        || !ctx_->characters->exists(crime.victim))
    {
        return; // release: a dead/routed record cannot open a case
    }
    // The doc's recordCrime signature is the rule; the CrimeContext's witness
    // list stays combat's informational copy — the pipeline re-derives the
    // witness set through the perception seam at the same position/tick (the
    // same deterministic answer, one grid query).
    justice::recordCrime(
        crime.type, ctx_->characters->get(crime.culprit), ctx_->characters->get(crime.victim), crime.position, *ctx_
    );
}

void JusticeSystem::noteAssault(
    characterId culprit, characterId victim, std::uint64_t applied
)
{
    assert(ctx_ != nullptr && "JusticeSystem: bind() before use");
    if (ctx_ == nullptr)
    {
        return;
    }
    justice::noteAssault(culprit, victim, applied, *ctx_);
}

bool JusticeSystem::hasWarrantIn(
    characterId suspect, territoryId territory
) const
{
    assert(ctx_ != nullptr && "JusticeSystem: bind() before use");
    if (ctx_ == nullptr)
    {
        return false;
    }
    // The member is a pointer to NON-const: the doc's mutating read (the lazy
    // purge) runs through it even from this const query.
    return justice::hasWarrantIn(*ctx_, suspect, territory);
}

bool JusticeSystem::hasLethalWarrantIn(
    characterId suspect, territoryId territory
) const
{
    assert(ctx_ != nullptr && "JusticeSystem: bind() before use");
    if (ctx_ == nullptr)
    {
        return false;
    }
    return justice::hasLethalWarrantIn(*ctx_, suspect, territory);
}

void JusticeSystem::holdsGuardGearTheft(
    characterId culprit, const Position &where
)
{
    // 2.4's guard-gear theft hook: THEFT evidence with the guardMarked marker
    // (+3000 — the doc's theft marker; the gear itself is the trace). The
    // victim is the guard corps (id 0 — the honest unknown; the doc's
    // conventions reserve 0 for the anonymous side).
    assert(ctx_ != nullptr && "JusticeSystem: bind() before use");
    if (ctx_ == nullptr || ctx_->justiceStore == nullptr || ctx_->worldStore == nullptr || ctx_->characters == nullptr)
    {
        return;
    }
    if (!ctx_->characters->exists(culprit) || !ctx_->characters->isPlayer(culprit))
    {
        return; // only players generate evidence (the doc's rule, same gate)
    }
    const territoryId terr = ctx_->worldStore->territoryAt(where);
    commitEvidence(
        CrimeType::THEFT,
        culprit,
        0,
        terr,
        *ctx_,
        witnessReports(culprit, where, *ctx_),
        scorePhysical(where, CrimeType::THEFT, *ctx_, /*guardMarkedTheft=*/true)
    );
}

bool JusticeSystem::isArrested(
    characterId who
) const
{
    assert(ctx_ != nullptr && ctx_->justiceStore != nullptr && "JusticeSystem: bind() before use");
    if (ctx_ == nullptr || ctx_->justiceStore == nullptr)
    {
        return false;
    }
    return justice::isArrested(*ctx_->justiceStore, who, ctx_->now);
}

void JusticeSystem::onBodyDiscovered(
    characterId victim, characterId discoverer
)
{
    assert(ctx_ != nullptr && "JusticeSystem: bind() before use");
    if (ctx_ == nullptr)
    {
        return;
    }
    appendDiscoveredBodyReport(victim, discoverer, *ctx_);
}

void JusticeSystem::onNpcKilled(
    characterId victim, characterId killer
)
{
    assert(ctx_ != nullptr && "JusticeSystem: bind() before use");
    if (ctx_ == nullptr)
    {
        return;
    }
    // 4.7's village-slaughter tally (the kill + the casus belli marker).
    noteNpcKilled(victim, killer, *ctx_); // 4.7 (citizen_justice.cpp)
}

std::int64_t JusticeSystem::massacreLootCurrency(
    characterId killer, const Position &at, std::int64_t purse
)
{
    assert(ctx_ != nullptr && "JusticeSystem: bind() before use");
    if (ctx_ == nullptr || ctx_->worldStore == nullptr)
    {
        return purse;
    }
    // 4.7's massacre rule: the marker zeroes the corpse's loot currency.
    return justice::massacreLootCurrency(
        killer, ctx_->worldStore->territoryAt(at), purse, *ctx_
    ); // 4.7 (the free function — the member name shadows unqualified lookup)
}

} // namespace harrowmere::justice
