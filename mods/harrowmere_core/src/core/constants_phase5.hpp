// core/constants_phase5.hpp — every A-row and card-pinned constant first
// introduced by Phase-5 cards (Appendix A is the single home of every number;
// §1.3). The Phase-5 plan (phase-5-image.md) pins rows A-146..A-160 in
// common.md Appendix A.16: "Terminal-stage constants (phases 5–7 — plan-pinned,
// not yet as built)".
//
// MERGE NOTE: this block is self-contained and conflict-free with the Phase-1
// and Phase-2 registries; flat-append it into `core/constants.hpp` if you
// prefer one file, or keep it included from there (the same shape
// `constants_phase2.hpp` established). Every constant cites its Appendix A
// row (or the pinning task card). This file lands the rows tasks 5.1.1 and
// 5.1.2 cite (A-146, A-147.a-e, A-148, A-149); the later Phase-5 cards
// (5.1.3, 5.1.4, 5.2.1, 5.2.2, 5.3.1) extend this block with A-150..A-160.
#pragma once

#include <array>
#include <cstdint>

#include "core/constants.hpp" // REGION_GRID, REGION_SIZE_M, REGION_COUNT (A-40)

namespace harrowmere
{

// ---- A-146: render frame budget (task 5.1.1 measure, 5.3.1 gate, 7.2.1) ----
//
// "16.6 ms render-side on the reference machine at 1080p" — the image must
// pay its way inside one 60 Hz frame. The capture suite (5.1.1) measures and
// reports; the image budget gate (5.3.1) asserts the percentile report
// against this number. Stored in milliseconds on the f64 side because the
// profiler publishes frame times as f64 ms.
inline constexpr double RENDER_FRAME_BUDGET_MS = 16.6; // A-146

// Capture suite percentile report: the suite records p50/p95 per scenario;
// 5.3.1's gate asserts p95 against A-146 on the reference machine (lavapipe
// CI numbers track regression SHAPE, never the absolute gate — D-IMG-3).
inline constexpr double RENDER_FRAME_BUDGET_P95_MS = RENDER_FRAME_BUDGET_MS;       // A-146
inline constexpr double RENDER_FRAME_BUDGET_P50_MS = RENDER_FRAME_BUDGET_MS * 0.6; // headroom: p50 well under the gate

// ---- A-147: terrain noise field (task 5.1.2) --------------------------------
//
// D-IMG-1: height is presentation. heightAt(x, z) is a pure function of the
// worldgen SEED value and the region data (climate elevation bias) 1.6.1
// already produced — it derives from the seed value, never from a sim RNG
// stream (the pitfall: borrowing the worldgen stream changes every downstream
// draw). The field is value-noise octaves summed with persistence; the
// climate elevation bias is applied per-region (mountains inland, flats on
// TEMPERATE/COASTAL plains); a coastal clamp within 2 km of COASTAL regions
// keeps beaches flat. Every knob is an Appendix A sub-row.
inline constexpr std::uint32_t TERRAIN_NOISE_OCTAVES   = 5;      // A-147.a
inline constexpr double TERRAIN_NOISE_BASE_FREQUENCY   = 0.8;    // A-147.b (cycles/km)
inline constexpr double TERRAIN_NOISE_PERSISTENCE      = 0.5;    // A-147.c
inline constexpr double TERRAIN_INLAND_MAX_AMPLITUDE_M = 120.0;  // A-147.d
inline constexpr double TERRAIN_COASTAL_CLAMP_M        = 8.0;    // A-147.e (max within COASTAL_BAND_M)
inline constexpr double TERRAIN_COASTAL_BAND_M         = 2000.0; // A-147.e (the 2 km band)

// The world patch the noise field tiles: [0, REGION_GRID * REGION_SIZE_M) on
// x and z (A-40: 16 x 16 km). The seed hash mixes into every octave so two
// builds of the same seed sample identical heights and a different seed
// differs at every sample.
inline constexpr double TERRAIN_WORLD_EXTENT_M = static_cast<double>(REGION_GRID) * static_cast<double>(REGION_SIZE_M);

// ---- A-148: terrain grid resolution (task 5.1.2) ----------------------------
//
// "512 steps across the world patch" — the static backdrop mesh's vertex
// budget. The mesh grid is 512 x 512 vertices spanning the world extent, so
// one cell is TERRAIN_WORLD_EXTENT_M / TERRAIN_GRID_STEPS metres on edge
// (~31.25 m at A-40's 16 km world — denser than the A-39 32 m spatial grid,
// so terrain never aliases a witness/consent radius). The bridge (5.1.2's
// consumer, scene_bridge.cpp) reads this for the spawn_object batch budget.
inline constexpr std::uint32_t TERRAIN_GRID_STEPS = 512; // A-148

// ---- A-149: region-edge gradient cap (task 5.1.2) ---------------------------
//
// "40 m per km" — no cliffs at region borders (borders are political, not
// geological). The heightfield's per-octave amplitude is shaped so the
// steepest legal gradient between two adjacent region centers (1 km apart on
// the A-40 grid) stays at or below this cap; the test asserts the bound by
// sampling two neighbors across every internal region edge. Stored as
// metres-per-kilometre (the spec's own unit).
inline constexpr double TERRAIN_REGION_EDGE_GRADIENT_M_PER_KM = 40.0; // A-149

// Convenience: the same constant in metres-per-metre (the unit heightAt's
// slope guard uses internally): 0.04 m/m.
inline constexpr double TERRAIN_REGION_EDGE_GRADIENT = TERRAIN_REGION_EDGE_GRADIENT_M_PER_KM / 1000.0;

// ---- A-153: species silhouette ratios (task 5.1.3) -------------------------
//
// Per-species height/girth ratios for the character silhouette primitives.
// The card: "per-species silhouette primitives (height/girth ratios pinned
// per species in A-153 — nine species, nine rows)". The ratios are
// presentation-only (the card: "they are presentation, not game rules — A-153
// is the exception that proves the §1.3 rule, one row per species, one
// rationale line each").
//
// height_ratio: the body's vertical scale relative to the 0.8 m sphere radius
// baseline (so a height ratio of 2.0 → 1.6 m tall). girth_ratio: the body's
// horizontal scale relative to the same baseline. The nine rows mirror
// species.json ids 0..8 (player, human, beastkin, taur, elf, vampire, dragon,
// goo_beast, wolf).
struct SpeciesSilhouetteRow
{
    std::uint16_t species;
    double heightRatio;
    double girthRatio;
    const char *rationale;
};

inline constexpr std::array<SpeciesSilhouetteRow, 9> SPECIES_SILHOUETTES{{
    {0, 2.0, 1.0, "player: human-scale default"},     // A-153 row 0
    {1, 2.0, 1.0, "human: baseline mortal"},          // A-153 row 1
    {2, 2.2, 1.3, "beastkin: burly anthropomorphic"}, // A-153 row 2
    {3, 2.8, 1.6, "taur: centaur-frame, larger"},     // A-153 row 3
    {4, 2.1, 0.85, "elf: slender, slightly taller"},  // A-153 row 4
    {5, 2.0, 1.0, "vampire: human-frame, varies"},    // A-153 row 5
    {6, 4.0, 2.5, "dragon: largest silhouette"},      // A-153 row 6
    {7, 1.6, 1.4, "goo_beast: squat, amorphous"},     // A-153 row 7
    {8, 1.2, 0.6, "wolf: small quadruped profile"},   // A-153 row 8
}};

// ---- A-154: walk bob rate (task 5.1.3) -------------------------------------
//
// "2.2 Hz at the reference walk speed; phase from traveled distance" — the
// readable gait. The bob is a vertical sine wave whose phase advances with
// the character's traveled distance (not wall time), so the gait is
// speed-true (a stationary character does not bob; a sprinting one bobs
// faster). distance-phase keeps it deterministic (R8: no wall clock).
inline constexpr double WALK_BOB_HZ          = 2.2;  // A-154
inline constexpr double WALK_BOB_AMPLITUDE_M = 0.08; // the visual bob height (presentation-only)

// ---- A-155: sky anchor colors (task 5.1.4) ---------------------------------
//
// Dawn/noon/dusk/night sRGB anchors + season multipliers. The sky gradient
// is keyed to the day/night phase (1.2.2's clock — the same phase the HUD
// prints), interpolated between the two nearest anchors. Season modulates
// the palette (winter desaturates, autumn warms) through the same anchor
// tables.
//
// Linear-RGB, components in [0, 1]. The anchors are presentation-only
// (A-155 is the exception that proves the §1.3 rule for atmosphere).
struct SkyAnchorRow
{
    const char *name;
    std::array<float, 3> color; // Linear-RGB
};

inline constexpr std::array<SkyAnchorRow, 4> SKY_ANCHORS{{
    {"dawn", {0.55f, 0.40f, 0.35f}},  // A-155 dawn (warm horizon)
    {"noon", {0.45f, 0.55f, 0.75f}},  // A-155 noon (blue sky)
    {"dusk", {0.60f, 0.35f, 0.30f}},  // A-155 dusk (sunset reds)
    {"night", {0.04f, 0.05f, 0.08f}}, // A-155 night (deep dark blue)
}};

// The phase anchors' positions on the dayPhase01 [0, 1) circle:
// 0.0 = midnight, 0.25 = sunrise (dawn), 0.5 = noon, 0.75 = sunset (dusk).
// The order MUST match SKY_ANCHORS (dawn, noon, dusk, night) so the
// interpolation walks the anchors in phase order.
inline constexpr std::array<double, 4> SKY_ANCHOR_PHASES{
    0.25, // dawn
    0.5,  // noon
    0.75, // dusk
    0.0   // night (wraps around)
};

// Season multipliers (winter desaturates, autumn warms). Applied to the
// interpolated anchor color. The multipliers are Linear-RGB channel scalars.
struct SeasonTintRow
{
    const char *name;
    std::array<float, 3> multiplier; // per-channel scalar
};

inline constexpr std::array<SeasonTintRow, 4> SEASON_TINTS{{
    {"spring", {1.00f, 1.00f, 1.00f}}, // neutral
    {"summer", {1.05f, 1.02f, 0.95f}}, // warm bright
    {"autumn", {1.10f, 0.95f, 0.85f}}, // warm desaturate
    {"winter", {0.85f, 0.90f, 1.05f}}, // cool desaturate
}};

// ---- A-156: exposure clamp (task 5.1.4) ------------------------------------
//
// "night floor 0.08, day ceiling 1.0 (tone target)" — dark-but-playable
// nights, clipped days. The tone floor exists so night is playable; a
// "realistic" black night fails the capture checklist by design (the card's
// pitfall). The ceiling clips day-bright scenes so the tone target stays
// in [floor, ceiling].
inline constexpr double EXPOSURE_NIGHT_FLOOR = 0.08; // A-156
inline constexpr double EXPOSURE_DAY_CEILING = 1.0;  // A-156

// ---- A-157: bloom strength (task 5.1.4) ------------------------------------
//
// "0.35 for sun disc and torch-range lights only" — bloom as a light cue,
// not a wash. The renderer's bloom pass (already in the tree) is enabled
// for the sun disc and torch-range lights only; its strength pinned here.
inline constexpr float BLOOM_STRENGTH_SUN   = 0.35f; // A-157
inline constexpr float BLOOM_STRENGTH_TORCH = 0.35f; // A-157

// ---- A-150: settlement footprints (task 5.2.1) -----------------------------
//
// "village 8-14 buildings, town 15-30, city 31-60" — buildings per territory
// tier; the density the empty-world pain demands. The range is [min, max];
// the actual count for one territory is drawn from the seeded hash.
struct SettlementFootprintRow
{
    std::uint32_t minBuildings;
    std::uint32_t maxBuildings;
};

inline constexpr SettlementFootprintRow SETTLEMENT_FOOTPRINT_VILLAGE = {8, 14}; // A-150
inline constexpr SettlementFootprintRow SETTLEMENT_FOOTPRINT_TOWN
    = {15, 30}; // A-150 (town = large village, not a separate TerritoryType)
inline constexpr SettlementFootprintRow SETTLEMENT_FOOTPRINT_CITY = {31, 60}; // A-150

// ---- A-151: sea level (task 5.2.1 placement + 5.2.2 water) ------------------
//
// "0 m (heightfield datum; the water plane)" — one datum so placement and
// water agree. The water plane is a single flat plane at y = 0; coasts
// read as coasts where the terrain dips below this level. Scatter placement
// reads A-151 to avoid placing props below sea level (5.2.1's
// TestScatter_NoPropsInWaterOrOnSteepSlope).
inline constexpr double SEA_LEVEL_M = 0.0; // A-151

// ---- A-152: scatter instances per view (task 5.2.1 cap, 5.3.1 gate) --------
//
// "12,000" — the per-view ceiling the budget gate culls against. The scatter
// field produces placements deterministically from (seed, climate, territory
// templates); the view caps the instance count to A-152 so a frame never
// draws more than this. 5.3.1's gate asserts the cap holds.
inline constexpr std::uint32_t SCATTER_INSTANCES_PER_VIEW = 12'000; // A-152

// ---- A-158: border styles (task 5.2.2, 6.2.3 palettes) ---------------------
//
// "owner = faction palette entry; unowned neutral tint; safe zone = double
// ribbon" — the political map on the land. The owner color comes from the
// faction palette (3.1.5's faction data); unowned territories use the
// neutral tint; safe-zone territories use the distinct double-ribbon style
// (the as-built tint vocabulary). Palettes swap values, never keys.
inline constexpr std::array<float, 3> BORDER_NEUTRAL_TINT = {0.55f, 0.50f, 0.45f}; // A-158 unowned
inline constexpr std::array<float, 3> BORDER_SAFE_TINT = {0.30f, 0.85f, 0.40f}; // A-158 safe zone (matches TINT_SAFE)
inline constexpr float BORDER_SAFE_DOUBLE_RIBBON_SPACING_M = 1.5f;              // A-158 the double-ribbon gap

// ---- A-159: border lift (task 5.2.2) ---------------------------------------
//
// "0.35 m above terrain" — one z-fight fix, used everywhere. Ribbons z-fight
// terrain unless lifted; ad-hoc per-call lifts are how borders start floating
// over valleys. The lift is applied uniformly to every ribbon vertex.
inline constexpr double BORDER_LIFT_M = 0.35; // A-159

// ---- A-160 (task 5.3.1 / 7.2.1) --------------------------------------------
//
// "terrain near radius: 1.5 km full resolution; beyond = decimated ring" —
// the standing decimation rule the 5.3.1 ladder codified. Cited by 5.3.1's
// escalation ladder (step 2) and 7.2.1's draw-scale culling. Lands here when
// 5.3.1 lands (the same append-only protocol).
inline constexpr double TERRAIN_NEAR_RADIUS_M = 1500.0; // A-160

// ---- A-150..A-152, A-158..A-160 landing notes -----------------------------
//
// A-150, A-151, A-152, A-158, A-159 land here with tasks 5.2.1 and 5.2.2.
// A-160 is cited by 5.3.1 and 7.2.1 (not yet landed). The remaining
// A-row in this block is A-160, which lands when 5.3.1 lands.

} // namespace harrowmere
