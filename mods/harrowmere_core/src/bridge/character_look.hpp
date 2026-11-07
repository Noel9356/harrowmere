#ifndef HARROWMERE_CORE_BRIDGE_CHARACTER_LOOK_HPP
#define HARROWMERE_CORE_BRIDGE_CHARACTER_LOOK_HPP

// bridge/character_look.hpp — per-character randomized presentation looks.
//
// The owner's defect (2026-11-05): every rendered character carried ONE
// species tint (and every far marker one shared grey box tint), so a crowd
// read as a row of identical silhouettes. The fix is the presentation-side
// character customisation the owner asked for: every character derives a
// DETERMINISTIC look from its own id — a tunic dye from the medieval
// commoner palette, a per-character head-tone jitter AROUND the species
// base (species identity survives; individuals stop being clones), and a
// height/girth jitter so the crowd silhouette varies.
//
// The look is presentation-only: the sim never reads it (R5 — the visual
// transform is not a game rule), and it is a PURE function of the character
// id (no RNG stream, D-IMG-2; no wall clock, R8). The same id wears the same
// clothes every frame, every session, after a save/load.

#include <array>
#include <cstdint>

namespace harrowmere
{
namespace bridge
{

// One character's randomized look (Linear-RGB tints).
struct CharacterLook
{
    std::array<float, 3> clothing;  // the body's tunic dye
    std::array<float, 3> pants;     // rev 26: the legs' dye (the blocky-body
                                    // split colors legs and torso separately)
    std::array<float, 3> hairTone;  // rev 26: the hair cap's tone
    std::array<float, 3> headTone;  // the head sphere's tone (species base jittered)
    float heightJitter;             // multiplies the A-153 silhouette height (0.88..1.14)
    float girthJitter;              // multiplies the body radius (0.90..1.12)
    std::uint32_t bodyVariant;      // rev 26: the blocky-mesh variant pick (0..2)
};

// The medieval commoner dye palette (Linear-RGB, the tunic colors a village
// crowd actually wears). Published here so the bridge, the probes and the
// pins all read ONE table (the A-187 rule: palettes swap values, never
// keys).
// rev 26: the blocky bodies render these dyes at FULL strength (no texture
// set - the flat fallback skips the A-189 pastel mix), so the palette keeps
// its authored medieval values; the keys and the count stay (A-187).
inline constexpr std::array<std::array<float, 3>, 10> CHARACTER_TUNIC_DYES{{
    {{0.280f, 0.190f, 0.110f}}, // walnut brown
    {{0.220f, 0.300f, 0.140f}}, // moss green
    {{0.450f, 0.160f, 0.090f}}, // rust red
    {{0.190f, 0.240f, 0.430f}}, // indigo blue (rev 31: lifted - the old
                                // 0.14/0.17/0.32 read near-black on every
                                // shaded side; woad-dyed wool is a mid blue)
    {{0.550f, 0.380f, 0.140f}}, // ochre yellow
    {{0.290f, 0.280f, 0.290f}}, // undyed grey wool (rev 31: was "charcoal"
                                // 0.16 - the darkest dye in the palette
                                // turned every shaded figure into a
                                // silhouette; undyed wool is a mid grey)
    {{0.720f, 0.640f, 0.480f}}, // cream wool
    {{0.300f, 0.300f, 0.140f}}, // olive
    {{0.340f, 0.100f, 0.140f}}, // wine
    {{0.140f, 0.360f, 0.360f}}, // teal (rev 31: lifted with the indigo)
}};

// rev 26: the trousers' palette - the muted working-class colors a village
// crowd's legs wear, distinct from the tunics so a standing figure reads
// tunic-over-trousers instead of one colored column.
inline constexpr std::array<std::array<float, 3>, 6> CHARACTER_PANTS_DYES{{
    {{0.180f, 0.140f, 0.100f}}, // undyed hemp brown
    {{0.200f, 0.210f, 0.230f}}, // slate grey (rev 31: lifted from 0.12 -
                                // shaded legs read as holes otherwise)
    {{0.220f, 0.180f, 0.130f}}, // fawn
    {{0.220f, 0.170f, 0.140f}}, // dark leather (rev 31: lifted from 0.14)
    {{0.160f, 0.170f, 0.120f}}, // drab green
    {{0.240f, 0.210f, 0.180f}}, // oatmeal
}};

// rev 26: the hair tones (the cap above the blocky head).
inline constexpr std::array<std::array<float, 3>, 6> CHARACTER_HAIR_TONES{{
    {{0.045f, 0.030f, 0.020f}}, // raven
    {{0.140f, 0.085f, 0.045f}}, // chestnut
    {{0.320f, 0.180f, 0.070f}}, // ginger
    {{0.230f, 0.170f, 0.090f}}, // dark blond
    {{0.560f, 0.520f, 0.460f}}, // ash grey
    {{0.780f, 0.760f, 0.720f}}, // elder white
}};

// The pure per-character derivation. `speciesBase` is the species' A-153
// palette color the head tone jitters around.
CharacterLook characterLookFor(std::uint64_t characterId, const std::array<float, 3> &speciesBase) noexcept;

// rev 28: the per-character "personal space" rest offset. Every NPC intent
// pulls toward the settlement center (the market side of town), so a
// gathered crowd stacks into one overlapping totem at the exact center -
// the first street-level capture read 18 villagers as ONE figure. The
// presentation spreads each visual by a deterministic id-hash offset -
// rev 28: the offset CLUSTERS (five plaza anchors + a 1.2 m disc jitter per
// member) so the crowd reads as milling groups, not the uniform ring the
// rev-26 capture showed. Pure from the id (D-IMG-2), presentation-only
// (R5: the sim never reads it), applied identically at creation and on
// every move so a character never teleports between frames.
struct StandOffset
{
    float x;
    float z;
};
StandOffset characterStandOffset(std::uint64_t characterId) noexcept;

} // namespace bridge
} // namespace harrowmere

#endif // HARROWMERE_CORE_BRIDGE_CHARACTER_LOOK_HPP
