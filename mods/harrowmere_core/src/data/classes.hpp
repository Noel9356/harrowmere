// classes.hpp — typed view over data/classes.json.
//
// Classes are NPC generation variety + optional player creation preset
// (task 2.0). The design reference mentions classes once ("species, classes,
// items, quests, and recipes are data-driven") without a struct; the
// schema is pinned by task 1.1:
//   { "id", "key", "statWeights" {..}, "skillWeights" {..}, "startingSkills" {..} }
//
// `suppression` in laws is keyed by SocialClassId (NOT this ClassData) —
// pinned decision D-CORRUPT-1.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/enums.hpp" // StatType, SkillType

namespace harrowmere::data
{

struct DataError; // forward decl (task 0.4)

// SkillRequirement — the runtime form of one skill's entry in a class row's
// "skillRequirements" block (task 2.3: "Skill requirements ... are declared
// in classes.json per skill": `{ "MELEE": { "reqStats": {"STRENGTH": 5},
// "reqSkills": {}, "reqActions": [] } }`).
//
// All three requirement kinds are plain point THRESHOLDS (not SCALE-grid
// values): a character must hold reqStats[s] >= stat points, reqSkills[k]
// >= skill points, and the action counters named by reqActions at >= count.
// An all-zero requirement (the default) gates nothing.
struct SkillRequirement
{
    // Minimum stat points per StatType (0 = no requirement).
    std::array<std::uint32_t, static_cast<std::size_t>(StatType::COUNT)> reqStats{};

    // Minimum skill points per SkillType (0 = no requirement).
    std::array<std::uint32_t, static_cast<std::size_t>(SkillType::COUNT)> reqSkills{};

    // Action requirements, in authored order: (counter key, required count).
    // v1 ships the two common counters — kills-by-type and crafts-by-type
    // ("kill_non_sapient_10" names counter "kill_non_sapient" at 10); the
    // vocabulary is data-extensible (unknown keys simply read 0 and stay
    // unmet — fail closed). Counters arrive from NpcState::actionCounters
    // (task 3.4) or the player action log (task 3.10) through
    // WorldContext::actionCounters.
    std::vector<std::pair<std::string, std::uint64_t>> reqActions;
};

// ClassData — runtime form of one classes.json row.
//
// statWeights / skillWeights are scaled u32 (1.0 → SCALE). The worldgen
// NPC generator (1.5) multiplies a class's stat weights against the
// species statMultiplier when seeding an NPC's starting stats. Missing
// keys default to SCALE (1.0x weight = no adjustment).
struct ClassData
{
    std::uint32_t id = 0;
    std::string key;

    // Expanded from JSON map; missing keys → SCALE (1.0x weight).
    std::array<std::uint32_t, static_cast<std::size_t>(StatType::COUNT)> statWeights{};
    std::array<std::uint32_t, static_cast<std::size_t>(SkillType::COUNT)> skillWeights{};

    // startingSkills: integer skill levels granted at NPC creation.
    // 0 = not granted. Array-indexed by SkillType.
    std::array<std::uint32_t, static_cast<std::size_t>(SkillType::COUNT)> startingSkills{};

    // skillRequirements (task 2.3): per-skill purchase gates, array-indexed
    // by SkillType. All-zero rows (the default; also what an undeclared or
    // empty requirement parses to) gate nothing — buySkill enforces only
    // what a class's block actually pins. The character's class row is
    // resolved at purchase time (progression/skills.cpp): the player's
    // PlayerCreation.classKey (task 2.0's store record); classless
    // characters (NPCs without a title-chain class key) have no gate.
    std::array<SkillRequirement, static_cast<std::size_t>(SkillType::COUNT)> skillRequirements{};
};

// ClassesLoader — parses + validates data/classes.json.
class ClassesLoader
{
public:
    ClassesLoader()                                 = default;
    ~ClassesLoader()                                = default;
    ClassesLoader(const ClassesLoader &)            = delete;
    ClassesLoader &operator=(const ClassesLoader &) = delete;

    bool load(const std::string &jsonPath, std::vector<DataError> &errors);

    const ClassData *classById(std::uint32_t id) const;
    const ClassData *classByKey(const std::string &key) const;

    bool empty() const noexcept
    {
        return classes_.empty();
    }
    std::size_t size() const noexcept
    {
        return classes_.size();
    }
    const std::vector<ClassData> &all() const noexcept
    {
        return classes_;
    }

private:
    std::vector<ClassData> classes_;
};

} // namespace harrowmere::data
