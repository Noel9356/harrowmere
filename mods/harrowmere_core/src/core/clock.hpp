// clock.hpp — WorldClock: deterministic game-time and real-time tracking.
//
// Plan reference: common.md §3.2 (R5/R8) and task 0.3 (D-TIME-1).
//
// WorldClock holds the two pinned clocks from D-TIME-1:
//
//   * worldTime — game seconds since world epoch; advances TIME_SCALE per real
//     second. Used for every doc duration expressed in days/hours/minutes
//     (evidence LIMIT_DAYS, WARRANT_DURATION, war notice, seasons, aging,
//     sentences, decay intervals, etc.).
//
//   * realTime  — real seconds since session start. Used for the three doc
//     rules the docs explicitly pin to real time:
//       (1) territory abandonment ("14 real-world days"),
//       (2) activity validation ("30 validated active minutes"),
//       (3) AFK / suspicion exemptions.
//
// Both clocks advance on every advanceTick() call. Integer math only (R5):
// no float accumulator drives either clock. dayPhase01() returns a double but
// is computed from worldTime via a single division each call — the value is
// never accumulated, so determinism is safe.
//
// TIME_SCALE and tickRate are configuration values (task 0.1's [harrowmere]
// section); defaults: timeScale = 60, tickRate = 10.

#pragma once

#include <cstdint>

namespace harrowmere::core
{

// A-3: 86'400 game seconds per game day
// ("a game-day is a real calendar day in game-time" — common.md Appendix A.1).
inline constexpr std::uint64_t DAY_LENGTH_SECONDS = 86'400;

// WorldClock — value type; safe to copy. The carry accumulators are part of
// the persisted state (task 7.1) so a save taken mid-tick-rate-change resumes
// cleanly without skipping or duplicating a game-second.
struct WorldClock
{
    std::uint64_t worldTime = 0;  // game seconds since world epoch (u64 per docs)
    std::uint64_t realTime  = 0;  // real seconds since session start
    std::uint32_t timeScale = 60; // game seconds per real second (D-TIME-1)
    std::uint32_t tickRate  = 10; // sim ticks per real second  (D-TICK-1)

    // Advance one tick on both clocks.
    //
    //   worldTime += timeScale / tickRate   (integer; remainder carried)
    //   realTime  += 1            / tickRate   (integer; remainder carried)
    //
    // The carry is the Bresenham-style accumulator: when tickRate does not
    // divide timeScale (or 1) cleanly, the remainder is preserved across
    // ticks so a full second's worth of ticks always lands exactly the
    // doc-mandated total. Without the carry, integer truncation would
    // silently lose game seconds and drift the simulation away from any
    // peer running the same seed (R6 determinism violation).
    //
    // tickRate must be > 0 — caller invariant (debug-asserted; release
    // returns without advancing to avoid the divide-by-zero trap, R2).
    void advanceTick();

    // Current game-day index (worldTime / 86'400). Same value across the
    // whole day. Used by aging (task 3.4), reputation decay (4.4), seasons.
    std::uint64_t gameDay() const noexcept
    {
        return worldTime / DAY_LENGTH_SECONDS;
    }

    // Phase within the current game-day, in [0, 1).
    //   0.0  = midnight (start of day)
    //   0.25 = 06:00
    //   0.5  = noon
    //   0.75 = 18:00
    //
    // Drives the sun direction (7.2P1 scene bridge) and the NIGHT suspicion
    // mask (4.5). Returns double per the spec — the value is recomputed each
    // call from worldTime; it is never stored, never accumulated.
    double dayPhase01() const noexcept
    {
        return static_cast<double>(worldTime % DAY_LENGTH_SECONDS) / static_cast<double>(DAY_LENGTH_SECONDS);
    }

    // --- persisted carry state (task 7.1) ---
    // Public so the save layer can serialize them with the rest of the clock;
    // do not mutate directly — call advanceTick().
    std::uint64_t worldCarry = 0;
    std::uint64_t realCarry  = 0;
};

} // namespace harrowmere::core
