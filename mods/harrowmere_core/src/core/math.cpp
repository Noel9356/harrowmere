// math.cpp — out-of-line definitions for the wide-intermediate helpers (task 0.2).
//
// The __int128 dependency is deliberately kept inside this single translation unit
// so the header set stays clean and portable for capability-header-style
// consumption (§4.2 rule 5 spirit). These helpers are therefore plain
// (non-constexpr); the constexpr surface of task 0.2 is isqrt64/clampScalar in
// math.hpp. Until task 0.1's harrowmere_plugin.cpp exists, this TU also doubles
// as the interim "dummy TU" that includes every task 0.2 header (Done-when).

#include "core/math.hpp"

#include <cassert>
#include <cstdint>
#include <limits>

#include "core/result.hpp"  // dummy-TU include coverage (task 0.2 Done-when)
#include "core/types.hpp"   // dummy-TU include coverage (task 0.2 Done-when)

namespace {

// Largest value a signed __int128 can hold (2^127 - 1).
constexpr __int128 kI128Max =
    static_cast<__int128>(~(static_cast<unsigned __int128>(0)) >> 1);

std::int64_t saturateToI64(__int128 q) noexcept {
    constexpr __int128 kMax =
        static_cast<__int128>(std::numeric_limits<std::int64_t>::max());
    constexpr __int128 kMin =
        static_cast<__int128>(std::numeric_limits<std::int64_t>::min());
    if (q > kMax) return std::numeric_limits<std::int64_t>::max();
    if (q < kMin) return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(q);
}

}  // namespace

std::int64_t mulDiv64(std::int64_t a, std::int64_t b, std::uint64_t den) noexcept {
    assert(den != 0 && "mulDiv64: den == 0 is a programmer error — guard at the call site (R2)");
    if (den == 0) return 0;  // deterministic release fallback; call sites still guard per R2
    const __int128 p = static_cast<__int128>(a) * static_cast<__int128>(b);  // exact: i64*i64 fits i128
    return saturateToI64(p / static_cast<__int128>(den));  // truncates toward zero (R2)
}

std::uint64_t mulDivU64(std::uint64_t a, std::uint64_t b, std::uint64_t den) noexcept {
    assert(den != 0 && "mulDivU64: den == 0 is a programmer error — guard at the call site (R2)");
    if (den == 0) return 0;  // deterministic release fallback; call sites still guard per R2
    const unsigned __int128 p =
        static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
    const unsigned __int128 q = p / static_cast<unsigned __int128>(den);
    constexpr unsigned __int128 kU64Max =
        static_cast<unsigned __int128>(std::numeric_limits<std::uint64_t>::max());
    if (q > kU64Max) return std::numeric_limits<std::uint64_t>::max();
    return static_cast<std::uint64_t>(q);
}

std::int64_t mulDiv128(std::int64_t a, std::int64_t b, std::int64_t c, std::uint64_t denSq) noexcept {
    assert(denSq != 0 && "mulDiv128: denSq == 0 is a programmer error — guard at the call site (R2)");
    if (denSq == 0) return 0;  // deterministic release fallback; call sites still guard per R2

    __int128 p = static_cast<__int128>(a) * static_cast<__int128>(b);  // exact: i64*i64 fits i128
    const __int128 cc = static_cast<__int128>(c);
    if (cc != 0) {
        const __int128 pm = p < 0 ? -p : p;
        const __int128 cm = cc < 0 ? -cc : cc;
        if (pm > kI128Max / cm) {
            // p * c would overflow i128. Then |p*c| > kI128Max >= i64max * denSq
            // (denSq is u64, i64max * (2^64 - 1) < 2^127), so the exact quotient
            // necessarily saturates i64 — return the sign-correct clamp (R3)
            // without performing the overflowing multiply. denSq > 0, so the
            // quotient's sign is sign(p) xor sign(c).
            return (p < 0) == (cc < 0) ? std::numeric_limits<std::int64_t>::max()
                                       : std::numeric_limits<std::int64_t>::min();
        }
    }
    p *= cc;
    return saturateToI64(p / static_cast<__int128>(denSq));  // truncates toward zero (R2)
}

std::int64_t applyXpDelta(std::int64_t availableXp, std::int64_t delta) noexcept {
    const __int128 r = static_cast<__int128>(availableXp) + static_cast<__int128>(delta);
    const __int128 xpFloor = -static_cast<__int128>(MAX_XP);     // A-2 (R4)
    if (r < xpFloor) return static_cast<std::int64_t>(xpFloor);  // -MAX_XP exactly
    return saturateToI64(r);  // upper side: i64 range guard only, not a game clamp
}
