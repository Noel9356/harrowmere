// tests/test_save_load.cpp — task 7.1's pinned battery: the roundtrip hash
// on a 30-game-day soak world, the dirty-flag contract, the anti-abuse
// fields' bit-exactness, the RNG continuation, the atomic tmp behavior and
// the version refusal.
//
// The worlds build through the 8.3 soak harness (tests/soak_main.cpp,
// compiled into this binary as a library): the save/load paths are exercised
// end to end over the REAL pipeline, not a fixture stub.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <doctest/doctest.h>

#include "character/character.hpp"
#include "core/clock.hpp"
#include "core/rng.hpp"
#include "persist/save.hpp"
#include "persist/state_hash.hpp"
#include "progression/power.hpp"

#include "soak_main.cpp" // the harness (HARROWMERE_SOAK_NO_MAIN is set for this target)

namespace
{

using namespace harrowmere;

// The pinned test data directory (tests/CMakeLists.txt bakes it).
std::filesystem::path testDataDir()
{
#ifdef HARROWMERE_CORE_DATA_DIR
    return std::filesystem::path(HARROWMERE_CORE_DATA_DIR);
#else
    return std::filesystem::path("mods") / "harrowmere_core" / "data";
#endif
}

// A per-test scratch directory (each case gets its own; cleaned on exit).
// Any stale content from an earlier run is wiped first: the atomic-tmp case
// asserts "no save exists yet", which only holds on a fresh directory.
std::filesystem::path scratchDir(
    const char *tag
)
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("harrowmere_test_" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

// The 30-game-day soak world (the card's Done-when: "Roundtrip hash test
// green on a 30-game-day soak world"). Built ONCE per process — the suite
// shares it; each case saves it fresh and loads into its own runtime.
struct SharedSoakWorld
{
    std::unique_ptr<soak::SoakRuntime> runtime;
    std::uint64_t preHash = 0;

    static const SharedSoakWorld &get()
    {
        static SharedSoakWorld world = [] {
            SharedSoakWorld built;
            built.runtime = std::make_unique<soak::SoakRuntime>();
            std::string error;
            const std::uint64_t seed       = 987654321ULL;
            const std::string newGameError = "soak newGame failed: " + error;
            REQUIRE_MESSAGE(soak::newGame(*built.runtime, seed, testDataDir().string(), error), newGameError.c_str());
            soak::runGameDays(*built.runtime, 30); // 30 game-days = 432'000 ticks
            built.preHash = soak::stateHash(*built.runtime);
            return built;
        }();
        return world;
    }
};

// The file's bytes as one string (the byte-compare helper).
std::string readFileText(
    const std::string &path
)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Loads `path` into a fresh runtime (content + wiring + the save).
std::unique_ptr<soak::SoakRuntime> loadRuntimeFrom(
    const std::string &path, std::string &error
)
{
    auto rt = std::make_unique<soak::SoakRuntime>();
    if (!soak::loadGame(*rt, path, testDataDir().string(), error))
    {
        return nullptr;
    }
    return rt;
}

} // namespace

// --------------------------------------------------------------------------
// TestSave_RoundtripHashEqual — canonical state hash before/after (the 8.1
// hasher): save(W) -> load -> hash must equal hash(W) exactly.
TEST_CASE(
    "TestSave_RoundtripHashEqual"
)
{
    const SharedSoakWorld &world    = SharedSoakWorld::get();
    const std::filesystem::path dir = scratchDir("roundtrip");
    const std::string savePath      = (dir / "thirty_days.json").string();

    std::string error;
    REQUIRE(world.preHash != 0);
    REQUIRE(soak::saveToFile(*world.runtime, savePath, error));

    // The save is deterministic: two builds of the same state are
    // byte-identical (R7's payoff).
    const std::string first = readFileText(savePath);

    std::unique_ptr<soak::SoakRuntime> loaded = loadRuntimeFrom(savePath, error);
    if (loaded == nullptr)
    {
        std::fprintf(stderr, "load failed: %s\n", error.c_str());
    }
    REQUIRE_MESSAGE(loaded != nullptr, "load failed (details on stderr)");

    const std::uint64_t postHash = soak::stateHash(*loaded);
    CHECK_MESSAGE(postHash == world.preHash, "the loaded world's canonical state hash differs from the saved world's");

    // The double roundtrip (8.2's property, pinned here too): save the loaded
    // world again — byte-identical file, same hash.
    const std::string secondPath = (dir / "thirty_days_again.json").string();
    REQUIRE(soak::saveToFile(*loaded, secondPath, error));
    CHECK(soak::stateHash(*loaded) == world.preHash);
    const std::string secondText = readFileText(secondPath);
    CHECK(secondText == first);
}

// --------------------------------------------------------------------------
// TestSave_DirtyFlagsSetOnLoad — cachedPower is not saved; every character
// restores powerDirty = true and the load must not leave a stale cache.
TEST_CASE(
    "TestSave_DirtyFlagsSetOnLoad"
)
{
    const SharedSoakWorld &world = SharedSoakWorld::get();
    const std::string savePath   = (scratchDir("dirty") / "world.json").string();

    std::string error;
    // The real pipeline re-dirties continuously (every activity-driven skill
    // XP grant fires markPowerDirty — 2.5's audited call sites), so a clean
    // flag is not "in place by construction" at a 30-game-day sample point.
    // The flush is explicit: compute every character's power once (the flag
    // clears, 2.5's pipeline), THEN save. The load must still restore every
    // flag dirty — the cache is not persisted, no matter how clean the saved
    // world was. (cachedPower/powerDirty are hash-excluded transients, so
    // this does not disturb the shared world's canonical hash.)
    for (auto &[id, c] : world.runtime->characters.all())
    {
        (void)id;
        (void)getCurrentPower(c, world.runtime->registry, world.runtime->itemInstances);
        CHECK_FALSE(c.powerDirty);
    }

    REQUIRE(soak::saveToFile(*world.runtime, savePath, error));

    std::unique_ptr<soak::SoakRuntime> loaded = loadRuntimeFrom(savePath, error);
    if (loaded == nullptr)
    {
        std::fprintf(stderr, "load failed: %s\n", error.c_str());
    }
    REQUIRE_MESSAGE(loaded != nullptr, "load failed (details on stderr)");
    for (const auto &[id, c] : loaded->characters.all())
    {
        (void)id;
        CHECK_MESSAGE(c.powerDirty, "character restored with a clean power cache");
        CHECK_EQ(c.cachedPower, 0ULL); // the cache itself is not persisted
    }
}

// --------------------------------------------------------------------------
// TestSave_PeakPowerAndTotalXpBitExact — the anti-abuse fields survive the
// roundtrip bit-exact (the sandbagging watermark and the lifetime XP).
TEST_CASE(
    "TestSave_PeakPowerAndTotalXpBitExact"
)
{
    soak::SoakRuntime rt;
    std::string error;
    REQUIRE_MESSAGE(soak::newGame(rt, 42ULL, testDataDir().string(), error), "soak newGame failed");

    Character &player          = rt.characters.get(rt.playerCharacterId);
    player.peakPower           = 1234567890123ULL;
    player.progression.totalXp = 9876543210987ULL;
    // A reputation record too (notoriety + lastDecayTime are the same class
    // of anti-abuse field): grant the player one, then roundtrip.
    justice::Reputation &rep = rt.justiceStore.reputations[rt.playerCharacterId];
    rep.notoriety            = 4242;
    rep.lastDecayTime        = 777777;

    soak::runTicks(rt, 200); // some pipeline traffic between the write and the save

    const std::string savePath = (scratchDir("peak") / "world.json").string();
    REQUIRE(soak::saveToFile(rt, savePath, error));

    std::unique_ptr<soak::SoakRuntime> loaded = loadRuntimeFrom(savePath, error);
    if (loaded == nullptr)
    {
        std::fprintf(stderr, "load failed: %s\n", error.c_str());
    }
    REQUIRE_MESSAGE(loaded != nullptr, "load failed (details on stderr)");

    const Character &loadedPlayer = loaded->characters.get(rt.playerCharacterId);
    CHECK_EQ(loadedPlayer.peakPower, player.peakPower);
    CHECK_EQ(loadedPlayer.progression.totalXp, player.progression.totalXp);
    REQUIRE(loaded->justiceStore.reputations.count(rt.playerCharacterId) == 1);
    CHECK_EQ(loaded->justiceStore.reputations.at(rt.playerCharacterId).notoriety, 4242);
    CHECK_EQ(loaded->justiceStore.reputations.at(rt.playerCharacterId).lastDecayTime, 777777ULL);
}

// --------------------------------------------------------------------------
// TestSave_RngContinuationIdentical — a loaded world continues the exact
// random sequence: run A (kept alive) and run B (loaded from A's save) run
// the same N ticks from the save point and land on the same state hash.
TEST_CASE(
    "TestSave_RngContinuationIdentical"
)
{
    // A fresh world: no critical deaths yet, so no pending replacement
    // timers exist at the save point (the scheduler's closure book is code,
    // not data — persist/README.md documents the reading; the continuation
    // window stays inside the first game-day, before any day-boundary birth
    // could draw a fresh NPC name).
    soak::SoakRuntime runA;
    std::string error;
    REQUIRE(soak::newGame(runA, 777ULL, testDataDir().string(), error));
    soak::runTicks(runA, 500); // world state to carry, still day 0

    const std::string savePath = (scratchDir("rng") / "world.json").string();
    REQUIRE(soak::saveToFile(runA, savePath, error));

    std::unique_ptr<soak::SoakRuntime> runB = loadRuntimeFrom(savePath, error);
    REQUIRE_MESSAGE(runB != nullptr, (std::string("load failed: ") + error).c_str());
    CHECK(soak::stateHash(*runB) == soak::stateHash(runA));

    soak::runTicks(runA, 2000);
    soak::runTicks(*runB, 2000);

    CHECK_MESSAGE(
        soak::stateHash(runA) == soak::stateHash(*runB),
        "the loaded world diverged from the kept-alive world over 2000 ticks"
    );
}

// --------------------------------------------------------------------------
// TestSave_AtomicTmpRemovedOnCrash — a crashed writer leaves <path>.tmp
// behind; the next save replaces it and no load ever sees it as the save.
TEST_CASE(
    "TestSave_AtomicTmpRemovedOnCrash"
)
{
    soak::SoakRuntime rt;
    std::string error;
    REQUIRE(soak::newGame(rt, 555ULL, testDataDir().string(), error));

    const std::string savePath = (scratchDir("atomic") / "world.json").string();
    const std::string tmpPath  = savePath + ".tmp";

    // Simulate the kill mid-write: garbage sits at the tmp path, no main file.
    {
        std::ofstream garbage(tmpPath, std::ios::binary | std::ios::trunc);
        garbage << "{\"format\": \"game-save\", \"version\": 1, \"trunc";
    }
    CHECK(persist::saveFileExists(savePath) == false); // the crash left no save
    error.clear();
    persist::LoadTargets missing;
    CHECK_MESSAGE(persist::readSaveFile(savePath, missing, error) == false, "a missing save must refuse");
    CHECK_FALSE(error.empty());

    // The next completed write replaces the tmp (rename over it) and removes
    // it from the filesystem: the tmp never survives a completed call.
    REQUIRE(soak::saveToFile(rt, savePath, error));
    CHECK_FALSE(std::filesystem::exists(tmpPath));
    CHECK(persist::saveFileExists(savePath));

    // And the save loads (the tmp never masqueraded as the save).
    std::unique_ptr<soak::SoakRuntime> loaded = loadRuntimeFrom(savePath, error);
    if (loaded == nullptr)
    {
        std::fprintf(stderr, "load failed: %s\n", error.c_str());
    }
    REQUIRE_MESSAGE(loaded != nullptr, "load failed (details on stderr)");
    CHECK_EQ(loaded->characters.population(), rt.characters.population());
}

// --------------------------------------------------------------------------
// TestLoad_RefusesWrongVersion — the version policy: a mismatch refuses with
// a clear message and touches nothing (no migration in v1).
TEST_CASE(
    "TestLoad_RefusesWrongVersion"
)
{
    const SharedSoakWorld &world = SharedSoakWorld::get();
    const std::string savePath   = (scratchDir("version") / "world.json").string();
    std::string error;
    REQUIRE(soak::saveToFile(*world.runtime, savePath, error));

    // Rewrite the document with version 2 (a future format).
    data::Json doc = data::Json::parse(readFileText(savePath));
    REQUIRE(doc["version"] == persist::SAVE_FORMAT_VERSION);
    doc["version"]           = persist::SAVE_FORMAT_VERSION + 1;
    const std::string v2Path = (scratchDir("version") / "world_v2.json").string();
    {
        std::ofstream out(v2Path, std::ios::binary | std::ios::trunc);
        out << doc.dump();
    }

    // A live set of targets: the refusal must leave every store untouched.
    soak::SoakRuntime rt;
    REQUIRE_MESSAGE(soak::newGame(rt, 1ULL, testDataDir().string(), error), "soak newGame failed");
    const std::uint64_t worldTimeBefore = rt.clock.worldTime;
    const std::size_t populationBefore  = rt.characters.population();

    persist::LoadTargets targets;
    targets.seed          = 0;
    targets.clock         = &rt.clock;
    targets.rng           = &rt.rng;
    targets.characters    = &rt.characters;
    targets.npcs          = &rt.npcs;
    targets.world         = rt.world.get();
    targets.justice       = &rt.justiceStore;
    targets.events        = &rt.eventEngine;
    targets.economy       = &rt.economy;
    targets.factions      = &rt.factionStore;
    targets.wars          = &rt.warStore;
    targets.claims        = &rt.claimStore;
    targets.corpses       = &rt.corpses;
    targets.itemInstances = &rt.itemInstances;
    targets.bags          = &rt.bags;
    targets.appearance    = &rt.appearanceHistory;
    targets.roles         = &rt.roles;
    targets.consents      = &rt.consents;
    targets.combatQueue   = &rt.combatQueue;
    targets.registry      = &rt.registry;

    error.clear();
    const bool accepted = persist::readSaveFile(v2Path, targets, error);
    CHECK_FALSE(accepted);
    CHECK_MESSAGE(error.find("version") != std::string::npos, (std::string("error was: ") + error).c_str());
    CHECK_EQ(rt.clock.worldTime, worldTimeBefore);
    CHECK_EQ(rt.characters.population(), populationBefore);
}
