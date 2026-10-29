// test_data_loaders_2.cpp — Task 1.2's pinned test suite.
//
// Implements the five tests pinned in phase-1-walk.md card 1.2's Tests
// block, plus the sanity canary from task 0.6's spec. Test names are
// verbatim from the card — do not rename (common.md §3.5: "Test names are
// pinned; keep them").
//
// The tests load the shipped JSON files from
// `mods/harrowmere_core/data/` and assert the structs they produce match
// the design reference's pinned values exactly. A change to the data
// files that breaks these tests is a deviation and must be recorded in
// DEVIATIONS.md (common.md §1.4).
//
// Test harness: doctest v2.4.12 (vendored per task 0.6).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "core/enums.hpp" // SuspicionAct (harrowmere::SuspicionAct)
#include "data/crime.hpp"
#include "data/laws.hpp" // SuspicionMask (harrowmere::data::SuspicionMask)
#include "data/roles.hpp"
#include "data/territory_templates.hpp"

namespace
{

// Resolves the absolute path to a shipped data file. The test binary is
// run from the repo root (per task 0.6's `scripts/ci.sh`), so a relative
// path is sufficient.
std::string dataFile(
    const std::string &name
)
{
    return "mods/harrowmere_core/data/" + name;
}

} // namespace

// Pull every type we use into scope so test cases read cleanly.
using namespace harrowmere;
using namespace harrowmere::data;

// ---------------------------------------------------------------------------
// Sanity canary — task 0.6's TestHarness_SanityAlwaysPasses. Guards
// against a silently-empty test run (if the test binary builds but no
// tests register, CI's "test count > 0" assertion fails elsewhere).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestHarness_SanityAlwaysPasses"
)
{
    CHECK(true);
}

// ---------------------------------------------------------------------------
// TestCrime_MurderLethalForceTrue_OthersFalse
// Loads crime_data.json and asserts:
//   * MURDER's `lethalForce` is true.
//   * Every other CrimeType's `lethalForce` is false.
// Pinned by design reference `issueWarrant` ("lethalForce copied onto
// warrants at issuance") and phase-1-walk.md card 1.2's pinned v1 row.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestCrime_MurderLethalForceTrue_OthersFalse"
)
{
    using namespace harrowmere::data;
    CrimeTable crimes;
    std::vector<CrimeDataError> errors;
    REQUIRE(loadCrimeData(dataFile("crime_data.json"), crimes, errors));
    REQUIRE(errors.empty());
    REQUIRE(crimes.complete());

    for (const auto &row : crimes.rows())
    {
        if (row.type == CrimeType::MURDER)
        {
            CHECK(row.lethalForce == true);
        } else
        {
            CHECK_MESSAGE(
                row.lethalForce == false,
                "non-MURDER CrimeType must have lethalForce=false; "
                "got true for: "
                    << crimeTypeKey(row.type)
            );
        }
    }
}

// ---------------------------------------------------------------------------
// TestLaws_DefaultSetMatchesDocStruct
// Loads laws.json and asserts the `default` set has all four maps
// present (convictionThreshold + suppression + exemptions + suspicion;
// cultureBias is added as a fifth pinned field per the docs' example).
// Also checks every convictionThreshold is in [0, kScale].
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestLaws_DefaultSetMatchesDocStruct"
)
{
    using namespace harrowmere::data;
    LawSetRegistry reg;
    std::vector<LawDataError> errors;
    REQUIRE(loadLawSets(dataFile("laws.json"), reg, errors));
    REQUIRE(errors.empty());

    const LawSet *def = reg.byKey("default");
    REQUIRE(def != nullptr);

    // convictionThreshold: every CrimeType present, in [0, kScale].
    // (Split into two CHECKs — doctest forbids `&&` inside CHECK_MESSAGE.)
    for (std::size_t i = 0; i < static_cast<std::size_t>(CrimeType::COUNT); ++i)
    {
        const auto t = static_cast<CrimeType>(i);
        const auto v = def->convictionThreshold[i];
        CHECK_MESSAGE(v >= 0, "convictionThreshold negative for " << crimeTypeKey(t) << ": " << v);
        CHECK_MESSAGE(v <= kScale, "convictionThreshold > kScale for " << crimeTypeKey(t) << ": " << v);
    }

    // The default set has empty suppression/exemptions/cultureBias/
    // suspicion blocks (it is DEFAULT_LAWS — no corruption, no species
    // conflicts, no overrides). The other three sets carry content.
    CHECK(def->suppression.empty());
    CHECK(def->exemptions.empty());
    CHECK(def->cultureBias.empty());
    CHECK(def->suspicion.empty());
    CHECK_FALSE(def->safe);

    // The xenophobic_village set is the canonical example from
    // design reference §Laws: beastkins/taurs are exempt from MURDER
    // (the docs' "human-kingdoms-kill-beastkins" rule). Use it as the
    // "all four maps present" check.
    const LawSet *xeno = reg.byKey("xenophobic_village");
    REQUIRE(xeno != nullptr);
    CHECK_FALSE(xeno->suppression.empty());
    CHECK_FALSE(xeno->exemptions.empty());
    CHECK_FALSE(xeno->cultureBias.empty());
    CHECK_FALSE(xeno->suspicion.empty());
    CHECK_FALSE(xeno->safe);

    // The safe_zone_laws set MUST have safe=true — the engine's ward-
    // report logic only attaches ward reports where safe=true.
    const LawSet *safe = reg.byKey("safe_zone_laws");
    REQUIRE(safe != nullptr);
    CHECK(safe->safe == true);
}

// ---------------------------------------------------------------------------
// TestLaws_ExemptionKeyedBySpeciesId
// Per phase-1-walk.md card 1.2 / common.md D-CORRUPT-1:
//   * `suppression` is keyed by SocialClassId.
//   * `exemptions` is keyed by speciesId (u16).
// This test asserts the exemption map uses speciesId keys, not string
// species keys like "beastkin". The xenophobic_village set exempts
// speciesId 2 (beastkin) and 3 (taur) from MURDER — verify both.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestLaws_ExemptionKeyedBySpeciesId"
)
{
    using namespace harrowmere::data;
    LawSetRegistry reg;
    std::vector<LawDataError> errors;
    REQUIRE(loadLawSets(dataFile("laws.json"), reg, errors));
    REQUIRE(errors.empty());

    const LawSet *xeno = reg.byKey("xenophobic_village");
    REQUIRE(xeno != nullptr);

    // The exemption map's keys are u16 speciesIds, not strings.
    // Beastkin = speciesId 2, Taur = speciesId 3 (per species.json,
    // task 1.1 / Appendix B.1).
    const auto &exemptions = xeno->exemptions;
    CHECK(exemptions.find(2) != exemptions.end()); // beastkin
    CHECK(exemptions.find(3) != exemptions.end()); // taur

    // Both species are exempt from MURDER only.
    const auto &beastkin = exemptions.at(2);
    CHECK(beastkin[static_cast<std::size_t>(CrimeType::MURDER)] == true);
    // All other crimes are non-exempt for beastkin in this set.
    for (std::size_t i = 0; i < static_cast<std::size_t>(CrimeType::COUNT); ++i)
    {
        if (static_cast<CrimeType>(i) == CrimeType::MURDER)
            continue;
        CHECK_MESSAGE(
            beastkin[i] == false,
            "non-MURDER CrimeType must not be exempt for beastkin; "
            "got true for: "
                << crimeTypeKey(static_cast<CrimeType>(i))
        );
    }

    // The doc's cultureBias is also keyed by speciesId (signed, can be
    // negative — xenophobia expresses as -6000 for beastkin/taur).
    const auto &bias = xeno->cultureBias;
    CHECK(bias.find(2) != bias.end());
    CHECK(bias.find(3) != bias.end());
    CHECK(bias.at(2) == -6000);
    CHECK(bias.at(3) == -6000);
}

// ---------------------------------------------------------------------------
// TestRoles_GuardCriticalAndHeavy
// Loads roles.json and asserts:
//   * The `guard` role exists.
//   * `guard.critical == true` (triggers onCriticalNpcDeath).
//   * `guard.baseReportWeight == 9000` (the heaviest non-captain weight).
// Also spot-checks `captain` (critical, 10000) and `child`
// (cannotTestify=true).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestRoles_GuardCriticalAndHeavy"
)
{
    using namespace harrowmere::data;
    RoleRegistry reg;
    std::vector<RoleDataError> errors;
    REQUIRE(loadRoles(dataFile("roles.json"), reg, errors));
    REQUIRE(errors.empty());
    REQUIRE(reg.hasPinnedV1Set());

    const RoleData *guard = reg.byKey("guard");
    REQUIRE(guard != nullptr);
    CHECK(guard->critical == true);
    CHECK(guard->baseReportWeight == 9000);
    CHECK(guard->integrityMin <= guard->integrityMax);

    const RoleData *captain = reg.byKey("captain");
    REQUIRE(captain != nullptr);
    CHECK(captain->critical == true);
    CHECK(captain->baseReportWeight == 10000);

    const RoleData *child = reg.byKey("child");
    REQUIRE(child != nullptr);
    CHECK(child->cannotTestify == true);
    CHECK(child->baseReportWeight == 0);

    // Every role's baseReportWeight is in [0, kScale].
    for (const auto &[k, r] : reg.roles())
    {
        CHECK_MESSAGE(
            r.baseReportWeight <= kScale, "role " << k << " baseReportWeight > kScale: " << r.baseReportWeight
        );
    }
}

// ---------------------------------------------------------------------------
// TestSuspicion_SixPinnedRowsLoad
// Loads suspicion.json and asserts the six pinned rows from design
// reference §Suspicion and Trust are present with the correct act,
// masks, and weight values.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestSuspicion_SixPinnedRowsLoad"
)
{
    using namespace harrowmere::data;
    std::vector<SuspicionTrigger> rows;
    std::vector<LawDataError> errors;
    REQUIRE(loadSuspicionTriggers(dataFile("suspicion.json"), rows, errors));
    REQUIRE(errors.empty());
    REQUIRE(rows.size() == 6);

    // The six pinned rows, in file order. Mask values are bit-sets
    // expressed via SuspicionMask | SuspicionMask, then converted to
    // u32 for the comparison (SuspicionTrigger.mask is u32).
    const struct
    {
        SuspicionAct act;
        std::uint32_t mask;
        std::uint32_t weight;
    } expected[] = {
        {SuspicionAct::CROUCH, toBits(SuspicionMask::SETTLEMENT | SuspicionMask::MARKET), 2000},
        {SuspicionAct::UNLIT, toBits(SuspicionMask::NIGHT | SuspicionMask::SETTLEMENT), 2500},
        {SuspicionAct::MASKED, toBits(SuspicionMask::SETTLEMENT | SuspicionMask::MARKET), 5000},
        {SuspicionAct::TRESPASS, toBits(SuspicionMask::PRIVATE_PROPERTY), 4000},
        {SuspicionAct::BLOODSTAINED, toBits(SuspicionMask::SETTLEMENT), 6000},
        {SuspicionAct::FLEE, toBits(SuspicionMask::SETTLEMENT | SuspicionMask::NIGHT), 3000},
    };

    REQUIRE(rows.size() == std::size(expected));
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        CHECK(rows[i].act == expected[i].act);
        CHECK(rows[i].mask == expected[i].mask);
        CHECK(rows[i].weight == expected[i].weight);
    }
}
