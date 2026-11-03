// tests/test_soak.cpp — task 8.3's doctest battery. The full-length runs are
// the standalone soak binary's (ctest labels: `soak_nightly_three_game_years`
// nightly, `soak_pr_one_game_year` per PR); these cases prove the harness's
// invariants green over short in-suite windows (a game-month each) so a
// regression trips the ordinary battery, not just the nightly.

#include <cstdio>
#include <string>

#include <doctest/doctest.h>

#include "persist/state_hash.hpp"

#include "soak_main.cpp" // the harness (HARROWMERE_SOAK_NO_MAIN is set for this target)

namespace
{

std::filesystem::path soakDataDir()
{
#ifdef HARROWMERE_CORE_DATA_DIR
    return std::filesystem::path(HARROWMERE_CORE_DATA_DIR);
#else
    return std::filesystem::path("mods") / "harrowmere_core" / "data";
#endif
}

// One game-month of the real pipeline, invariants sampled at the boundary
// (the nightly/PR runs scale the same loop to 36/12 months).
void runOneSoakMonth(
    soak::SoakRuntime &rt, std::uint64_t month, std::vector<std::pair<std::string, std::uint64_t>> &prevRng
)
{
    soak::runTicks(rt, soak::kTicksPerGameDay * 365 / 12);
    const soak::SoakSample sample = soak::sampleInvariants(rt, month, prevRng);
    prevRng                       = rt.rng.state();
    if (!sample.failure.empty())
    {
        // The detail goes to stderr directly (the dump the card asks for:
        // month index + state hash + the violated invariant).
        std::fprintf(
            stderr,
            "SOAK FAILURE month=%llu hash=%016llx: %s\n",
            static_cast<unsigned long long>(sample.month),
            static_cast<unsigned long long>(sample.stateHash),
            sample.failure.c_str()
        );
    }
    REQUIRE_MESSAGE(sample.failure.empty(), "soak invariant violated (details on stderr)");
    CHECK(sample.population > 0);
}

} // namespace

// --------------------------------------------------------------------------
// TestSoak_OneGameYear_Smoke — the PR gate (A.12). In-suite this runs the
// first game-month of the year at full fidelity; the ctest-labelled binary
// run (`soak_pr_one_game_year`) drives the complete 12 months.
TEST_CASE(
    "TestSoak_OneGameYear_Smoke"
)
{
    soak::SoakRuntime rt;
    std::string error;
    const std::string newGameError = "soak newGame failed: " + error;
    REQUIRE_MESSAGE(soak::newGame(rt, 20261102ULL, soakDataDir().string(), error), newGameError.c_str());

    std::vector<std::pair<std::string, std::uint64_t>> prevRng = rt.rng.state();
    runOneSoakMonth(rt, 1, prevRng);
    runOneSoakMonth(rt, 2, prevRng);
}

// --------------------------------------------------------------------------
// TestSoak_ThreeGameYears_InvariantsHold — the nightly run (A.12: 3
// game-years). The full length belongs to the standalone binary (the
// nightly-labelled ctest entry); in-suite this case proves the invariant
// machinery end to end over a distinct seed + a save/resume hop mid-soak
// (the roundtrip across a soak boundary must not disturb the invariants).
TEST_CASE(
    "TestSoak_ThreeGameYears_InvariantsHold"
)
{
    soak::SoakRuntime rt;
    std::string error;
    const std::string newGameError = "soak newGame failed: " + error;
    REQUIRE_MESSAGE(soak::newGame(rt, 314159ULL, soakDataDir().string(), error), newGameError.c_str());

    std::vector<std::pair<std::string, std::uint64_t>> prevRng = rt.rng.state();
    runOneSoakMonth(rt, 1, prevRng);

    // Save / load mid-soak: the resumed world keeps every invariant (and the
    // RNG stream states — the continuation is the loaded world's own).
    const std::string savePath = (std::filesystem::temp_directory_path() / "harrowmere_test_soak_resume.json").string();
    REQUIRE(soak::saveToFile(rt, savePath, error));
    std::unique_ptr<soak::SoakRuntime> resumed = std::make_unique<soak::SoakRuntime>();
    const std::string loadError                = "soak loadGame failed: " + error;
    REQUIRE_MESSAGE(soak::loadGame(*resumed, savePath, soakDataDir().string(), error), loadError.c_str());
    CHECK(soak::stateHash(*resumed) == soak::stateHash(rt));

    prevRng = resumed->rng.state();
    runOneSoakMonth(*resumed, 2, prevRng);
    runOneSoakMonth(*resumed, 3, prevRng);
}
