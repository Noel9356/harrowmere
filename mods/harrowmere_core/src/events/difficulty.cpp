// difficulty.cpp — task 5.2's implementation (see difficulty.hpp for the
// pinned cadence + the performance selection). The three doc functions are
// kept line-for-line against the design reference; the only additions are
// the runtime objective-sample bridge (the template carries the cap, the
// event books carry the amount) and the per-attempt hook the engine calls
// at each state exit.

#include "events/difficulty.hpp"

#include <algorithm>

#include "core/math.hpp" // clampScalar (task 0.2)

namespace harrowmere::events
{

namespace
{

// The 8.4 instrumentation seam's book (see difficulty.hpp). Process-wide,
// never hashed, never saved — the probe's job to drain.
std::vector<std::uint32_t> g_difficultySamples;

} // namespace

std::vector<std::uint32_t> drainDifficultySamples()
{
    std::vector<std::uint32_t> out;
    out.swap(g_difficultySamples);
    return out;
}

void clearDifficultySamples()
{
    g_difficultySamples.clear();
}

void updateStateDifficulty(
    EventState &st, std::uint32_t performance
)
{
    // design reference verbatim — R1: delta is SIGNED (the doc's own
    // warning: "delta must be signed; unsigned arithmetic underflowed on
    // failure"). SCALE clamps both fields.
    const std::int64_t delta = static_cast<std::int64_t>(performance) - static_cast<std::int64_t>(st.successRate);
    st.attempts += 1;
    st.successRate = static_cast<std::uint32_t>(clampScalar(
        static_cast<std::int64_t>(st.successRate) + delta / static_cast<std::int64_t>(st.attempts),
        static_cast<std::int64_t>(0),
        static_cast<std::int64_t>(SCALE)
    ));
    st.difficulty  = static_cast<std::uint32_t>(clampScalar(
        static_cast<std::int64_t>(st.difficulty) + delta / static_cast<std::int64_t>(st.attempts),
        static_cast<std::int64_t>(0),
        static_cast<std::int64_t>(SCALE)
    ));
}

std::uint32_t binaryPerformance(
    bool success
) noexcept
{
    return success ? static_cast<std::uint32_t>(SCALE) : 0u;
}

std::uint32_t continuousPerformance(
    const std::vector<ObjectiveSample> &objectives
)
{
    // design reference verbatim: cap-zero objectives are skipped (R2's
    // guard at the call site), the survivors normalize and average; count
    // == 0 reads 0 (the doc's own `if (count == 0) return 0`).
    std::uint64_t count = 0;
    std::uint64_t total = 0;
    for (const ObjectiveSample &o : objectives)
    {
        if (o.cap == 0)
            continue; // cap-zero guard (R2 pattern)
        total += std::min<std::uint64_t>(SCALE, o.amount * SCALE / o.cap);
        count += 1;
    }
    return count == 0 ? 0u : static_cast<std::uint32_t>(total / count);
}

std::uint64_t objectiveAmount(
    const Event &ev, const Objective &objective
)
{
    const std::size_t slot = static_cast<std::size_t>(objective.actionType);
    std::uint64_t total    = 0;
    for (const characterId p : ev.participants) // sorted (R7)
    {
        const auto it = ev.actionAmounts.find(p);
        if (it != ev.actionAmounts.end())
        {
            total += it->second[slot];
        }
    }
    return total;
}

std::vector<ObjectiveSample> collectObjectiveSamples(
    const Event &ev, const EventTemplate &tpl
)
{
    std::vector<ObjectiveSample> samples;
    const TemplateState *state = tpl.state(ev.currentState.stateId);
    if (state == nullptr)
    {
        return samples;
    }
    samples.reserve(state->objectives.size());
    for (const Objective &o : state->objectives) // declared order (deterministic)
    {
        samples.push_back(ObjectiveSample{o.cap, objectiveAmount(ev, o)});
    }
    return samples;
}

std::uint32_t performanceForState(
    const Event &ev, const EventTemplate &tpl, bool success
)
{
    const TemplateState *state = tpl.state(ev.currentState.stateId);
    if (state == nullptr)
    {
        return binaryPerformance(success);
    }
    for (const Objective &o : state->objectives)
    {
        if (o.cap != 0)
        {
            return continuousPerformance(collectObjectiveSamples(ev, tpl));
        }
    }
    return binaryPerformance(success); // go/no-fail state (no capped objectives)
}

void noteStateAttempt(
    Event &ev, const EventTemplate &tpl, bool success
)
{
    updateStateDifficulty(ev.currentState, performanceForState(ev, tpl, success));
    g_difficultySamples.push_back(ev.currentState.successRate); // the 8.4 instrumentation seam
}

} // namespace harrowmere::events
