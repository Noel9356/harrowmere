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
#include <vector>

#include "core/enums.hpp"  // StatType, SkillType

namespace harrowmere::data {

struct DataError;  // forward decl (task 0.4)

// ClassData — runtime form of one classes.json row.
//
// statWeights / skillWeights are scaled u32 (1.0 → SCALE). The worldgen
// NPC generator (1.5) multiplies a class's stat weights against the
// species statMultiplier when seeding an NPC's starting stats. Missing
// keys default to SCALE (1.0x weight = no adjustment).
struct ClassData {
    std::uint32_t id = 0;
    std::string   key;

    // Expanded from JSON map; missing keys → SCALE (1.0x weight).
    std::array<std::uint32_t, static_cast<std::size_t>(StatType::COUNT)>  statWeights{};
    std::array<std::uint32_t, static_cast<std::size_t>(SkillType::COUNT)> skillWeights{};

    // startingSkills: integer skill levels granted at NPC creation.
    // 0 = not granted. Array-indexed by SkillType.
    std::array<std::uint32_t, static_cast<std::size_t>(SkillType::COUNT)> startingSkills{};
};

// ClassesLoader — parses + validates data/classes.json.
class ClassesLoader {
public:
    ClassesLoader() = default;
    ~ClassesLoader() = default;
    ClassesLoader(const ClassesLoader&) = delete;
    ClassesLoader& operator=(const ClassesLoader&) = delete;

    bool load(const std::string& jsonPath, std::vector<DataError>& errors);

    const ClassData* classById(std::uint32_t id) const;
    const ClassData* classByKey(const std::string& key) const;

    bool        empty() const noexcept { return classes_.empty(); }
    std::size_t size()  const noexcept { return classes_.size(); }
    const std::vector<ClassData>& all() const noexcept { return classes_; }

private:
    std::vector<ClassData> classes_;
};

} // namespace harrowmere::data
