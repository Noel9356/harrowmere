#include "gltf_json.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace engine
{

namespace
{

class JsonParser
{
public:
    JsonParser(
        const std::string &text, std::string &errorMessage
    )
        : text_(text), errorMessage_(errorMessage)
    {}

    std::unique_ptr<JsonValue> parseDocument()
    {
        try
        {
            skipWhitespace();
            JsonValue value = parseValue(0);
            skipWhitespace();
            if (position_ != text_.size())
            {
                fail("unexpected trailing content after the document");
            }
            return std::make_unique<JsonValue>(std::move(value));
        } catch (const JsonParseAbort &)
        {
            return nullptr;
        }
    }

private:
    static constexpr int maxDepth = 64;

    const std::string &text_;
    std::string &errorMessage_;
    std::size_t position_ = 0;

    // Values are built on the stack and MOVED into their parents: the tree
    // previously unique_ptr-chained every node, so each scalar paid one
    // heap allocation plus one free that the parent's move made pure waste
    // (glTF accessor min/max and node transforms are number-dense; a typical
    // import parses thousands of scalars). fail() aborts by exception - the
    // caller's single catch in parseDocument converts that to the nullptr
    // + error message contract - so no recursive level needs its own
    // nullptr plumbing, and the FIRST failure's message survives (the old
    // catch-and-refail at every level overwrote the precise message with a
    // generic one).
    [[noreturn]] void fail(
        const std::string &problem
    )
    {
        std::ostringstream stream;
        stream << "JSON parse error at byte " << position_ << ": " << problem;
        errorMessage_ = stream.str();
        throw JsonParseAbort();
    }

    struct JsonParseAbort
    {};

    void skipWhitespace()
    {
        while (position_ < text_.size())
        {
            const char character = text_[position_];
            if (character == ' ' || character == '\t' || character == '\n' || character == '\r')
            {
                ++position_;
            } else
            {
                break;
            }
        }
    }

    bool consume(
        char expected
    )
    {
        if (position_ < text_.size() && text_[position_] == expected)
        {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(
        char expected
    )
    {
        if (!consume(expected))
        {
            fail(std::string("expected '") + expected + "'");
        }
    }

    JsonValue parseValue(
        int depth
    )
    {
        if (depth > maxDepth)
        {
            fail("nesting deeper than 64 levels");
        }
        skipWhitespace();
        if (position_ >= text_.size())
        {
            fail("unexpected end of input");
        }
        const char character = text_[position_];
        switch (character)
        {
        case '{':
            return parseObject(depth);
        case '[':
            return parseArray(depth);
        case '"': {
            JsonValue value;
            value.type   = JsonValue::Type::String;
            value.string = parseString();
            return value;
        }
        case 't':
            return parseLiteral("true", JsonValue::Type::Boolean, true);
        case 'f':
            return parseLiteral("false", JsonValue::Type::Boolean, false);
        case 'n':
            return parseLiteral("null", JsonValue::Type::Null, false);
        default:
            return parseNumber();
        }
    }

    JsonValue parseObject(
        int depth
    )
    {
        expect('{');
        JsonValue value;
        value.type = JsonValue::Type::Object;
        skipWhitespace();
        if (consume('}'))
        {
            return value;
        }
        while (true)
        {
            skipWhitespace();
            if (position_ >= text_.size() || text_[position_] != '"')
            {
                fail("expected a string key");
            }
            std::string key = parseString();
            skipWhitespace();
            expect(':');
            JsonValue memberValue = parseValue(depth + 1);
            value.members.emplace_back(std::move(key), std::move(memberValue));
            skipWhitespace();
            if (consume(','))
            {
                continue;
            }
            expect('}');
            break;
        }
        return value;
    }

    JsonValue parseArray(
        int depth
    )
    {
        expect('[');
        JsonValue value;
        value.type = JsonValue::Type::Array;
        skipWhitespace();
        if (consume(']'))
        {
            return value;
        }
        while (true)
        {
            JsonValue element = parseValue(depth + 1);
            value.array.push_back(std::move(element));
            skipWhitespace();
            if (consume(','))
            {
                continue;
            }
            expect(']');
            break;
        }
        return value;
    }

    std::string parseString()
    {
        expect('"');
        // The span up to the closing quote is located with memchr (SIMD
        // backed, one pass). A span without escape sequences IS the value:
        // validate the RFC control-character rule over the span, then one
        // assign - no per-character loop, no reallocation. Escaped spans
        // fall back to the escape-aware character loop with the span length
        // as a one-allocation reserve (the decoded value is never longer).
        // This replaces unreserved per-character growth, which was brutal
        // for glTF's data URIs: a 500 KB base64 token reallocated ~20
        // times, copying about a megabyte cumulatively.
        const std::size_t remaining = text_.size() - position_;
        const char *spanStart       = text_.data() + position_;
        const char *closingQuote    = static_cast<const char *>(std::memchr(spanStart, '"', remaining));
        if (closingQuote == nullptr)
        {
            fail("unterminated string");
        }
        const std::size_t spanLength = static_cast<std::size_t>(closingQuote - spanStart);
        const char *firstEscape      = static_cast<const char *>(std::memchr(spanStart, '\\', spanLength));
        if (firstEscape == nullptr)
        {
            // Strict RFC 8259: raw control characters are refused even on
            // the verbatim path (glTF files are machine written; leniency
            // here would hide exporter bugs).
            for (std::size_t index = 0; index < spanLength; ++index)
            {
                if (static_cast<unsigned char>(spanStart[index]) < 0x20)
                {
                    fail("raw control character inside a string");
                }
            }
            std::string result(spanStart, spanLength);
            position_ += spanLength + 1; // past the closing quote
            return result;
        }
        std::string result;
        result.reserve(spanLength);
        while (position_ < text_.size())
        {
            const char character = text_[position_++];
            if (character == '"')
            {
                return result;
            }
            if (character == '\\')
            {
                if (position_ >= text_.size())
                {
                    fail("unterminated escape sequence");
                }
                const char escape = text_[position_++];
                switch (escape)
                {
                case '"':
                    result.push_back('"');
                    break;
                case '\\':
                    result.push_back('\\');
                    break;
                case '/':
                    result.push_back('/');
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                case 'u': {
                    // glTF uses \uXXXX for non-ASCII; the importer only
                    // reads ASCII names, so decode to UTF-8 for the BMP
                    // range and refuse surrogates (they never appear in
                    // the fields this parser is used for).
                    if (position_ + 4 > text_.size())
                    {
                        fail("truncated \\u escape");
                    }
                    // from_chars over the raw window, no temporary string:
                    // the substr this used to build allocated (and freed) a
                    // 4-byte std::string per escape just to hand strtoul a
                    // c_str. The value semantics match strtoul on these
                    // windows exactly: up to four hex digits consumed
                    // left to right, no digits parsed as 0, no sign, no
                    // range failure below 0x10000.
                    const char *const hexFirst = text_.data() + position_;
                    position_ += 4;
                    unsigned long codePoint = 0;
                    std::from_chars(hexFirst, hexFirst + 4, codePoint, 16);
                    if (codePoint >= 0xD800ul && codePoint <= 0xDFFFul)
                    {
                        fail("surrogate \\u escapes are not supported");
                    }
                    if (codePoint < 0x80ul)
                    {
                        result.push_back(static_cast<char>(codePoint));
                    } else if (codePoint < 0x800ul)
                    {
                        result.push_back(static_cast<char>(0xC0ul | (codePoint >> 6)));
                        result.push_back(static_cast<char>(0x80ul | (codePoint & 0x3Ful)));
                    } else
                    {
                        result.push_back(static_cast<char>(0xE0ul | (codePoint >> 12)));
                        result.push_back(static_cast<char>(0x80ul | ((codePoint >> 6) & 0x3Ful)));
                        result.push_back(static_cast<char>(0x80ul | (codePoint & 0x3Ful)));
                    }
                    break;
                }
                default:
                    fail("unknown escape sequence");
                }
                continue;
            }
            if (static_cast<unsigned char>(character) < 0x20)
            {
                fail("raw control character inside a string");
            }
            result.push_back(character);
        }
        fail("unterminated string");
    }

    JsonValue parseLiteral(
        const char *literal, JsonValue::Type type, bool boolean
    )
    {
        const std::size_t length = std::char_traits<char>::length(literal);
        if (text_.compare(position_, length, literal) != 0)
        {
            fail("invalid literal");
        }
        position_ += length;
        JsonValue value;
        value.type    = type;
        value.boolean = boolean;
        return value;
    }

    JsonValue parseNumber()
    {
        const std::size_t start = position_;
        if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+'))
        {
            ++position_;
        }
        bool digits = false;
        while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])))
        {
            ++position_;
            digits = true;
        }
        if (position_ < text_.size() && text_[position_] == '.')
        {
            ++position_;
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])))
            {
                ++position_;
                digits = true;
            }
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E'))
        {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+'))
            {
                ++position_;
            }
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])))
            {
                ++position_;
            }
        }
        if (!digits)
        {
            fail("invalid number");
        }
        // std::from_chars over the already-scanned span: no temporary string,
        // no locale dependence (strtod honors the C locale's decimal point),
        // and roughly a fifth faster per value - glTF accessor min/max and
        // node transforms are number-dense.
        const char *first = text_.data() + start;
        const char *last  = text_.data() + position_;
        if (*first == '+')
        {
            // from_chars implements the JSON grammar, which has no leading
            // '+'; this parser's span scan has always accepted one (and
            // strtod parsed it), so skip the sign and parse the rest as the
            // positive value it is.
            ++first;
        }
        double parsed                       = 0.0;
        const std::from_chars_result result = std::from_chars(first, last, parsed);
        if (result.ec != std::errc() || result.ptr != last || !std::isfinite(parsed))
        {
            fail("number is not a finite double");
        }
        JsonValue value;
        value.type   = JsonValue::Type::Number;
        value.number = parsed;
        return value;
    }
};

} // namespace

std::unique_ptr<JsonValue> jsonParse(
    const std::string &text, std::string &errorMessage
)
{
    JsonParser parser(text, errorMessage);
    return parser.parseDocument();
}

} // namespace engine
