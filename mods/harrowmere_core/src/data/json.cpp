#include "data/json.hpp"

#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <utility>

namespace harrowmere::data
{
namespace
{

// Parser safety bound - an engineering constant of the data layer, not a game
// rule: it keeps hostile or accidental input from exhausting memory in error
// bookkeeping and gives the builder a loud failure instead of unbounded
// growth (see src/data/README.md, "Engineering constants"; flagged for the
// lead in DEVIATIONS.md).
constexpr std::size_t kMaxNestingDepth = 200;

// 2^63 as an IEEE-754 double (exactly representable): values at or beyond it
// cannot round-trip through std::int64_t. scaleToGrid clamps such authoring
// absurdities; the schema layer's bounds checks then reject them loudly.
constexpr double kInt64MagnitudeLimit = 9.223372036854775808e18;

// Strict SAX DOM builder.
//
// nlohmann's default parse keeps the LAST of duplicate object keys silently,
// and the parser-callback form cannot see sibling keys reliably; strictness
// is therefore implemented as a SAX consumer that builds the DOM itself and
// rejects a duplicate key the moment it is read, carrying the JSON pointer of
// the offender (task 0.4 spec item 3 + the card's pitfall: "choose strict").
// Building iteratively (no recursion) is what makes the depth cap honest.
class StrictDomBuilder
{
public:
    using number_integer_t  = Json::number_integer_t;
    using number_unsigned_t = Json::number_unsigned_t;
    using number_float_t    = Json::number_float_t;
    using string_t          = Json::string_t;
    using binary_t          = Json::binary_t;

    // ---- value events -------------------------------------------------------
    bool null()
    {
        return attach(Json(nullptr));
    }

    bool boolean(
        bool value
    )
    {
        return attach(Json(value));
    }

    bool number_integer(
        number_integer_t value
    )
    {
        return attach(Json(value));
    }

    bool number_unsigned(
        number_unsigned_t value
    )
    {
        return attach(Json(value));
    }

    bool number_float(
        number_float_t value, const string_t & /*raw*/
    )
    {
        return attach(Json(value));
    }

    bool string(
        string_t &value
    )
    {
        return attach(Json(value));
    }

    bool binary(
        binary_t & /*value*/
    )
    {
        // JSON text never produces binary values (only the binary input
        // formats do); arriving here means the bytes were not JSON content.
        return fail("", "binary values are not JSON content");
    }

    // ---- container events ---------------------------------------------------
    bool start_object(
        std::size_t /*elementCount*/
    )
    {
        if (stack_.size() >= kMaxNestingDepth)
        {
            return fail(currentPointer(), "nesting deeper than the parser depth cap (200)");
        }
        stack_.push_back(Frame{Json::object(), std::set<std::string>{}, childPointer(), std::string{}});
        return true;
    }

    bool key(
        string_t &key
    )
    {
        // A key event only fires inside an open object frame.
        Frame &frame = stack_.back();
        if (frame.seenKeys.count(key) != 0)
        {
            return fail(
                frame.pointer + "/" + pointerEscape(key),
                "duplicate object key '" + key + "' (strict loader: last-wins overwriting is not allowed)"
            );
        }
        frame.seenKeys.insert(key);
        frame.currentKey = std::move(key);
        return true;
    }

    bool end_object()
    {
        Json done = std::move(stack_.back().value);
        stack_.pop_back();
        return attach(std::move(done));
    }

    bool start_array(
        std::size_t /*elementCount*/
    )
    {
        if (stack_.size() >= kMaxNestingDepth)
        {
            return fail(currentPointer(), "nesting deeper than the parser depth cap (200)");
        }
        stack_.push_back(Frame{Json::array(), std::set<std::string>{}, childPointer(), std::string{}});
        return true;
    }

    bool end_array()
    {
        Json done = std::move(stack_.back().value);
        stack_.pop_back();
        return attach(std::move(done));
    }

    // ---- failure plumbing ---------------------------------------------------
    bool parse_error(
        std::size_t position, const std::string & /*lastToken*/, const Json::exception &ex
    )
    {
        return fail(currentPointer(), "parse error at byte " + std::to_string(position) + ": " + ex.what());
    }

    // ---- results ------------------------------------------------------------
    bool failed() const
    {
        return failed_;
    }

    const DataError &error() const
    {
        return error_;
    }

    Json takeDom()
    {
        return std::move(root_);
    }

private:
    struct Frame
    {
        Json value;                     // the object or array being built
        std::set<std::string> seenKeys; // objects: keys read so far
        std::string pointer;            // pointer of this container itself
        std::string currentKey;         // objects: key of the member being read
    };

    // Pointer of the next member of the current top frame (the container being
    // started, or the scalar about to attach); "" at the root.
    std::string childPointer() const
    {
        if (stack_.empty())
        {
            return std::string{};
        }
        const Frame &top = stack_.back();
        if (top.value.is_object())
        {
            return top.pointer + "/" + pointerEscape(top.currentKey);
        }
        return top.pointer + "/" + std::to_string(top.value.size());
    }

    std::string currentPointer() const
    {
        return stack_.empty() ? std::string{} : stack_.back().pointer;
    }

    bool attach(
        Json value
    )
    {
        if (stack_.empty())
        {
            root_ = std::move(value);
            return true;
        }
        Frame &top = stack_.back();
        if (top.value.is_object())
        {
            top.value[top.currentKey] = std::move(value);
        } else
        {
            top.value.push_back(std::move(value));
        }
        return true;
    }

    bool fail(
        const std::string &jsonPointer, std::string message
    )
    {
        failed_ = true;
        error_  = DataError{std::string{}, jsonPointer, std::move(message)};
        return false;
    }

    std::vector<Frame> stack_;
    Json root_;
    bool failed_ = false;
    DataError error_{};
};

} // namespace

std::string DataError::toString() const
{
    std::string out = file;
    if (!jsonPointer.empty())
    {
        out += ": ";
        out += jsonPointer;
    }
    out += ": ";
    out += message;
    return out;
}

std::size_t utf8ErrorOffset(
    std::string_view text
)
{
    const auto *bytes      = reinterpret_cast<const unsigned char *>(text.data());
    const std::size_t size = text.size();
    std::size_t i          = 0;
    while (i < size)
    {
        const unsigned char lead = bytes[i];
        if (lead <= 0x7F)
        {
            ++i;
            continue;
        }

        std::size_t continuations = 0;
        unsigned char firstLow    = 0x80; // inclusive range of the FIRST continuation
        unsigned char firstHigh   = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF)
        {
            continuations = 1; // 2-byte; 0xC0/0xC1 would be overlong, excluded
        } else if (lead == 0xE0)
        {
            continuations = 2;
            firstLow      = 0xA0; // no overlong 3-byte
        } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF)
        {
            continuations = 2;
        } else if (lead == 0xED)
        {
            continuations = 2;
            firstHigh     = 0x9F; // no surrogates (U+D800..U+DFFF)
        } else if (lead == 0xF0)
        {
            continuations = 3;
            firstLow      = 0x90; // no overlong 4-byte
        } else if (lead >= 0xF1 && lead <= 0xF3)
        {
            continuations = 3;
        } else if (lead == 0xF4)
        {
            continuations = 3;
            firstHigh     = 0x8F; // nothing above U+10FFFF
        } else
        {
            return i; // 0x80..0xC1 (lone continuation / overlong) and 0xF5..0xFF
        }

        if (size - i - 1 < continuations)
        {
            return i; // truncated sequence at end of input
        }
        const unsigned char first = bytes[i + 1];
        if (first < firstLow || first > firstHigh)
        {
            return i;
        }
        for (std::size_t k = 2; k <= continuations; ++k)
        {
            const unsigned char next = bytes[i + k];
            if (next < 0x80 || next > 0xBF)
            {
                return i;
            }
        }
        i += continuations + 1;
    }
    return static_cast<std::size_t>(std::string_view::npos);
}

Result<Json, DataError> parseJsonText(
    std::string_view text, const std::string &fileLabel
)
{
    // 1. UTF-8 first - the friendliest error a broken file can produce.
    const std::size_t badByte = utf8ErrorOffset(text);
    if (badByte != std::string_view::npos)
    {
        return Result<Json, DataError>::err(
            DataError{
                fileLabel, "", "file is not valid UTF-8 (first invalid byte at offset " + std::to_string(badByte) + ")"
            }
        );
    }

    // 2. Tolerate (and skip) a single leading UTF-8 BOM: some editors add one
    //    and rejecting whole files over it helps nobody.
    std::string bytes(text);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        bytes.erase(0, 3);
    }

    // 3. Strict SAX build: duplicate keys rejected at read time, nothing but
    //    whitespace may follow the top-level value (sax_parse's strict
    //    default), no comments (JSON has none).
    StrictDomBuilder builder;
    if (!Json::sax_parse(bytes, &builder))
    {
        DataError error
            = builder.failed() ? builder.error() : DataError{std::string{}, std::string{}, "unspecified parse failure"};
        error.file = fileLabel;
        return Result<Json, DataError>::err(std::move(error));
    }
    return Result<Json, DataError>::ok(builder.takeDom());
}

Result<Json, DataError> loadJsonFile(
    const std::string &path
)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return Result<Json, DataError>::err(DataError{path, std::string{}, "cannot open file"});
    }
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad())
    {
        return Result<Json, DataError>::err(DataError{path, std::string{}, "read failure"});
    }
    return parseJsonText(bytes, path);
}

std::int64_t scaleToGrid(
    double value
)
{
    const double scaled = value * static_cast<double>(SCALE);
    // Clamp authoring absurdities instead of invoking undefined behavior in
    // std::llround; the schema layer's bounds checks reject the clamped
    // value loudly (schema.cpp reports it against the field's [min, max]).
    if (scaled >= kInt64MagnitudeLimit)
    {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (scaled <= -kInt64MagnitudeLimit)
    {
        return std::numeric_limits<std::int64_t>::min();
    }
    // std::llround: round half away from zero - the pinned, deterministic
    // rule, identical on every IEEE-754 platform the engine builds for.
    return static_cast<std::int64_t>(std::llround(scaled));
}

std::int64_t scaledValue(
    const Json &field
)
{
    if (field.is_number_unsigned())
    {
        const auto raw = field.get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            return std::numeric_limits<std::int64_t>::max();
        }
        return static_cast<std::int64_t>(raw);
    }
    if (field.is_number_integer())
    {
        return field.get<std::int64_t>();
    }
    return scaleToGrid(field.get<double>()); // fractional (schema-validated)
}

Json mergeData(
    const Json &base, const Json &overrideJson
)
{
    if (base.is_object() && overrideJson.is_object())
    {
        Json merged = base;
        for (auto member = overrideJson.begin(); member != overrideJson.end(); ++member)
        {
            const auto existing = merged.find(member.key());
            if (existing != merged.end())
            {
                Json child           = mergeData(*existing, member.value());
                merged[member.key()] = std::move(child);
            } else
            {
                merged[member.key()] = member.value();
            }
        }
        return merged;
    }
    // Arrays and scalars replace wholesale - never concatenate.
    return overrideJson;
}

std::string pointerEscape(
    std::string_view key
)
{
    std::string escaped;
    escaped.reserve(key.size());
    for (const char c : key)
    {
        if (c == '~')
        {
            escaped += "~0";
        } else if (c == '/')
        {
            escaped += "~1";
        } else
        {
            escaped += c;
        }
    }
    return escaped;
}

} // namespace harrowmere::data
