// items.cpp — ItemsLoader implementation.
//
// Same nlohmann::json + DataError pattern as species.cpp. See the header
// comment in species.cpp for the assumed 0.4 API surface and the merge-time
// adaptation points.

#include "data/items.hpp"

#include <map>
#include <nlohmann/json.hpp>

#include "core/constants.hpp" // SCALE (task 0.2)
#include "data/schema.hpp"    // DataError (task 0.4)

namespace harrowmere::data
{

namespace
{

bool parseSlotName(
    const std::string &s, ItemSlot &out
)
{
    if (s == "main_hand")
    {
        out = ItemSlot::MAIN_HAND;
        return true;
    }
    if (s == "off_hand")
    {
        out = ItemSlot::OFF_HAND;
        return true;
    }
    if (s == "head")
    {
        out = ItemSlot::HEAD;
        return true;
    }
    if (s == "face")
    {
        out = ItemSlot::FACE;
        return true;
    }
    if (s == "body")
    {
        out = ItemSlot::BODY;
        return true;
    }
    if (s == "hands")
    {
        out = ItemSlot::HANDS;
        return true;
    }
    if (s == "legs")
    {
        out = ItemSlot::LEGS;
        return true;
    }
    if (s == "feet")
    {
        out = ItemSlot::FEET;
        return true;
    }
    if (s == "back")
    {
        out = ItemSlot::BACK;
        return true;
    }
    return false;
}

bool parseSuspicionActName(
    const std::string &s, SuspicionAct &out
)
{
    if (s == "CROUCH")
    {
        out = SuspicionAct::CROUCH;
        return true;
    }
    if (s == "UNLIT")
    {
        out = SuspicionAct::UNLIT;
        return true;
    }
    if (s == "MASKED")
    {
        out = SuspicionAct::MASKED;
        return true;
    }
    if (s == "TRESPASS")
    {
        out = SuspicionAct::TRESPASS;
        return true;
    }
    if (s == "BLOODSTAINED")
    {
        out = SuspicionAct::BLOODSTAINED;
        return true;
    }
    if (s == "FLEE")
    {
        out = SuspicionAct::FLEE;
        return true;
    }
    return false;
}

bool scaleFloat(
    double v, std::uint32_t &out, std::string &errMsg
)
{
    if (v < 0.0)
    {
        errMsg = "negative scaled value not allowed";
        return false;
    }
    const double scaled = v * static_cast<double>(SCALE);
    if (scaled > static_cast<double>(0xFFFFFFFFu))
    {
        errMsg = "scaled value overflows u32";
        return false;
    }
    out = static_cast<std::uint32_t>(scaled + 0.5);
    return true;
}

void emitError(
    std::vector<DataError> &errors, const std::string &file, const std::string &pointer, const std::string &msg
)
{
    errors.push_back(DataError{file, pointer, msg});
}

} // namespace

bool ItemsLoader::load(
    const std::string &jsonPath, std::vector<DataError> &errors
)
{
    items_.clear();

    nlohmann::json root;
    {
        DataError loadErr{};
        if (!loadJsonFile(jsonPath, root, loadErr))
        {
            errors.push_back(loadErr);
            return false;
        }
    }

    if (!root.is_array())
    {
        emitError(errors, jsonPath, "", "items.json root must be an array");
        return false;
    }

    std::vector<ItemData> parsed;
    parsed.reserve(root.size());

    for (std::size_t i = 0; i < root.size(); ++i)
    {
        const auto &entry     = root[i];
        const std::string ptr = "/" + std::to_string(i);

        if (!entry.is_object())
        {
            emitError(errors, jsonPath, ptr, "item entry must be an object");
            continue;
        }

        ItemData it{};

        // id (required, u32)
        if (!entry.contains("id") || !entry["id"].is_number_integer())
        {
            emitError(errors, jsonPath, ptr + "/id", "required integer 'id' missing");
            continue;
        }
        const auto idRaw = entry["id"].get<std::int64_t>();
        if (idRaw < 0 || idRaw > 0xFFFFFFFFLL)
        {
            emitError(errors, jsonPath, ptr + "/id", "id out of u32 range");
            continue;
        }
        it.id = static_cast<std::uint32_t>(idRaw);

        // key (required, non-empty string)
        if (!entry.contains("key") || !entry["key"].is_string())
        {
            emitError(errors, jsonPath, ptr + "/key", "required string 'key' missing");
            continue;
        }
        it.key = entry["key"].get<std::string>();
        if (it.key.empty())
        {
            emitError(errors, jsonPath, ptr + "/key", "'key' must not be empty");
            continue;
        }

        // name (required, string)
        if (!entry.contains("name") || !entry["name"].is_string())
        {
            emitError(errors, jsonPath, ptr + "/name", "required string 'name' missing");
            continue;
        }
        it.name = entry["name"].get<std::string>();

        // rarity (required, 0..SCALE)
        if (!entry.contains("rarity") || !entry["rarity"].is_number_integer())
        {
            emitError(errors, jsonPath, ptr + "/rarity", "required integer 'rarity' missing");
            continue;
        }
        const auto rarRaw = entry["rarity"].get<std::int64_t>();
        if (rarRaw < 0 || rarRaw > static_cast<std::int64_t>(SCALE))
        {
            emitError(errors, jsonPath, ptr + "/rarity", "rarity must be in [0, SCALE]");
            continue;
        }
        it.rarity = static_cast<std::uint32_t>(rarRaw);

        // level (required, u32)
        if (!entry.contains("level") || !entry["level"].is_number_integer())
        {
            emitError(errors, jsonPath, ptr + "/level", "required integer 'level' missing");
            continue;
        }
        const auto lvlRaw = entry["level"].get<std::int64_t>();
        if (lvlRaw < 0)
        {
            emitError(errors, jsonPath, ptr + "/level", "level must be >= 0");
            continue;
        }
        it.level = static_cast<std::uint32_t>(lvlRaw);

        // baseValue (required, currency — i64 to allow debt scenarios)
        if (!entry.contains("baseValue") || !entry["baseValue"].is_number_integer())
        {
            emitError(errors, jsonPath, ptr + "/baseValue", "required integer 'baseValue' missing");
            continue;
        }
        it.baseValue = entry["baseValue"].get<std::int64_t>();

        // equippable (required, bool)
        if (!entry.contains("equippable") || !entry["equippable"].is_boolean())
        {
            emitError(errors, jsonPath, ptr + "/equippable", "required bool 'equippable' missing");
            continue;
        }
        it.equippable = entry["equippable"].get<bool>();

        // slot (optional, only meaningful if equippable)
        if (entry.contains("slot"))
        {
            if (!entry["slot"].is_string())
            {
                emitError(errors, jsonPath, ptr + "/slot", "'slot' must be a string");
                continue;
            }
            const auto slotStr = entry["slot"].get<std::string>();
            if (!parseSlotName(slotStr, it.slot))
            {
                emitError(errors, jsonPath, ptr + "/slot", "unknown slot '" + slotStr + "'");
                continue;
            }
        }

        // guardMarked (required per task 1.1 spec, but allow default-false if absent
        // for forward-compat with mods that omit it).
        if (entry.contains("guardMarked"))
        {
            if (!entry["guardMarked"].is_boolean())
            {
                emitError(errors, jsonPath, ptr + "/guardMarked", "'guardMarked' must be bool");
                continue;
            }
            it.guardMarked = entry["guardMarked"].get<bool>();
        }

        // stackable (required per task 1.1 spec; allow default-false if absent).
        if (entry.contains("stackable"))
        {
            if (!entry["stackable"].is_boolean())
            {
                emitError(errors, jsonPath, ptr + "/stackable", "'stackable' must be bool");
                continue;
            }
            it.stackable = entry["stackable"].get<bool>();
        }

        // consumable (optional sub-object)
        if (entry.contains("consumable"))
        {
            if (!entry["consumable"].is_object())
            {
                emitError(errors, jsonPath, ptr + "/consumable", "'consumable' must be an object");
                continue;
            }
            const auto &c    = entry["consumable"];
            it.hasConsumable = true;
            if (c.contains("xpGain"))
            {
                if (!c["xpGain"].is_number_integer())
                {
                    emitError(errors, jsonPath, ptr + "/consumable/xpGain", "'xpGain' must be integer");
                    continue;
                }
                const auto x = c["xpGain"].get<std::int64_t>();
                if (x < 0)
                {
                    emitError(errors, jsonPath, ptr + "/consumable/xpGain", "'xpGain' must be >= 0");
                    continue;
                }
                it.consumable.xpGain = static_cast<std::uint32_t>(x);
            }
            if (c.contains("healHp"))
            {
                if (!c["healHp"].is_number_integer())
                {
                    emitError(errors, jsonPath, ptr + "/consumable/healHp", "'healHp' must be integer");
                    continue;
                }
                const auto h = c["healHp"].get<std::int64_t>();
                if (h < 0)
                {
                    emitError(errors, jsonPath, ptr + "/consumable/healHp", "'healHp' must be >= 0");
                    continue;
                }
                it.consumable.healHp = static_cast<std::uint32_t>(h);
            }
            if (c.contains("manaRich"))
            {
                if (!c["manaRich"].is_boolean())
                {
                    emitError(errors, jsonPath, ptr + "/consumable/manaRich", "'manaRich' must be bool");
                    continue;
                }
                it.consumable.manaRich = c["manaRich"].get<bool>();
            }
        }

        // weaponFactor (optional, float → scaled u32). 0 = not a weapon.
        if (entry.contains("weaponFactor"))
        {
            if (!entry["weaponFactor"].is_number())
            {
                emitError(errors, jsonPath, ptr + "/weaponFactor", "'weaponFactor' must be a number");
                continue;
            }
            std::string errMsg;
            if (!scaleFloat(entry["weaponFactor"].get<double>(), it.weaponFactor, errMsg))
            {
                emitError(errors, jsonPath, ptr + "/weaponFactor", errMsg);
                continue;
            }
        }

        // enablesAct (optional, string → SuspicionAct)
        if (entry.contains("enablesAct"))
        {
            if (!entry["enablesAct"].is_string())
            {
                emitError(errors, jsonPath, ptr + "/enablesAct", "'enablesAct' must be a string");
                continue;
            }
            const auto actStr = entry["enablesAct"].get<std::string>();
            if (!parseSuspicionActName(actStr, it.enablesAct))
            {
                emitError(errors, jsonPath, ptr + "/enablesAct", "unknown suspicion act '" + actStr + "'");
                continue;
            }
        }

        parsed.push_back(std::move(it));
    }

    // Duplicate id / key check.
    std::map<std::uint32_t, std::size_t> seenIds;
    std::map<std::string, std::size_t> seenKeys;
    for (std::size_t i = 0; i < parsed.size(); ++i)
    {
        const auto &it = parsed[i];
        if (auto f = seenIds.find(it.id); f != seenIds.end())
        {
            emitError(
                errors,
                jsonPath,
                "/" + std::to_string(i) + "/id",
                "duplicate item id " + std::to_string(it.id) + " (first seen at index " + std::to_string(f->second)
                    + ")"
            );
            return false;
        }
        seenIds[it.id] = i;
        if (auto f = seenKeys.find(it.key); f != seenKeys.end())
        {
            emitError(
                errors,
                jsonPath,
                "/" + std::to_string(i) + "/key",
                "duplicate item key '" + it.key + "' (first seen at index " + std::to_string(f->second) + ")"
            );
            return false;
        }
        seenKeys[it.key] = i;
    }

    items_ = std::move(parsed);
    return true;
}

const ItemData *ItemsLoader::itemById(
    std::uint32_t id
) const
{
    for (const auto &it : items_)
        if (it.id == id)
            return &it;
    return nullptr;
}

const ItemData *ItemsLoader::itemByKey(
    const std::string &key
) const
{
    for (const auto &it : items_)
        if (it.key == key)
            return &it;
    return nullptr;
}

} // namespace harrowmere::data
