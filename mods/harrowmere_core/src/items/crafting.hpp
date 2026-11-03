#ifndef HARROWMERE_CORE_ITEMS_CRAFTING_HPP
#define HARROWMERE_CORE_ITEMS_CRAFTING_HPP

// crafting.hpp — the recipe table and the craft action (task 2.4).
//
// Recipes are DATA (design rule: "Events, states, flags, species, classes,
// items, quests, and recipes are data-driven through JSON"); the card pins
// the row shape:
//     { "key", "inputs": {itemKey: n}, "output": itemKey,
//       "reqSkill": {"CRAFTING": x}, "station": null }
// v1 ships six recipes (bandage, torch, iron sword, leather armor, rations,
// mana tonic — the mana-rich consumable, the lore hook). The `station` field
// is parsed and pinned to null: stations are Appendix E ("Recipes with
// stations ... reserved"), so a non-null station is a data error at load.
//
// Input/output keys are ITEM keys (items.json rows — the card writes
// "{itemKey: n}"); the loader cross-references every key against the
// DataRegistry at load time, so a dangling reference fails loudly at startup
// (the B.8 note's "loaders validate referenced item/good keys exist").
// Materials ship as stackable item rows in items.json; when the regional
// economy (3.7) brings goods.json online, mana_herb joins it per the B.8
// note (DEVIATIONS.md carries the note).
//
// The craft action: consume inputs -> check reqSkill (plain point
// thresholds, the same semantics progression's SkillRequirement uses) ->
// produce the output (stackable: +1 unit; instanced: one spawned instance
// into the bag's loose list) -> increment the craft counter (2.3's
// reqActions input; the player-side write path is 3.10's
// PlayerActions::recordCraft, NpcState::actionCounters takes the NPC side in
// 3.4). Crafting never touches XP (the docs' only XP sources are kills and
// consumed items — 3.4's pitfall note applies) and never dirties power
// (bag-only change; 2.5's audited list does not name crafting).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "character/character.hpp" // Character (task 2.0, the doc struct)
#include "core/enums.hpp"          // SkillType (closed list, task 1.1)
#include "core/result.hpp"         // Result<void, E> (task 0.2)
#include "core/types.hpp"          // characterId (task 0.2)
#include "data/json.hpp"           // DataError (the canonical error row type)
#include "items/inventory.hpp"     // Inventory, ItemInstanceRegistry

namespace harrowmere::data
{
class DataRegistry; // registry.hpp (task 1.3)
} // namespace harrowmere::data

namespace harrowmere::player
{
struct PlayerActions; // controller.hpp (task 3.10) — the craft-counter write path
} // namespace harrowmere::player

namespace harrowmere
{

// One recipes.json row (the card's pinned shape).
struct RecipeData
{
    std::string key;                             // "craft_iron_sword"
    std::map<std::string, std::uint32_t> inputs; // item key -> units consumed
    std::string output;                          // item key produced (1 unit per craft)
    std::map<SkillType, std::uint64_t> reqSkill; // skill -> required points (plain thresholds)
    bool hasStation = false;                     // v1: always false (see load errors)
    std::string station;                         // reserved (Appendix E)
};

// The loaded recipe table: rows keyed by recipe key (R7: std::map, key
// ascending), cross-referenced against the registry's items at load.
class RecipeTable
{
public:
    RecipeTable() = default;

    // Load + validate `path` (recipes.json). Validation (loud startup, the
    // 0.4/1.3 pattern):
    //   * every row has a unique non-empty key, at least one input, a
    //     non-empty output;
    //   * every input and the output key resolve to an items.json row in
    //     `data` (dangling references are load errors — B.8's note);
    //   * reqSkill keys are SkillType names from the closed list (1.1);
    //   * `station` is null or absent (Appendix E reserves the field).
    // A failed load leaves the table empty (no partial state).
    bool load(const std::string &path, const data::DataRegistry &data, std::vector<data::DataError> &errors);

    [[nodiscard]] const RecipeData *recipe(const std::string &key) const;

    // Key-ascending view (R7).
    const std::map<std::string, RecipeData> &all() const noexcept
    {
        return recipes_;
    }

    std::size_t size() const noexcept
    {
        return recipes_.size();
    }

private:
    std::map<std::string, RecipeData> recipes_;
};

// ---- the craft action ---------------------------------------------------------

enum class CraftError
{
    UnknownRecipe,        // no row carries the key
    UnknownItem,          // an input/output key misses items.json (unloaded table misuse)
    MissingInputs,        // the bag lacks one or more inputs (nothing consumed)
    SkillRequirementUnmet // one reqSkill threshold not reached (nothing consumed)
};

using CraftResult = Result<void, CraftError>;

// craft — produce `recipeKey`'s output into `bag`. The full gate order:
// recipe lookup -> reqSkill check -> input consumption -> output spawn ->
// craft counter. Every refusal lands BEFORE any side effect: a blocked craft
// never consumes inputs (rule-level failure, §3.4 — the UI reports the
// button press it just refused).
//
// `actions` is the player action log (3.10); present = the craft counter
// increments through PlayerActions::recordCraft (the single write path).
// NPCs craft through their NpcState::actionCounters once 3.4 lands — the
// extension point is this one parameter, never the recipe data.
CraftResult craft(
    Character &c,
    items::Inventory &bag,
    items::ItemInstanceRegistry &instances,
    const data::DataRegistry &data,
    const RecipeTable &recipes,
    const std::string &recipeKey,
    player::PlayerActions *actions = nullptr
);

} // namespace harrowmere

#endif // HARROWMERE_CORE_ITEMS_CRAFTING_HPP
