// types.hpp — Harrowmere's doc-exact id aliases and the id generator (task 0.2).
//
// Aliases are verbatim from the design reference §Conventions (task 0.2 spec:
// "verbatim aliases from design reference §Conventions").
//
// R10 (common.md §3.2): ids are opaque — never encode meaning into an id, never
// compare ids across id spaces, ids are never reused (IdGenerator is monotonic).
//
// 0 is reserved in EVERY id space and is never issued:
//   * game_events.hpp (task 0.7): killer == 0 means "the environment"
//   * Territory (task 1.4): parent == 0 means "root / no parent"
//   * species 0 is hardcoded to "player" (task 1.1, Appendix A.2 species row)
// IdGenerator therefore starts every counter at FIRST_ID == 1. That value is
// structural (it follows from the reserved 0 above), not a tunable; it is
// proposed to the lead as Appendix A row A-121 via the deviation protocol (§1.4).

#pragma once

#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
#include <string>

// ---------------------------------------------------------------------------
// Doc-exact aliases (design reference §Conventions) — task 0.2 spec, verbatim.
// ---------------------------------------------------------------------------

using characterId = std::uint64_t;
using territoryId = std::uint64_t;
using factionId   = std::uint32_t;
using regionId    = std::uint16_t;
using speciesId   = std::uint16_t;

// f64 x/y/z per docs — the ONLY floating point allowed in game state (R5, §3.2).
struct Position { double x, y, z; };

// ---------------------------------------------------------------------------
// IdGenerator — monotonically increasing ids per id space (task 0.2).
//
//   * One independent counter per id space; ids are never reused (R10).
//   * counters_ is std::map — deterministic iteration order (R7, §3.2).
//   * Serialized in saves: state() / loadState() are task 7.1's contract.
//     loadState NEVER rewinds a counter — a stale save cannot re-issue ids.
//   * Narrow id spaces (factionId u32, regionId u16) saturate at their doc
//     width and assert in debug: exhausting a space is a programmer error,
//     and saturation — unlike wraparound — cannot reuse an issued id.
//   * speciesId has no generator space on purpose: species ids are assigned
//     from data files (task 1.1); only id 0 = "player" is hardcoded.
// ---------------------------------------------------------------------------
class IdGenerator {
public:
    // Lowest issued id of any id space. 0 is reserved (see file header).
    static constexpr std::uint64_t FIRST_ID = 1;

    // A fresh world starts every known counter at `next` (normally FIRST_ID).
    explicit IdGenerator(std::uint64_t next = FIRST_ID);

    characterId   nextCharacter();
    territoryId   nextTerritory();
    factionId     nextFaction();
    regionId      nextRegion();
    std::uint64_t nextEvidence();  // the docs' nextEvidenceId() (pinned decision, task 0.2)
    std::uint64_t nextWarrant();
    std::uint64_t nextEvent();

    // The id `space` WOULD issue next, without consuming it (diagnostics/tests).
    std::uint64_t peek(const std::string& space) const;

    // Save serialization (task 7.1): full counter map, deterministic order (std::map, R7).
    std::map<std::string, std::uint64_t> state() const;

    // Restore counters from a save. Monotonic: counters only move forward (R10).
    void loadState(const std::map<std::string, std::uint64_t>& s);

private:
    std::uint64_t nextFor(const char* space);

    template <class NarrowId>
    static NarrowId narrowId(std::uint64_t v) {
        if (v > static_cast<std::uint64_t>(std::numeric_limits<NarrowId>::max())) {
            assert(false && "id space exhausted at its doc width (R10: ids are never reused)");
            return std::numeric_limits<NarrowId>::max();  // deterministic saturation in release
        }
        return static_cast<NarrowId>(v);
    }

    std::uint64_t default_next_;                     // start value for not-yet-used spaces
    std::map<std::string, std::uint64_t> counters_;  // R7: ordered, deterministic
};

inline IdGenerator::IdGenerator(std::uint64_t next)
    : default_next_(next < FIRST_ID ? FIRST_ID : next) {
    // The id spaces R10 names, plus the record spaces game_events.hpp pins (task 0.7).
    for (const char* space : {"character", "territory", "faction", "region",
                              "evidence", "warrant", "event"}) {
        counters_.emplace(space, default_next_);
    }
}

inline std::uint64_t IdGenerator::nextFor(const char* space) {
    std::uint64_t& c = counters_.emplace(space, default_next_).first->second;
    if (c < FIRST_ID) c = FIRST_ID;  // defensive; ctor and loadState sanitize already
    const std::uint64_t id = c;
    ++c;                             // monotonic per space — ids are never reused (R10)
    return id;
}

inline characterId IdGenerator::nextCharacter() { return static_cast<characterId>(nextFor("character")); }
inline territoryId IdGenerator::nextTerritory() { return static_cast<territoryId>(nextFor("territory")); }
inline factionId   IdGenerator::nextFaction()   { return narrowId<factionId>(nextFor("faction")); }
inline regionId    IdGenerator::nextRegion()    { return narrowId<regionId>(nextFor("region")); }
inline std::uint64_t IdGenerator::nextEvidence() { return nextFor("evidence"); }
inline std::uint64_t IdGenerator::nextWarrant()  { return nextFor("warrant"); }
inline std::uint64_t IdGenerator::nextEvent()    { return nextFor("event"); }

inline std::uint64_t IdGenerator::peek(const std::string& space) const {
    const auto it = counters_.find(space);
    return it == counters_.end() ? default_next_ : it->second;
}

inline std::map<std::string, std::uint64_t> IdGenerator::state() const { return counters_; }

inline void IdGenerator::loadState(const std::map<std::string, std::uint64_t>& s) {
    for (const auto& entry : s) {
        const std::uint64_t loaded = entry.second < FIRST_ID ? FIRST_ID : entry.second;
        const auto it = counters_.find(entry.first);
        if (it == counters_.end()) {
            counters_.emplace(entry.first, loaded);
        } else if (loaded > it->second) {
            it->second = loaded;  // never rewind — a stale save cannot re-issue ids (R10)
        }
    }
}
