// classes.cpp — ClassesLoader implementation.
// See species.cpp for the assumed 0.4 API + merge-time notes.

#include "data/classes.hpp"

#include <map>
#include <nlohmann/json.hpp>

#include "core/constants.hpp" // SCALE (task 0.2)
#include "data/schema.hpp"    // DataError (task 0.4)

namespace harrowmere::data
{

namespace
{

bool parseStatName(
    const std::string &s, StatType &out
)
{
    if (s == "STRENGTH")
    {
        out = StatType::STRENGTH;
        return true;
    }
    if (s == "DEFENCE")
    {
        out = StatType::DEFENCE;
        return true;
    }
    if (s == "AGILITY")
    {
        out = StatType::AGILITY;
        return true;
    }
    if (s == "INTELLIGENCE")
    {
        out = StatType::INTELLIGENCE;
        return true;
    }
    if (s == "ENDURANCE")
    {
        out = StatType::ENDURANCE;
        return true;
    }
    if (s == "PERCEPTION")
    {
        out = StatType::PERCEPTION;
        return true;
    }
    return false;
}

bool parseSkillName(
    const std::string &s, SkillType &out
)
{
    if (s == "MELEE")
    {
        out = SkillType::MELEE;
        return true;
    }
    if (s == "ARCHERY")
    {
        out = SkillType::ARCHERY;
        return true;
    }
    if (s == "STEALTH")
    {
        out = SkillType::STEALTH;
        return true;
    }
    if (s == "ARCANA")
    {
        out = SkillType::ARCANA;
        return true;
    }
    if (s == "SURVIVAL")
    {
        out = SkillType::SURVIVAL;
        return true;
    }
    if (s == "PERSUASION")
    {
        out = SkillType::PERSUASION;
        return true;
    }
    if (s == "CRAFTING")
    {
        out = SkillType::CRAFTING;
        return true;
    }
    if (s == "ESCAPE")
    {
        out = SkillType::ESCAPE;
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
        errMsg = "negative weight not allowed";
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

bool ClassesLoader::load(
    const std::string &jsonPath, std::vector<DataError> &errors
)
{
    classes_.clear();

    nlohmann::json root;
    {
        // 0.4: strict load (UTF-8, no trailing garbage, no duplicate keys);
        // the Result carries the DataError on failure.
        const auto loaded = loadJsonFile(jsonPath);
        if (!loaded.isOk())
        {
            errors.push_back(loaded.error());
            return false;
        }
        root = loaded.value();
    }

    if (!root.is_array())
    {
        emitError(errors, jsonPath, "", "classes.json root must be an array");
        return false;
    }

    std::vector<ClassData> parsed;
    parsed.reserve(root.size());

    for (std::size_t i = 0; i < root.size(); ++i)
    {
        const auto &entry     = root[i];
        const std::string ptr = "/" + std::to_string(i);

        if (!entry.is_object())
        {
            emitError(errors, jsonPath, ptr, "class entry must be an object");
            continue;
        }

        ClassData cd{};

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
        cd.id = static_cast<std::uint32_t>(idRaw);

        // key (required, non-empty string)
        if (!entry.contains("key") || !entry["key"].is_string())
        {
            emitError(errors, jsonPath, ptr + "/key", "required string 'key' missing");
            continue;
        }
        cd.key = entry["key"].get<std::string>();
        if (cd.key.empty())
        {
            emitError(errors, jsonPath, ptr + "/key", "'key' must not be empty");
            continue;
        }

        // Default weights to SCALE (1.0x) before overriding.
        cd.statWeights.fill(SCALE);
        cd.skillWeights.fill(SCALE);
        cd.startingSkills.fill(0);

        // statWeights (optional object — defaults to all 1.0x)
        if (entry.contains("statWeights"))
        {
            if (!entry["statWeights"].is_object())
            {
                emitError(errors, jsonPath, ptr + "/statWeights", "'statWeights' must be an object");
                continue;
            }
            for (const auto &[k, v] : entry["statWeights"].items())
            {
                StatType st{};
                if (!parseStatName(k, st))
                {
                    emitError(errors, jsonPath, ptr + "/statWeights/" + k, "unknown stat '" + k + "'");
                    continue;
                }
                if (!v.is_number())
                {
                    emitError(errors, jsonPath, ptr + "/statWeights/" + k, "weight must be a number");
                    continue;
                }
                std::string errMsg;
                if (!scaleFloat(v.get<double>(), cd.statWeights[static_cast<std::size_t>(st)], errMsg))
                {
                    emitError(errors, jsonPath, ptr + "/statWeights/" + k, errMsg);
                    continue;
                }
            }
        }

        // skillWeights (optional object)
        if (entry.contains("skillWeights"))
        {
            if (!entry["skillWeights"].is_object())
            {
                emitError(errors, jsonPath, ptr + "/skillWeights", "'skillWeights' must be an object");
                continue;
            }
            for (const auto &[k, v] : entry["skillWeights"].items())
            {
                SkillType sk{};
                if (!parseSkillName(k, sk))
                {
                    emitError(errors, jsonPath, ptr + "/skillWeights/" + k, "unknown skill '" + k + "'");
                    continue;
                }
                if (!v.is_number())
                {
                    emitError(errors, jsonPath, ptr + "/skillWeights/" + k, "weight must be a number");
                    continue;
                }
                std::string errMsg;
                if (!scaleFloat(v.get<double>(), cd.skillWeights[static_cast<std::size_t>(sk)], errMsg))
                {
                    emitError(errors, jsonPath, ptr + "/skillWeights/" + k, errMsg);
                    continue;
                }
            }
        }

        // startingSkills (optional object — integer skill levels)
        if (entry.contains("startingSkills"))
        {
            if (!entry["startingSkills"].is_object())
            {
                emitError(errors, jsonPath, ptr + "/startingSkills", "'startingSkills' must be an object");
                continue;
            }
            for (const auto &[k, v] : entry["startingSkills"].items())
            {
                SkillType sk{};
                if (!parseSkillName(k, sk))
                {
                    emitError(errors, jsonPath, ptr + "/startingSkills/" + k, "unknown skill '" + k + "'");
                    continue;
                }
                if (!v.is_number_integer())
                {
                    emitError(errors, jsonPath, ptr + "/startingSkills/" + k, "starting skill must be an integer");
                    continue;
                }
                const auto n = v.get<std::int64_t>();
                if (n < 0)
                {
                    emitError(errors, jsonPath, ptr + "/startingSkills/" + k, "starting skill must be >= 0");
                    continue;
                }
                cd.startingSkills[static_cast<std::size_t>(sk)] = static_cast<std::uint32_t>(n);
            }
        }

        parsed.push_back(std::move(cd));
    }

    // Duplicate id / key check.
    std::map<std::uint32_t, std::size_t> seenIds;
    std::map<std::string, std::size_t> seenKeys;
    for (std::size_t i = 0; i < parsed.size(); ++i)
    {
        const auto &cd = parsed[i];
        if (auto f = seenIds.find(cd.id); f != seenIds.end())
        {
            emitError(
                errors,
                jsonPath,
                "/" + std::to_string(i) + "/id",
                "duplicate class id " + std::to_string(cd.id) + " (first seen at index " + std::to_string(f->second)
                    + ")"
            );
            return false;
        }
        seenIds[cd.id] = i;
        if (auto f = seenKeys.find(cd.key); f != seenKeys.end())
        {
            emitError(
                errors,
                jsonPath,
                "/" + std::to_string(i) + "/key",
                "duplicate class key '" + cd.key + "' (first seen at index " + std::to_string(f->second) + ")"
            );
            return false;
        }
        seenKeys[cd.key] = i;
    }

    classes_ = std::move(parsed);
    return true;
}

const ClassData *ClassesLoader::classById(
    std::uint32_t id
) const
{
    for (const auto &c : classes_)
        if (c.id == id)
            return &c;
    return nullptr;
}

const ClassData *ClassesLoader::classByKey(
    const std::string &key
) const
{
    for (const auto &c : classes_)
        if (c.key == key)
            return &c;
    return nullptr;
}

} // namespace harrowmere::data
