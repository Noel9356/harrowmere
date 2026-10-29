// test_math.cpp — task 0.2's test file (phase-1-walk.md, task 0.2 card).
//
// The four TEST_CASEs marked "pinned" are the card's pinned minimum set (§1.5);
// their names are pinned — keep them exactly. The remaining cases are
// supplementary pins for semantics the card fixes in prose (R2 negatives, the
// u64 variant, i128 guard behavior, id generation, Result).
//
// Framework: doctest (task 0.6 vendors the single header at third_party/doctest/).
// One test binary: harrowmere_core_tests.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/constants.hpp"
#include "core/math.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <type_traits>

namespace {

// --- design-source factors for TestMath_MulDiv128_ConvictMagnitude ------------
// Named here with their Appendix A rows; they migrate into constants.hpp with
// their owning cards (constants.hpp is append-only, task 0.2).

constexpr std::uint64_t BASE_LEVEL_COST       = 100;   // A-7
constexpr std::uint64_t LEVEL_COST_RATE_NUM   = 115;   // A-8
constexpr std::uint64_t LEVEL_COST_RATE_DEN   = 100;   // A-8
constexpr std::int64_t  CONVICTION_PENALTY_FACTOR = 20000;  // A-77 (2.0x)
constexpr std::int64_t  MAX_REPUTATION            = 10000;  // A-87 (+-SCALE)
constexpr std::int64_t  REP_HATE_SLOPE            = 2000;   // D-REP-2 formula row (A.9)

// Design source F.3 precomputeCosts: costs[0] = base;
// costs[i] = max(base, costs[i-1] * rateNum / rateDen). levelCosts[i] is the XP
// to advance from level i to i+1.
std::uint64_t levelCostAt(std::uint64_t level) {
    std::uint64_t cost = BASE_LEVEL_COST;
    for (std::uint64_t i = 1; i <= level; ++i) {
        const std::uint64_t next = cost * LEVEL_COST_RATE_NUM / LEVEL_COST_RATE_DEN;
        cost = next < BASE_LEVEL_COST ? BASE_LEVEL_COST : next;
    }
    return cost;
}

// Design source F.4 getZoneMultiplier: safe territories double the penalty.
constexpr std::int64_t zoneMultiplier(bool safeTerritory) {
    return safeTerritory ? 2 * static_cast<std::int64_t>(SCALE)
                         : static_cast<std::int64_t>(SCALE);
}

// Design source F.4 getReputationMultiplier (D-REP-2): the hated pay more.
std::int64_t reputationMultiplier(std::int64_t reputation) {
    const std::int64_t hate = reputation < 0 ? -reputation : 0;
    return static_cast<std::int64_t>(SCALE) + hate * REP_HATE_SLOPE / static_cast<std::int64_t>(SCALE);
}

constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kI64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();

}  // namespace

// ---------------------------------------------------------------------------
// Pinned minimum set (task 0.2 "Tests")
// ---------------------------------------------------------------------------

TEST_CASE("TestMath_Isqrt64_ExactSquares") {
    REQUIRE(isqrt64(0) == 0);
    REQUIRE(isqrt64(1) == 1);
    for (std::uint64_t n = 0; n <= 20000; ++n) {
        CHECK(isqrt64(n * n) == n);
    }
    // Giant squares: (2^32 - 1)^2 is the largest exact square representable in u64.
    const std::uint64_t giant = 4294967295ULL;  // 2^32 - 1
    CHECK(isqrt64(giant * giant) == giant);
    CHECK(isqrt64(1ULL << 62) == (1ULL << 31));
}

TEST_CASE("TestMath_MulDiv64_LargeValuesNoOverflow") {
    // The card's pinned case: a = SCALE, b = SCALE, den = 7 — 10^8 / 7, truncating.
    CHECK(mulDiv64(static_cast<std::int64_t>(SCALE), static_cast<std::int64_t>(SCALE), 7) == 14285714);

    // Oracle sweep against a raw __int128 computation, across magnitudes and signs.
    const std::int64_t vals[] = {1, 7, 9999, 10000, 123456789, 1000000000000LL,
                                 kI64Max, -1, -7, -9999, -10000, kI64Min};
    const std::uint64_t dens[] = {1, 2, 3, 7, 10000, 999983, kU64Max};
    for (std::int64_t a : vals) {
        for (std::int64_t b : vals) {
            for (std::uint64_t d : dens) {
                const __int128 p = static_cast<__int128>(a) * static_cast<__int128>(b);
                const __int128 q = p / static_cast<__int128>(d);  // truncates toward zero
                const std::int64_t expected =
                    q > static_cast<__int128>(kI64Max) ? kI64Max
                  : q < static_cast<__int128>(kI64Min) ? kI64Min
                  : static_cast<std::int64_t>(q);
                CHECK(mulDiv64(a, b, d) == expected);
            }
        }
    }
}

TEST_CASE("TestMath_MulDiv128_ConvictMagnitude") {
    // The card pins: "factors from Appendix A row A-30 at level 100 produce the
    // documented value". Registry correction (recorded for the merge): A-30 is
    // DEFENCE_FACTOR (combat, A.4). The convict factors are A-77
    // (CONVICTION_PENALTY_FACTOR), levelCosts (A-7/A-8, design source F.3) and the
    // two multiplier formulas (getZoneMultiplier; getReputationMultiplier, D-REP-2).
    // The pitfall register ties this test to convict's factor*cost*zone*rep product.
    const std::uint64_t cost100 = levelCostAt(100);  // levelCosts[100]

    // Design source F.4 justice.md, convict():
    //   debt = (u128)CONVICTION_PENALTY_FACTOR * levelCosts[level]
    //          * getZoneMultiplier(...) * getReputationMultiplier(culprit)
    //          / (SCALE * SCALE * SCALE)
    // composed through the pinned helpers. A-77 = 2 * SCALE makes the first
    // division exact, so the staged composition equals the single-expression
    // i128 oracle (asserted below).
    const std::int64_t twoLevelCost =
        mulDiv64(CONVICTION_PENALTY_FACTOR, static_cast<std::int64_t>(cost100), SCALE);
    CHECK(twoLevelCost == 2 * static_cast<std::int64_t>(cost100));  // A-77: "~2 levels before multipliers"

    // Neutral case (non-safe zone, reputation 0): the documented value — exactly
    // two levels of XP debt.
    const std::int64_t debtNeutral =
        mulDiv128(twoLevelCost, zoneMultiplier(false), reputationMultiplier(0), SCALE * SCALE);
    CHECK(debtNeutral == 2 * static_cast<std::int64_t>(cost100));

    // Safe zone doubles the penalty (getZoneMultiplier): exactly four levels.
    const std::int64_t debtSafe =
        mulDiv128(twoLevelCost, zoneMultiplier(true), reputationMultiplier(0), SCALE * SCALE);
    CHECK(debtSafe == 4 * static_cast<std::int64_t>(cost100));

    // Maximum hatred (rep = -MAX_REPUTATION, A-87): equals the i128 oracle of the
    // doc's single-expression formula.
    const std::int64_t repMult = reputationMultiplier(-MAX_REPUTATION);
    const std::int64_t debtHated =
        mulDiv128(twoLevelCost, zoneMultiplier(true), repMult, SCALE * SCALE);
    const __int128 oracle =
        static_cast<__int128>(CONVICTION_PENALTY_FACTOR) * static_cast<__int128>(cost100)
        * static_cast<__int128>(zoneMultiplier(true)) * static_cast<__int128>(repMult)
        / (static_cast<__int128>(SCALE) * static_cast<__int128>(SCALE) * static_cast<__int128>(SCALE));
    CHECK(debtHated == static_cast<std::int64_t>(oracle));

    // Why mulDiv128/R3 exists (pitfall register: "Triple products in u64
    // (convict's factor*cost*zone*rep) — silent overflow"): the naive doc-order
    // product at level 100 exceeds u64 range. The helpers keep every
    // intermediate in 128 bits and stay exact.
    const __int128 fullProduct =
        static_cast<__int128>(CONVICTION_PENALTY_FACTOR) * static_cast<__int128>(cost100)
        * static_cast<__int128>(zoneMultiplier(true)) * static_cast<__int128>(repMult);
    CHECK(fullProduct > static_cast<__int128>(kU64Max));
    CHECK(fullProduct == static_cast<__int128>(twoLevelCost) * static_cast<__int128>(zoneMultiplier(true))
                             * static_cast<__int128>(repMult) * static_cast<__int128>(SCALE));
}

TEST_CASE("TestMath_ApplyXpDelta_ClampsAtNegativeMaxXp") {
    const std::int64_t minXp = -MAX_XP;  // A-2 = -10^15 (R4)
    CHECK(applyXpDelta(minXp, 0) == minXp);
    CHECK(applyXpDelta(minXp, -1) == minXp);              // at the floor, stays
    CHECK(applyXpDelta(minXp + 1, -1) == minXp);          // lands exactly on the floor
    CHECK(applyXpDelta(0, -MAX_XP - 1) == minXp);         // overshoot clamps to -MAX_XP exactly
    CHECK(applyXpDelta(-MAX_XP / 2, -MAX_XP) == minXp);
    CHECK(applyXpDelta(0, kI64Min) == minXp);             // extreme negative delta still clamps
    CHECK(applyXpDelta(100, 50) == 150);                  // gains pass through
    CHECK(applyXpDelta(-100, 100) == 0);                  // debt repaid exactly
    CHECK(applyXpDelta(kI64Max, kI64Max) == kI64Max);     // upper side: i64 range guard only
    CHECK(applyXpDelta(kI64Max, 1) == kI64Max);
}

// ---------------------------------------------------------------------------
// Supplementary pins (card prose semantics)
// ---------------------------------------------------------------------------

TEST_CASE("TestMath_Isqrt64_NonSquaresFloor") {
    for (std::uint64_t n = 1; n <= 20000; ++n) {
        CHECK(isqrt64(n * n - 1) == n - 1);
        CHECK(isqrt64(n * n + 1) == n);
    }
    const std::uint64_t giant = 4294967295ULL;
    CHECK(isqrt64(giant * giant - 1) == giant - 1);
    CHECK(isqrt64(giant * giant + 1) == giant);  // 2^32 squared would overflow u64
}

TEST_CASE("TestMath_MulDiv64_TruncatesTowardZero") {
    // R2 pinned decision: truncation toward zero, NOT floor, on negatives —
    // only applyXpDelta may produce negatives, and it clamps.
    CHECK(mulDiv64(-7, 1, 2) == -3);   // toward zero, not -4
    CHECK(mulDiv64(-9, 3, 10) == -2);  // -27/10 -> -2, not -3
    CHECK(mulDiv64(9, 3, 10) == 2);
    CHECK(mulDiv64(-10, 1, 5) == -2);  // exact negatives stay exact
}

TEST_CASE("TestMath_MulDivU64_LargeValuesNoOverflow") {
    CHECK(mulDivU64(SCALE, SCALE, 7) == 14285714);
    CHECK(mulDivU64(kU64Max, kU64Max, 3) == kU64Max);  // overflows u64 -> saturates (R3)
    CHECK(mulDivU64(kU64Max, kU64Max, kU64Max) == kU64Max);  // exact: (m^2)/m = m
    CHECK(mulDivU64(kU64Max, 2, kU64Max) == 2);
}

TEST_CASE("TestMath_MulDiv128_OracleAndSaturation") {
    CHECK(mulDiv128(kI64Max, kI64Max, kI64Max, 1) == kI64Max);  // i128 overflow guarded -> saturate
    CHECK(mulDiv128(kI64Min, kI64Max, kI64Max, 1) == kI64Min);  // sign-correct saturation
    CHECK(mulDiv128(kI64Max, kI64Max, kI64Max, 7) == kI64Max);
    CHECK(mulDiv128(static_cast<std::int64_t>(SCALE), static_cast<std::int64_t>(SCALE),
                    static_cast<std::int64_t>(SCALE), SCALE) == static_cast<std::int64_t>(SCALE) * static_cast<std::int64_t>(SCALE));  // 10^12/10^4 = 10^8
    CHECK(mulDiv128(-9, 3, 10, 100) == -2);  // -270/100 -> -2 (R2 toward zero)
#ifdef NDEBUG
    // Release fallback for the programmer error (asserts abort in debug builds):
    // den == 0 returns a deterministic 0, never UB (math.hpp header contract).
    CHECK(mulDiv128(2, 3, 5, 0) == 0);
#endif
}

TEST_CASE("TestMath_ClampScalar_ClampsBothSides") {
    CHECK(clampScalar(5, 0, 10) == 5);
    CHECK(clampScalar(-1, 0, 10) == 0);
    CHECK(clampScalar(11, 0, 10) == 10);
    CHECK(clampScalar(0, 0, 10) == 0);
    CHECK(clampScalar(10, 0, 10) == 10);
    CHECK(clampScalar<std::int64_t>(15000, 0, static_cast<std::int64_t>(SCALE)) == static_cast<std::int64_t>(SCALE));
    CHECK(clampScalar(0.25, 0.0, 1.0) == 0.25);
    CHECK(clampScalar(1.5, 0.0, 1.0) == 1.0);
}

TEST_CASE("TestTypes_IdAliasesAreDocExact") {
    CHECK(sizeof(characterId) == 8);
    CHECK(sizeof(territoryId) == 8);
    CHECK(sizeof(factionId) == 4);
    CHECK(sizeof(regionId) == 2);
    CHECK(sizeof(speciesId) == 2);
    CHECK(sizeof(Position) == 3 * sizeof(double));
    CHECK(std::is_trivially_copyable<Position>::value);
    CHECK(std::is_standard_layout<Position>::value);
}

TEST_CASE("TestIdGenerator_MonotonicPerSpace_ZeroReserved") {
    IdGenerator ids;
    CHECK(ids.nextCharacter() == 1);  // 0 is reserved (killer 0 = environment, task 0.7)
    CHECK(ids.nextCharacter() == 2);
    CHECK(ids.nextEvidence() == 1);   // id spaces are independent
    CHECK(ids.nextCharacter() == 3);
    CHECK(ids.peek("character") == 4);  // peek consumes nothing
    CHECK(ids.peek("warrant") == 1);
    CHECK(ids.nextWarrant() == 1);

    IdGenerator fromTen(10);  // fresh-world start value is caller-chosen
    CHECK(fromTen.nextCharacter() == 10);
    CHECK(fromTen.nextEvent() == 10);
    CHECK(fromTen.nextRegion() == 10);

    IdGenerator zero(0);  // sanitize: below FIRST_ID clamps up to FIRST_ID
    CHECK(zero.nextCharacter() == 1);
}

TEST_CASE("TestIdGenerator_StateRoundTrip_NeverReuses") {
    IdGenerator a;
    (void)a.nextCharacter();
    (void)a.nextCharacter();
    (void)a.nextEvidence();
    (void)a.nextEvent();
    const auto saved = a.state();

    IdGenerator b;  // fresh world, then a save is loaded into it
    b.loadState(saved);
    CHECK(b.nextCharacter() == 3);  // continues from the save, never re-issues
    CHECK(b.nextEvidence() == 2);
    CHECK(b.nextEvent() == 2);

    // A stale save (older counters) must not rewind a live generator (R10).
    b.loadState(saved);
    CHECK(b.peek("character") == 4);
    CHECK(b.nextCharacter() == 4);
}

TEST_CASE("TestResult_OkErrRoundtrip") {
    const Result<int, const char*> r = Result<int, const char*>::ok(42);
    REQUIRE(r.isOk());
    CHECK(r.value() == 42);
    CHECK(static_cast<bool>(r));

    const Result<int, const char*> e = Result<int, const char*>::err("bad");
    REQUIRE(e.isErr());
    CHECK(std::string(e.error()) == "bad");
    CHECK_FALSE(static_cast<bool>(e));
}

TEST_CASE("TestResult_VoidSpecialization") {
    const Result<void, int> ok = Result<void, int>::ok();
    CHECK(ok.isOk());
    CHECK_FALSE(ok.isErr());

    const Result<void, int> err = Result<void, int>::err(7);
    CHECK(err.isErr());
    CHECK(err.error() == 7);
}
