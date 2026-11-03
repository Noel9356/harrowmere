#ifndef HARROWMERE_CORE_EVENTS_CONTRIBUTION_HPP
#define HARROWMERE_CORE_EVENTS_CONTRIBUTION_HPP

// contribution.hpp — task 5.3: what counts as contribution, what it pays,
// and the anti-farm guards — the doc's full validation rules verbatim.
//
//   getActionAmount / shareEvent / getContribution / updatePowerSnapshot /
//   getEffectiveContribution / getRewardShare / validateActionAmount are
//   design reference verbatim (the card keeps the doc's function names —
//   the doc↔code diff rule, §3.3).
//
//   recordAction is the WRITE half of validateActionAmount's contract ("the
//   validated amount is what accumulates into actionAmounts[actor][type]"):
//   validate first, accumulate the validated amount, return it. It is the
//   ONLY path in the tree that writes ev.actionAmounts (the 5.3 Done-when;
//   endEvent's clear is the lifecycle tail, not a per-actor write) — the
//   reviewer's grep lands here.
//
//   validateActionAmount's signature note: the card's `Ctx&` carries the
//   template registry (`ctx.events.getProgressDelta(actor, ev, target)`);
//   our WorldContext is kernel-free plain data with no template table
//   member (D-SINK-1), so the template rides an explicit parameter — the
//   same adaptation the power pipeline's registry injection took
//   (DEVIATIONS.md carries the row).
//
// The router (ContributionRouter) is the real IEventsSink: combat's damage
// hook (3.2 step 5) and the healer duty's heal hook land here, and the
// event books record through the same validate-then-accumulate path the
// free functions pin. The router owns 5.3's hpLost arm: "when a character
// in targetIds deals damage to an event participant, add the applied
// amount to event.hpLost[victim.id]".
//
// The snapshot cadence (ContributionTickSystem, step 5): a participant's
// power watermark samples every SNAPSHOT_INTERVAL (A-106 = 60 game-s) of
// participation — the tick slice stamps the first sight of each
// participant and refreshes on the interval; the watermark itself only
// ever rises (updatePowerSnapshot's MAX) and clears at endEvent.

#include <cstdint>
#include <map>
#include <vector>

#include "combat/damage.hpp"      // DamageEvent (3.2 step 5's payload)
#include "core/scheduler.hpp"     // IStepSystem (the step-5 slice)
#include "core/types.hpp"         // characterId, SCALE-grid ids
#include "core/world_context.hpp" // IEventsSink (the seam), WorldContext
#include "events/engine.hpp"      // Event, EventEngine (the leave marks)
#include "events/template.hpp"    // EventTemplate, ActionType

namespace harrowmere
{
struct Character; // character/character.hpp — reference only here
}

namespace harrowmere::events
{

// ---- the doc's free functions -------------------------------------------------

// getActionAmount — design reference verbatim.
std::uint32_t getActionAmount(const Event &ev, characterId actor, ActionType a);

// shareEvent — design reference verbatim (the same eventId, both nonzero).
bool shareEvent(const Character &a, const Character &b);

// getProgressDelta — the OBJECTIVE clamp: the remaining progress the event's
// bound OBJECTIVE-type objectives will still accept (the largest remaining
// cap minus the participants' accrued OBJECTIVE total, capped to SCALE).
// This is the v1 reading of the card's provider bindings: `object_delivered`
// and `territory_reached` claims validate against this remaining budget (the
// world verbs that fire them join with 6.4's quest bindings); the
// `damage_to_targets` / `healing_given` bindings accrue through their own
// DAMAGE/HEALING slots and never pass through here. An event without an
// OBJECTIVE-type objective accepts nothing (0).
std::uint32_t getProgressDelta(const Event &ev, const EventTemplate &tpl, characterId actor);

// validateActionAmount — the doc's full guard set, verbatim in body: the
// self-harm farm guard, the invulnerable target, the template-targets-only
// rule ("trading hits with a friend pays nothing"), the player-target
// consent-or-warrant arm, the remaining-hp cap; the HEALING triple-min
// (heal only what targets removed — heal loops die here); the OBJECTIVE
// progress clamp; the MAX_ACTION_AMOUNT default. Returns the validated
// amount; writes nothing (recordAction is the accumulate half).
std::uint32_t validateActionAmount(
    Event &ev,
    ActionType t,
    std::uint32_t amount,
    const Character &actor,
    const Character &target,
    const EventTemplate &tpl,
    WorldContext &ctx
);

// recordAction — validate + accumulate (the single actionAmounts writer).
std::uint32_t recordAction(
    Event &ev,
    ActionType t,
    std::uint32_t amount,
    const Character &actor,
    const Character &target,
    const EventTemplate &tpl,
    WorldContext &ctx
);

// getContribution — design reference verbatim (cap-zero guard).
std::uint64_t getContribution(const Event &ev, characterId actor, const EventTemplate &t);

// updatePowerSnapshot — design reference verbatim (MAX watermark, reset
// only by endEvent's clear).
void updatePowerSnapshot(Event &ev, characterId actor, WorldContext &ctx);

// getEffectiveContribution — the isqrt damping (overpowered pay
// proportionally): raw * SCALE / (SCALE + isqrt(snapshot)).
std::uint64_t getEffectiveContribution(const Event &ev, characterId actor, const EventTemplate &t);

// getRewardShare — design reference verbatim. The total is O(n) per call —
// 5.4's distributor computes the total ONCE and never calls this in the
// payout loop (the card pitfall).
std::uint64_t getRewardShare(const Event &ev, characterId actor, const EventTemplate &t);

// ---- the 5.4 instrumentation seam ---------------------------------------------
//
// TestRewards_TotalComputedOnce ("instrumentation counts effective-
// contribution evaluations == n, not n²") needs a visible counter on the
// single-pass guarantee. The counter never enters sim logic (a plain u64
// tally over a pure read — no determinism surface); the test resets and
// reads it around one distributeRewards call.
namespace detail
{
extern std::uint64_t g_effectiveContributionEvaluations; // defined in contribution.cpp
} // namespace detail

std::uint64_t effectiveContributionEvaluations() noexcept;
void resetEffectiveContributionEvaluations() noexcept;

// ---- the runtime sinks ---------------------------------------------------------

// ContributionRouter — the real IEventsSink (the plugin's `simCtx_.events`
// swap; the null default was Phase 2's D-SINK-1 stub). Damage lands from
// 3.2's step-5 hook, heals from the healer duty; each records through
// validate-then-accumulate against the actor's own event, and the
// target-side hpLost arm accrues when the damaged participant's attacker
// is one of the event's targets.
class ContributionRouter final : public IEventsSink
{
public:
    // The engine (leave marks + mutable event access), the template table
    // (the contribution weights + the objective bindings) and the context
    // the validation guards read (consent/warrant/world/npc reads — the
    // sink methods carry no WorldContext, so the binding is the seam the
    // plugin wires). Bind all three once, before any tick (the engine's
    // binding pattern).
    void bind(
        EventEngine *engine, const EventTemplateTable *templates, WorldContext *ctx
    )
    {
        engine_    = engine;
        templates_ = templates;
        ctx_       = ctx;
    }

    // 3.2 step 5's hook (the pipeline fires it when either side is in an
    // event; the applied damage already rode the event's clamps).
    void onCombatDamage(const DamageEvent &ev) override;

    // The healer duty's hook (roles.cpp fires it for patients in an event).
    void onHeal(characterId healer, characterId target, std::uint64_t amount) override;

    // 3.7's publication: nothing to record (the books read prices, not the
    // reverse) — the override exists to keep the sink concrete.
    void onPriceChanged(
        regionId /*region*/, goodId /*good*/, std::uint64_t /*oldPrice*/, std::uint64_t /*newPrice*/
    ) override
    {}

private:
    const EventTemplate *templateOf(const Event &ev) const;

    EventEngine *engine_                 = nullptr;
    const EventTemplateTable *templates_ = nullptr;
    WorldContext *ctx_                   = nullptr;
};

// ContributionTickSystem — step 5's snapshot slice (registered AFTER the
// event engine's tick): every participant of every active event samples
// its power watermark on the A-106 interval (the first sight of a
// participant stamps the baseline; the refresh lands one interval later).
// The per-event stamp book prunes with the event (ended events leave no
// cadence state behind).
class ContributionTickSystem final : public IStepSystem
{
public:
    EventEngine *engine = nullptr;

    void run(WorldContext &ctx) override;

    // ---- save seam (task 7.1): the per-(event, actor) snapshot cadence rides
    // the save's "runtime" block — without it the load tick re-snapshots one
    // extra cadence step per actor.
    using SnapshotAtMap = std::map<std::uint64_t, std::map<characterId, std::uint64_t>>;
    [[nodiscard]] const SnapshotAtMap &snapshotAt() const noexcept
    {
        return snapshotAt_;
    }
    void restoreSnapshotAt(
        SnapshotAtMap rows
    )
    {
        snapshotAt_ = std::move(rows);
    }
    // The walk's cadence stamp rides the SAME runtime block (the 8.3 as-built):
    // the gate is part of the cadence contract its books serve — a fresh gate
    // would walk the loaded world one pass early and re-stamp the participants
    // whose power changed since their last stamp (the 8.3 soak's
    // TestSave_RngContinuationIdentical finding: the kept-alive world and the
    // resumed one diverged on exactly those book stamps).
    [[nodiscard]] std::uint64_t nextWalkAt() const noexcept
    {
        return nextWalkAt_;
    }
    void restoreNextWalkAt(
        std::uint64_t at
    ) noexcept
    {
        nextWalkAt_ = at;
    }

private:
    SnapshotAtMap snapshotAt_; // event -> actor -> last stamp
    // The walk's cadence gate (the 8.3 as-built, DEVIATIONS.md): the book
    // walk rides the A-106 interval — a war BATTLE's participant book is both
    // factions' membership, and walking it every tick measured ~2.1 ms/tick
    // on the 8.3 soak's step profile. Persisted with its books (see the
    // accessor note above).
    std::uint64_t nextWalkAt_ = 0;
};

} // namespace harrowmere::events

#endif // HARROWMERE_CORE_EVENTS_CONTRIBUTION_HPP
