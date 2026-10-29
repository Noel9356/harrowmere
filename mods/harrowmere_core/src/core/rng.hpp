// rng.hpp — SeedRng: deterministic, named-stream RNG (R6).
//
// Plan reference: common.md §3.2 (R6), task 0.3.
//
// All randomness in the game draws from one of the pinned named streams.
// Each stream is an independent pcg64 sequence seeded deterministically from
// (worldSeed, streamName) via splitmix64. Reordering two features does not
// change the other's sequence, and re-running the same world seed produces
// byte-identical streams — this is what makes the simulation reproducible.
//
// === PINNED STREAMS (use these EXACT names; new streams need a deviation row) ===
//
//   "worldgen"  — world generation: regions, territories, populations, names
//   "npc_life"  — NPC needs, duties, migration, replacement rolls
//   "weather"   — per-region weather state machine (task 3.8)
//   "events"    — event engine randomness: transitions, spawns (5.x)
//   "justice"   — evidence/warrant/conviction rolls (4.x)
//   "economy"   — price recompute, production jitter (3.7)
//   "combat"    — hit chance and damage rolls (3.2/3.3)
//   "misc"      — debug/test convenience only — NEVER used by a game-rule roll
//
// === PITFALL ===
//   Never call draw("misc") for a game-rule roll — pick the pinned stream by
//   domain. Tests for a system should seed only their stream so failures
//   localize (a regression in `combat` should not look like a regression in
//   `justice`).
//
// Save/restore (task 7.1):
//   state() returns 2 entries per pinned stream ("<stream>:hi" / "<stream>:lo")
//   carrying the high and low 64 bits of the pcg64 state_. The stream
//   selector is NOT serialized — callers must re-seed (which sets the same
//   stream) before loadState, so the saved state_ lands in a matching
//   stream.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "rng/pcg64.hpp"

namespace harrowmere::core {

// SCALE = 10'000 — common.md §3.2, Appendix A row A-1.
// Re-declared here (as RNG_SCALE) so rng.hpp does not depend on math.hpp
// (avoids a header cycle: math.hpp's tests will later need SeedRng).
inline constexpr std::uint64_t RNG_SCALE = 10'000;

class SeedRng {
public:
    // Initialize all pinned streams deterministically from `worldSeed`.
    // Idempotent: calling again resets every stream to the same state.
    void seedAll(std::uint64_t worldSeed);

    // Draw a uniform 64-bit value from the named stream.
    //
    // Unknown stream name: programmer error (the pinned list is closed) —
    // debug-asserts and returns 0 in release (the deterministic-recovery
    // pattern from common.md §3.4; never throws).
    std::uint64_t draw(const char* stream);

    // Uniform integer in [lo, hiExcl). Requires hiExcl > lo.
    //
    // Uses the high 32 bits of draw() — the best-quality bits of pcg64's
    // XSL-RR output — and a single modulo. Bias is < 2^-32 for span up to
    // 2^32, which is negligible for game tuning rolls; the trade-off is
    // documented here so a future reviewer doesn't "fix" it without
    // re-running the determinism soak (8.3).
    //
    // R2 guard: if hiExcl <= lo, debug-asserts and returns lo (no division
    // by zero when span == 0).
    std::uint32_t range(const char* stream, std::uint32_t lo, std::uint32_t hiExcl);

    // Convenience for probabilities on the SCALE grid: returns [0, RNG_SCALE).
    // Equivalent to range(stream, 0, 10'000).
    std::uint32_t pct(const char* stream) {
        return range(stream, 0, static_cast<std::uint32_t>(RNG_SCALE));
    }

    // --- save / restore (task 7.1) ---
    //
    // Returns 2 entries per pinned stream:
    //   ("<stream>:hi", high 64 bits of state_)
    //   ("<stream>:lo", low  64 bits of state_)
    // Iteration order is alphabetical by stream name (std::map ordering) so
    // save output is byte-stable across runs.
    std::vector<std::pair<std::string, std::uint64_t>> state() const;

    // Restore stream state from a previously-saved snapshot.
    //
    // Pre: seedAll(worldSeed) has been called with the same world seed that
    // produced the snapshot (so the stream selectors match). Streams missing
    // from the snapshot are left at their post-seedAll state — the caller
    // is expected to snapshot all streams via state().
    void loadState(const std::vector<std::pair<std::string, std::uint64_t>>& s);

private:
    // std::map (not unordered_map): R7 — anything whose iteration order
    // could affect outcomes uses an ordered container. state() iterates
    // this map; unordered iteration would make save files non-deterministic
    // across runs (the soak harness 8.1 would flag it).
    std::map<std::string, rng::pcg64> streams_;
};

}  // namespace harrowmere::core
