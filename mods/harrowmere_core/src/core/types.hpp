#ifndef HARROWMERE_CORE_CORE_TYPES_HPP
#define HARROWMERE_CORE_CORE_TYPES_HPP

// The doc-exact type aliases every harrowmere_core header includes (common.md
// §3.2, rule R10: "ids are opaque" - never encode meaning into an id, never
// compare ids across id spaces).
//
// Provenance (task 0.7): this file's full content is pinned verbatim by task
// 0.2 ("types.hpp - verbatim aliases from design reference §Conventions"), but
// 0.2 runs in parallel with 0.7 and 0.7's event structs reference the aliases,
// so the pinned block lands here first. Task 0.2 owns the file from then on
// and completes it (see DEVIATIONS.md); until that card lands the file is
// append-only and carries exactly the pinned block below.

#include <cstdint>

#include <algorithm> // std::max
#include <map>
#include <string>

namespace harrowmere
{

// types.hpp - verbatim aliases from design reference §Conventions
using characterId = std::uint64_t;
using territoryId = std::uint64_t;
using factionId   = std::uint32_t;
using regionId    = std::uint16_t;
using speciesId   = std::uint16_t;

struct Position
{
    double x, y, z; // f64 x/y/z per docs
};

// ---- IdGenerator (task 0.2) -------------------------------------------------
//
// Monotonic per-id-space id generation (R10: ids are opaque, never reused,
// never meaningful across spaces). One counter per named space ("character",
// "evidence", "warrant", "event", "region"); a counter's value is the next id
// that space will issue. 0 is reserved in every id space (the docs'
// conventions: killer 0 = environment, report source 0 = ward/anonymous), so
// every space starts at FIRST_ID = 1. std::map keeps save output and iteration
// deterministic (R7); the counters serialize with the save (task 7.1) and
// loadState merges with max() so a stale save can never rewind a live
// generator.
class IdGenerator
{
public:
    static constexpr std::uint64_t FIRST_ID = 1; // 0 is reserved in every id space

    IdGenerator() = default;

    // Fresh-world start value, caller-chosen (a loaded save seeds a new
    // generator with its stored "next ids"). Values below FIRST_ID clamp up
    // to FIRST_ID.
    explicit IdGenerator(
        std::uint64_t next
    )
        : start_(next < FIRST_ID ? FIRST_ID : next)
    {}

    characterId nextCharacter()
    {
        return nextIn("character");
    }

    std::uint64_t nextEvidence()
    {
        return nextIn("evidence");
    }

    std::uint64_t nextWarrant()
    {
        return nextIn("warrant");
    }

    std::uint64_t nextEvent()
    {
        return nextIn("event");
    }

    std::uint64_t nextRegion()
    {
        return nextIn("region");
    }

    // The next id `space` will issue; consumes nothing. A space that has
    // never been drawn reads as the generator's start value.
    std::uint64_t peek(
        const std::string &space
    ) const
    {
        const auto it = counters_.find(space);
        return it == counters_.end() ? start_ : it->second;
    }

    // Save/restore (task 7.1 serializes this map verbatim; std::map keeps
    // the order deterministic, R7).
    std::map<std::string, std::uint64_t> state() const
    {
        return counters_;
    }

    // Merge a saved counter snapshot into this generator. Never rewinds:
    // each space takes the max of its current and its saved value, and a
    // space not drawn yet takes max(saved, start_). Ids are never reused
    // (R10).
    void loadState(
        const std::map<std::string, std::uint64_t> &saved
    )
    {
        for (const auto &entry : saved)
        {
            const auto it = counters_.find(entry.first);
            if (it == counters_.end())
            {
                const std::uint64_t begin = entry.second < start_ ? start_ : entry.second;
                counters_.emplace(entry.first, begin);
            } else if (it->second < entry.second)
            {
                it->second = entry.second;
            }
        }
    }

private:
    // Draw the next id from `space`: a fresh space starts at start_, then
    // every draw returns the current value and advances the counter.
    std::uint64_t nextIn(
        const std::string &space
    )
    {
        const auto inserted = counters_.emplace(space, start_);
        return inserted.first->second++;
    }

    std::uint64_t start_ = FIRST_ID;                // start value for spaces not yet drawn
    std::map<std::string, std::uint64_t> counters_; // space -> next id to issue
};

} // namespace harrowmere

#endif // HARROWMERE_CORE_CORE_TYPES_HPP
