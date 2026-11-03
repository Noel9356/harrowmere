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

// skillRequirements' reqActions entries are "<counter>_<count>" strings
// (task 2.3: `["kill_non_sapient_10"]`-style): the counter key is everything
// before the LAST underscore, the count a positive integer. Splitting at the
// last underscore keeps underscored counter names intact
// ("kill_non_sapient_10" -> "kill_non_sapient", 10).
bool parseActionRequirement(
    const std::string &s, std::pair<std::string, std::uint64_t> &out, std::string &errMsg
)
{
    const std::size_t split = s.rfind('_');
    if (split == std::string::npos || split == 0 || split + 1 == s.size())
    {
        errMsg = "reqActions entry must be '<counter>_<count>' (e.g. 'kill_non_sapient_10')";
        return false;
    }
    const std::string countText = s.substr(split + 1);
    std::uint64_t count         = 0;
    for (const char c : countText)
    {
        if (c < '0' || c > '9')
        {
            errMsg = "reqActions count must be digits ('" + s + "')";
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (count > (0xFFFF'FFFF'FFFF'FFFFull - digit) / 10ull)
        {
            errMsg = "reqActions count overflows u64 ('" + s + "')";
            return false;
        }
        count = count * 10ull + digit;
    }
    if (count == 0)
    {
        errMsg = "reqActions count must be >= 1 ('" + s + "')";
        return false;
    }
    out.first  = s.substr(0, split);
    out.second = count;
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

        // skillRequirements (optional object — task 2.3's per-skill purchase
        // gates): { "MELEE": { "reqStats": {"STRENGTH": 5}, "reqSkills": {},
        // "reqActions": ["kill_non_sapient_10"] } }. Requirement values are
        // plain point thresholds (NOT SCALE-grid); an undeclared or empty
        // requirement gates nothing (all-zero default).
        if (entry.contains("skillRequirements"))
        {
            if (!entry["skillRequirements"].is_object())
            {
                emitError(errors, jsonPath, ptr + "/skillRequirements", "'skillRequirements' must be an object");
                continue;
            }
            bool reqOk = true;
            for (const auto &[skillKey, reqEntry] : entry["skillRequirements"].items())
            {
                SkillType sk{};
                if (!parseSkillName(skillKey, sk))
                {
                    emitError(
                        errors, jsonPath, ptr + "/skillRequirements/" + skillKey, "unknown skill '" + skillKey + "'"
                    );
                    reqOk = false;
                    continue;
                }
                if (!reqEntry.is_object())
                {
                    emitError(
                        errors, jsonPath, ptr + "/skillRequirements/" + skillKey, "requirement must be an object"
                    );
                    reqOk = false;
                    continue;
                }
                const std::string reqPtr = ptr + "/skillRequirements/" + skillKey;
                SkillRequirement &req    = cd.skillRequirements[static_cast<std::size_t>(sk)];

                // reqStats (optional object of stat-name -> minimum points)
                if (reqEntry.contains("reqStats"))
                {
                    if (!reqEntry["reqStats"].is_object())
                    {
                        emitError(errors, jsonPath, reqPtr + "/reqStats", "'reqStats' must be an object");
                        reqOk = false;
                        continue;
                    }
                    for (const auto &[statKey, v] : reqEntry["reqStats"].items())
                    {
                        StatType st{};
                        if (!parseStatName(statKey, st))
                        {
                            emitError(
                                errors, jsonPath, reqPtr + "/reqStats/" + statKey, "unknown stat '" + statKey + "'"
                            );
                            reqOk = false;
                            continue;
                        }
                        if (!v.is_number_integer() || v.get<std::int64_t>() < 0)
                        {
                            emitError(
                                errors,
                                jsonPath,
                                reqPtr + "/reqStats/" + statKey,
                                "requirement must be a non-negative integer"
                            );
                            reqOk = false;
                            continue;
                        }
                        req.reqStats[static_cast<std::size_t>(st)] = static_cast<std::uint32_t>(v.get<std::int64_t>());
                    }
                    if (!reqOk)
                        continue;
                }

                // reqSkills (optional object of skill-name -> minimum points)
                if (reqEntry.contains("reqSkills"))
                {
                    if (!reqEntry["reqSkills"].is_object())
                    {
                        emitError(errors, jsonPath, reqPtr + "/reqSkills", "'reqSkills' must be an object");
                        reqOk = false;
                        continue;
                    }
                    for (const auto &[needKey, v] : reqEntry["reqSkills"].items())
                    {
                        SkillType need{};
                        if (!parseSkillName(needKey, need))
                        {
                            emitError(
                                errors, jsonPath, reqPtr + "/reqSkills/" + needKey, "unknown skill '" + needKey + "'"
                            );
                            reqOk = false;
                            continue;
                        }
                        if (!v.is_number_integer() || v.get<std::int64_t>() < 0)
                        {
                            emitError(
                                errors,
                                jsonPath,
                                reqPtr + "/reqSkills/" + needKey,
                                "requirement must be a non-negative integer"
                            );
                            reqOk = false;
                            continue;
                        }
                        req.reqSkills[static_cast<std::size_t>(need)]
                            = static_cast<std::uint32_t>(v.get<std::int64_t>());
                    }
                    if (!reqOk)
                        continue;
                }

                // reqActions (optional array of "<counter>_<count>" strings)
                if (reqEntry.contains("reqActions"))
                {
                    if (!reqEntry["reqActions"].is_array())
                    {
                        emitError(errors, jsonPath, reqPtr + "/reqActions", "'reqActions' must be an array");
                        reqOk = false;
                        continue;
                    }
                    for (std::size_t a = 0; a < reqEntry["reqActions"].size(); ++a)
                    {
                        const auto &v = reqEntry["reqActions"][a];
                        if (!v.is_string())
                        {
                            emitError(
                                errors,
                                jsonPath,
                                reqPtr + "/reqActions/" + std::to_string(a),
                                "reqActions entry must be a string"
                            );
                            reqOk = false;
                            continue;
                        }
                        std::pair<std::string, std::uint64_t> parsed{};
                        std::string errMsg;
                        if (!parseActionRequirement(v.get<std::string>(), parsed, errMsg))
                        {
                            emitError(errors, jsonPath, reqPtr + "/reqActions/" + std::to_string(a), errMsg);
                            reqOk = false;
                            continue;
                        }
                        req.reqActions.push_back(std::move(parsed));
                    }
                    if (!reqOk)
                        continue;
                }
            }
            if (!reqOk)
                continue;
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
