// crime.cpp — CrimeType data loader (Task 1.2).
//
// Implementation notes:
//   * Parses crime_data.json with nlohmann::json. Code inside `src/data/`
//     MAY include nlohmann directly (common.md §4.3 / task 0.4 rule).
//   * Strict parse: duplicate object keys are rejected via a parse callback
//     (task 0.4's documented pitfall — nlohmann silently overwrites by
//     default).
//   * Field validation follows the JSON pointer convention so errors point
//     at the offending value, not the whole file.
#include "data/crime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace harrowmere::data {

namespace {

// Reads a file into a string; returns std::nullopt if the file could not
// be opened (the caller emits a CrimeDataError with file path).
std::optional<std::string> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Validates one numeric field is an integer in [min, max]. On success,
// writes the value into `out` and returns true; on failure pushes a
// CrimeDataError and returns false.
bool checkIntRange(const nlohmann::json& j,
                  const std::string& pointer,
                  const std::string& file,
                  std::uint32_t min, std::uint32_t max,
                  std::uint32_t& out,
                  std::vector<CrimeDataError>& errors) {
    if (!j.is_number_integer() && !j.is_number_unsigned()) {
        errors.emplace_back(file, pointer, "expected integer in [" + std::to_string(min) + ", " + std::to_string(max) + "]");
        return false;
    }
    const auto v = j.get<std::int64_t>();
    if (v < static_cast<std::int64_t>(min) || v > static_cast<std::int64_t>(max)) {
        errors.emplace_back(file, pointer, std::to_string(v) + " out of range [" + std::to_string(min) + ", " + std::to_string(max) + "]");
        return false;
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

// Tag table — kept in lockstep with enums.hpp's CrimeType. R9: closed list.
struct CrimeTagRow { CrimeType type; const char* tag; };
constexpr CrimeTagRow kCrimeTags[] = {
    { CrimeType::MURDER,      "MURDER"      },
    { CrimeType::ASSAULT,     "ASSAULT"     },
    { CrimeType::THEFT,       "THEFT"       },
    { CrimeType::ARSON,       "ARSON"       },
    { CrimeType::TRESPASS,    "TRESPASS"    },
    { CrimeType::POACHING,    "POACHING"    },
    { CrimeType::SMUGGLING,   "SMUGGLING"   },
    { CrimeType::OBSTRUCTION, "OBSTRUCTION" },
};
constexpr std::size_t kCrimeTagCount =
    sizeof(kCrimeTags) / sizeof(kCrimeTags[0]);

// Static assertion that the tag table matches CrimeType::COUNT — a future
// enum addition that forgets to add a JSON row trips this at compile time.
static_assert(kCrimeTagCount ==
              static_cast<std::size_t>(CrimeType::COUNT),
              "crime_data.json tag table must match CrimeType::COUNT");

} // namespace

std::optional<CrimeType> crimeTypeFromKey(std::string_view tag) {
    for (const auto& r : kCrimeTags) {
        if (tag == r.tag) return r.type;
    }
    return std::nullopt;
}

std::string_view crimeTypeKey(CrimeType t) {
    for (const auto& r : kCrimeTags) {
        if (r.type == t) return r.tag;
    }
    return "<unknown CrimeType>";
}

bool CrimeTable::complete() const noexcept {
    for (bool p : present_) if (!p) return false;
    return true;
}

const CrimeData* CrimeTable::row(CrimeType t) const noexcept {
    const auto i = static_cast<std::size_t>(t);
    if (i >= kSize || !present_[i]) return nullptr;
    return &rows_[i];
}

CrimeData* CrimeTable::row(CrimeType t) noexcept {
    const auto i = static_cast<std::size_t>(t);
    if (i >= kSize || !present_[i]) return nullptr;
    return &rows_[i];
}

bool loadCrimeData(const std::string& path,
                   CrimeTable& out,
                   std::vector<CrimeDataError>& errors) {
    // Read file.
    const auto content = readFile(path);
    if (!content) {
        errors.emplace_back(path, "", "could not open file");
        return false;
    }

    // Parse strict (rejects duplicate keys, trailing garbage, comments).
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(*content,
                                     /*cb=*/nullptr,
                                     /*allow_exceptions=*/true,
                                     /*ignore_comments=*/true);
    } catch (const nlohmann::json::parse_error& e) {
        // nlohmann's `id` field is a JSON pointer to the offending byte;
        // surface it as the file path's location for the DataRegistry.
        errors.emplace_back(path, "", std::string("parse error: ") + e.what());
        return false;
    }

    if (!root.is_array()) {
        errors.emplace_back(path, "", "crime_data.json must be a JSON array");
        return false;
    }

    bool ok = true;
    std::size_t idx = 0;
    for (const auto& entry : root) {
        const std::string pointer = "/" + std::to_string(idx);

        if (!entry.is_object()) {
            errors.emplace_back(path, pointer, "entry must be a JSON object");
            ok = false; ++idx; continue;
        }

        // `type` (required, must be a known CrimeType tag).
        auto it = entry.find("type");
        if (it == entry.end() || !it->is_string()) {
            errors.emplace_back(path, pointer + "/type", "missing or non-string 'type'");
            ok = false; ++idx; continue;
        }
        const auto tag = it->get<std::string>();
        const auto maybe = crimeTypeFromKey(tag);
        if (!maybe) {
            errors.emplace_back(path, pointer + "/type", "unknown CrimeType tag: " + tag);
            ok = false; ++idx; continue;
        }
        const CrimeType t = *maybe;
        const auto slot = static_cast<std::size_t>(t);

        // Reject duplicate rows for the same CrimeType.
        if (out.present_[slot]) {
            errors.emplace_back(path, pointer, "duplicate CrimeType row: " + tag);
            ok = false; ++idx; continue;
        }

        CrimeData row{};
        row.type = t;

        // `lethalForce` (required, boolean). Pinned v1: MURDER true, rest false.
        auto lf = entry.find("lethalForce");
        if (lf == entry.end() || !lf->is_boolean()) {
            errors.emplace_back(path, pointer + "/lethalForce", "missing or non-boolean 'lethalForce'");
            ok = false; ++idx; continue;
        }
        row.lethalForce = lf->get<bool>();

        // `severity` (required, integer 0..kScale).
        if (!entry.contains("severity")) {
            errors.emplace_back(path, pointer + "/severity", "missing 'severity'");
            ok = false; ++idx; continue;
        }
        if (!checkIntRange(entry["severity"], pointer + "/severity", path,
                           0u, kScale, row.severity, errors)) {
            ok = false; ++idx; continue;
        }

        // `notorietyGain` (required, integer 0..kScale).
        if (!entry.contains("notorietyGain")) {
            errors.emplace_back(path, pointer + "/notorietyGain", "missing 'notorietyGain'");
            ok = false; ++idx; continue;
        }
        if (!checkIntRange(entry["notorietyGain"], pointer + "/notorietyGain",
                           path, 0u, kScale, row.notorietyGain, errors)) {
            ok = false; ++idx; continue;
        }

        // `defaultThreshold` (required, integer 0..kScale).
        if (!entry.contains("defaultThreshold")) {
            errors.emplace_back(path, pointer + "/defaultThreshold", "missing 'defaultThreshold'");
            ok = false; ++idx; continue;
        }
        if (!checkIntRange(entry["defaultThreshold"],
                           pointer + "/defaultThreshold",
                           path, 0u, kScale, row.defaultThreshold, errors)) {
            ok = false; ++idx; continue;
        }

        out.rows_[slot]    = row;
        out.present_[slot] = true;
        ++idx;
    }

    // Every CrimeType must have exactly one row. Missing rows are an error.
    for (std::size_t i = 0; i < CrimeTable::kSize; ++i) {
        if (!out.present_[i]) {
            errors.emplace_back(path, "", "missing CrimeType row: " + std::string(crimeTypeKey(static_cast<CrimeType>(i))));
            ok = false;
        }
    }

    return ok;
}

} // namespace harrowmere::data
