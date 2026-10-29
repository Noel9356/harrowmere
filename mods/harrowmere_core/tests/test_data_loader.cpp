// Task 0.4 - data layer tests. The five PINNED test names from the card come
// first (names are pinned, common.md §1.2 - keep them); the cases after the
// pinned block are supplementary coverage of the same contract. The file
// defines no main: task 0.6's tests/main.cpp (doctest implementation) owns
// it - until that lands, the standalone verification harness supplies one
// (scripts/task04_verify/, not part of the repo build).
//
// Pinned by the card:
//   TestDataLoader_MissingRequiredField_ReportsFileAndPath
//   TestDataLoader_ScaledNumber_RoundsToScaleGrid
//   TestDataLoader_DuplicateKey_Rejected
//   TestDataMerge_ObjectDeep_ArrayReplace
//   TestDataLoader_AllShippedFilesValidate
#include "doctest/doctest.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "data/config.hpp"
#include "data/json.hpp"
#include "data/schema.hpp"

namespace
{

// SCALE (A-1) comes from the constants header task 0.2 owns; the data layer
// references it unqualified from inside namespace harrowmere::data, and this
// using-declaration is the test-side equivalent of that assumption.
using harrowmere::SCALE;

// Closed sets the schema DSL consumes - passed as arrays (a braced list does
// not implicitly convert to std::span; the pinned API takes spans).
constexpr const char *kSuspicionActs[] = {
    "CROUCH", "UNLIT", "MASKED", "TRESPASS", "BLOODSTAINED", "FLEE"};
constexpr const char *kSuspicionMasks[] = {
    "SETTLEMENT", "NIGHT", "PRIVATE_PROPERTY", "MARKET"};
constexpr const char *kStatNames[] = {
    "STRENGTH", "DEFENCE", "AGILITY", "INTELLIGENCE", "ENDURANCE", "PERCEPTION"};

// Scratch files live under the system temp dir, one fixed subdirectory - the
// tests are deterministic and never write into the repository.
const std::filesystem::path &scratchDir()
{
    static const std::filesystem::path dir = [] {
        const auto dir = std::filesystem::temp_directory_path() / "harrowmere_task04_tests";
        std::filesystem::create_directories(dir);
        return dir;
    }();
    return dir;
}

std::string writeTempFile(const char *name, std::string_view bytes)
{
    const auto path = scratchDir() / name;
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path.string();
}

// parseJsonText is the loader's memory face; every strictness behavior is
// identical to loadJsonFile's, with a stable label for the error's file field.
harrowmere::data::Json parseOrThrow(std::string_view text)
{
    const auto parsed = harrowmere::data::parseJsonText(text, "inline.json");
    REQUIRE(parsed.isOk());
    return parsed.value();
}

} // namespace

// ---- the five pinned tests ---------------------------------------------------

TEST_CASE("TestDataLoader_MissingRequiredField_ReportsFileAndPath")
{
    using namespace harrowmere::data;

    Schema items;
    items.field("id", JsonType::Integer, 0, 1'000'000)
        .field("key", JsonType::String)
        .field("notes", JsonType::String, false); // optional (species rows carry "notes")

    const std::string path = writeTempFile("missing_required.json", R"({"id": 7, "notes": "x"})");
    const auto loaded = loadJsonFile(path);
    REQUIRE(loaded.isOk());

    const std::vector<DataError> errors = items.validate(loaded.value(), path);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].file == path);
    CHECK(errors[0].jsonPointer == "/key");
    CHECK(errors[0].message.find("required") != std::string::npos);
    CHECK(errors[0].message.find("key") != std::string::npos);
    // The one-line report carries file + path + expected in order.
    CHECK(errors[0].toString().find(path + ": /key: ") == 0);
}

TEST_CASE("TestDataLoader_ScaledNumber_RoundsToScaleGrid")
{
    using namespace harrowmere::data;

    Schema multipliers;
    multipliers.field("value", JsonType::Scaled, 0,
                      static_cast<std::int64_t>(10 * SCALE)); // [0, 10.0x] on the grid

    const auto check = [&](std::string_view text, std::int64_t expectedGrid) {
        const Json json = parseOrThrow(text);
        MESSAGE("input: ", text);
        REQUIRE(multipliers.validate(json, "inline.json").empty());
        CHECK(scaledValue(json["value"]) == expectedGrid);
    };

    // Fractional authored values: THE conversion, round half away from zero.
    check(R"({"value": 2.0})", 20'000);
    check(R"({"value": 0.37})", 3'700);
    check(R"({"value": 2.675})", 26'750);
    check(R"({"value": 0.0001})", 1);
    check(R"({"value": 2.5})", 25'000);
    // Integer authored values: already on the grid, used as-is (Appendix B
    // authors weights and biases this way: "severity": 9000).
    check(R"({"value": 9000})", 9'000);
    // The helper itself, including negatives (cultureBias ships -6000).
    CHECK(scaleToGrid(-0.6) == -6'000);
    CHECK(scaleToGrid(1.5) == 15'000);

    // A fractional value that scales OUT of bounds is rejected with the grid
    // number and the raw authoring value both present.
    Schema strict;
    strict.field("value", JsonType::Scaled, 0, static_cast<std::int64_t>(SCALE));
    const Json tooBig = parseOrThrow(R"({"value": 1.5})"); // -> 15000 > SCALE
    const std::vector<DataError> errors = strict.validate(tooBig, "inline.json");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].jsonPointer == "/value");
    CHECK(errors[0].message.find("15000") != std::string::npos);
    CHECK(errors[0].message.find("1.5") != std::string::npos);

    // A fractional value in an INTEGER field is an error, never a silent cast.
    Schema integral;
    integral.field("lifespan_years", JsonType::Integer);
    const Json fractional = parseOrThrow(R"({"lifespan_years": 60.5})");
    REQUIRE(integral.validate(fractional, "inline.json").size() == 1);
}

TEST_CASE("TestDataLoader_DuplicateKey_Rejected")
{
    using namespace harrowmere::data;

    const std::string path = writeTempFile("duplicate_key.json", R"({"id": 1, "id": 2})");
    const auto loaded = loadJsonFile(path);
    REQUIRE(loaded.isErr());
    CHECK(loaded.error().file == path);
    CHECK(loaded.error().jsonPointer == "/id");
    CHECK(loaded.error().message.find("duplicate") != std::string::npos);

    // Nested duplicates carry the full pointer, and same-value duplicates are
    // still duplicates (strictness is syntactic, not value-based).
    const auto nested = parseJsonText(R"({"a": {"x": 1, "x": 1}})", "t.json");
    REQUIRE(nested.isErr());
    CHECK(nested.error().jsonPointer == "/a/x");

    // The same document with distinct keys parses.
    const auto fine = parseJsonText(R"({"id": 1, "id2": 2})", "t.json");
    CHECK(fine.isOk());
}

TEST_CASE("TestDataMerge_ObjectDeep_ArrayReplace")
{
    using namespace harrowmere::data;

    const Json base = parseOrThrow(R"({
        "keep": {"deep": {"x": 1, "y": 2}, "arr": [1, 2, 3]},
        "scalar": 5,
        "only_base": true
    })");
    const Json over = parseOrThrow(R"({
        "keep": {"deep": {"y": 9}, "arr": [42]},
        "scalar": 7,
        "only_over": "new"
    })");

    const Json merged = mergeData(base, over);

    // Objects deep-merge: overridden keys win, sibling keys survive.
    CHECK(merged["keep"]["deep"]["x"] == Json(1));
    CHECK(merged["keep"]["deep"]["y"] == Json(9));
    // Arrays replace wholesale - never concatenated, never element-merged.
    CHECK(merged["keep"]["arr"] == Json::array({42}));
    // Scalars replace.
    CHECK(merged["scalar"] == Json(7));
    // Keys only present on one side are kept.
    CHECK(merged["only_base"] == Json(true));
    CHECK(merged["only_over"] == Json("new"));
    // The inputs are untouched (merge is pure - the reload path depends on it).
    CHECK(base["keep"]["deep"]["y"] == Json(2));
    CHECK(base["keep"]["arr"].size() == 3);
}

TEST_CASE("TestDataLoader_AllShippedFilesValidate")
{
    using namespace harrowmere::data;

    // The shipped data set lives in the mod's package (task 0.4 pinned
    // decision). Task 0.6's harness pins the production define; the
    // standalone verification harness passes the same path.
#ifndef HARROWMERE_CORE_DATA_DIR
#define HARROWMERE_CORE_DATA_DIR "mods/harrowmere_core/data"
#endif

    const std::filesystem::path dataDir(HARROWMERE_CORE_DATA_DIR);
    REQUIRE(std::filesystem::exists(dataDir));

    // v0.1 ships an empty data set (only .gitkeep): zero files is a valid
    // pass of this test (task 0.4 Done-when). Tasks 1.1/1.2 add files and
    // their per-file schemas; the strict load below is the floor every one
    // of them must clear.
    std::size_t checked = 0;
    for (const auto &entry : std::filesystem::directory_iterator(dataDir))
    {
        if (entry.path().extension() != ".json")
        {
            continue;
        }
        MESSAGE("file: ", entry.path().string());
        const auto loaded = loadJsonFile(entry.path().string());
        REQUIRE(loaded.isOk());
        ++checked;
    }
    CHECK(checked >= 0);
}

// ---- supplementary coverage (beyond the card's pinned minimum) ---------------

TEST_CASE("TestDataLoader_TrailingGarbage_Rejected")
{
    using namespace harrowmere::data;

    const auto trailing = parseJsonText(R"({"a": 1} {"b": 2})", "t.json");
    REQUIRE(trailing.isErr());
    CHECK(trailing.error().message.find("parse error") != std::string::npos);

    const auto after = parseJsonText("[1, 2] and then some prose", "t.json");
    CHECK(after.isErr());

    // Trailing whitespace is fine; a BOM is tolerated and skipped.
    const auto bom = parseJsonText("\xEF\xBB\xBF{\"a\": 1}\n  ", "t.json");
    REQUIRE(bom.isOk());
    CHECK(bom.value()["a"] == Json(1));

    // An empty file is a parse error, not a silent empty document.
    const auto empty = parseJsonText("", "t.json");
    CHECK(empty.isErr());
}

TEST_CASE("TestDataLoader_Utf8InvalidByte_Rejected")
{
    using namespace harrowmere::data;

    // 0xFF is never a valid UTF-8 lead byte.
    const auto invalid = parseJsonText("{\"name\": \"\xFF\"}", "t.json");
    REQUIRE(invalid.isErr());
    CHECK(invalid.error().message.find("UTF-8") != std::string::npos);

    // A lone continuation byte (0x80) is invalid.
    CHECK(utf8ErrorOffset("\x80") == 0);
    // A 3-byte sequence cut short is invalid at its lead byte.
    CHECK(utf8ErrorOffset("ok\xE2\x82") == 2);
    // A surrogate lead (ED A0 ...) is invalid.
    CHECK(utf8ErrorOffset("\xED\xA0\x80") == 0);
    // Well-formed multi-byte text passes (U+00E9 e-acute, U+2713 check mark).
    CHECK(utf8ErrorOffset("caf\xC3\xA9 \xE2\x9C\x93") == std::string_view::npos);
    // Valid JSON with non-ASCII content loads.
    const auto unicode = parseJsonText("{\"name\": \"caf\xC3\xA9\"}", "t.json");
    REQUIRE(unicode.isOk());
    CHECK(unicode.value()["name"] == Json("caf\xC3\xA9"));
}

TEST_CASE("TestDataLoader_FileMissing_ReportsFile")
{
    using namespace harrowmere::data;

    const auto missing = loadJsonFile("/nonexistent/harrowmere/data/nowhere.json");
    REQUIRE(missing.isErr());
    CHECK(missing.error().file == "/nonexistent/harrowmere/data/nowhere.json");
    CHECK(missing.error().jsonPointer.empty());
    CHECK(missing.error().message.find("cannot open") != std::string::npos);
}

TEST_CASE("TestDataLoader_UnknownField_Rejected")
{
    using namespace harrowmere::data;

    Schema row;
    row.field("id", JsonType::Integer).field("key", JsonType::String);
    // "raritty" is exactly the authoring typo strictness exists to catch.
    const Json typo = parseOrThrow(R"({"id": 1, "key": "iron_sword", "raritty": 2000})");
    const std::vector<DataError> errors = row.validate(typo, "items.json");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].jsonPointer == "/raritty");
    CHECK(errors[0].message.find("unexpected field") != std::string::npos);
}

TEST_CASE("TestDataSchema_EnumField_RejectsUnknownValueAndListsAllowed")
{
    using namespace harrowmere::data;

    Schema row;
    row.enumerate("act", kSuspicionActs)
        .field("weight", JsonType::Scaled, 0, static_cast<std::int64_t>(SCALE));

    const Json good = parseOrThrow(R"({"act": "CROUCH", "weight": 2000})");
    CHECK(row.validate(good, "suspicion.json").empty());

    const Json bad = parseOrThrow(R"({"act": "SNEAK", "weight": 2000})");
    const std::vector<DataError> errors = row.validate(bad, "suspicion.json");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].jsonPointer == "/act");
    CHECK(errors[0].message.find("SNEAK") != std::string::npos);
    CHECK(errors[0].message.find("CROUCH") != std::string::npos);

    // A non-string where an enum belongs is a type error that still lists the set.
    const Json notString = parseOrThrow(R"({"act": 3, "weight": 2000})");
    REQUIRE(row.validate(notString, "suspicion.json").size() == 1);
}

TEST_CASE("TestDataSchema_ArrayElementErrors_CarryIndexPointers")
{
    using namespace harrowmere::data;

    Schema record;
    record.array("names", Schema::scalar(JsonType::String));
    const Json broken = parseOrThrow(R"({"names": ["ana", "bo", 3, "dee"]})");
    const std::vector<DataError> errors = record.validate(broken, "names.json");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].jsonPointer == "/names/2");
    CHECK(errors[0].message.find("expected string") != std::string::npos);

    // The wrong container itself is reported at the field pointer.
    const Json notArray = parseOrThrow(R"({"names": "ana"})");
    const std::vector<DataError> containerErrors = record.validate(notArray, "names.json");
    REQUIRE(containerErrors.size() == 1);
    CHECK(containerErrors[0].jsonPointer == "/names");
}

TEST_CASE("TestDataSchema_MapField_ValidatesKeysAndValues")
{
    using namespace harrowmere::data;

    // The statMultiplier shape from Appendix B.1: free-keyed in general, but
    // this field's keys are the four doc stat names (enums.hpp, task 1.1,
    // closes the list; ENDURANCE/PERCEPTION are species-file additions).
    Schema species;
    species.field("key", JsonType::String)
        .map("statMultiplier", JsonType::Scaled, kStatNames);

    const Json good = parseOrThrow(
        R"({"key": "beastkin", "statMultiplier": {"STRENGTH": 2.0, "INTELLIGENCE": 1.0}})");
    REQUIRE(species.validate(good, "species.json").empty());
    CHECK(scaledValue(good["statMultiplier"]["STRENGTH"]) == 20'000);

    const Json unknownStat = parseOrThrow(
        R"({"key": "elf", "statMultiplier": {"STRENGHT": 1.0}})"); // typo'd stat
    const std::vector<DataError> keyErrors = species.validate(unknownStat, "species.json");
    REQUIRE(keyErrors.size() == 1);
    CHECK(keyErrors[0].jsonPointer == "/statMultiplier/STRENGHT");
    CHECK(keyErrors[0].message.find("unknown map key") != std::string::npos);

    const Json badValue = parseOrThrow(
        R"({"key": "elf", "statMultiplier": {"ARCANA": true}})");
    const std::vector<DataError> valueErrors = species.validate(badValue, "species.json");
    REQUIRE(valueErrors.size() == 1);
    CHECK(valueErrors[0].jsonPointer == "/statMultiplier/ARCANA");

    // A free-keyed map (cultureBias: speciesId strings -> bias) accepts any
    // key and checks only the value type.
    Schema laws;
    laws.map("cultureBias", JsonType::Integer);
    const Json bias = parseOrThrow(R"({"cultureBias": {"2": -6000, "4": 1000}})");
    CHECK(laws.validate(bias, "laws.json").empty());
}

TEST_CASE("TestDataSchema_RootArrayOfRecords_SuspicionShapeValidates")
{
    using namespace harrowmere::data;

    // suspicion.json (Appendix B.8) is an array of records at the document
    // root - the shape Schema::arrayOf exists for.
    Schema mask = Schema::enumeration(kSuspicionMasks);
    Schema row;
    row.enumerate("act", kSuspicionActs)
        .array("masks", mask)
        .field("weight", JsonType::Scaled, 0, static_cast<std::int64_t>(SCALE));
    const Schema suspicion = Schema::arrayOf(row);

    const Json pinnedRows = parseOrThrow(R"([
        { "act": "CROUCH",       "masks": ["SETTLEMENT", "MARKET"], "weight": 2000 },
        { "act": "UNLIT",        "masks": ["NIGHT", "SETTLEMENT"],  "weight": 2500 },
        { "act": "MASKED",       "masks": ["SETTLEMENT", "MARKET"], "weight": 5000 },
        { "act": "TRESPASS",     "masks": ["PRIVATE_PROPERTY"],     "weight": 4000 },
        { "act": "BLOODSTAINED", "masks": ["SETTLEMENT"],           "weight": 6000 },
        { "act": "FLEE",         "masks": ["SETTLEMENT", "NIGHT"],  "weight": 3000 }
    ])");
    REQUIRE(suspicion.validate(pinnedRows, "suspicion.json").empty());

    const Json brokenRow = parseOrThrow(R"([
        { "act": "CROUCH", "masks": ["SETTLEMENT", "MARKET"], "weight": 2000 },
        { "act": "FLEE",   "masks": ["SETTLEMENT"],           "weight": 3000, "extra": 1 }
    ])");
    const std::vector<DataError> errors = suspicion.validate(brokenRow, "suspicion.json");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].jsonPointer == "/1/extra");

    // A non-array root is reported at the document pointer.
    const Json notArray = parseOrThrow(R"({"act": "CROUCH"})");
    const std::vector<DataError> rootErrors = suspicion.validate(notArray, "suspicion.json");
    REQUIRE(rootErrors.size() == 1);
    CHECK(rootErrors[0].jsonPointer.empty());
}

TEST_CASE("TestDataMerge_ScalarAndNull_ReplaceWholesale")
{
    using namespace harrowmere::data;

    const Json base = parseOrThrow(R"({"a": [1, 2], "b": {"x": 1}, "c": 3})");

    // An override array replaces a base object wholesale (and vice versa) -
    // there is no type-coercing merge, only object deep-merge.
    const Json arrayWithScalar = mergeData(base, parseOrThrow(R"({"b": 9})"));
    CHECK(arrayWithScalar["b"] == Json(9));

    // A null override is a scalar: it replaces (a later schema check is what
    // rejects a nulled required field - the merge itself has no delete
    // semantics to invent).
    const Json nulled = mergeData(base, parseOrThrow(R"({"c": null})"));
    CHECK(nulled["c"] == Json(nullptr));

    // An override object replaces a base array.
    const Json objectOverArray = mergeData(base, parseOrThrow(R"({"a": {"x": 5}})"));
    CHECK(objectOverArray["a"]["x"] == Json(5));
}

TEST_CASE("TestDataConfig_MissingKeysFallBackToPinnedDefaults")
{
    using namespace harrowmere::data;

    // A source with nothing set: every pinned default applies (the exact
    // values mods/harrowmere_core/config.toml ships, task 0.1).
    class EmptySource final : public ConfigSource
    {
    public:
        std::optional<std::int64_t> integer(const char *, const char *) const override
        {
            return std::nullopt;
        }
        std::optional<std::string> string(const char *, const char *) const override
        {
            return std::nullopt;
        }
    };

    const EmptySource empty;
    const GameConfig config = GameConfig::load(empty);
    CHECK(config.tickRate() == 10);            // D-TICK-1
    CHECK(config.timeScale() == 60);           // D-TIME-1
    CHECK(config.autosaveGameHours() == 6);    // task 0.1 config.toml
    CHECK(config.seed() == 0);                 // 0 = random, persisted by 7.1
    CHECK(config.modDirectory() == "mods");    // task 0.4 pinned fallback

    // Values present are read through the [harrowmere] / [engine] sections;
    // garbage (negative or oversized) falls back rather than reaching the sim.
    class StagedSource final : public ConfigSource
    {
    public:
        std::optional<std::int64_t> integer(const char *section, const char *key) const override
        {
            if (std::string(section) == "harrowmere" && std::string(key) == "tick_rate")
            {
                return std::int64_t{20};
            }
            if (std::string(section) == "harrowmere" && std::string(key) == "time_scale")
            {
                return std::int64_t{-5}; // garbage -> default
            }
            if (std::string(section) == "harrowmere" && std::string(key) == "seed")
            {
                return std::int64_t{123456789};
            }
            return std::nullopt;
        }
        std::optional<std::string> string(const char *section, const char *key) const override
        {
            if (std::string(section) == "engine" && std::string(key) == "mod_directory")
            {
                return std::string{"content_mods"};
            }
            return std::nullopt;
        }
    };

    const StagedSource staged;
    const GameConfig overridden = GameConfig::load(staged);
    CHECK(overridden.tickRate() == 20);
    CHECK(overridden.timeScale() == 60); // garbage rejected, default kept
    CHECK(overridden.seed() == 123456789);
    CHECK(overridden.modDirectory() == "content_mods");
}

TEST_CASE("TestDataConfig_DeclaredKeyGate_AndDataDirectoryResolution")
{
    using namespace harrowmere::data;

    // The gate: exactly the four keys mod.toml declares (task 0.1), nothing
    // else - "harrowmere.billing" must be a programming error, and free-to-
    // play means billing does not exist (common.md §2.2).
    CHECK(isDeclaredConfigKey("harrowmere.tick_rate"));
    CHECK(isDeclaredConfigKey("harrowmere.time_scale"));
    CHECK(isDeclaredConfigKey("harrowmere.autosave_game_hours"));
    CHECK(isDeclaredConfigKey("harrowmere.seed"));
    CHECK_FALSE(isDeclaredConfigKey("harrowmere.billing"));
    CHECK_FALSE(isDeclaredConfigKey(""));
    CHECK_FALSE(isDeclaredConfigKey("engine.mod_directory")); // engine-owned, not ours

    // Base content directory: <exe_dir>/<mod_directory>/harrowmere_core/data
    // (task 0.4 pinned decision - the runtime layout puts packaged data there).
    const GameConfig config =
        GameConfig::withValues(10, 60, 6, 0, "mods");
    CHECK(config.coreDataDirectory("/opt/harrowmere/runtime") ==
          "/opt/harrowmere/runtime/mods/harrowmere_core/data");

    const GameConfig relocated =
        GameConfig::withValues(10, 60, 6, 0, "content_mods");
    CHECK(relocated.coreDataDirectory("/opt/harrowmere/runtime") ==
          "/opt/harrowmere/runtime/content_mods/harrowmere_core/data");
}
