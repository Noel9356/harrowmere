#ifndef ENGINE_MODULES_ASSET_GLTF_JSON_HPP
#define ENGINE_MODULES_ASSET_GLTF_JSON_HPP

// A minimal JSON reader for glTF 2.0 files.
//
// The engine's dependency policy (cmake/engine_dependencies.cmake) pins
// exactly the dependencies the runtime needs; a JSON library would be one
// more fetched tree for what is, honestly, a narrow read problem: the glTF
// importer only consumes the JSON document's structural parts (nodes,
// meshes, accessors, materials) - geometry stays in the binary buffers,
// which this importer never decodes (bounding boxes come from the
// accessor.min/max fields, which live in the JSON itself). That narrow
// problem is ~200 lines of recursive descent, no exceptions, no locale
// surprises, and one precise error message per malformed document.
//
// The parser is deliberately strict about what it accepts (RFC 8259 subset:
// no comments, no trailing commas, no NaN/Infinity) because glTF files are
// machine written; leniency here would hide exporter bugs.

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine
{

class JsonValue;
using JsonObjectMember = std::pair<std::string, JsonValue>;

// One JSON value. A pure value type: scalars are two moves away from their
// tokens (the parser returns values by value, so a parsed number/bool costs
// zero heap allocations - the tree previously unique_ptr-chained every
// scalar, one malloc/free pair per JSON value, thousands per glTF import).
// glTF documents are parsed once per import, so shared ownership would buy
// nothing.
class JsonValue
{
public:
    enum class Type
    {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object
    };

    Type type     = Type::Null;
    bool boolean  = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    // Object members in document order. Lookup is a linear scan: glTF
    // objects are narrow (nodes, accessors, materials - single-digit to
    // low-teens members with short, SSO-resident keys), so the scan beats a
    // std::map lookup (one allocation and a second key copy per member, then
    // a cache-hostile node walk per find) at exactly the widths the format
    // produces. Duplicate keys are malformed documents; matching the
    // historical map-backed behavior, the LAST occurrence wins.
    std::vector<JsonObjectMember> members;

    bool isNull() const
    {
        return type == Type::Null;
    }
    bool isBoolean() const
    {
        return type == Type::Boolean;
    }
    bool isNumber() const
    {
        return type == Type::Number;
    }
    bool isString() const
    {
        return type == Type::String;
    }
    bool isArray() const
    {
        return type == Type::Array;
    }
    bool isObject() const
    {
        return type == Type::Object;
    }

    // string_view overloads: lookups with literals (the common case) skip
    // building a std::string key, and member.first == string_view compares
    // without measuring C strings. Same linear scan, same last-match-wins.
    const JsonValue *find(
        std::string_view key
    ) const
    {
        if (type != Type::Object)
        {
            return nullptr;
        }
        const JsonValue *lastMatch = nullptr;
        for (const JsonObjectMember &member : members)
        {
            if (member.first == key)
            {
                lastMatch = &member.second;
            }
        }
        return lastMatch;
    }

    // Convenience typed getters with defaults; they never throw.
    double numberOr(
        double fallback
    ) const
    {
        return isNumber() ? number : fallback;
    }
    bool booleanOr(
        bool fallback
    ) const
    {
        return isBoolean() ? boolean : fallback;
    }
    std::string stringOr(
        const std::string &fallback
    ) const
    {
        return isString() ? string : fallback;
    }
    // View into the value's own storage: for large string values (glTF data
    // URIs are megabyte-scale base64) this avoids one full copy per read.
    // The view is valid as long as the JsonValue is.
    std::string_view stringViewOr(
        std::string_view fallback
    ) const
    {
        return isString() ? std::string_view(string) : fallback;
    }
};

// Parses one JSON document. On failure returns nullptr and fills errorMessage
// with the position and the problem - the caller reports the file name around
// it.
std::unique_ptr<JsonValue> jsonParse(const std::string &text, std::string &errorMessage);

} // namespace engine

#endif // ENGINE_MODULES_ASSET_GLTF_JSON_HPP
