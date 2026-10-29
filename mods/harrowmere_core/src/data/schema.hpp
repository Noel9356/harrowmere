#ifndef HARROWMERE_CORE_DATA_SCHEMA_HPP
#define HARROWMERE_CORE_DATA_SCHEMA_HPP

// The schema DSL + validator (task 0.4 spec item 2). Every data/*.json file
// is validated UP FRONT against a Schema before any of it reaches a typed
// struct: a broken row fails startup with file + JSON pointer + what was
// expected - never a mid-sim crash (phase-1 exit checklist).
//
// Forms:
//   Schema{}                     a record: an object with named fields (the
//                                form a fresh Schema takes; unknown fields are
//                                errors - strictness catches typos like
//                                "raritty" instead of losing them silently)
//   Schema::scalar(t[, min, max]) a leaf value of one JsonType (bounds apply
//                                to Integer and Scaled)
//   Schema::enumeration({...})   a leaf string from a closed set
//   Schema::arrayOf(element)     a whole-document array (suspicion.json and
//                                the name tables are arrays at the root)
//
// Record builders (the card's pinned signatures, plus the bounded overload
// FieldSpec's own min/max fields require):
//   .field(name, type[, required])
//   .field(name, type, min, max[, required])
//   .array(name, element[, required])
//   .enumerate(name, values[, required])
//   .map(name, valueType[, keys][, required])  a free-keyed object field
//
// The leaf forms, the bounded overload and map() extend the card's DSL
// skeleton; the Appendix B starter files need them (statMultiplier and
// skillWeights are free-keyed objects; suspicion.json and the name tables are
// root arrays; severity/rarity carry bounds). See DEVIATIONS.md and
// src/data/README.md ("Schema DSL") for the worked examples.

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "data/json.hpp"

namespace harrowmere::data
{

enum class JsonType : std::uint8_t
{
    Null,
    Boolean,
    Integer, // JSON integer; a fractional value is an error
    Scaled,  // SCALE-grid number: integer input = grid units as-is, fractional
             // input = multiplied by SCALE (numeric policy, task 0.4 item 5)
    String,
    Array,
    Object,
};

// FieldSpec's "no bound" sentinels: the full std::int64_t range.
inline constexpr std::int64_t kNoMinBound = std::numeric_limits<std::int64_t>::min();
inline constexpr std::int64_t kNoMaxBound = std::numeric_limits<std::int64_t>::max();

// The pinned row type (task 0.4 spec item 2), kept verbatim.
struct FieldSpec
{
    const char *name;
    JsonType type;
    bool required;
    std::int64_t min;
    std::int64_t max;
};

class Schema
{
public:
    // A fresh Schema is a record schema (see above).
    Schema();

    // ---- leaf forms (array elements, whole-document roots) ------------------
    static Schema scalar(JsonType type, std::int64_t min = kNoMinBound, std::int64_t max = kNoMaxBound);
    static Schema enumeration(std::span<const char *const> values);
    static Schema arrayOf(const Schema &element);

    // ---- record builders -----------------------------------------------------
    Schema &field(const char *name, JsonType type, bool required = true);
    Schema &field(const char *name, JsonType type, std::int64_t min, std::int64_t max, bool required = true);
    Schema &array(const char *name, const Schema &element, bool required = true);
    Schema &enumerate(const char *name, std::span<const char *const> values, bool required = true);
    // A free-keyed object field: every value is `valueType`; when `keys` is
    // non-empty, every key must come from it. statMultiplier's keys are the
    // StatType names, suspicion rows' masks are a closed set - while the
    // *referential* checks (a key naming a real species, a good that exists)
    // are task 1.3's cross-ref pass, closed-set keying is validated here.
    // Map values carry no bounds in v1 (none of Appendix B needs them).
    Schema &map(const char *name, JsonType valueType, std::span<const char *const> keys = {}, bool required = true);

    // Validate `value` against this schema; every violation is one DataError
    // (file + pointer + expected). The report is capped (README,
    // "Engineering constants") so a badly wrong file cannot flood the log.
    std::vector<DataError> validate(const Json &value, const std::string &filePath) const;

    // Introspection (tests, README examples).
    bool isRecord() const
    {
        return kind_ == Kind::Record;
    }

    bool isScalar() const
    {
        return kind_ == Kind::Scalar;
    }

private:
    enum class Kind : std::uint8_t
    {
        Record,
        Scalar,
        Enum,
        ArrayOf,
    };

    struct Entry
    {
        FieldSpec spec{nullptr, JsonType::Null, true, kNoMinBound, kNoMaxBound};
        std::shared_ptr<const Schema> element;  // Array fields: element schema
        std::vector<std::string> enumValues;    // Enum fields: the closed set
        bool isMap            = false;          // Map fields (free-keyed object)
        JsonType mapValueType = JsonType::Null; // Map fields: value type
        std::vector<std::string> mapKeys;       // Map fields: closed key set ("" = free)
    };

    void validateAt(
        const Json &value, const std::string &file, const std::string &pointer, std::vector<DataError> &errors
    ) const;
    void validateRecord(
        const Json &value, const std::string &file, const std::string &pointer, std::vector<DataError> &errors
    ) const;
    void validateEntry(
        const Entry &entry,
        const Json &value,
        const std::string &file,
        const std::string &fieldPointer,
        std::vector<DataError> &errors
    ) const;
    void validateScalar(
        const Json &value,
        const std::string &file,
        const std::string &pointer,
        JsonType type,
        std::int64_t min,
        std::int64_t max,
        std::vector<DataError> &errors
    ) const;
    void validateEnum(
        const std::vector<std::string> &values,
        const Json &value,
        const std::string &file,
        const std::string &pointer,
        std::vector<DataError> &errors
    ) const;

    bool hasEntry(const char *name) const;

    Kind kind_           = Kind::Record;
    JsonType scalarType_ = JsonType::Object; // Scalar leaf payload
    std::int64_t min_    = kNoMinBound;
    std::int64_t max_    = kNoMaxBound;
    std::vector<std::string> enumValues_;   // Enum leaf payload
    std::shared_ptr<const Schema> element_; // ArrayOf leaf payload
    std::vector<Entry> entries_;            // Record payload
};

} // namespace harrowmere::data

#endif // HARROWMERE_CORE_DATA_SCHEMA_HPP
