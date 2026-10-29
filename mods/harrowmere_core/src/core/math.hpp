// math.hpp — fixed-point helpers on the SCALE grid (task 0.2; rules R1–R5, §3.2).
//
// Semantics pinned here (task 0.2 "Pinned decisions"):
//   * R2 — the docs' `/` is C++ truncating division (toward zero). No rounding
//     helper exists; call sites that need care get an explicit comment + test.
//   * R2 — a zero denominator is guarded at CALL SITES with a pinned fallback
//     (`if (denominator == 0) return <pinned fallback>;`). The helpers below treat
//     den == 0 as a programmer error: assert in debug, deterministic 0 in release —
//     never undefined behavior.
//   * R3 — wide intermediates use __int128; results saturate into i64/u64 range.
//     Saturation is overflow protection, not a game rule — SCALE-grid call sites
//     sit far below these bounds.
//   * R5 — no floating point anywhere in game rules; these helpers are integer-only.

#pragma once

#include <cassert>
#include <cstdint>

#include "constants.hpp"  // SCALE (A-1) — the one definition; never redefine it here

// Exact integer square root (floor), no floating point. Pitfall (task 0.2): sqrtl
// breaks determinism/precision at huge inputs — this is the exact bit-by-bit binary
// method, correct for every u64 input. constexpr, so compile-time tables can use it.
constexpr std::uint64_t isqrt64(std::uint64_t v) noexcept {
    if (v == 0) return 0;
    std::uint64_t res = 0;
    std::uint64_t bit = 1ULL << 62;  // highest power of four within u64
    while (bit > v) bit >>= 2;
    while (bit != 0) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

// Clamp into [lo, hi]; asserts lo <= hi in debug (task 0.2 spec).
template <class T>
constexpr T clampScalar(T v, T lo, T hi) {
    assert(lo <= hi);
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// (a * b) / den — 128-bit intermediate, truncating toward zero (R2).
// Saturates into i64 range (R3 clamp; overflow protection, not a game rule).
std::int64_t mulDiv64(std::int64_t a, std::int64_t b, std::uint64_t den) noexcept;

// Unsigned variant: (a * b) / den — 128-bit intermediate, truncating.
// Saturates into u64 range.
std::uint64_t mulDivU64(std::uint64_t a, std::uint64_t b, std::uint64_t den) noexcept;

// (a * b * c) / denSq — triple SCALE-grid products (R3), e.g. convict()'s penalty.
// `denSq` is the full compound denominator (typically a product of doc
// denominators, hence the name). 128-bit intermediate, truncating toward zero (R2);
// result clamped into i64 (R3). A triple product whose next multiply would overflow
// i128 itself is caught and returned as the sign-correct i64 saturation — the exact
// quotient provably exceeds i64 range in that case (see math.cpp).
std::int64_t mulDiv128(std::int64_t a, std::int64_t b, std::int64_t c, std::uint64_t denSq) noexcept;

// R4 — XP clamp: returns availableXp + delta, floored at -MAX_XP (A-2) exactly and
// never below. The only helper allowed to produce/propagate negatives on the XP
// path (R1). Upper side has no game clamp ("+inf" per R4) — only i64 range
// saturation. availableXp only; totalXp never decreases (design reference).
std::int64_t applyXpDelta(std::int64_t availableXp, std::int64_t delta) noexcept;
