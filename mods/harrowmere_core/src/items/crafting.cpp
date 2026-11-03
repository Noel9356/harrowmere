// crafting.cpp — the recipe table and the craft action (task 2.4).
//
// The loader walks the 0.4 JSON layer's one door (data/json.hpp — game code
// outside src/data/ never includes nlohmann directly, D-DATA-1) and reports
// every failure as a DataError row with file + RFC-6901 pointer + reason, so
// a broken recipes.json fails startup loudly instead of mid-sim.

#include "items/crafting.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "character/character.hpp"
#include "data/items.hpp"
#include "data/json.hpp"
#include "data/registry.hpp"
#include "items/inventory.hpp"
#include "player/controller.hpp" // PlayerActions::recordCraft (2.3's reqActions write path)

namespace harrowmere
{

namespace
{

// SkillType name -> enum value over the CLOSED list (task 1.1). An unknown
// name is a data error, never an invented entry.
bool parseSkillType(
    const std::string &name, SkillType &out
)
{
    const std::pair<const char *, SkillType> table[] = {
        {"MELEE", SkillType::MELEE},
        {"ARCHERY", SkillType::ARCHERY},
        {"STEALTH", SkillType::STEALTH},
        {"ARCANA", SkillType::ARCANA},
        {"SURVIVAL", SkillType::SURVIVAL},
        {"PERSUASION", SkillType::PERSUASION},
        {"CRAFTING", SkillType::CRAFTING},
        {"ESCAPE", SkillType::ESCAPE},
    };
    for (const auto &[key, value] : table)
    {
        if (name == key)
        {
            out = value;
            return true;
        }
    }
    return false;
}

} // namespace

bool RecipeTable::load(
    const std::string &path, const data::DataRegistry &data, std::vector<data::DataError> &errors
)
{
    std::optional<data::Json> loaded;
    {
        auto parsed = data::loadJsonFile(path);
        if (parsed.isErr())
        {
            errors.push_back(parsed.error());
            return false; // no partial state: a failed load leaves the table as it was
        }
        loaded = std::move(parsed.value());
    }
    if (!loaded->is_array())
    {
        errors.push_back({path, "", "recipes.json must be a top-level array of recipe rows"});
        return false;
    }

    RecipeTable parsed;
    std::size_t index = 0;
    for (const auto &row : *loaded)
    {
        const std::string pointer = "/" + std::to_string(index);
        if (!row.is_object())
        {
            errors.push_back({path, pointer, "recipe row must be an object"});
            ++index;
            continue;
        }

        RecipeData recipe;

        // key — unique, non-empty.
        if (!row.contains("key") || !row["key"].is_string() || row["key"].get<std::string>().empty())
        {
            errors.push_back({path, pointer + "/key", "recipe key must be a non-empty string"});
            ++index;
            continue;
        }
        recipe.key = row["key"].get<std::string>();
        if (parsed.recipes_.find(recipe.key) != parsed.recipes_.end())
        {
            errors.push_back({path, pointer + "/key", "duplicate recipe key '" + recipe.key + "'"});
            ++index;
            continue;
        }

        // inputs — non-empty map of item key -> positive count; every key
        // must resolve to an items.json row (the B.8 cross-ref note).
        if (!row.contains("inputs") || !row["inputs"].is_object() || row["inputs"].empty())
        {
            errors.push_back({path, pointer + "/inputs", "inputs must be a non-empty object of itemKey: count"});
            ++index;
            continue;
        }
        bool inputsClean = true;
        for (auto const &[itemKey, countNode] : row["inputs"].items())
        {
            if (!countNode.is_number_unsigned() || countNode.get<std::uint64_t>() == 0
                || countNode.get<std::uint64_t>() > 0xFFFFu)
            {
                errors.push_back(
                    {path,
                        pointer + "/inputs/" + data::pointerEscape(itemKey),
                        "input count must be a positive integer (<= 65535)"}
                );
                inputsClean = false;
                continue;
            }
            if (data.itemByKey(itemKey) == nullptr)
            {
                errors.push_back(
                    {path,
                        pointer + "/inputs/" + data::pointerEscape(itemKey),
                        "input key '" + itemKey + "' does not resolve to an items.json row"}
                );
                inputsClean = false;
                continue;
            }
            recipe.inputs[itemKey] = static_cast<std::uint32_t>(countNode.get<std::uint64_t>());
        }

        // output — one item key, resolved like the inputs.
        if (!row.contains("output") || !row["output"].is_string())
        {
            errors.push_back({path, pointer + "/output", "output must be an item key string"});
            ++index;
            continue;
        }
        recipe.output = row["output"].get<std::string>();
        if (data.itemByKey(recipe.output) == nullptr)
        {
            errors.push_back(
                {path, pointer + "/output", "output key '" + recipe.output + "' does not resolve to an items.json row"}
            );
            inputsClean = false;
        }

        // reqSkill — map of closed-list skill name -> required points.
        if (row.contains("reqSkill"))
        {
            if (!row["reqSkill"].is_object())
            {
                errors.push_back({path, pointer + "/reqSkill", "reqSkill must be an object of skillName: points"});
                inputsClean = false;
            } else
            {
                for (auto const &[skillName, reqNode] : row["reqSkill"].items())
                {
                    SkillType skill{};
                    if (!parseSkillType(skillName, skill))
                    {
                        errors.push_back(
                            {path,
                                pointer + "/reqSkill/" + data::pointerEscape(skillName),
                                "'" + skillName + "' is not a SkillType (closed list, task 1.1)"}
                        );
                        inputsClean = false;
                        continue;
                    }
                    if (!reqNode.is_number_unsigned())
                    {
                        errors.push_back(
                            {path,
                                pointer + "/reqSkill/" + data::pointerEscape(skillName),
                                "required points must be a non-negative integer"}
                        );
                        inputsClean = false;
                        continue;
                    }
                    recipe.reqSkill[skill] = reqNode.get<std::uint64_t>();
                }
            }
        }

        // station — reserved (Appendix E); v1 accepts only null/absent.
        if (row.contains("station") && !row["station"].is_null())
        {
            recipe.hasStation = true;
            recipe.station    = row["station"].is_string() ? row["station"].get<std::string>() : "";
            errors.push_back(
                {path,
                    pointer + "/station",
                    "craft stations are reserved (Appendix E); v1 recipes carry \"station\": null"}
            );
            inputsClean = false;
        }

        if (inputsClean)
            parsed.recipes_.emplace(recipe.key, std::move(recipe));
        ++index;
    }

    if (!errors.empty())
        return false; // no partial state
    recipes_ = std::move(parsed.recipes_);
    return true;
}

const RecipeData *RecipeTable::recipe(
    const std::string &key
) const
{
    const auto it = recipes_.find(key);
    return it != recipes_.end() ? &it->second : nullptr;
}

CraftResult craft(
    Character &c,
    items::Inventory &bag,
    items::ItemInstanceRegistry &instances,
    const data::DataRegistry &data,
    const RecipeTable &recipes,
    const std::string &recipeKey,
    player::PlayerActions *actions
)
{
    const RecipeData *recipe = recipes.recipe(recipeKey);
    if (recipe == nullptr)
        return CraftResult::err(CraftError::UnknownRecipe);

    // Gate 1 — reqSkill: plain point thresholds (the same semantics
    // progression's SkillRequirement uses: raw progression points, never
    // effectiveness). Every requirement checked before ANY side effect.
    for (const auto &[skill, required] : recipe->reqSkill)
    {
        if (c.progression.skills[static_cast<std::size_t>(skill)] < required)
            return CraftResult::err(CraftError::SkillRequirementUnmet);
    }

    // Gate 2 — inputs: resolve keys to definitions and check the bag holds
    // every count BEFORE consuming anything.
    const data::ItemData *outputItem = data.itemByKey(recipe->output);
    if (outputItem == nullptr)
        return CraftResult::err(CraftError::UnknownItem); // an unloaded table was hand-fed rows

    struct ResolvedInput
    {
        std::uint64_t defId;
        std::uint32_t count;
    };
    std::vector<ResolvedInput> resolved;
    resolved.reserve(recipe->inputs.size());
    for (const auto &[itemKey, count] : recipe->inputs)
    {
        const data::ItemData *item = data.itemByKey(itemKey);
        if (item == nullptr)
            return CraftResult::err(CraftError::UnknownItem);
        resolved.push_back({item->id, count});
    }
    for (const ResolvedInput &input : resolved)
    {
        if (items::countStacked(bag, input.defId) < input.count)
            return CraftResult::err(CraftError::MissingInputs);
    }

    // Consume (takeStacked erases zeroed stacks — the stack map stores no 0).
    for (const ResolvedInput &input : resolved)
    {
        const bool taken = items::takeStacked(bag, input.defId, input.count);
        assert(taken && "craft: input vanished between the check and the take (programmer error)");
        (void)taken;
    }

    // Produce the output: stackables join the stack map; instanced items
    // draw a fresh instance id into the bag's loose list (the card's
    // pitfall: equippables are per-instance, two swords score twice).
    if (outputItem->stackable)
        items::addStacked(bag, outputItem->id, 1);
    else
        items::addLooseInstance(bag, instances.spawn(outputItem->id));

    // The craft counter (2.3's reqActions input) through the single write
    // path; NPCs carry their own map in NpcState (3.4).
    if (actions != nullptr)
        actions->recordCraft();

    return CraftResult::ok();
}

} // namespace harrowmere
