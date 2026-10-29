#ifndef HARROWMERE_CORE_DATA_JSON_HPP
#define HARROWMERE_CORE_DATA_JSON_HPP

// The JSON data layer (task 0.4) - the game's one door for game *content*
// (D-DATA-1, common.md §4.3): species, items, crime data, laws, roles,
// territory templates, suspicion triggers, event templates, quests, recipes.
// Configuration is TOML and belongs to the engine; the one configuration read
// this layer owns is wrapped in data/config.hpp.
//
// This header is the ONLY place outside src/data/ where nlohmann headers may
// appear (task 0.4 spec item 1): game code includes data/json.hpp and works
// with the Json alias and the functions below - never with nlohmann directly
// (review-enforced). The library is vendored and pinned at
// third_party/nlohmann_json (v3.11.3, single_include, fetched from the tag).
//
// Strictness contract (phase-1 "Playable when": a broken data row "fails
// startup with file + path + expected, never a mid-sim crash"):
//   * the file must be valid UTF-8 (a single leading BOM is tolerated and
//     skipped);
//   * trailing garbage after the top-level value is rejected;
//   * duplicate keys in one object are an error (nlohmann's default parse
//     silently keeps the last - the card's pitfall; the strict choice is made
//     here, once, for every data file);
//   * syntax errors report the byte position and the innermost JSON pointer.
//
// Compile-time dependency: core/constants.hpp (task 0.2, a PARALLEL card) for
// SCALE (Appendix A row A-1). Verification builds satisfy the include with a
// stub until that card merges; the merged tree carries the real file.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/constants.hpp"

namespace harrowmere::data
{

// The vendored DOM type, re-exported so callers never name nlohmann.
using Json = nlohmann::json;

// One data failure: everything a startup error message needs - file + JSON
// pointer (RFC 6901) + what was expected.
struct DataError
{
    std::string file;        // path as given to the loader
    std::string jsonPointer; // pointer to the offending value; "" = whole file
    std::string message;     // states what went wrong / what was expected

    // "<file>: <pointer>: <message>" - the one-line form startup logs print
    // (the pointer part is omitted when empty).
    std::string toString() const;
};

// The data layer's Result. Task 0.2 pins core/result.hpp as the game's Result
// home; task 0.4 is a parallel card, so this minimal local form keeps the
// data layer compilable before 0.2 merges. At merge: if 0.2's Result is
// API-compatible, collapse this class to
//     template <typename T, typename E> using Result = harrowmere::Result<T, E>;
// (one line - DEVIATIONS.md tracks it); if it is not, the handful of call
// sites in json.cpp adapt mechanically.
template <typename T, typename E> class Result
{
public:
    static Result ok(
        T value
    )
    {
        return Result(std::in_place_index<0>, std::move(value));
    }

    static Result err(
        E error
    )
    {
        return Result(std::in_place_index<1>, std::move(error));
    }

    bool isOk() const
    {
        return storage_.index() == 0;
    }

    bool isErr() const
    {
        return !isOk();
    }

    explicit operator bool() const
    {
        return isOk();
    }

    const T &value() const
    {
        return std::get<0>(storage_);
    }

    const E &error() const
    {
        return std::get<1>(storage_);
    }

private:
    template <std::size_t I, typename V>
    Result(
        std::in_place_index_t<I> where, V &&v
    )
        : storage_(where, std::forward<V>(v))
    {}

    std::variant<T, E> storage_;
};

// Strict load of one JSON content file (see the contract above).
Result<Json, DataError> loadJsonFile(const std::string &path);

// The same strict parse from memory; `fileLabel` is stamped into any error.
Result<Json, DataError> parseJsonText(std::string_view text, const std::string &fileLabel);

// THE scale conversion (task 0.4 pitfall: "do the scale conversion in one
// pinned helper so rounding is consistent everywhere"). Fractional authored
// value -> SCALE grid (A-1: SCALE = 10'000), round half away from zero:
// 0.37 -> 3700, 2.0 -> 20'000, 2.675 -> 26'750. This is the single conversion
// point in the game; there is no second one to drift from it.
std::int64_t scaleToGrid(double value);

// Grid value of a validated *scaled* field - the numeric policy in one
// function (task 0.4 spec item 5; design rule: "Data can be non-integer but
// will be scaled and casted into integer to be use"):
//   integer JSON number    -> used as-is: already on the grid. Appendix B
//                             authors grid values as integers ("severity":
//                             9000, "xenophobiaBias": 2000);
//   fractional JSON number -> scaleToGrid(value). Appendix B authors
//                             multipliers as fractions ("statMultiplier":
//                             2.0, "perCapitaNeed": 0.5, "damageTakenMultiplier":
//                             0.25).
// Behavior is undefined for non-numbers - schema validation runs first.
std::int64_t scaledValue(const Json &field);

// Mod merge policy (implemented now per the card; consumed by task 7.5) -
// mirrors the engine's TOML layer philosophy ("rows authored wholesale and
// replaced wholesale"):
//   both objects -> deep-merge key by key (the override wins; keys only in
//                   the override are added; keys only in the base are kept);
//   otherwise    -> the override replaces the base wholesale (arrays NEVER
//                   concatenate; a scalar or array replaces whatever was
//                   there, including a base object).
Json mergeData(const Json &base, const Json &overrideJson);

// UTF-8 sweep used by the loader (exposed for task 1.3's reload path).
// Returns the byte offset of the first invalid sequence, or npos when the
// whole text is valid UTF-8. Rejects lone continuations, overlong forms,
// surrogates and code points above U+10FFFF; a leading BOM counts as valid.
std::size_t utf8ErrorOffset(std::string_view text);

// Escape one object key for use inside a JSON pointer (RFC 6901):
// '~' -> "~0", '/' -> "~1".
std::string pointerEscape(std::string_view key);

} // namespace harrowmere::data

#endif // HARROWMERE_CORE_DATA_JSON_HPP
