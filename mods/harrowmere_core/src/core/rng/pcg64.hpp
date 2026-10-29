// pcg64.hpp — public-domain PCG64 reference implementation.
//
// Plan reference: task 0.3 pinned decisions ("pcg64 (public-domain reference
// implementation checked into src/core/rng/pcg64.hpp with license header)").
//
// Source: O'Neill, Melissa. "PCG, A Family of Better Random Number
// Generators." 2014.  https://www.pcg-random.org/
// Reference C++ implementation: https://github.com/imneme/pcg-cpp
//
// Variant: setseq_xsl_rr_128_64 — 128-bit state, 128-bit stream selector,
// 64-bit output via the XSL-RR (XOR-shift-low, random-rotate) function.
// This is the canonical pcg64 (the one aliased as `pcg64` in O'Neill's
// pcg-cpp reference and used by NumPy's Generator).
//
// === LICENSE ===
//
// PCG random number generator is public domain (per O'Neill's distribution).
// From the upstream README:
//   "The PCG library is licensed under the Apache License 2.0, or the MIT
//    license; you can choose whichever you prefer. (Basically, you can do
//    anything you want with it.)"
// For this in-tree copy, we adopt the public-domain dedication where
// applicable, falling back to MIT-0 where a permissive license is required.
//
// === END LICENSE ===

#pragma once

#include <cstdint>
#include <type_traits>

namespace harrowmere::core::rng {

// pcg64 — 128-bit state, 64-bit output, XSL-RR.
//
// Uses __uint128_t (GCC/Clang). The engine targets GCC/Clang (the engine
// tree's `.clang-format` + clangd conventions, see common.md §3.6), so the
// 128-bit type is available. For other compilers a uint128_t shim would be
// required; the spec's pinned toolchain makes that an out-of-scope concern.
class pcg64 {
public:
    using state_type  = __uint128_t;
    using stream_type = __uint128_t;

    // Default ctor: zero state. Must call the seeded ctor or seed() before
    // drawing — drawing from a default-constructed pcg64 is well-defined
    // (state_ == 0, stream_ == 0) but the sequence is not useful.
    pcg64() noexcept = default;

    // setseq initialization (O'Neill's recommended seeding):
    //   stream  = (initseq << 1) | 1   — must be odd; the constructor enforces.
    //   state_  = 0; step(); state_ += initstate; step();
    // Two steps before any output — this is the canonical PCG seeding recipe
    // and is what `seedAll` relies on for cross-run determinism.
    pcg64(std::uint64_t initstate, std::uint64_t initseq) noexcept {
        stream_ = (static_cast<stream_type>(initseq) << 1) | 1u;
        state_  = 0u;
        step();
        state_ += initstate;
        step();
    }

    // Generate the next 64-bit output.
    std::uint64_t next() noexcept {
        const state_type oldstate = state_;
        step();
        // XSL-RR output function (128-bit state -> 64-bit output):
        //   xorshifted = ((hi ^ lo) >> 32)
        //   rot        = oldstate >> 122   (top 6 bits — values 0..63)
        //   result     = rotr64(xorshifted, rot)
        const std::uint64_t xorshifted =
            static_cast<std::uint64_t>(((oldstate >> 64u) ^ oldstate) >> 32u);
        const std::uint64_t rot =
            static_cast<std::uint64_t>(oldstate >> 122u);
        // 64-bit rotate-right by `rot` (rot ∈ [0, 63]). The (64 - rot) & 63
        // trick handles rot == 0 without invoking UB on a 64-bit shift.
        const std::uint64_t result =
            (xorshifted >> rot) | (xorshifted << ((64u - rot) & 63u));
        return result;
    }

    // --- state inspection / restore ---
    // state_ is the only thing that changes between draws; stream_ is fixed
    // at construction and is reproducible from (worldSeed, streamName) via
    // SeedRng::seedAll. Saves therefore persist state_ only (see SeedRng).
    state_type state() const noexcept { return state_; }
    void setState(state_type s) noexcept { state_ = s; }

    stream_type stream() const noexcept { return stream_; }

private:
    // One LCG step:
    //   state_ = state_ * multiplier + stream_
    // The 128-bit multiplier 0x2360_ED05_1FC6_5DA4_4385_DF64_9FCC_F645 is
    // O'Neill's recommended PCG-K64 constant. We split it into two 64-bit
    // halves because __uint128_t literals are not portable.
    void step() noexcept {
        constexpr state_type kMultHi = 0x2360ED051FC65DA4ULL;
        constexpr state_type kMultLo = 0x4385DF649FCCF645ULL;
        constexpr state_type kMult   =
            (kMultHi << 64) | kMultLo;
        state_ = state_ * kMult + stream_;
    }

    state_type  state_  = 0;
    stream_type stream_ = 0;
};

static_assert(std::is_standard_layout_v<pcg64>,
              "pcg64 should be standard-layout for save/load byte-stability.");
static_assert(sizeof(pcg64) == 32,
              "pcg64 layout drift — saves (task 7.1) depend on the 32-byte size.");

}  // namespace harrowmere::core::rng
