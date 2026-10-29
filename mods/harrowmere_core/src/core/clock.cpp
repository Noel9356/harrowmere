// clock.cpp — see clock.hpp.
//
// Plan reference: common.md §3.2 (R2, R5, R8); task 0.3 (D-TIME-1).

#include "clock.hpp"

#include <cassert>

namespace harrowmere::core {

void WorldClock::advanceTick() {
    // R2: every division by a value that can be zero is guarded. tickRate is
    // config-flavored ([harrowmere].tick_rate) and a misconfiguration to 0
    // would otherwise crash the tick — guard explicitly with the pinned
    // fallback (no advance).
    if (tickRate == 0) {
        assert(false && "WorldClock.tickRate must be > 0 — set by [harrowmere].tick_rate");
        return;
    }

    // --- realTime: 1 real second per `tickRate` ticks ---
    // Bresenham-style accumulator: per real second, `tickRate` ticks fire and
    // the carry resolves to exactly +1 realTime. No float, no drift.
    realCarry += 1;
    realTime  += realCarry / tickRate;
    realCarry  = realCarry % tickRate;

    // --- worldTime: timeScale game-seconds per real second, over tickRate ticks ---
    // Same accumulator pattern. For the pinned defaults (timeScale=60,
    // tickRate=10) every tick lands exactly +6 game-seconds with zero carry.
    // For non-divisible rates (e.g. timeScale=60, tickRate=11) the carry
    // distributes the lost fractional second across ticks so a full real
    // second's worth of ticks still sums to exactly `timeScale`.
    worldCarry += timeScale;
    worldTime  += worldCarry / tickRate;
    worldCarry  = worldCarry % tickRate;
}

}  // namespace harrowmere::core
