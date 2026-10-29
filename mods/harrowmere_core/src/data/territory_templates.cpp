// territory_templates.cpp — Territory template loader (Task 1.2).
#include "data/territory_templates.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace harrowmere::data {

namespace {

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

struct TerritoryTypeRow { TerritoryType type; const char* tag; };
constexpr TerritoryTypeRow kTerritoryTypeTags[] = {
    { TerritoryType::VILLAGE, "VILLAGE" },
    { TerritoryType::CITY,    "CITY"    },
    { TerritoryType::KINGDOM, "KINGDOM" },
    { TerritoryType::EMPIRE,  "EMPIRE"  },
};
constexpr std::size_t kTerritoryTypeCount =
    sizeof(kTerritoryTypeTags) / sizeof(kTerritoryTypeTags[0]);
static_assert(kTerritoryTypeCount ==
              static_cast<std::size_t>(TerritoryType::COUNT),
              "territory type tag table must match TerritoryType::COUNT");

// True iff v is a positive power of two (1, 2, 4, 8, ...).
bool isPowerOfTwo(std::uint32_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

bool parseTypeEntry(const nlohmann::json& obj,
                    const std::string& pointer,
                    const std::string& file,
                    TerritoryTypeTemplate& out,
                    std::vector<TerritoryDataError>& errors) {
    if (!obj.is_object()) {
        errors.emplace_back(file, pointer, "type entry must be a JSON object");
        return false;
    }
    bool ok = true;

    // `population` (required, [min, max]).
    auto pop = obj.find("population");
    if (pop == obj.end() || !pop->is_array() || pop->size() != 2) {
        errors.emplace_back(file, pointer + "/population", "missing 'population' or not a 2-element array");
        ok = false;
    } else {
        const auto& lo = (*pop)[0];
        const auto& hi = (*pop)[1];
        if (!lo.is_number_integer() && !lo.is_number_unsigned()) {
            errors.emplace_back(file, pointer + "/population/0", "population[0] must be an integer");
            ok = false;
        } else if (lo.get<std::int64_t>() < 0) {
            errors.emplace_back(file, pointer + "/population/0", "population[0] must be >= 0");
            ok = false;
        } else {
            out.populationMin = static_cast<std::uint32_t>(lo.get<std::int64_t>());
        }
        if (!hi.is_number_integer() && !hi.is_number_unsigned()) {
            errors.emplace_back(file, pointer + "/population/1", "population[1] must be an integer");
            ok = false;
        } else if (hi.get<std::int64_t>() < 0) {
            errors.emplace_back(file, pointer + "/population/1", "population[1] must be >= 0");
            ok = false;
        } else {
            out.populationMax = static_cast<std::uint32_t>(hi.get<std::int64_t>());
        }
        if (ok && out.populationMin > out.populationMax) {
            errors.emplace_back(file, pointer + "/population", "population[0] > population[1]");
            ok = false;
        }
    }

    // `lawSet` (required, string, non-empty).
    auto ls = obj.find("lawSet");
    if (ls == obj.end() || !ls->is_string()) {
        errors.emplace_back(file, pointer + "/lawSet", "missing or non-string 'lawSet'");
        ok = false;
    } else {
        out.lawSet = ls->get<std::string>();
        if (out.lawSet.empty()) {
            errors.emplace_back(file, pointer + "/lawSet", "'lawSet' must not be empty");
            ok = false;
        }
    }

    // `safe` (optional, boolean).
    if (obj.contains("safe")) {
        if (!obj["safe"].is_boolean()) {
            errors.emplace_back(file, pointer + "/safe", "'safe' must be boolean");
            ok = false;
        } else {
            out.safe = obj["safe"].get<bool>();
        }
    }

    // `arena` (optional, boolean).
    if (obj.contains("arena")) {
        if (!obj["arena"].is_boolean()) {
            errors.emplace_back(file, pointer + "/arena", "'arena' must be boolean");
            ok = false;
        } else {
            out.arena = obj["arena"].get<bool>();
        }
    }

    return ok;
}

} // namespace

std::optional<TerritoryType> territoryTypeFromKey(std::string_view tag) {
    for (const auto& r : kTerritoryTypeTags) {
        if (tag == r.tag) return r.type;
    }
    return std::nullopt;
}
std::string_view territoryTypeKey(TerritoryType t) {
    for (const auto& r : kTerritoryTypeTags) {
        if (r.type == t) return r.tag;
    }
    return "<unknown TerritoryType>";
}

bool TerritoryTemplateSet::complete() const noexcept {
    for (bool p : present_) if (!p) return false;
    return true;
}

const TerritoryTypeTemplate*
TerritoryTemplateSet::byType(TerritoryType t) const noexcept {
    const auto i = static_cast<std::size_t>(t);
    if (i >= present_.size() || !present_[i]) return nullptr;
    return &types_[i];
}
TerritoryTypeTemplate*
TerritoryTemplateSet::byType(TerritoryType t) noexcept {
    const auto i = static_cast<std::size_t>(t);
    if (i >= present_.size() || !present_[i]) return nullptr;
    return &types_[i];
}

std::optional<std::uint32_t>
TerritoryTemplateSet::flagBit(std::string_view name) const noexcept {
    auto it = region_flags_.find(std::string(name));
    if (it == region_flags_.end()) return std::nullopt;
    return it->second;
}

bool loadTerritoryTemplates(const std::string& path,
                            TerritoryTemplateSet& out,
                            std::vector<TerritoryDataError>& errors) {
    const auto content = readFile(path);
    if (!content) {
        errors.emplace_back(path, "", "could not open file");
        return false;
    }
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(*content, nullptr, true, true);
    } catch (const nlohmann::json::parse_error& e) {
        errors.emplace_back(path, "", std::string("parse error: ") + e.what());
        return false;
    }
    if (!root.is_object()) {
        errors.emplace_back(path, "", "territory_templates.json must be a JSON object");
        return false;
    }

    bool ok = true;

    // `types` (required, must cover every TerritoryType).
    auto types = root.find("types");
    if (types == root.end() || !types->is_object()) {
        errors.emplace_back(path, "/types", "missing 'types' or not a JSON object");
        ok = false;
    } else {
        for (auto it = types->begin(); it != types->end(); ++it) {
            const std::string& tag = it.key();
            const auto t = territoryTypeFromKey(tag);
            if (!t) {
                errors.emplace_back(path, "/types/" + tag, "unknown TerritoryType tag: " + tag);
                ok = false; continue;
            }
            const auto slot = static_cast<std::size_t>(*t);
            if (out.present_[slot]) {
                errors.emplace_back(path, "/types/" + tag, "duplicate TerritoryType row: " + tag);
                ok = false; continue;
            }
            TerritoryTypeTemplate tt{};
            tt.type = *t;
            if (parseTypeEntry(*it, "/types/" + tag, path, tt, errors)) {
                out.types_[slot]    = tt;
                out.present_[slot] = true;
            } else {
                ok = false;
            }
        }
        // Every TerritoryType must be present.
        for (std::size_t i = 0; i < out.present_.size(); ++i) {
            if (!out.present_[i]) {
                errors.emplace_back(path, "/types", std::string("missing TerritoryType: ") + std::string(territoryTypeKey(static_cast<TerritoryType>(i))));
                ok = false;
            }
        }
    }

    // `villages_per_city` (required, positive integer; worldgen divides by it).
    if (root.contains("villages_per_city")) {
        const auto& v = root["villages_per_city"];
        if (!v.is_number_integer() && !v.is_number_unsigned()) {
            errors.emplace_back(path, "/villages_per_city", "must be an integer");
            ok = false;
        } else if (v.get<std::int64_t>() <= 0) {
            errors.emplace_back(path, "/villages_per_city", "must be > 0 (worldgen divides by it)");
            ok = false;
        } else {
            out.villages_per_city_ = static_cast<std::uint32_t>(v.get<std::int64_t>());
        }
    } else {
        errors.emplace_back(path, "/villages_per_city", "missing key");
        ok = false;
    }

    // `cities_per_kingdom` (required, positive integer).
    if (root.contains("cities_per_kingdom")) {
        const auto& v = root["cities_per_kingdom"];
        if (!v.is_number_integer() && !v.is_number_unsigned()) {
            errors.emplace_back(path, "/cities_per_kingdom", "must be an integer");
            ok = false;
        } else if (v.get<std::int64_t>() <= 0) {
            errors.emplace_back(path, "/cities_per_kingdom", "must be > 0");
            ok = false;
        } else {
            out.cities_per_kingdom_ = static_cast<std::uint32_t>(v.get<std::int64_t>());
        }
    } else {
        errors.emplace_back(path, "/cities_per_kingdom", "missing key");
        ok = false;
    }

    // `kingdoms_per_empire` (required, positive integer).
    if (root.contains("kingdoms_per_empire")) {
        const auto& v = root["kingdoms_per_empire"];
        if (!v.is_number_integer() && !v.is_number_unsigned()) {
            errors.emplace_back(path, "/kingdoms_per_empire", "must be an integer");
            ok = false;
        } else if (v.get<std::int64_t>() <= 0) {
            errors.emplace_back(path, "/kingdoms_per_empire", "must be > 0");
            ok = false;
        } else {
            out.kingdoms_per_empire_ = static_cast<std::uint32_t>(v.get<std::int64_t>());
        }
    } else {
        errors.emplace_back(path, "/kingdoms_per_empire", "missing key");
        ok = false;
    }

    // `regionFlags` (optional, map<string, positive-power-of-two-int>).
    if (root.contains("regionFlags")) {
        const auto& rf = root["regionFlags"];
        if (!rf.is_object()) {
            errors.emplace_back(path, "/regionFlags", "'regionFlags' must be a JSON object");
            ok = false;
        } else {
            for (auto it = rf.begin(); it != rf.end(); ++it) {
                const std::string& name = it.key();
                if (!it->is_number_integer() && !it->is_number_unsigned()) {
                    errors.emplace_back(path, "/regionFlags/" + name, "flag bit must be an integer");
                    ok = false; continue;
                }
                const auto v = it->get<std::int64_t>();
                if (v <= 0 || v > 0xFFFFFFFFLL) {
                    errors.emplace_back(path, "/regionFlags/" + name, std::to_string(v) + " out of u32 range");
                    ok = false; continue;
                }
                if (!isPowerOfTwo(static_cast<std::uint32_t>(v))) {
                    errors.emplace_back(path, "/regionFlags/" + name, std::to_string(v) + " is not a power of two (1, 2, 4, 8, ...)");
                    ok = false; continue;
                }
                out.region_flags_[name] = static_cast<std::uint32_t>(v);
            }
        }
    }

    return ok;
}

} // namespace harrowmere::data
