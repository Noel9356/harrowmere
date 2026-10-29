#include "data/schema.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace harrowmere::data
{
namespace
{

// Report cap: a wrong file lists every violation, but a startup log drowning
// in ten thousand lines helps nobody. Engineering constant of the data layer,
// not a game rule (README, "Engineering constants").
constexpr std::size_t kMaxValidationErrors = 100;

// Human name of a JSON value's actual type (message text only).
std::string jsonTypeName(const Json &value)
{
    switch (value.type())
    {
    case Json::value_t::null:
        return "null";
    case Json::value_t::object:
        return "object";
    case Json::value_t::array:
        return "array";
    case Json::value_t::string:
        return "string";
    case Json::value_t::boolean:
        return "boolean";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
        return "integer";
    case Json::value_t::number_float:
        return "fractional number";
    case Json::value_t::binary:
        return "binary";
    case Json::value_t::discarded:
        return "discarded";
    }
    return "unknown";
}

// Human name of a schema-side type (message text only).
const char *schemaTypeName(JsonType type)
{
    switch (type)
    {
    case JsonType::Null:
        return "null";
    case JsonType::Boolean:
        return "boolean";
    case JsonType::Integer:
        return "integer";
    case JsonType::Scaled:
        return "scaled number";
    case JsonType::String:
        return "string";
    case JsonType::Array:
        return "array";
    case JsonType::Object:
        return "object";
    }
    return "unknown";
}

// "A, B, C" - the closed set as it appears in error messages.
std::string joinNames(const std::vector<std::string> &names)
{
    std::string joined;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        if (i != 0)
        {
            joined += ", ";
        }
        joined += names[i];
    }
    return joined;
}

// The grid value of an INTEGER json number, clamped at the std::int64_t range
// (a JSON unsigned above INT64_MAX is reported against the field's bounds).
std::int64_t integerValueOrClamp(const Json &value)
{
    if (value.is_number_unsigned())
    {
        const auto raw = value.get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            return std::numeric_limits<std::int64_t>::max();
        }
        return static_cast<std::int64_t>(raw);
    }
    return value.get<std::int64_t>();
}

} // namespace

Schema::Schema() = default;

Schema Schema::scalar(JsonType type, std::int64_t min, std::int64_t max)
{
    Schema schema;
    schema.kind_ = Kind::Scalar;
    schema.scalarType_ = type;
    schema.min_ = min;
    schema.max_ = max;
    return schema;
}

Schema Schema::enumeration(std::span<const char *const> values)
{
    Schema schema;
    schema.kind_ = Kind::Enum;
    schema.enumValues_.assign(values.begin(), values.end());
    return schema;
}

Schema Schema::arrayOf(const Schema &element)
{
    Schema schema;
    schema.kind_ = Kind::ArrayOf;
    schema.element_ = std::make_shared<Schema>(element);
    return schema;
}

Schema &Schema::field(const char *name, JsonType type, bool required)
{
    return field(name, type, kNoMinBound, kNoMaxBound, required);
}

Schema &Schema::field(const char *name, JsonType type,
                      std::int64_t min, std::int64_t max, bool required)
{
    Entry entry;
    entry.spec = FieldSpec{name, type, required, min, max};
    entries_.push_back(std::move(entry));
    return *this;
}

Schema &Schema::array(const char *name, const Schema &element, bool required)
{
    Entry entry;
    entry.spec = FieldSpec{name, JsonType::Array, required, kNoMinBound, kNoMaxBound};
    entry.element = std::make_shared<Schema>(element);
    entries_.push_back(std::move(entry));
    return *this;
}

Schema &Schema::enumerate(const char *name, std::span<const char *const> values, bool required)
{
    Entry entry;
    entry.spec = FieldSpec{name, JsonType::String, required, kNoMinBound, kNoMaxBound};
    entry.enumValues.assign(values.begin(), values.end());
    entries_.push_back(std::move(entry));
    return *this;
}

Schema &Schema::map(const char *name, JsonType valueType,
                    std::span<const char *const> keys, bool required)
{
    Entry entry;
    entry.spec = FieldSpec{name, JsonType::Object, required, kNoMinBound, kNoMaxBound};
    entry.isMap = true;
    entry.mapValueType = valueType;
    entry.mapKeys.assign(keys.begin(), keys.end());
    entries_.push_back(std::move(entry));
    return *this;
}

std::vector<DataError> Schema::validate(const Json &value, const std::string &filePath) const
{
    std::vector<DataError> errors;
    validateAt(value, filePath, std::string{}, errors);
    if (errors.size() >= kMaxValidationErrors)
    {
        errors.push_back(DataError{
            filePath, std::string{},
            "error report capped at " + std::to_string(kMaxValidationErrors) +
                " entries - fix the reported errors and re-validate"});
    }
    return errors;
}

bool Schema::hasEntry(const char *name) const
{
    for (const Entry &entry : entries_)
    {
        if (std::string(entry.spec.name) == name)
        {
            return true;
        }
    }
    return false;
}

void Schema::validateAt(const Json &value, const std::string &file,
                        const std::string &pointer, std::vector<DataError> &errors) const
{
    switch (kind_)
    {
    case Kind::Scalar:
        validateScalar(value, file, pointer, scalarType_, min_, max_, errors);
        return;
    case Kind::Enum:
        validateEnum(enumValues_, value, file, pointer, errors);
        return;
    case Kind::ArrayOf:
    {
        if (!value.is_array())
        {
            errors.push_back(DataError{file, pointer,
                                       "expected array, got " + jsonTypeName(value)});
            return;
        }
        std::size_t index = 0;
        for (const Json &element : value)
        {
            if (errors.size() >= kMaxValidationErrors)
            {
                return;
            }
            element_->validateAt(element, file, pointer + "/" + std::to_string(index), errors);
            ++index;
        }
        return;
    }
    case Kind::Record:
        validateRecord(value, file, pointer, errors);
        return;
    }
}

void Schema::validateRecord(const Json &value, const std::string &file,
                            const std::string &pointer, std::vector<DataError> &errors) const
{
    if (!value.is_object())
    {
        errors.push_back(DataError{file, pointer,
                                   "expected object (record), got " + jsonTypeName(value)});
        return;
    }

    // Unknown fields are errors (strict): a typo'd field name must surface as
    // "unexpected", not vanish as "silently ignored". nlohmann objects
    // iterate in sorted key order, so the report itself is deterministic.
    for (auto member = value.begin(); member != value.end(); ++member)
    {
        if (errors.size() >= kMaxValidationErrors)
        {
            return;
        }
        if (!hasEntry(member.key().c_str()))
        {
            errors.push_back(DataError{
                file, pointer + "/" + pointerEscape(member.key()),
                "unexpected field '" + member.key() + "' (not in the record's schema - strict loader)"});
        }
    }

    for (const Entry &entry : entries_)
    {
        if (errors.size() >= kMaxValidationErrors)
        {
            return;
        }
        const std::string fieldPointer = pointer + "/" + pointerEscape(entry.spec.name);
        const auto found = value.find(entry.spec.name);
        if (found == value.end())
        {
            if (entry.spec.required)
            {
                errors.push_back(DataError{
                    file, fieldPointer,
                    "required field '" + std::string(entry.spec.name) + "' (" +
                        schemaTypeName(entry.spec.type) + ") is missing"});
            }
            continue;
        }
        validateEntry(entry, *found, file, fieldPointer, errors);
    }
}

void Schema::validateEntry(const Entry &entry, const Json &value, const std::string &file,
                           const std::string &fieldPointer, std::vector<DataError> &errors) const
{
    if (entry.isMap)
    {
        if (!value.is_object())
        {
            errors.push_back(DataError{file, fieldPointer,
                                       "expected object (map), got " + jsonTypeName(value)});
            return;
        }
        for (auto member = value.begin(); member != value.end(); ++member)
        {
            if (errors.size() >= kMaxValidationErrors)
            {
                return;
            }
            const std::string valuePointer = fieldPointer + "/" + pointerEscape(member.key());
            bool keyKnown = true;
            if (!entry.mapKeys.empty())
            {
                keyKnown = false;
                for (const std::string &allowed : entry.mapKeys)
                {
                    if (member.key() == allowed)
                    {
                        keyKnown = true;
                        break;
                    }
                }
                if (!keyKnown)
                {
                    errors.push_back(DataError{
                        file, valuePointer,
                        "unknown map key '" + member.key() + "' (expected one of [" +
                            joinNames(entry.mapKeys) + "])"});
                }
            }
            if (keyKnown)
            {
                validateScalar(member.value(), file, valuePointer, entry.mapValueType,
                               kNoMinBound, kNoMaxBound, errors);
            }
        }
        return;
    }

    if (entry.element)
    {
        if (!value.is_array())
        {
            errors.push_back(DataError{file, fieldPointer,
                                       "expected array, got " + jsonTypeName(value)});
            return;
        }
        std::size_t index = 0;
        for (const Json &element : value)
        {
            if (errors.size() >= kMaxValidationErrors)
            {
                return;
            }
            entry.element->validateAt(element, file,
                                      fieldPointer + "/" + std::to_string(index), errors);
            ++index;
        }
        return;
    }

    if (!entry.enumValues.empty())
    {
        validateEnum(entry.enumValues, value, file, fieldPointer, errors);
        return;
    }

    validateScalar(value, file, fieldPointer, entry.spec.type,
                   entry.spec.min, entry.spec.max, errors);
}

void Schema::validateScalar(const Json &value, const std::string &file,
                            const std::string &pointer, JsonType type,
                            std::int64_t min, std::int64_t max,
                            std::vector<DataError> &errors) const
{
    switch (type)
    {
    case JsonType::Null:
        if (!value.is_null())
        {
            errors.push_back(DataError{file, pointer, "expected null, got " + jsonTypeName(value)});
        }
        return;
    case JsonType::Boolean:
        if (!value.is_boolean())
        {
            errors.push_back(DataError{file, pointer, "expected boolean, got " + jsonTypeName(value)});
        }
        return;
    case JsonType::String:
        if (!value.is_string())
        {
            errors.push_back(DataError{file, pointer, "expected string, got " + jsonTypeName(value)});
        }
        return;
    case JsonType::Array:
        if (!value.is_array())
        {
            errors.push_back(DataError{file, pointer, "expected array, got " + jsonTypeName(value)});
        }
        return;
    case JsonType::Object:
        if (!value.is_object())
        {
            errors.push_back(DataError{file, pointer, "expected object, got " + jsonTypeName(value)});
        }
        return;
    case JsonType::Integer:
    {
        if (value.is_number_float())
        {
            errors.push_back(DataError{file, pointer,
                                       "expected integer, got fractional number " + value.dump()});
            return;
        }
        if (!value.is_number_integer())
        {
            errors.push_back(DataError{file, pointer,
                                       "expected integer, got " + jsonTypeName(value)});
            return;
        }
        const std::int64_t number = integerValueOrClamp(value);
        if (number < min || number > max)
        {
            errors.push_back(DataError{
                file, pointer,
                "expected integer in [" + std::to_string(min) + ", " + std::to_string(max) +
                    "], got " + std::to_string(number)});
        }
        return;
    }
    case JsonType::Scaled:
    {
        std::int64_t grid = 0;
        bool fractional = false;
        if (value.is_number_float())
        {
            grid = scaleToGrid(value.get<double>());
            fractional = true;
        }
        else if (value.is_number_integer())
        {
            grid = integerValueOrClamp(value);
        }
        else
        {
            errors.push_back(DataError{file, pointer,
                                       "expected scaled number, got " + jsonTypeName(value)});
            return;
        }
        if (grid < min || grid > max)
        {
            errors.push_back(DataError{
                file, pointer,
                "expected scaled value in [" + std::to_string(min) + ", " + std::to_string(max) +
                    "] on the SCALE grid (fractional input is multiplied by SCALE), got " +
                    std::to_string(grid) + (fractional ? " (from raw " + value.dump() + ")" : "")});
        }
        return;
    }
    }
}

void Schema::validateEnum(const std::vector<std::string> &values, const Json &value,
                          const std::string &file, const std::string &pointer,
                          std::vector<DataError> &errors) const
{
    if (!value.is_string())
    {
        errors.push_back(DataError{
            file, pointer,
            "expected string (one of [" + joinNames(values) + "]), got " + jsonTypeName(value)});
        return;
    }
    const std::string text = value.get<std::string>();
    for (const std::string &allowed : values)
    {
        if (text == allowed)
        {
            return;
        }
    }
    errors.push_back(DataError{
        file, pointer,
        "expected one of [" + joinNames(values) + "], got '" + text + "'"});
}

} // namespace harrowmere::data
