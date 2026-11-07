// bridge/character_look.cpp — the per-character randomized look derivation.
//
// See character_look.hpp for the contract. Everything here is a pure
// function of (characterId, speciesBase): splitmix64 hashes (the same
// public-domain hash the scatter field uses, D-IMG-2 — never the worldgen
// RNG stream), table lookups, small arithmetic. No state, no clock.

#include "bridge/character_look.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace harrowmere
{
namespace bridge
{

namespace
{

// splitmix64 — the standard public-domain 64-bit hash (the scatter field's
// choice; one algorithm across the presentation hashes, D-IMG-2).
inline std::uint64_t splitmix64(
    std::uint64_t x
) noexcept
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Map a u64 hash to [0, 1).
inline double hashTo01(
    std::uint64_t h
) noexcept
{
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0); // 2^53
}



} // namespace

CharacterLook characterLookFor(
    const std::uint64_t characterId, const std::array<float, 3> &speciesBase
) noexcept
{
    const std::uint64_t dyeHash   = splitmix64(characterId ^ 0xD1E5ull);
    const std::uint64_t toneHash  = splitmix64(characterId ^ 0x703Full);
    const std::uint64_t shapeHash = splitmix64(characterId ^ 0x5BEFull);
    const std::uint64_t legHash   = splitmix64(characterId ^ 0x1E65ull);
    const std::uint64_t hairHash  = splitmix64(characterId ^ 0x5A1Full);

    CharacterLook look;

    // The tunic dye: uniform draw over the palette.
    look.clothing = CHARACTER_TUNIC_DYES[dyeHash % CHARACTER_TUNIC_DYES.size()];
    // rev 26: the trousers and the hair get their own draws - the blocky
    // body's parts are tinted separately, so a standing figure reads
    // tunic-over-trousers with a haircut, not one colored column.
    look.pants    = CHARACTER_PANTS_DYES[legHash % CHARACTER_PANTS_DYES.size()];
    look.hairTone = CHARACTER_HAIR_TONES[hairHash % CHARACTER_HAIR_TONES.size()];
    look.bodyVariant = static_cast<std::uint32_t>(shapeHash % 3u);

    // The head tone: the species base with a per-character luminance and
    // hue nudge (±14% luminance, ±10% on the green/red balance) — species
    // identity survives, individuals stop being clones. Not for the player
    // (id 0 keeps the authored parchment — the player's look is the
    // customisation UI's business, not a hash's).
    if (characterId == 0)
    {
        look.headTone = speciesBase;
    } else
    {
        const double lum = 0.86 + 0.28 * hashTo01(toneHash);              // 0.86..1.14
        const double bal = 0.90 + 0.20 * hashTo01(toneHash ^ 0xABCDull);  // 0.90..1.10
        for (std::size_t i = 0; i < 3; ++i)
        {
            const double channel = static_cast<double>(speciesBase[i]) * lum
                * (i == 1 ? bal : (2.0 - bal)); // green breathes against red/blue
            look.headTone[i] = static_cast<float>(std::clamp(channel, 0.02, 1.0));
        }
    }

    // The crowd silhouette: height ±14%, girth ±12%.
    look.heightJitter = static_cast<float>(0.88 + 0.26 * hashTo01(shapeHash));
    look.girthJitter  = static_cast<float>(0.90 + 0.22 * hashTo01(shapeHash ^ 0x1234ull));

    return look;
}

StandOffset characterStandOffset(
    const std::uint64_t characterId
) noexcept
{
    // rev 28 - the crowd CLUSTERS instead of ringing: the uniform ring the
    // first street-level capture showed read as a queue drilled around the
    // well. Five plaza anchors (the well's sides, the market-stall line,
    // the two cottage-door approaches) each pull a hash-assigned share of
    // the crowd, and every member jitters inside a 2.4 m personal-space
    // disc around its anchor - a milling market crowd, not a parade. All
    // draws are pure from the id (D-IMG-2), presentation-only (R5); the
    // 5.5 m ceiling keeps the whole cluster field on the plaza (the first
    // cottage ring starts ~9 m out); the 0.9 m floor between cluster
    // centers stops two clumps stacking into one blob.
    static constexpr double kPlazaClusters[5][2] = {
        {0.0, -2.1},   // the well's south rim
        {2.7, 0.9},    // the stall line, east
        {-2.6, 1.3},   // the west cottage doors
        {-1.2, -3.4},  // the south approach
        {3.4, -1.8},   // the well's north-east rim
    };
    const std::uint64_t anchorHash = splitmix64(characterId ^ 0x57A7ull);
    const std::uint64_t jitterHash = splitmix64(characterId ^ 0x7A11ull);
    const auto &center             = kPlazaClusters[anchorHash % 5u];
    const double ju                = static_cast<double>(jitterHash % 1000u) / 1000.0;
    const double jv                = static_cast<double>((jitterHash >> 11) % 1000u) / 1000.0;
    const double radius            = 1.2 * std::sqrt(ju); // disc-uniform jitter
    const double angle             = 6.283185307179586 * jv;
    const double x                 = center[0] + std::cos(angle) * radius;
    const double z                 = center[1] + std::sin(angle) * radius;
    return {static_cast<float>(x), static_cast<float>(z)};
}

} // namespace bridge
} // namespace harrowmere
