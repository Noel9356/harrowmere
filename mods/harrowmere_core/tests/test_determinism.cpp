// tests/test_determinism.cpp — task 8.1's pinned battery: machine-checkable
// determinism. Same seed + same command script → identical canonical state
// hashes (the 8.1 hasher over the persisted stores), sampled BETWEEN full
// ticks — the card pitfall pins the sample point after step 12 (the power
// flush): a mid-tick hash would see transient dirty caches and flake, so
// every hash here reads the stores only between runTick() calls.
//
//   TestDet_WorldgenHashStable    — 1.5's stage-7 summary hash reproduces
//                                   across two generations of one seed, the
//                                   population plan and spawn agree, and the
//                                   full persisted store set hashes
//                                   identically at tick 0.
//   TestDet_TickScriptHashStable  — world(seed S) → N=2000 ticks under a
//                                   fixed scripted intent log (the 3.10
//                                   SimCommand vocabulary, injected FIFO into
//                                   the drain queue) → identical hashes at
//                                   the halfway mark and at the end, across
//                                   two independent in-process runs.
//   TestDet_DifferentSeedDifferentHash — a different seed produces a
//                                   different summary hash AND a different
//                                   canonical state hash (the harness must be
//                                   able to fail: any R6/R7 violation on
//                                   main trips here first, within one run —
//                                   the card's Done-when).
//
// The worlds build through the 8.3 soak harness (tests/soak_main.cpp,
// compiled into this binary as a library) — the exact 14-step pipeline the
// runtime assembles, no renderer, R8-clean (no wall time inside the sim; the
// script is a pure function of the tick index).

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <doctest/doctest.h>

#include "bridge/harrowmere_capability.hpp" // the SimCommand vocabulary (3.10)
#include "character/character.hpp"

#include "soak_main.cpp" // the harness (HARROWMERE_SOAK_NO_MAIN is set for this target)

namespace
{

using namespace harrowmere;

// The pinned test data directory (tests/CMakeLists.txt bakes it).
std::filesystem::path detDataDir()
{
#ifdef HARROWMERE_CORE_DATA_DIR
    return std::filesystem::path(HARROWMERE_CORE_DATA_DIR);
#else
    return std::filesystem::path("mods") / "harrowmere_core" / "data";
#endif
}

// The pinned determinism seed (arbitrary but fixed; worldgen, the population
// plan and the scripted run all key off it).
constexpr std::uint64_t kDetSeed = 20261102ULL;

// A fresh world on `seed` (content + wiring + population); null on failure
// (the reason goes to stderr — the caller REQUIREs).
std::unique_ptr<soak::SoakRuntime> detWorld(
    std::uint64_t seed
)
{
    auto rt = std::make_unique<soak::SoakRuntime>();
    std::string error;
    if (!soak::newGame(*rt, seed, detDataDir().string(), error))
    {
        std::fprintf(stderr, "det newGame(%llu) failed: %s\n", static_cast<unsigned long long>(seed), error.c_str());
        return nullptr;
    }
    return rt;
}

// The fixed scripted intent log: a pure function of the tick index. Every
// 50th tick one intent — an Emote toggle every 400th (CROUCH/MASK alternate),
// otherwise a Move intent cycling through eight compass directions (sprint on
// the fifth slot). The same sequence feeds every run; no clock, no RNG of its
// own (R8).
SimCommand scriptedIntent(
    std::uint64_t tick
)
{
    static constexpr double kDirs[8][2]
        = {{1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}, {-1.0, 1.0}, {-1.0, 0.0}, {-1.0, -1.0}, {0.0, -1.0}, {1.0, -1.0}};

    if (tick % 400 == 0)
    {
        SimCommand cmd;
        cmd.kind      = SimCommand::Kind::Emote;
        cmd.emote.act = (tick % 800 == 0) ? EmoteIntent::Act::CROUCH : EmoteIntent::Act::MASK;
        return cmd;
    }

    const std::size_t slot = static_cast<std::size_t>((tick / 50) % 8);
    SimCommand cmd;
    cmd.kind        = SimCommand::Kind::Move;
    cmd.move.dirX   = kDirs[slot][0];
    cmd.move.dirZ   = kDirs[slot][1];
    cmd.move.sprint = (slot == 4);
    return cmd;
}

// Runs `ticks` ticks of the scripted log: the intent for tick i is pushed
// just before tick i, so step 9 drains it the same tick in every run (the
// pipeline order is the script's order — the drain is FIFO).
void runScripted(
    soak::SoakRuntime &rt, std::uint64_t ticks
)
{
    for (std::uint64_t i = 1; i <= ticks; ++i)
    {
        if (i % 50 == 0)
        {
            rt.intentQueue.push(scriptedIntent(i));
        }
        soak::runTick(rt);
    }
}

} // namespace

// --------------------------------------------------------------------------
// TestDet_WorldgenHashStable — 1.5 stage 7: the same seed generates the same
// world. Two independent generations agree on the stage-7 summary hash, the
// population plan, the spawn, and — through the 8.1 hasher — the entire
// persisted store set at tick 0.
TEST_CASE(
    "TestDet_WorldgenHashStable"
)
{
    std::unique_ptr<soak::SoakRuntime> first  = detWorld(kDetSeed);
    std::unique_ptr<soak::SoakRuntime> second = detWorld(kDetSeed);
    REQUIRE_MESSAGE(first != nullptr, "the first generation failed (details on stderr)");
    REQUIRE_MESSAGE(second != nullptr, "the second generation failed (details on stderr)");

    CHECK_MESSAGE(
        first->genResult.summaryHash == second->genResult.summaryHash,
        "the same seed produced different stage-7 worldgen summary hashes"
    );
    CHECK_EQ(first->genResult.nextCharacterId, second->genResult.nextCharacterId);
    REQUIRE(first->npcPlan.size() == second->npcPlan.size());
    CHECK(first->npcPlan == second->npcPlan);
    CHECK_EQ(first->genResult.playerSpawn.x, second->genResult.playerSpawn.x);
    CHECK_EQ(first->genResult.playerSpawn.y, second->genResult.playerSpawn.y);
    CHECK_EQ(first->genResult.playerSpawn.z, second->genResult.playerSpawn.z);

    // The canonical hasher over the whole store set agrees at tick 0.
    CHECK_MESSAGE(
        soak::stateHash(*first) == soak::stateHash(*second),
        "the same seed hashed to different canonical state hashes at tick 0"
    );
}

// --------------------------------------------------------------------------
// TestDet_TickScriptHashStable — the card's harness shape: generate the world
// once per run from the pinned seed, run N=2000 ticks under the fixed
// scripted intent log, hash. Two in-process runs must land on the same hash
// at the halfway mark and at the end (both samples BETWEEN full ticks — after
// step 12, never mid-tick).
TEST_CASE(
    "TestDet_TickScriptHashStable"
)
{
    std::unique_ptr<soak::SoakRuntime> runA = detWorld(kDetSeed);
    std::unique_ptr<soak::SoakRuntime> runB = detWorld(kDetSeed);
    REQUIRE_MESSAGE(runA != nullptr, "run A's generation failed (details on stderr)");
    REQUIRE_MESSAGE(runB != nullptr, "run B's generation failed (details on stderr)");

    runScripted(*runA, 1000);
    runScripted(*runB, 1000);
    const std::uint64_t midA = soak::stateHash(*runA);
    const std::uint64_t midB = soak::stateHash(*runB);
    CHECK_MESSAGE(
        midA == midB, "the halfway hashes diverged — the tick pipeline is not seed-deterministic under the scripted log"
    );

    runScripted(*runA, 1000);
    runScripted(*runB, 1000);
    const std::uint64_t endA = soak::stateHash(*runA);
    const std::uint64_t endB = soak::stateHash(*runB);
    CHECK_MESSAGE(
        endA == endB,
        "the 2000-tick hashes diverged — the tick pipeline is not seed-deterministic under the scripted log"
    );

    // The script actually moved the world (the harness can fail): 2000 ticks
    // of scripted traffic must not land on the tick-0 state.
    std::unique_ptr<soak::SoakRuntime> pristine = detWorld(kDetSeed);
    REQUIRE_MESSAGE(pristine != nullptr, "the pristine generation failed (details on stderr)");
    CHECK_MESSAGE(
        endA != soak::stateHash(*pristine),
        "2000 scripted ticks left the canonical state unchanged — the harness would never detect a violation"
    );
}

// --------------------------------------------------------------------------
// TestDet_DifferentSeedDifferentHash — the harness must be able to fail. A
// different seed generates a different summary hash and a different canonical
// state hash (sampled at tick 0 — worldgen alone already separates them).
TEST_CASE(
    "TestDet_DifferentSeedDifferentHash"
)
{
    std::unique_ptr<soak::SoakRuntime> first  = detWorld(kDetSeed);
    std::unique_ptr<soak::SoakRuntime> second = detWorld(kDetSeed + 1);
    REQUIRE_MESSAGE(first != nullptr, "the first generation failed (details on stderr)");
    REQUIRE_MESSAGE(second != nullptr, "the second generation failed (details on stderr)");

    CHECK_MESSAGE(
        first->genResult.summaryHash != second->genResult.summaryHash,
        "two different seeds produced identical worldgen summary hashes"
    );
    CHECK_MESSAGE(
        soak::stateHash(*first) != soak::stateHash(*second), "two different seeds hashed identically at tick 0"
    );
}
