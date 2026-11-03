#ifndef HARROWMERE_CORE_EVENTS_DIFFICULTY_HPP
#define HARROWMERE_CORE_EVENTS_DIFFICULTY_HPP

// difficulty.hpp — task 5.2: the self-balancing difficulty loop, implemented
// so the doc's underflow bug cannot recur (the design reference's own
// warning, quoted on updateStateDifficulty below: "delta must be signed;
// unsigned arithmetic underflowed on failure" — R1's founding example).
//
// The three functions are design reference verbatim:
//   updateStateDifficulty(state, performance) — SIGNED delta (i64), the
//       moving-average update divided by the attempt count, clamped to
//       [0, SCALE] on both fields.
//   binaryPerformance(success) — SCALE on success, 0 on failure.
//   continuousPerformance(objectives) — the per-objective normalized
//       average with the cap-zero guard (R2: "if (denominator == 0) return
//       <pinned fallback>" lives at the call site — a cap-0 objective is
//       SKIPPED, and an all-skipped span reads 0).
//
// Cadence (pinned D-DIFF-1): one difficulty update per state ATTEMPT — a
// state attempt concludes when a declared transition fires, or when the
// maxDurationDays forced exit resolves the event. Per-transition, never
// per-tick: the doc's `attempts` counter is the attempt ledger, and the
// pinned division test (4000 → 0 at attempts 1, 3200 at attempts 5) is an
// attempt-count read, not a tick count.
//
// Performance selection at the attempt's conclusion: a state that declares
// capped objectives reads CONTINUOUS performance (its objective amounts —
// derived from the event books: the Σ of each participant's validated
// actionAmounts for the objective's action type); an objective-less state
// reads BINARY performance (a declared transition fired = the attempt
// succeeded; the maxDurationDays forced exit = it did not).

#include <cstdint>
#include <vector>

#include "core/constants.hpp"  // SCALE (A-1)
#include "events/engine.hpp"   // EventState, Event (the doc structs)
#include "events/template.hpp" // Objective, TemplateState, EventTemplate

namespace harrowmere::events
{

// updateStateDifficulty — design reference verbatim (R1: delta is SIGNED —
// the doc's own warning: "delta must be signed; unsigned arithmetic
// underflowed on failure"). SCALE clamps both fields.
void updateStateDifficulty(EventState &st, std::uint32_t performance);

// binaryPerformance — design reference verbatim.
std::uint32_t binaryPerformance(bool success) noexcept;

// The runtime form of one objective sample: the template row's cap plus the
// amount the event books accrued for it. continuousPerformance's input —
// the design reference's `objective.amount` field, which lives in the event
// books (actionAmounts) rather than on the template row (the template is
// shared by every instance; the accrual is per-instance state).
struct ObjectiveSample
{
    std::uint32_t cap    = 0;
    std::uint64_t amount = 0;
};

// continuousPerformance — design reference verbatim: the cap-zero guard
// skips the objective (R2's pinned pattern — an uncapped objective has no
// normalization denominator), the survivors normalize to min(SCALE,
// amount * SCALE / cap) and average; an empty (or all-skipped) span reads 0.
std::uint32_t continuousPerformance(const std::vector<ObjectiveSample> &objectives);

// objectiveAmount — the event books' accrual for one objective row: the Σ of
// every PARTICIPANT's validated actionAmounts for the objective's action
// type (sorted participant walk, R7). This is the doc's `objective.amount`
// — DAMAGE/HEALING objectives accrue through their own slots (the v1
// bindings `damage_to_targets` / `healing_given`), OBJECTIVE-type
// objectives through the OBJECTIVE slot (the `object_delivered` /
// `territory_reached` bindings' claims validate into it, 5.3).
std::uint64_t objectiveAmount(const Event &ev, const Objective &objective);

// collectObjectiveSamples — the exited state's objective rows as runtime
// samples (declared order; deterministic input to continuousPerformance).
std::vector<ObjectiveSample> collectObjectiveSamples(const Event &ev, const EventTemplate &tpl);

// performanceForState — D-DIFF-1's read: continuous when the state declares
// at least one capped objective, binary otherwise (`success` is the
// conclusion verdict: a declared transition fired = true; the
// maxDurationDays forced exit = false).
std::uint32_t performanceForState(const Event &ev, const EventTemplate &tpl, bool success);

// noteStateAttempt — the engine's per-transition hook (D-DIFF-1): compute
// the concluded attempt's performance and run updateStateDifficulty ONCE.
// `success` only feeds the binary arm (see performanceForState).
void noteStateAttempt(Event &ev, const EventTemplate &tpl, bool success);

// ---- the 8.4 instrumentation seam (the 5.4 counter precedent) -----------------
//
// Every concluded attempt's post-update successRate joins a process-wide
// sample book: the balance pass's equilibrium read (successRate hovers
// 3000-7000 over time) needs the POST-attempt values, which are invisible to
// store walks — the attempt concludes and the event ends in the same tick
// (the per-tick sweep clears the ended book). Not sim state: never hashed,
// never saved; the book grows with attempt volume and is the probe's job to
// drain (the harness drains it on its daily event read).
std::vector<std::uint32_t> drainDifficultySamples();
void clearDifficultySamples();

} // namespace harrowmere::events

#endif // HARROWMERE_CORE_EVENTS_DIFFICULTY_HPP
