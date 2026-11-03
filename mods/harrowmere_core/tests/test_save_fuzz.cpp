// tests/test_save_fuzz.cpp — task 8.2's pinned battery: roundtrips survive
// chaos.
//
//   TestRoundtrip_MilestonePreJustice / MidJustice / EventActive / Wartime
//       — the card's four fixture-world milestones (pre-justice, mid-justice,
//       event-active, wartime), each saved at its milestone and loaded into a
//       fresh runtime over the REAL pipeline (the 8.3 soak harness): the
//       loaded world's canonical state hash equals the saved world's.
//   TestRoundtrip_DoubleRoundtripStable — the card's property,
//       hash(save(load(save(W)))) == hash(save(W)): per milestone, plus the
//       byte-identical re-save (R7's deterministic serialization payoff).
//   TestFuzz_TruncatedRefusedCleanly — every sampled truncation of a valid
//       save refuses cleanly: the file + the reason come back, and not one
//       store row is applied (no partial load — the 8.2 pitfall: refusing is
//       the only correct answer to a corrupt file).
//   TestFuzz_FlippedByteRefusedOrAcceptedDeterministically — single-byte
//       flips over non-checksum fields (a fixed stride plus targeted
//       structural bytes: the format tag, the version digit, the seed
//       digits): the outcome — refuse, or accept as a legitimate world — and
//       the resulting state are identical across independent loads. Per the
//       card pitfall, "accepted deterministically" means the REFUSAL is
//       deterministic; a corrupted-but-parseable file that still validates
//       (a flipped name byte) is a well-formed world, never half-applied.
//
// The wartime fixture arms through the real declareWar path on its pinned
// seed (the first ordered faction pair whose declaration validates); the
// fuzz parent is the mid-justice world's save. The corpus stays inside the
// card's < 30 s CI budget: sampled cut points and offsets, every attempt
// into a fresh runtime.

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "character/character.hpp"
#include "combat/attacks.hpp" // AttackRequest — the input layer's combat seam
#include "factions/factions.hpp"
#include "factions/war.hpp"
#include "events/engine.hpp"

#include "soak_main.cpp" // the harness (HARROWMERE_SOAK_NO_MAIN is set for this target)

namespace
{

using namespace harrowmere;

// The pinned test data directory (tests/CMakeLists.txt bakes it).
std::filesystem::path fuzzDataDir()
{
#ifdef HARROWMERE_CORE_DATA_DIR
    return std::filesystem::path(HARROWMERE_CORE_DATA_DIR);
#else
    return std::filesystem::path("mods") / "harrowmere_core" / "data";
#endif
}

// A per-test scratch directory (each case gets its own; cleaned on exit).
std::filesystem::path fuzzScratchDir(
    const char *tag
)
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("harrowmere_test_" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

// The fixture seeds (pinned): the justice + war fixture world and the
// event-active world (whose event schedule spawns within the cap window).
constexpr std::uint64_t kFuzzSeed  = 868686868ULL;
constexpr std::uint64_t kEventSeed = 20261102ULL;
// The war seed: its day-1 world has two factions holding region-adjacent
// territories — a BorderDispute that validates (the pinned declaration).
constexpr std::uint64_t kWarSeed = 15ULL;

// A fresh world on `seed` (content + wiring + population); null on failure
// (the reason goes to stderr — the caller REQUIREs).
std::unique_ptr<soak::SoakRuntime> fuzzWorld(
    std::uint64_t seed
)
{
    auto rt = std::make_unique<soak::SoakRuntime>();
    std::string error;
    if (!soak::newGame(*rt, seed, fuzzDataDir().string(), error))
    {
        std::fprintf(stderr, "fuzz newGame(%llu) failed: %s\n", static_cast<unsigned long long>(seed), error.c_str());
        return nullptr;
    }
    return rt;
}

// ---- the four milestone fixtures ---------------------------------------------
//
// Each builds once per process (the suite shares them; every case saves its
// fixture fresh and loads into its own runtime — saving never mutates the
// source world).

// pre-justice: a day-0 world, sampled before any justice sweep has run — the
// justice store holds no evidence and no warrants.
std::unique_ptr<soak::SoakRuntime> preJusticeWorld()
{
    std::unique_ptr<soak::SoakRuntime> rt = fuzzWorld(kFuzzSeed);
    if (rt == nullptr)
    {
        return nullptr;
    }
    soak::runTicks(*rt, 200);
    CHECK(rt->justiceStore.evidenceStore.empty());
    CHECK(rt->justiceStore.warrants.empty());
    return rt;
}

// mid-justice: the player assaults a neighbour through the real combat
// pipeline — the input layer's seam (an AttackRequest pushed straight into
// the CombatQueue; the headless harness has no input layer to translate the
// intent) — and the ASSAULT_THRESHOLD crossing records the crime at commit
// time (4.1's noteAssault accumulator); one more game-day runs the sweep
// machinery over the live case, so the justice store carries evidence.
// Deterministic: the pinned seed, the first living attackable neighbour, the
// combat stream's own rolls. This world is also the fuzz corpus's parent.
std::unique_ptr<soak::SoakRuntime> midJusticeWorld()
{
    std::unique_ptr<soak::SoakRuntime> rt = fuzzWorld(kFuzzSeed);
    if (rt == nullptr)
    {
        return nullptr;
    }

    // The victim: the first living, attackable (non-invulnerable) neighbour —
    // the id-ascending map keeps the pick deterministic.
    characterId victim = 0;
    for (const auto &[id, c] : rt->characters.all())
    {
        if (id == rt->playerCharacterId || c.body.currentHp == 0)
        {
            continue;
        }
        if (!rt->npcs.contains(id) || rt->npcs.get(id).invulnerable)
        {
            continue;
        }
        victim = id;
        break;
    }
    REQUIRE(victim != 0);

    // Fixture-level positioning: the player moves onto the victim's tile
    // (one tick lets step 11 sync the grid), then strikes through the
    // combat seam until the assault records.
    Character &player    = rt->characters.get(rt->playerCharacterId);
    player.body.position = rt->characters.get(victim).body.position;
    rt->player.position  = player.body.position;
    soak::runTicks(*rt, 1);

    for (int strike = 0; strike < 20 && rt->justiceStore.evidenceStore.empty(); ++strike)
    {
        AttackRequest strikeRequest;
        strikeRequest.attacker = rt->playerCharacterId;
        strikeRequest.target   = victim;
        rt->combatQueue.push(strikeRequest);
        soak::runTicks(*rt, 1);
    }
    REQUIRE_MESSAGE(
        !rt->justiceStore.evidenceStore.empty(), "the armed assault produced no evidence within 20 strikes"
    );

    soak::runGameDays(*rt, 1); // the sweep machinery over the live case
    return rt;
}

// event-active: the world runs until the event engine holds a live event.
std::unique_ptr<soak::SoakRuntime> eventActiveWorld()
{
    std::unique_ptr<soak::SoakRuntime> rt = fuzzWorld(kEventSeed);
    if (rt == nullptr)
    {
        return nullptr;
    }
    for (std::uint64_t day = 0; day < 32 && rt->eventEngine.activeCount() == 0; ++day)
    {
        soak::runGameDays(*rt, 1);
    }
    REQUIRE_MESSAGE(rt->eventEngine.activeCount() > 0, "the pinned seed produced no live event within 32 game-days");
    return rt;
}

// wartime: a 1-game-day world where the first ordered faction pair holding
// region-adjacent territories arms a real war through WarStore::declareWar
// (6.2's declaration rules, the BorderDispute casus belli; the cb IN-OUT —
// the store keeps the kind that validated).
std::unique_ptr<soak::SoakRuntime> wartimeWorld()
{
    std::unique_ptr<soak::SoakRuntime> rt = fuzzWorld(kWarSeed);
    if (rt == nullptr)
    {
        return nullptr;
    }
    soak::runGameDays(*rt, 1);

    bool armed = false;
    for (const auto &[aId, a] : rt->factionStore.all())
    {
        (void)a;
        for (const auto &[bId, b] : rt->factionStore.all())
        {
            (void)b;
            if (aId == bId)
            {
                continue;
            }
            factions::CasusBelli cb;
            cb.kind = factions::CasusBelliKind::BorderDispute;
            if (rt->warStore.declareWar(aId, bId, cb, rt->simCtx).isOk())
            {
                armed = true;
                break;
            }
        }
        if (armed)
        {
            break;
        }
    }
    REQUIRE_MESSAGE(armed, "the pinned seed produced no declarable BorderDispute after 1 game-day");
    CHECK_FALSE(rt->warStore.all().empty());
    return rt;
}

// The shared fixture bundle (built once per process, in a fixed order).
struct MilestoneFixtures
{
    std::unique_ptr<soak::SoakRuntime> preJustice;
    std::unique_ptr<soak::SoakRuntime> midJustice;
    std::unique_ptr<soak::SoakRuntime> eventActive;
    std::unique_ptr<soak::SoakRuntime> wartime;
    std::string midJusticeSave; // the fuzz parent's exact bytes

    std::uint32_t parentPopulation = 0;

    static const MilestoneFixtures &get()
    {
        static MilestoneFixtures fixtures = [] {
            MilestoneFixtures built;
            built.preJustice  = preJusticeWorld();
            built.midJustice  = midJusticeWorld();
            built.eventActive = eventActiveWorld();
            built.wartime     = wartimeWorld();

            // The fuzz parent: the mid-justice world on disk.
            std::string error;
            const std::string path = (fuzzScratchDir("fuzz_parent") / "parent.json").string();
            REQUIRE_MESSAGE(built.midJustice != nullptr, "the mid-justice fixture failed");
            REQUIRE_MESSAGE(
                soak::saveToFile(*built.midJustice, path, error),
                (std::string("the fuzz parent save failed: ") + error).c_str()
            );
            {
                std::ifstream in(path, std::ios::binary);
                built.midJusticeSave.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            built.parentPopulation = built.midJustice->characters.population();
            return built;
        }();
        return fixtures;
    }
};

// The file's bytes as one string (the byte-compare helper).
std::string fuzzReadFileText(
    const std::string &path
)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Saves `world` to <scratch>/<tag>.json and loads it into a fresh runtime;
// returns the loaded runtime (null on any failure, reason on stderr) — the
// milestone roundtrip's shared half.
std::unique_ptr<soak::SoakRuntime> saveAndLoad(
    const soak::SoakRuntime &world, const char *tag, const char *file
)
{
    const std::string savePath = (fuzzScratchDir(tag) / std::string(file)).string();
    std::string error;
    if (!soak::saveToFile(world, savePath, error))
    {
        std::fprintf(stderr, "save failed: %s\n", error.c_str());
        return nullptr;
    }

    auto loaded = std::make_unique<soak::SoakRuntime>();
    if (!soak::loadGame(*loaded, savePath, fuzzDataDir().string(), error))
    {
        std::fprintf(stderr, "load failed: %s\n", error.c_str());
        return nullptr;
    }
    return loaded;
}

// Writes `bytes` to `path`, then attempts one load into a fresh runtime via
// the soak loadGame door. Returns whether the load was accepted; on
// acceptance `populationOut` carries the loaded row count; on refusal
// `errorOut` carries the file-stamped refusal reason.
bool fuzzLoadAttempt(
    const std::string &path, const std::string &bytes, std::uint32_t &populationOut, std::string &errorOut
)
{
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << bytes;
    }

    auto loader = std::make_unique<soak::SoakRuntime>();
    std::string error;
    // Nothing applied yet: a refusal must leave the loader exactly here.
    const std::uint32_t populationBefore = loader->characters.population();
    const std::uint64_t worldTimeBefore  = loader->clock.worldTime;

    const bool accepted = soak::loadGame(*loader, path, fuzzDataDir().string(), errorOut);
    if (accepted)
    {
        populationOut = loader->characters.population();
    } else
    {
        // No partial load: the refusal touched nothing (the 8.2 pitfall —
        // accepting a corrupted-but-parseable file silently is worse than
        // refusing; half-applying is worse than both).
        CHECK_EQ(loader->characters.population(), populationBefore);
        CHECK_EQ(loader->clock.worldTime, worldTimeBefore);
        CHECK_FALSE(errorOut.empty());
    }
    (void)worldTimeBefore;
    return accepted;
}

} // namespace

// --------------------------------------------------------------------------
// The four milestone roundtrips — save at the milestone, load into a fresh
// runtime, identical canonical state hash.
TEST_CASE(
    "TestRoundtrip_MilestonePreJustice"
)
{
    const MilestoneFixtures &fixtures = MilestoneFixtures::get();
    REQUIRE(fixtures.preJustice != nullptr);
    std::unique_ptr<soak::SoakRuntime> loaded = saveAndLoad(*fixtures.preJustice, "rt_pre_justice", "world.json");
    REQUIRE_MESSAGE(loaded != nullptr, "the pre-justice roundtrip failed (details on stderr)");
    CHECK_MESSAGE(
        soak::stateHash(*loaded) == soak::stateHash(*fixtures.preJustice),
        "the loaded pre-justice world's canonical state hash differs"
    );
}

TEST_CASE(
    "TestRoundtrip_MilestoneMidJustice"
)
{
    const MilestoneFixtures &fixtures = MilestoneFixtures::get();
    REQUIRE(fixtures.midJustice != nullptr);
    std::unique_ptr<soak::SoakRuntime> loaded = saveAndLoad(*fixtures.midJustice, "rt_mid_justice", "world.json");
    REQUIRE_MESSAGE(loaded != nullptr, "the mid-justice roundtrip failed (details on stderr)");
    CHECK_MESSAGE(
        soak::stateHash(*loaded) == soak::stateHash(*fixtures.midJustice),
        "the loaded mid-justice world's canonical state hash differs"
    );
}

TEST_CASE(
    "TestRoundtrip_MilestoneEventActive"
)
{
    const MilestoneFixtures &fixtures = MilestoneFixtures::get();
    REQUIRE(fixtures.eventActive != nullptr);
    std::unique_ptr<soak::SoakRuntime> loaded = saveAndLoad(*fixtures.eventActive, "rt_event_active", "world.json");
    REQUIRE_MESSAGE(loaded != nullptr, "the event-active roundtrip failed (details on stderr)");
    CHECK_MESSAGE(
        soak::stateHash(*loaded) == soak::stateHash(*fixtures.eventActive),
        "the loaded event-active world's canonical state hash differs"
    );
}

TEST_CASE(
    "TestRoundtrip_MilestoneWartime"
)
{
    const MilestoneFixtures &fixtures = MilestoneFixtures::get();
    REQUIRE(fixtures.wartime != nullptr);
    std::unique_ptr<soak::SoakRuntime> loaded = saveAndLoad(*fixtures.wartime, "rt_wartime", "world.json");
    REQUIRE_MESSAGE(loaded != nullptr, "the wartime roundtrip failed (details on stderr)");
    CHECK_MESSAGE(
        soak::stateHash(*loaded) == soak::stateHash(*fixtures.wartime),
        "the loaded wartime world's canonical state hash differs"
    );
}

// --------------------------------------------------------------------------
// TestRoundtrip_DoubleRoundtripStable — the card's property:
// hash(save(load(save(W)))) == hash(save(W)) — per milestone, with the
// re-save byte-identical to the first save of the loaded world.
TEST_CASE(
    "TestRoundtrip_DoubleRoundtripStable"
)
{
    const MilestoneFixtures &fixtures = MilestoneFixtures::get();
    const std::initializer_list<std::pair<const char *, const soak::SoakRuntime *>> milestones
        = {{"dbl_pre_justice", fixtures.preJustice.get()},
            {"dbl_mid_justice", fixtures.midJustice.get()},
            {"dbl_event_active", fixtures.eventActive.get()},
            {"dbl_wartime", fixtures.wartime.get()}};

    for (const auto &[tag, world] : milestones)
    {
        REQUIRE(world != nullptr);
        CAPTURE(tag);

        const std::string firstPath  = (fuzzScratchDir(tag) / "first.json").string();
        const std::string secondPath = (fuzzScratchDir(tag) / "second.json").string();
        const std::string thirdPath  = (fuzzScratchDir(tag) / "third.json").string();
        std::string error;

        REQUIRE_MESSAGE(
            soak::saveToFile(*world, firstPath, error), (std::string(tag) + ": the first save failed: " + error).c_str()
        );

        auto loadedOnce = std::make_unique<soak::SoakRuntime>();
        REQUIRE_MESSAGE(
            soak::loadGame(*loadedOnce, firstPath, fuzzDataDir().string(), error),
            (std::string(tag) + ": the first load failed: " + error).c_str()
        );
        CHECK_MESSAGE(
            soak::stateHash(*loadedOnce) == soak::stateHash(*world),
            (std::string(tag) + ": the first roundtrip's hash differs").c_str()
        );

        REQUIRE_MESSAGE(
            soak::saveToFile(*loadedOnce, secondPath, error),
            (std::string(tag) + ": the second save failed: " + error).c_str()
        );

        auto loadedTwice = std::make_unique<soak::SoakRuntime>();
        REQUIRE_MESSAGE(
            soak::loadGame(*loadedTwice, secondPath, fuzzDataDir().string(), error),
            (std::string(tag) + ": the second load failed: " + error).c_str()
        );

        REQUIRE_MESSAGE(
            soak::saveToFile(*loadedTwice, thirdPath, error),
            (std::string(tag) + ": the third save failed: " + error).c_str()
        );

        // hash(save(load(save(W)))) == hash(save(W)), and the bytes agree.
        CHECK_MESSAGE(
            soak::stateHash(*loadedTwice) == soak::stateHash(*world),
            (std::string(tag) + ": the double roundtrip's hash differs").c_str()
        );
        CHECK_MESSAGE(
            fuzzReadFileText(secondPath) == fuzzReadFileText(thirdPath),
            (std::string(tag) + ": the re-saves are not byte-identical").c_str()
        );
    }
}

// --------------------------------------------------------------------------
// TestFuzz_TruncatedRefusedCleanly — every sampled truncation of the valid
// parent save refuses cleanly: the file + reason come back, and the loader's
// stores are untouched.
TEST_CASE(
    "TestFuzz_TruncatedRefusedCleanly"
)
{
    const MilestoneFixtures &fixtures = MilestoneFixtures::get();
    const std::string &parent         = fixtures.midJusticeSave;
    REQUIRE(parent.size() > 1024);

    // Sampled cut points: the JSON head (structure), a fixed stride through
    // the body, and the closing bytes. None may land on the full length —
    // the untruncated file is the one save that must load.
    std::vector<std::size_t> cuts;
    cuts.push_back(0);
    cuts.push_back(1);
    cuts.push_back(2);
    cuts.push_back(3);
    const std::size_t stride = parent.size() / 8 + 1;
    for (std::size_t cut = stride; cut < parent.size(); cut += stride)
    {
        cuts.push_back(cut);
    }
    cuts.push_back(parent.size() - 1);

    const std::filesystem::path dir = fuzzScratchDir("fuzz_trunc");
    for (const std::size_t cut : cuts)
    {
        CAPTURE(cut);
        const std::string path   = (dir / ("cut_" + std::to_string(cut) + ".json")).string();
        std::uint32_t population = 0;
        std::string error;
        const bool accepted = fuzzLoadAttempt(path, parent.substr(0, cut), population, error);
        CHECK_MESSAGE(
            !accepted, (std::string("a truncation at byte ") + std::to_string(cut) + " was accepted").c_str()
        );
        CHECK_MESSAGE(error.find(path) == 0, (std::string("the refusal did not name the file: ") + error).c_str());
    }
}

// --------------------------------------------------------------------------
// TestFuzz_FlippedByteRefusedOrAcceptedDeterministically — single-byte flips
// (a fixed stride over the body plus targeted structural bytes): the outcome
// is deterministic across two independent loads, and an accepted flip loads
// a well-formed world (row count intact, refusal rules untouched).
TEST_CASE(
    "TestFuzz_FlippedByteRefusedOrAcceptedDeterministically"
)
{
    const MilestoneFixtures &fixtures = MilestoneFixtures::get();
    const std::string &parent         = fixtures.midJusticeSave;
    REQUIRE(parent.size() > 1024);

    // The sampled offsets (the probe's fixed stride) plus three targeted
    // structural bytes: inside the format tag, the version digit, and the
    // seed digits — the header fields every load validates or applies first.
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < 12; ++i)
    {
        offsets.push_back(static_cast<std::size_t>((static_cast<std::uint64_t>(i) * 997ULL + 7ULL) % parent.size()));
    }
    const std::size_t formatPos = parent.find("\"game-save\"");
    REQUIRE_MESSAGE(formatPos != std::string::npos, "the parent save has no format tag");
    offsets.push_back(formatPos + 1);
    const std::size_t versionPos = parent.find("\"version\"");
    REQUIRE_MESSAGE(versionPos != std::string::npos, "the parent save has no version field");
    offsets.push_back(versionPos + 11); // the digit inside the version number
    const std::size_t seedPos = parent.find("\"seed\"");
    REQUIRE_MESSAGE(seedPos != std::string::npos, "the parent save has no seed field");
    offsets.push_back(seedPos + 8); // a digit inside the seed

    const std::filesystem::path dir = fuzzScratchDir("fuzz_flips");
    for (const std::size_t offset : offsets)
    {
        REQUIRE(offset < parent.size());
        CAPTURE(offset);

        std::string bytes = parent;
        bytes[offset]     = static_cast<char>(bytes[offset] ^ 0x5A);

        // Two independent loads of the same corrupted bytes.
        const std::string firstPath    = (dir / ("flip_" + std::to_string(offset) + "_a.json")).string();
        const std::string secondPath   = (dir / ("flip_" + std::to_string(offset) + "_b.json")).string();
        std::uint32_t firstPopulation  = 0;
        std::uint32_t secondPopulation = 0;
        std::string firstError;
        std::string secondError;
        const bool firstAccepted  = fuzzLoadAttempt(firstPath, bytes, firstPopulation, firstError);
        const bool secondAccepted = fuzzLoadAttempt(secondPath, bytes, secondPopulation, secondError);

        CHECK_MESSAGE(firstAccepted == secondAccepted, "the same flip produced different outcomes across two loads");
        if (firstAccepted && secondAccepted)
        {
            // An accepted flip is a legitimate world: the row count is
            // intact, and the two loads agree on it.
            CHECK_EQ(firstPopulation, secondPopulation);
            CHECK_EQ(firstPopulation, fixtures.parentPopulation);
        } else
        {
            CHECK_MESSAGE(
                firstError.find(firstPath) == 0,
                (std::string("the refusal did not name the file: ") + firstError).c_str()
            );
        }
    }
}
