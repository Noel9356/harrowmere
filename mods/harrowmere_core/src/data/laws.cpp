// laws.cpp — LawSet loader (Task 1.2).
#include "data/laws.hpp"
#include "data/crime.hpp"  // for crimeTypeFromKey / crimeTypeKey

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>

namespace harrowmere::data {

namespace {

// File read helper — shared shape with crime.cpp; kept local because the
// header is small and we do not want to introduce a util module in 1.2.
std::optional<std::string> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// String-tag tables — kept in lockstep with enums.hpp. R9 closed lists.
struct SocialClassRow { SocialClassId id; const char* tag; };
constexpr SocialClassRow kSocialClassTags[] = {
    { SocialClassId::NONE,     "NONE"     },
    { SocialClassId::COMMONER, "COMMONER" },
    { SocialClassId::NOBLE,    "NOBLE"    },
    { SocialClassId::ROYAL,    "ROYAL"    },
};
constexpr std::size_t kSocialClassCount =
    sizeof(kSocialClassTags) / sizeof(kSocialClassTags[0]);
static_assert(kSocialClassCount ==
              static_cast<std::size_t>(SocialClassId::COUNT),
              "social class tag table must match SocialClassId::COUNT");

struct SuspicionActRow { SuspicionAct act; const char* tag; };
constexpr SuspicionActRow kSuspicionActTags[] = {
    { SuspicionAct::CROUCH,       "CROUCH"       },
    { SuspicionAct::UNLIT,        "UNLIT"        },
    { SuspicionAct::MASKED,       "MASKED"       },
    { SuspicionAct::TRESPASS,     "TRESPASS"     },
    { SuspicionAct::BLOODSTAINED, "BLOODSTAINED" },
    { SuspicionAct::FLEE,         "FLEE"         },
};
constexpr std::size_t kSuspicionActCount =
    sizeof(kSuspicionActTags) / sizeof(kSuspicionActTags[0]);

struct SuspicionMaskRow { SuspicionMask bit; const char* tag; };
constexpr SuspicionMaskRow kSuspicionMaskTags[] = {
    { SuspicionMask::SETTLEMENT,       "SETTLEMENT"       },
    { SuspicionMask::NIGHT,            "NIGHT"            },
    { SuspicionMask::PRIVATE_PROPERTY, "PRIVATE_PROPERTY" },
    { SuspicionMask::MARKET,            "MARKET"            },
};
constexpr std::size_t kSuspicionMaskCount =
    sizeof(kSuspicionMaskTags) / sizeof(kSuspicionMaskTags[0]);

// Reads a CrimeType threshold map out of a JSON object. Every CrimeType
// tag in the enum MUST appear (the docs' data is canonical — missing
// entries are an error). Returns true on success.
bool parseThresholdMap(const nlohmann::json& obj,
                       const std::string& pointerBase,
                       const std::string& file,
                       std::array<std::uint32_t,
                                  static_cast<std::size_t>(CrimeType::COUNT)>& out,
                       std::vector<LawDataError>& errors) {
    if (!obj.is_object()) {
        errors.emplace_back(file, pointerBase, "threshold block must be a JSON object");
        return false;
    }
    bool ok = true;
    std::array<bool, static_cast<std::size_t>(CrimeType::COUNT)> seen{};

    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const std::string& tag = it.key();
        const auto maybe = crimeTypeFromKey(tag); // from crime.hpp
        if (!maybe) {
            errors.emplace_back(file, pointerBase + "/" + tag, "unknown CrimeType tag: " + tag);
            ok = false; continue;
        }
        const auto slot = static_cast<std::size_t>(*maybe);
        if (seen[slot]) {
            errors.emplace_back(file, pointerBase + "/" + tag, "duplicate threshold for CrimeType: " + tag);
            ok = false; continue;
        }
        if (!it->is_number_integer() && !it->is_number_unsigned()) {
            errors.emplace_back(file, pointerBase + "/" + tag, "threshold must be an integer");
            ok = false; continue;
        }
        const auto v = it->get<std::int64_t>();
        if (v < 0 || v > static_cast<std::int64_t>(kScale)) {
            errors.emplace_back(file, pointerBase + "/" + tag, std::to_string(v) + " out of range [0, " + std::to_string(kScale) + "]");
            ok = false; continue;
        }
        out[slot] = static_cast<std::uint32_t>(v);
        seen[slot] = true;
    }
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (!seen[i]) {
            errors.emplace_back(file, pointerBase, "missing threshold for CrimeType: " + std::string(crimeTypeKey(static_cast<CrimeType>(i))));
            ok = false;
        }
    }
    return ok;
}

// Parses `suppression` block: map<string, map<string, int>> where the
// outer key is a SocialClassId tag and the inner key is a CrimeType tag.
bool parseSuppression(const nlohmann::json& obj,
                      const std::string& pointerBase,
                      const std::string& file,
                      LawSet& out,
                      std::vector<LawDataError>& errors) {
    if (!obj.is_object()) {
        errors.emplace_back(file, pointerBase, "suppression block must be a JSON object");
        return false;
    }
    bool ok = true;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const std::string& tag = it.key();
        const auto cls = socialClassFromKey(tag);
        if (!cls) {
            errors.emplace_back(file, pointerBase + "/" + tag, "unknown SocialClassId tag: " + tag);
            ok = false; continue;
        }
        if (!it->is_object()) {
            errors.emplace_back(file, pointerBase + "/" + tag, "suppression entry must be a JSON object");
            ok = false; continue;
        }
        std::array<std::uint32_t,
                   static_cast<std::size_t>(CrimeType::COUNT)> arr{};
        arr.fill(kScale); // missing entries = no damping
        for (auto cit = it->begin(); cit != it->end(); ++cit) {
            const auto cr = crimeTypeFromKey(cit.key());
            if (!cr) {
                errors.emplace_back(file, pointerBase + "/" + tag + "/" + cit.key(), "unknown CrimeType tag: " + cit.key());
                ok = false; continue;
            }
            if (!cit->is_number_integer() && !cit->is_number_unsigned()) {
                errors.emplace_back(file, pointerBase + "/" + tag + "/" + cit.key(), "suppression value must be an integer");
                ok = false; continue;
            }
            const auto v = cit->get<std::int64_t>();
            if (v < 0 || v > static_cast<std::int64_t>(kScale)) {
                errors.emplace_back(file, pointerBase + "/" + tag + "/" + cit.key(), std::to_string(v) + " out of range [0, " + std::to_string(kScale) + "]");
                ok = false; continue;
            }
            arr[static_cast<std::size_t>(*cr)] = static_cast<std::uint32_t>(v);
        }
        out.suppression[*cls] = arr;
    }
    return ok;
}

// Parses `exemptions` block: map<string, map<string, bool>> where the
// outer key is a speciesId (decimal u16 string) and the inner key is a
// CrimeType tag.
bool parseExemptions(const nlohmann::json& obj,
                     const std::string& pointerBase,
                     const std::string& file,
                     LawSet& out,
                     std::vector<LawDataError>& errors) {
    if (!obj.is_object()) {
        errors.emplace_back(file, pointerBase, "exemptions block must be a JSON object");
        return false;
    }
    bool ok = true;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const std::string& key = it.key();
        // speciesId is a u16; parse strictly.
        try {
            const unsigned long v = std::stoul(key);
            if (v > 0xFFFFu) {
                errors.emplace_back(file, pointerBase + "/" + key, "speciesId out of u16 range");
                ok = false; continue;
            }
            const std::uint16_t sid = static_cast<std::uint16_t>(v);
            if (!it->is_object()) {
                errors.emplace_back(file, pointerBase + "/" + key, "exemption entry must be a JSON object");
                ok = false; continue;
            }
            std::array<bool,
                       static_cast<std::size_t>(CrimeType::COUNT)> arr{};
            for (auto cit = it->begin(); cit != it->end(); ++cit) {
                const auto cr = crimeTypeFromKey(cit.key());
                if (!cr) {
                    errors.emplace_back(file, pointerBase + "/" + key + "/" + cit.key(), "unknown CrimeType tag: " + cit.key());
                    ok = false; continue;
                }
                if (!cit->is_boolean()) {
                    errors.emplace_back(file, pointerBase + "/" + key + "/" + cit.key(), "exemption value must be boolean");
                    ok = false; continue;
                }
                arr[static_cast<std::size_t>(*cr)] = cit->get<bool>();
            }
            out.exemptions[sid] = arr;
        } catch (const std::exception&) {
            errors.emplace_back(file, pointerBase + "/" + key, "exemption key must be a decimal speciesId");
            ok = false; continue;
        }
    }
    return ok;
}

// Parses `cultureBias` block: map<string, int>.
bool parseCultureBias(const nlohmann::json& obj,
                      const std::string& pointerBase,
                      const std::string& file,
                      LawSet& out,
                      std::vector<LawDataError>& errors) {
    if (!obj.is_object()) {
        errors.emplace_back(file, pointerBase, "cultureBias block must be a JSON object");
        return false;
    }
    bool ok = true;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const std::string& key = it.key();
        try {
            const unsigned long v = std::stoul(key);
            if (v > 0xFFFFu) {
                errors.emplace_back(file, pointerBase + "/" + key, "speciesId out of u16 range");
                ok = false; continue;
            }
            const std::uint16_t sid = static_cast<std::uint16_t>(v);
            if (!it->is_number_integer()) {
                errors.emplace_back(file, pointerBase + "/" + key, "cultureBias value must be an integer");
                ok = false; continue;
            }
            const auto bias = it->get<std::int64_t>();
            if (bias < -static_cast<std::int64_t>(kScale) ||
                bias >  static_cast<std::int64_t>(kScale)) {
                errors.emplace_back(file, pointerBase + "/" + key, std::to_string(bias) + " out of range [-" + std::to_string(kScale) + ", " + std::to_string(kScale) + "]");
                ok = false; continue;
            }
            out.cultureBias[sid] = static_cast<std::int32_t>(bias);
        } catch (const std::exception&) {
            errors.emplace_back(file, pointerBase + "/" + key, "cultureBias key must be a decimal speciesId");
            ok = false; continue;
        }
    }
    return ok;
}

// Parses one suspicion trigger row: { "act": <tag>, "masks": [<tags>],
// "weight": <int> }.
bool parseSuspicionRow(const nlohmann::json& obj,
                       const std::string& pointer,
                       const std::string& file,
                       SuspicionTrigger& out,
                       std::vector<LawDataError>& errors) {
    if (!obj.is_object()) {
        errors.emplace_back(file, pointer, "suspicion row must be a JSON object");
        return false;
    }
    bool ok = true;
    auto a = obj.find("act");
    if (a == obj.end() || !a->is_string()) {
        errors.emplace_back(file, pointer + "/act", "missing or non-string 'act'");
        ok = false;
    } else {
        const auto act = suspicionActFromKey(a->get<std::string>());
        if (!act) {
            errors.emplace_back(file, pointer + "/act", "unknown SuspicionAct tag: " + a->get<std::string>());
            ok = false;
        } else {
            out.act = *act;
        }
    }

    auto m = obj.find("masks");
    if (m == obj.end() || !m->is_array()) {
        errors.emplace_back(file, pointer + "/masks", "missing or non-array 'masks'");
        ok = false;
    } else {
        std::vector<std::string> tags;
        for (auto mit = m->begin(); mit != m->end(); ++mit) {
            if (!mit->is_string()) {
                errors.emplace_back(file, pointer + "/masks", "mask entries must be strings");
                ok = false; continue;
            }
            tags.push_back(mit->get<std::string>());
        }
        out.mask = parseSuspicionMasks(tags);
        // parseSuspicionMasks silently ignores unknown tags; flag them here.
        for (const auto& t : tags) {
            if (!suspicionMaskFromKey(t)) {
                errors.emplace_back(file, pointer + "/masks", "unknown SuspicionMask tag: " + t);
                ok = false;
            }
        }
    }

    auto w = obj.find("weight");
    if (w == obj.end() || !w->is_number_integer()) {
        errors.emplace_back(file, pointer + "/weight", "missing or non-integer 'weight'");
        ok = false;
    } else {
        const auto v = w->get<std::int64_t>();
        if (v < 0 || v > static_cast<std::int64_t>(kScale)) {
            errors.emplace_back(file, pointer + "/weight", std::to_string(v) + " out of range [0, " + std::to_string(kScale) + "]");
            ok = false;
        } else {
            out.weight = static_cast<std::uint32_t>(v);
        }
    }

    return ok;
}

bool parseSuspicionList(const nlohmann::json& arr,
                        const std::string& pointerBase,
                        const std::string& file,
                        LawSet& out,
                        std::vector<LawDataError>& errors) {
    if (!arr.is_array()) {
        errors.emplace_back(file, pointerBase, "suspicion block must be a JSON array");
        return false;
    }
    bool ok = true;
    std::size_t idx = 0;
    for (auto it = arr.begin(); it != arr.end(); ++it, ++idx) {
        SuspicionTrigger row;
        const std::string pointer = pointerBase + "/" + std::to_string(idx);
        if (parseSuspicionRow(*it, pointer, file, row, errors)) {
            out.suspicion.push_back(std::move(row));
        } else {
            ok = false;
        }
    }
    return ok;
}

// Parses one LawSet entry.
bool parseLawSet(const nlohmann::json& obj,
                 const std::string& pointer,
                 const std::string& file,
                 LawSet& out,
                 std::vector<LawDataError>& errors) {
    if (!obj.is_object()) {
        errors.emplace_back(file, pointer, "law set must be a JSON object");
        return false;
    }
    bool ok = true;

    // `key` (required, string, unique within the file).
    auto k = obj.find("key");
    if (k == obj.end() || !k->is_string()) {
        errors.emplace_back(file, pointer + "/key", "missing or non-string 'key'");
        return false;
    }
    out.key = k->get<std::string>();

    // `convictionThreshold` (required).
    auto ct = obj.find("convictionThreshold");
    if (ct == obj.end()) {
        errors.emplace_back(file, pointer + "/convictionThreshold", "missing 'convictionThreshold' block");
        ok = false;
    } else {
        ok = parseThresholdMap(*ct, pointer + "/convictionThreshold",
                                file, out.convictionThreshold, errors) && ok;
    }

    // `suppression` (optional — empty = no damping).
    if (obj.contains("suppression")) {
        ok = parseSuppression(obj["suppression"], pointer + "/suppression",
                               file, out, errors) && ok;
    }
    // `exemptions` (optional — empty = no exemptions).
    if (obj.contains("exemptions")) {
        ok = parseExemptions(obj["exemptions"], pointer + "/exemptions",
                             file, out, errors) && ok;
    }
    // `cultureBias` (optional — empty = no bias).
    if (obj.contains("cultureBias")) {
        ok = parseCultureBias(obj["cultureBias"], pointer + "/cultureBias",
                              file, out, errors) && ok;
    }
    // `suspicion` (optional — empty = inherit suspicion.json).
    if (obj.contains("suspicion")) {
        ok = parseSuspicionList(obj["suspicion"], pointer + "/suspicion",
                                 file, out, errors) && ok;
    }
    // `safe` (optional, default false — true only for safe_zone_laws).
    if (obj.contains("safe")) {
        if (!obj["safe"].is_boolean()) {
            errors.emplace_back(file, pointer + "/safe", "'safe' must be boolean");
            ok = false;
        } else {
            out.safe = obj["safe"].get<bool>();
        }
    }

    return ok;
}

} // namespace

std::optional<SocialClassId> socialClassFromKey(std::string_view tag) {
    for (const auto& r : kSocialClassTags) {
        if (tag == r.tag) return r.id;
    }
    return std::nullopt;
}

std::optional<SuspicionAct> suspicionActFromKey(std::string_view tag) {
    for (const auto& r : kSuspicionActTags) {
        if (tag == r.tag) return r.act;
    }
    return std::nullopt;
}

std::optional<SuspicionMask> suspicionMaskFromKey(std::string_view tag) {
    for (const auto& r : kSuspicionMaskTags) {
        if (tag == r.tag) return r.bit;
    }
    return std::nullopt;
}

std::uint32_t parseSuspicionMasks(const std::vector<std::string>& tags) {
    std::uint32_t bits = 0;
    for (const auto& t : tags) {
        if (const auto b = suspicionMaskFromKey(t)) {
            bits |= static_cast<std::uint32_t>(*b);
        }
    }
    return bits;
}

bool LawSetRegistry::hasDefaults() const noexcept {
    return sets_.find("default") != sets_.end();
}

const LawSet* LawSetRegistry::byKey(std::string_view key) const noexcept {
    auto it = sets_.find(std::string(key));
    return it == sets_.end() ? nullptr : &it->second;
}
LawSet* LawSetRegistry::byKey(std::string_view key) noexcept {
    auto it = sets_.find(std::string(key));
    return it == sets_.end() ? nullptr : &it->second;
}

bool loadLawSets(const std::string& path,
                 LawSetRegistry& out,
                 std::vector<LawDataError>& errors) {
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
        errors.emplace_back(path, "", "laws.json must be a JSON object");
        return false;
    }
    auto sets = root.find("sets");
    if (sets == root.end() || !sets->is_array()) {
        errors.emplace_back(path, "/sets", "laws.json must contain a 'sets' array");
        return false;
    }

    bool ok = true;
    std::size_t idx = 0;
    for (auto it = sets->begin(); it != sets->end(); ++it, ++idx) {
        const std::string pointer = "/sets/" + std::to_string(idx);
        LawSet ls;
        if (parseLawSet(*it, pointer, path, ls, errors)) {
            if (out.sets_.find(ls.key) != out.sets_.end()) {
                errors.emplace_back(path, pointer, "duplicate LawSet key: " + ls.key);
                ok = false;
            } else {
                out.sets_[ls.key] = std::move(ls);
            }
        } else {
            ok = false;
        }
    }

    if (!out.hasDefaults()) {
        errors.emplace_back(path, "/sets", "laws.json MUST define the 'default' set (DEFAULT_LAWS)");
        ok = false;
    }

    return ok;
}

bool loadSuspicionTriggers(const std::string& path,
                           std::vector<SuspicionTrigger>& out,
                           std::vector<LawDataError>& errors) {
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
    if (!root.is_array()) {
        errors.emplace_back(path, "", "suspicion.json must be a JSON array");
        return false;
    }

    bool ok = true;
    std::size_t idx = 0;
    for (auto it = root.begin(); it != root.end(); ++it, ++idx) {
        SuspicionTrigger row;
        const std::string pointer = "/" + std::to_string(idx);
        if (parseSuspicionRow(*it, pointer, path, row, errors)) {
            out.push_back(std::move(row));
        } else {
            ok = false;
        }
    }
    return ok;
}

} // namespace harrowmere::data
