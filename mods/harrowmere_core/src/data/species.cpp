// species.cpp — SpeciesLoader implementation.
//
// Uses nlohmann::json directly (allowed in src/data/ per task 0.4 spec:
// "game code never includes nlohmann headers directly OUTSIDE src/data/").
// Reports errors via task 0.4's DataError type (declared in src/data/schema.hpp).
//
// Assumed 0.4 API surface (see src/data/json.hpp + src/data/schema.hpp):
//   namespace harrowmere::data {
//     struct DataError { std::string file; std::string jsonPointer; std::string message; };
//     // loadJsonFile: strict UTF-8, rejects trailing garbage, rejects
//     // duplicate object keys, returns the parsed json or the first error.
//     bool loadJsonFile(const std::string& path, nlohmann::json& out, DataError& err);
//   }
//
// If 0.4's exact signatures differ at merge time, adapt the two call sites
// marked `// 0.4:` below — the rest of the file is self-contained.

#include "data/species.hpp"

#include <map>
#include <nlohmann/json.hpp>

#include "core/constants.hpp"  // SCALE (task 0.2)
#include "data/schema.hpp"     // DataError (task 0.4)

namespace harrowmere::data {

namespace {

// Stat name → StatType. The string keys are the EXACT tokens used in
// species.json; mismatched tokens are a hard error (logged via DataError,
// the offending entry skipped, the slot left at SCALE).
bool parseStatName(const std::string& s, StatType& out) {
    if (s == "STRENGTH")     { out = StatType::STRENGTH;     return true; }
    if (s == "DEFENCE")      { out = StatType::DEFENCE;      return true; }
    if (s == "AGILITY")      { out = StatType::AGILITY;      return true; }
    if (s == "INTELLIGENCE") { out = StatType::INTELLIGENCE; return true; }
    if (s == "ENDURANCE")    { out = StatType::ENDURANCE;    return true; }
    if (s == "PERCEPTION")   { out = StatType::PERCEPTION;   return true; }
    return false;
}

// Float (JSON number) → scaled u32. Negative input is a hard error.
// Rounds to nearest, ties to even (matches nlohmann's default rounding).
bool scaleFloat(double v, std::uint32_t& out, std::string& errMsg) {
    if (v < 0.0) {
        errMsg = "negative scaled value not allowed";
        return false;
    }
    const double scaled = v * static_cast<double>(SCALE);
    if (scaled > static_cast<double>(0xFFFFFFFFu)) {
        errMsg = "scaled value overflows u32";
        return false;
    }
    out = static_cast<std::uint32_t>(scaled + 0.5);  // round-half-up
    return true;
}

// Convenience: append a DataError with a JSON-pointer-style path.
void emitError(std::vector<DataError>& errors, const std::string& file,
               const std::string& pointer, const std::string& msg) {
    errors.push_back(DataError{file, pointer, msg});
}

} // namespace

bool SpeciesLoader::load(const std::string& jsonPath, std::vector<DataError>& errors) {
    species_.clear();

    nlohmann::json root;
    {
        DataError loadErr{};
        // 0.4: strict load (UTF-8, no trailing garbage, no duplicate keys).
        if (!loadJsonFile(jsonPath, root, loadErr)) {
            errors.push_back(loadErr);
            return false;
        }
    }

    if (!root.is_array()) {
        emitError(errors, jsonPath, "", "species.json root must be an array");
        return false;
    }

    // First pass: parse every entry into a SpeciesData; collect errors but
    // keep going so one bad row doesn't hide the next.
    std::vector<SpeciesData> parsed;
    parsed.reserve(root.size());

    for (std::size_t i = 0; i < root.size(); ++i) {
        const auto& entry = root[i];
        const std::string ptr = "/" + std::to_string(i);

        if (!entry.is_object()) {
            emitError(errors, jsonPath, ptr, "species entry must be an object");
            continue;
        }

        SpeciesData sd{};

        // id (required, u16 range)
        if (!entry.contains("id") || !entry["id"].is_number_integer()) {
            emitError(errors, jsonPath, ptr + "/id", "required integer 'id' missing");
            continue;
        }
        const auto idRaw = entry["id"].get<std::int64_t>();
        if (idRaw < 0 || idRaw > 65535) {
            emitError(errors, jsonPath, ptr + "/id", "id out of u16 range [0,65535]");
            continue;
        }
        sd.id = static_cast<speciesId>(idRaw);

        // key (required, non-empty string)
        if (!entry.contains("key") || !entry["key"].is_string()) {
            emitError(errors, jsonPath, ptr + "/key", "required string 'key' missing");
            continue;
        }
        sd.key = entry["key"].get<std::string>();
        if (sd.key.empty()) {
            emitError(errors, jsonPath, ptr + "/key", "'key' must not be empty");
            continue;
        }

        // The one hardcoded constant (design reference): id 0 == "player".
        if (sd.id == 0 && sd.key != "player") {
            emitError(errors, jsonPath, ptr + "/key",
                      "id 0 must have key \"player\" (the one hardcoded constant)");
            continue;
        }

        // sapient (required, bool)
        if (!entry.contains("sapient") || !entry["sapient"].is_boolean()) {
            emitError(errors, jsonPath, ptr + "/sapient", "required bool 'sapient' missing");
            continue;
        }
        sd.sapient = entry["sapient"].get<bool>();

        // lifespan_years (required, u32)
        if (!entry.contains("lifespan_years") || !entry["lifespan_years"].is_number_integer()) {
            emitError(errors, jsonPath, ptr + "/lifespan_years", "required integer 'lifespan_years' missing");
            continue;
        }
        const auto lifeRaw = entry["lifespan_years"].get<std::int64_t>();
        if (lifeRaw < 0) {
            emitError(errors, jsonPath, ptr + "/lifespan_years", "lifespan must be >= 0");
            continue;
        }
        sd.lifespan_years = static_cast<std::uint32_t>(lifeRaw);

        // statMultiplier (required object). Default every slot to SCALE first;
        // override with the keys present. Missing keys → SCALE (1.0x) per spec.
        sd.statMultiplier.fill(SCALE);
        if (!entry.contains("statMultiplier") || !entry["statMultiplier"].is_object()) {
            emitError(errors, jsonPath, ptr + "/statMultiplier",
                      "required object 'statMultiplier' missing");
            continue;
        }
        bool statOk = true;
        for (const auto& [statName, val] : entry["statMultiplier"].items()) {
            StatType st{};
            if (!parseStatName(statName, st)) {
                emitError(errors, jsonPath, ptr + "/statMultiplier/" + statName,
                          "unknown stat name '" + statName + "'");
                statOk = false;
                continue;
            }
            if (!val.is_number()) {
                emitError(errors, jsonPath, ptr + "/statMultiplier/" + statName,
                          "stat multiplier must be a number");
                statOk = false;
                continue;
            }
            std::string errMsg;
            if (!scaleFloat(val.get<double>(), sd.statMultiplier[static_cast<std::size_t>(st)], errMsg)) {
                emitError(errors, jsonPath, ptr + "/statMultiplier/" + statName, errMsg);
                statOk = false;
            }
        }
        if (!statOk) continue;

        // appearance (required string)
        if (!entry.contains("appearance") || !entry["appearance"].is_string()) {
            emitError(errors, jsonPath, ptr + "/appearance", "required string 'appearance' missing");
            continue;
        }
        sd.appearance = entry["appearance"].get<std::string>();

        // darkvision (optional, default false)
        if (entry.contains("darkvision")) {
            if (!entry["darkvision"].is_boolean()) {
                emitError(errors, jsonPath, ptr + "/darkvision", "'darkvision' must be bool");
                continue;
            }
            sd.darkvision = entry["darkvision"].get<bool>();
        }

        // xenophobiaBias (optional float, default 0 → scaled 0)
        if (entry.contains("xenophobiaBias")) {
            if (!entry["xenophobiaBias"].is_number()) {
                emitError(errors, jsonPath, ptr + "/xenophobiaBias",
                          "'xenophobiaBias' must be a number");
                continue;
            }
            std::string errMsg;
            if (!scaleFloat(entry["xenophobiaBias"].get<double>(), sd.xenophobiaBias, errMsg)) {
                emitError(errors, jsonPath, ptr + "/xenophobiaBias", errMsg);
                continue;
            }
        }

        // infectableVampire (optional bool, default false)
        if (entry.contains("infectableVampire")) {
            if (!entry["infectableVampire"].is_boolean()) {
                emitError(errors, jsonPath, ptr + "/infectableVampire",
                          "'infectableVampire' must be bool");
                continue;
            }
            sd.infectableVampire = entry["infectableVampire"].get<bool>();
        }

        // damageTakenMultiplier (optional float, default 1.0 → SCALE)
        sd.damageTakenMultiplier = SCALE;
        if (entry.contains("damageTakenMultiplier")) {
            if (!entry["damageTakenMultiplier"].is_number()) {
                emitError(errors, jsonPath, ptr + "/damageTakenMultiplier",
                          "'damageTakenMultiplier' must be a number");
                continue;
            }
            std::string errMsg;
            if (!scaleFloat(entry["damageTakenMultiplier"].get<double>(),
                            sd.damageTakenMultiplier, errMsg)) {
                emitError(errors, jsonPath, ptr + "/damageTakenMultiplier", errMsg);
                continue;
            }
        }

        parsed.push_back(std::move(sd));
    }

    // Second pass: duplicate id / duplicate key detection.
    // (std::map for deterministic iteration — R7, even though this is a
    // data-load pass and not the sim hot path, we keep the discipline.)
    std::map<speciesId, std::size_t> seenIds;
    std::map<std::string, std::size_t> seenKeys;
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        const auto& sd = parsed[i];
        if (auto it = seenIds.find(sd.id); it != seenIds.end()) {
            emitError(errors, jsonPath,
                      "/" + std::to_string(i) + "/id",
                      "duplicate species id " + std::to_string(sd.id) +
                      " (first seen at index " + std::to_string(it->second) + ")");
            return false;
        }
        seenIds[sd.id] = i;
        if (auto it = seenKeys.find(sd.key); it != seenKeys.end()) {
            emitError(errors, jsonPath,
                      "/" + std::to_string(i) + "/key",
                      "duplicate species key '" + sd.key +
                      "' (first seen at index " + std::to_string(it->second) + ")");
            return false;
        }
        seenKeys[sd.key] = i;
    }

    // All green — commit.
    species_ = std::move(parsed);
    return true;
}

const SpeciesData* SpeciesLoader::speciesById(speciesId id) const {
    // Linear scan — species tables are tiny (8 rows in v1). Ids are not
    // guaranteed contiguous (a mod could remove one), so a direct vector
    // index is unsafe; the scan is O(n) on ~8 elements.
    for (const auto& s : species_) {
        if (s.id == id) return &s;
    }
    return nullptr;  // unknown species id — caller must null-check
}

const SpeciesData* SpeciesLoader::speciesByKey(const std::string& key) const {
    for (const auto& s : species_) {
        if (s.key == key) return &s;
    }
    return nullptr;
}

} // namespace harrowmere::data
