// contribution.cpp — task 5.3's implementation (see contribution.hpp for the
// pinned readings). The doc functions stay line-for-line against the design
// reference; the router and the tick slice are the runtime wiring the card's
// recording note describes ("combat (3.2 step 5) and heals call
// validateActionAmount first; the validated amount is what accumulates").

#include "events/contribution.hpp"

#include <algorithm>

#include "character/character.hpp"
#include "character/status.hpp"  // consentsToPvp (2.1's THE consent check)
#include "core/math.hpp"         // isqrt64 (task 0.2)
#include "events/difficulty.hpp" // objectiveAmount (the OBJECTIVE budget's accrual read)
#include "npc/state.hpp"         // D-INVULN-1's invulnerable flag owner
#include "progression/power.hpp" // getCurrentPower + markPowerDirty (2.5)

namespace harrowmere::events
{

namespace
{

// The hook payloads ride u64; the books are u32 (the doc's own field types).
// HP and contribution amounts sit far below u32 range — the clamp is a cast
// guard, never a game rule.
std::uint32_t toU32(
    std::uint64_t v
) noexcept
{
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(v, 0xFFFF'FFFFull));
}

} // namespace

// ---- the doc's free functions ---------------------------------------------------

std::uint32_t getActionAmount(
    const Event &ev, characterId actor, ActionType a
)
{
    const auto it = ev.actionAmounts.find(actor);
    if (it == ev.actionAmounts.end())
    {
        return 0;
    }
    return it->second[static_cast<std::size_t>(a)];
}

bool shareEvent(
    const Character &a, const Character &b
)
{
    return a.eventId != 0 && a.eventId == b.eventId;
}

std::uint32_t getProgressDelta(
    const Event &ev, const EventTemplate &tpl, characterId /*actor*/
)
{
    // The v1 provider reading (contribution.hpp): the remaining budget of
    // the event's OBJECTIVE-type objectives — the largest remaining cap
    // minus the participants' accrued OBJECTIVE total, capped to SCALE. An
    // event without an OBJECTIVE-type objective accepts nothing.
    const TemplateState *state = tpl.state(ev.currentState.stateId);
    if (state == nullptr)
    {
        return 0;
    }
    std::uint32_t best = 0;
    for (const Objective &o : state->objectives) // declared order (deterministic)
    {
        if (o.actionType != ActionType::OBJECTIVE || o.cap == 0)
        {
            continue;
        }
        const std::uint64_t accrued   = objectiveAmount(ev, o);
        const std::uint64_t remaining = accrued >= o.cap ? 0u : o.cap - accrued;
        best                          = std::max(best, toU32(std::min<std::uint64_t>(remaining, SCALE)));
    }
    return best;
}

std::uint32_t validateActionAmount(
    Event &ev,
    ActionType t,
    std::uint32_t amount,
    const Character &actor,
    const Character &target,
    const EventTemplate &tpl,
    WorldContext &ctx
)
{
    if (amount == 0)
    {
        return 0;
    }
    switch (t)
    {
    case ActionType::DAMAGE:
        if (actor.id == target.id)
        {
            return 0; // self-harm farm guard (the doc's own note)
        }
        if (!ctx.characters->isPlayer(target.id) && ctx.npcs != nullptr && ctx.npcs->contains(target.id)
            && ctx.npcs->get(target.id).invulnerable)
        {
            return 0; // D-INVULN-1 (the flag rides NpcState; the player analog is always false)
        }
        if (ev.targetIds.count(target.id) == 0)
        {
            return 0; // only template targets pay ("trading hits with a friend pays nothing")
        }
        if (ctx.characters->isPlayer(target.id) && !consentsToPvp(target, actor)
            && (ctx.justice == nullptr
                || !ctx.justice->hasWarrantIn(target.id, ctx.world->territoryAt(target.body.position))))
        {
            return 0; // the consent-or-warrant arm (2.1's check + 4.2's jurisdiction)
        }
        return std::min<std::uint32_t>(amount, toU32(target.body.currentHp)); // capped at remaining hp
    case ActionType::HEALING:
        if (actor.id == target.id)
        {
            return 0;
        }
        if (!shareEvent(actor, target))
        {
            return 0; // same eventId
        }
        {
            const auto lost             = ev.hpLost.find(target.id);
            const std::uint32_t removed = lost == ev.hpLost.end() ? 0u : lost->second;
            const std::uint64_t room
                = target.body.maxHp > target.body.currentHp ? target.body.maxHp - target.body.currentHp : 0u;
            return std::min<std::uint32_t>(
                {amount, toU32(room), removed}
            ); // heal only what targets removed (heal loops die here)
        }
    case ActionType::OBJECTIVE:
        return std::min<std::uint32_t>(amount, getProgressDelta(ev, tpl, actor.id));
    default:
        return std::min<std::uint32_t>(amount, MAX_ACTION_AMOUNT); // A-104's default clamp
    }
}

std::uint32_t recordAction(
    Event &ev,
    ActionType t,
    std::uint32_t amount,
    const Character &actor,
    const Character &target,
    const EventTemplate &tpl,
    WorldContext &ctx
)
{
    // The write half of validateActionAmount's contract: validate first,
    // accumulate the VALIDATED amount. The only actionAmounts writer in the
    // tree (the 5.3 Done-when's grep lands here; endEvent's clear is the
    // lifecycle tail, not a per-actor write).
    const std::uint32_t validated = validateActionAmount(ev, t, amount, actor, target, tpl, ctx);
    if (validated == 0)
    {
        return 0;
    }
    ev.actionAmounts[actor.id][static_cast<std::size_t>(t)] += validated;
    return validated;
}

std::uint64_t getContribution(
    const Event &ev, characterId actor, const EventTemplate &t
)
{
    // design reference verbatim; cap-zero guard (R2) before dividing.
    std::uint64_t total = 0;
    for (std::uint8_t a = 0; a < static_cast<std::uint8_t>(ActionType::COUNT); ++a)
    {
        if (t.actionCap[a] == 0)
        {
            continue;
        }
        const std::uint64_t norm = std::min<std::uint64_t>(
            SCALE,
            static_cast<std::uint64_t>(getActionAmount(ev, actor, static_cast<ActionType>(a))) * SCALE / t.actionCap[a]
        );
        total += static_cast<std::uint64_t>(t.actionWeight[a]) * norm / SCALE;
    }
    return total;
}

void updatePowerSnapshot(
    Event &ev, characterId actor, WorldContext &ctx
)
{
    if (ctx.characters == nullptr || ctx.data == nullptr || ctx.itemInstances == nullptr)
    {
        return;
    }
    // The single-lookup probe (the 8.3 soak's step profile: the per-tick
    // exists() + get() pair walked the character tree twice per refresh for
    // every battle participant — the contribution walk is the hot loop that
    // pays it thousands of times per interval).
    Character *c = ctx.characters->find(actor);
    if (c == nullptr)
    {
        return;
    }
    const std::uint64_t cur = getCurrentPower(*c, *ctx.data, *ctx.itemInstances);
    auto &snap              = ev.currentState.powerSnapshots[actor];
    if (cur > snap)
    {
        snap = cur; // MAX watermark, reset only by endEvent's clear
    }
}

std::uint64_t getEffectiveContribution(
    const Event &ev, characterId actor, const EventTemplate &t
)
{
    detail::g_effectiveContributionEvaluations += 1; // the 5.4 instrumentation seam
    const std::uint64_t raw  = getContribution(ev, actor, t);
    const auto it            = ev.currentState.powerSnapshots.find(actor);
    const std::uint64_t snap = it == ev.currentState.powerSnapshots.end() ? 0u : it->second;
    return raw * SCALE / (SCALE + isqrt64(snap)); // isqrt damping — overpowered pay proportionally
}

std::uint64_t getRewardShare(
    const Event &ev, characterId actor, const EventTemplate &t
)
{
    // design reference verbatim. The total is O(n) per call — 5.4's
    // distributor computes the total ONCE; do not call getRewardShare in
    // the payout loop (the card pitfall).
    const std::uint64_t mine = getEffectiveContribution(ev, actor, t);
    if (mine <= MIN_CONTRIBUTION)
    {
        return 0;
    }
    std::uint64_t total = 0;
    for (const characterId p : ev.participants)
    {
        total += getEffectiveContribution(ev, p, t);
    }
    if (total == 0)
    {
        return 0; // R2's pinned fallback at the call site
    }
    return mine * SCALE / total;
}

namespace detail
{
std::uint64_t g_effectiveContributionEvaluations = 0;
} // namespace detail

std::uint64_t effectiveContributionEvaluations() noexcept
{
    return detail::g_effectiveContributionEvaluations;
}

void resetEffectiveContributionEvaluations() noexcept
{
    detail::g_effectiveContributionEvaluations = 0;
}

// ---- the router -----------------------------------------------------------------

const EventTemplate *ContributionRouter::templateOf(
    const Event &ev
) const
{
    return templates_ != nullptr ? templates_->byId(ev.templateId) : nullptr;
}

void ContributionRouter::onCombatDamage(
    const DamageEvent &dmg
)
{
    if (engine_ == nullptr || templates_ == nullptr || ctx_ == nullptr || ctx_->characters == nullptr
        || !ctx_->characters->exists(dmg.target))
    {
        return;
    }
    Character &target = ctx_->characters->get(dmg.target);
    Character *source
        = (dmg.source != 0 && ctx_->characters->exists(dmg.source)) ? &ctx_->characters->get(dmg.source) : nullptr;
    const std::uint32_t applied = toU32(dmg.amount); // 3.2's hook: the payload IS the applied amount

    // The attacker's ledger: the source's own event, through the full guard
    // set. The doc-order validation view restores the pre-hit hp (the
    // pipeline applied and hp-capped the damage before this hook fired, so
    // the doc's "capped at remaining hp" arm reads hpBefore = currentHp +
    // applied — the killing blow pays its full applied slice exactly as
    // validate-then-apply would have).
    if (source != nullptr && source->eventId != 0 && !engine_->hasLeftEarly(source->eventId, source->id))
    {
        Event *ev                = engine_->findMutable(source->eventId);
        const EventTemplate *tpl = ev != nullptr ? templateOf(*ev) : nullptr;
        if (ev != nullptr && tpl != nullptr)
        {
            Character targetView      = target; // plain-struct copy; the store record is never touched
            targetView.body.currentHp = std::min<std::uint64_t>(target.body.currentHp + applied, targetView.body.maxHp);
            recordAction(*ev, ActionType::DAMAGE, applied, *source, targetView, *tpl, *ctx_);
        }
    }

    // The heal economy's accrual arm: when a character in targetIds deals
    // damage to an event participant, the applied amount lands in
    // ev.hpLost[victim.id] — the budget the participant's healers share.
    if (target.eventId != 0 && source != nullptr)
    {
        Event *ev = engine_->findMutable(target.eventId);
        if (ev != nullptr && ev->targetIds.count(source->id) != 0)
        {
            std::uint32_t &lost = ev->hpLost[target.id];
            lost                = toU32(static_cast<std::uint64_t>(lost) + applied);
        }
    }
}

void ContributionRouter::onHeal(
    characterId healerId, characterId targetId, std::uint64_t amount
)
{
    if (engine_ == nullptr || templates_ == nullptr || ctx_ == nullptr || ctx_->characters == nullptr || amount == 0)
    {
        return;
    }
    if (!ctx_->characters->exists(healerId) || !ctx_->characters->exists(targetId))
    {
        return;
    }
    Character &healer = ctx_->characters->get(healerId);
    Character &target = ctx_->characters->get(targetId);
    if (healer.eventId == 0 || engine_->hasLeftEarly(healer.eventId, healer.id))
    {
        return; // the leave mark refuses later contribution (5.1's leave-early rule)
    }
    Event *ev                = engine_->findMutable(healer.eventId);
    const EventTemplate *tpl = ev != nullptr ? templateOf(*ev) : nullptr;
    if (ev == nullptr || tpl == nullptr)
    {
        return;
    }
    // The doc-order validation view: the duty applied (and room-clamped)
    // the heal before this hook fired, so the doc's "max_hp - hp" arm reads
    // the pre-heal room (currentHp - healed) — the triple-min then clamps
    // against exactly the hp the target still missed when the heal began.
    Character targetView      = target;
    targetView.body.currentHp = target.body.currentHp > amount ? target.body.currentHp - amount : 0u;
    recordAction(*ev, ActionType::HEALING, toU32(amount), healer, targetView, *tpl, *ctx_);
}

// ---- the snapshot cadence ---------------------------------------------------------

void ContributionTickSystem::run(
    WorldContext &ctx
)
{
    if (engine == nullptr)
    {
        return;
    }
    // Ended events leave the book (the prune; findMutable is active-only).
    for (auto bookIt = snapshotAt_.begin(); bookIt != snapshotAt_.end();)
    {
        if (engine->findMutable(bookIt->first) == nullptr)
        {
            bookIt = snapshotAt_.erase(bookIt);
        } else
        {
            ++bookIt;
        }
    }
    // The walk rides the A-106 cadence (the 8.3 as-built, DEVIATIONS.md): the
    // refresh decision below reads (now - stamp) off the persisted book, so
    // gating the walk to one pass per SNAPSHOT_INTERVAL_GAME_S leaves every
    // refresh exactly where the per-tick walk put it and only delays a brand
    // new participant's baseline stamp by at most one interval. Per-tick it
    // re-walked a war BATTLE's full participant book (both factions'
    // membership) — ~2.1 ms/tick on the 8.3 soak's step profile.
    if (ctx.now < nextWalkAt_)
    {
        return;
    }
    nextWalkAt_ = ctx.now + SNAPSHOT_INTERVAL_GAME_S;
    // Per active event (id ascending, R7), per participant (sorted, R7):
    // the first sight stamps the baseline; the watermark refreshes every
    // A-106 interval of participation. The walk is the pipeline's hot loop
    // (a war BATTLE's book is both factions' membership — tens of thousands
    // of rows per pass), so it moves TWO sorted ranges against each other
    // (participants, a sorted vector; the book, an id-ordered map) instead of
    // paying a random-access book.find per participant, and the mutable event
    // handle is found ONCE per event (the per-row findMutable re-walked the
    // engine's map every refresh — both shapes measured on the 8.3 soak's
    // step profile).
    //
    // The walk also rides the snapshot's reader set (the 8.3 as-built,
    // DEVIATIONS.md): the runtime's ONLY snapshot reader is the reward
    // damping (getEffectiveContribution — payEventRewards' single pass), so
    // an event whose template carries no paying reward table never reads
    // what the walk records. The 8.3 soak's step profile measured exactly
    // that dead work as the pipeline's hotspot: 16 rewardless war BATTLEs
    // (both factions' membership, ~37.7k participant rows) refreshed every
    // A-106 interval ate ~88 of every 92 s of a war month's step cost. The
    // gate is payEventRewards' own (hasRewardTable && !allZero — the same
    // predicate, one reading); a rewardless event's snapshot map stays
    // empty — the exact state the 5.1 lifecycle starts every event in, so
    // save/load and the canonical hash read an ordinary state.
    for (const auto &[eventId, evConst] : engine->active())
    {
        const EventTemplate *tpl
            = engine->templates() != nullptr ? engine->templates()->byId(evConst.templateId) : nullptr;
        if (tpl == nullptr || !tpl->hasRewardTable || tpl->rewards.allZero())
        {
            continue; // no paying reader — the recording is dead state (the 8.3 as-built)
        }
        auto &book  = snapshotAt_[eventId];
        Event *ev   = engine->findMutable(eventId);
        auto bookIt = book.begin();
        for (const characterId p : evConst.participants)
        {
            while (bookIt != book.end() && bookIt->first < p)
            {
                ++bookIt; // stale book rows (a left-early participant's id stays) ride along
            }
            if (bookIt == book.end() || bookIt->first != p)
            {
                bookIt = book.emplace_hint(bookIt, p, ctx.now); // the baseline stamp
                continue;
            }
            if (ctx.now - bookIt->second >= SNAPSHOT_INTERVAL_GAME_S)
            {
                if (ev != nullptr)
                {
                    updatePowerSnapshot(*ev, p, ctx);
                }
                bookIt->second = ctx.now;
            }
            ++bookIt;
        }
    }
}

} // namespace harrowmere::events
