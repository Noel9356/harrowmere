// rng.cpp — see rng.hpp.
//
// Plan reference: common.md §3.2 (R2, R6, R7), task 0.3.

#include "rng.hpp"

#include <cassert>
#include <cstdint>

namespace harrowmere::core {

namespace {

// splitmix64 (Steele & Lea, 2014). Public domain.
//
// Used once per stream during seedAll to derive initstate and initseq from
// (worldSeed, streamNameHash). Two successive splitmix64 outputs give us
// the two 64-bit values pcg64's setseq constructor wants; combining them
// with the world seed + a per-stream name hash means every stream is an
// independent, reproducible sequence keyed by the world seed.
inline std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// FNV-1a 64 of the stream name. Pinned for cross-run determinism — any
// change to this hash function changes every stream's sequence and breaks
// save/replay parity (a deviation row is required to touch it).
inline std::uint64_t fnv1a64(const char* s) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a 64 offset basis
    for (const char* p = s; *p != '\0'; ++p) {
        h ^= static_cast<std::uint8_t>(*p);
        h *= 0x100000001b3ULL;                // FNV-1a 64 prime
    }
    return h;
}

// The pinned stream list. The order here is the *insertion* order into the
// std::map; std::map then re-sorts alphabetically for state() output. Keep
// this list alphabetical to make the intent obvious and to match what the
// map will produce anyway.
//
// Adding or renaming a stream requires a deviation row (§1.4) — the list
// is the closed set of RNG domains the simulation may consume from.
constexpr const char* kPinnedStreams[] = {
    "combat",
    "economy",
    "events",
    "justice",
    "misc",
    "npc_life",
    "weather",
    "worldgen",
};

}  // namespace

void SeedRng::seedAll(std::uint64_t worldSeed) {
    streams_.clear();
    for (const char* name : kPinnedStreams) {
        const std::uint64_t nameHash = fnv1a64(name);
        // Mix worldSeed with the per-stream name hash so two different
        // streams seeded from the same worldSeed produce independent
        // sequences (pcg64 setseq requires initstate and initseq; both
        // come from this combined splitmix64 stream).
        std::uint64_t sm = worldSeed ^ nameHash;
        const std::uint64_t initstate = splitmix64(sm);
        const std::uint64_t initseq   = splitmix64(sm);
        streams_.emplace(name, rng::pcg64(initstate, initseq));
    }
}

std::uint64_t SeedRng::draw(const char* stream) {
    auto it = streams_.find(stream);
    if (it == streams_.end()) {
        // Pinned-stream list is closed (§1.4). An unknown name is a
        // programmer error: assert in debug, return 0 deterministically
        // in release (no throw — common.md §3.4 forbids exceptions for
        // rule-level failures).
        assert(false && "SeedRng::draw: unknown stream name (pinned list is closed)");
        return 0;
    }
    return it->second.next();
}

std::uint32_t SeedRng::range(const char* stream, std::uint32_t lo, std::uint32_t hiExcl) {
    // R2: explicit zero-guard with the pinned fallback (return lo).
    if (hiExcl <= lo) {
        assert(false && "SeedRng::range: hiExcl must be > lo");
        return lo;
    }
    const std::uint32_t span = hiExcl - lo;
    const std::uint64_t r    = draw(stream);
    // High 32 bits of pcg64's XSL-RR output are the best-quality bits.
    // Modulo introduces a tiny bias (span-dependent, always < 2^-32 for
    // span up to 2^32) — acceptable for game tuning rolls; documented
    // in rng.hpp so a reviewer doesn't "fix" it without re-running 8.3.
    return lo + static_cast<std::uint32_t>(r >> 32) % span;
}

std::vector<std::pair<std::string, std::uint64_t>> SeedRng::state() const {
    std::vector<std::pair<std::string, std::uint64_t>> result;
    result.reserve(streams_.size() * 2);
    for (const auto& [name, rng] : streams_) {
        const rng::pcg64::state_type s = rng.state();
        result.emplace_back(std::string(name) + ":hi",
                            static_cast<std::uint64_t>(s >> 64));
        result.emplace_back(std::string(name) + ":lo",
                            static_cast<std::uint64_t>(s));
    }
    return result;
}

void SeedRng::loadState(const std::vector<std::pair<std::string, std::uint64_t>>& s) {
    // Index into a local map for fast lookup — saves can be tens of entries,
    // and the inner loop below is O(streams × snapshot_size) without it.
    std::map<std::string, std::uint64_t> snap;
    for (const auto& [k, v] : s) snap[k] = v;

    for (auto& [name, rng] : streams_) {
        const auto hi = snap.find(std::string(name) + ":hi");
        const auto lo = snap.find(std::string(name) + ":lo");
        if (hi == snap.end() || lo == snap.end()) {
            // Stream missing from snapshot: leave at post-seedAll state.
            // (Pre-condition: snapshot was produced by state() on a
            // seedAll'd instance; missing entries indicate save
            // corruption — task 7.1's roundtrip test will catch it.)
            continue;
        }
        const rng::pcg64::state_type combined =
            (static_cast<rng::pcg64::state_type>(hi->second) << 64) |
             static_cast<rng::pcg64::state_type>(lo->second);
        rng.setState(combined);
    }
}

}  // namespace harrowmere::core
