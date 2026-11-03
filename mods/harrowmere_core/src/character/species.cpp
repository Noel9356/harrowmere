// species.cpp — species application (task 2.2). Every rule below reads the
// DataRegistry's species rows; no species id, key, or name appears in code
// (the card's grep contract — species behavior is data).

#include "character/species.hpp"

#include "data/registry.hpp" // DataRegistry (species rows)

namespace harrowmere
{

bool isSapient(
    const data::DataRegistry &data, speciesId species
)
{
    // design reference verbatim:
    //   isSapient(speciesId):
    //       return speciesData[speciesId].sapient
    // Release-mode unknown id: the registry's deterministic zero row reads
    // sapient = false (an unknown id is a data bug, 1.3's contract).
    return data.species(species).sapient;
}

std::uint64_t getStatEffectiveness(
    const data::DataRegistry &data, const Character &c, StatType stat
)
{
    // design reference §Species, verbatim:
    //   getStatEffectiveness(character, statId):
    //       effectiveness = getEffectiveness(character.progression.stats[statId], STAT_THRESHOLD)
    //       return effectiveness * speciesData[character.species].statMultiplier[statId] / SCALE
    const std::uint64_t point         = c.progression.stats[static_cast<std::size_t>(stat)];
    const std::uint64_t effectiveness = getEffectiveness(point, STAT_THRESHOLD);
    const std::uint64_t multiplier    = data.species(c.species).multiplier(stat);
    // Multiply BEFORE divide (the doc's form). v1 caps bind the point at 30
    // and the multipliers are small SCALE-grid values — the product is far
    // inside u64 (no mulDiv128 needed; R3 covers triple SCALE products).
    return effectiveness * multiplier / SCALE;
}

std::uint64_t effectiveLifespan(
    const data::DataRegistry &data, const Character &c
)
{
    // A-5: lifespan * (SCALE + level * 100) / SCALE — the doc line
    // "lifespan increases as level increases": linear, up to 2x at level
    // 100 (60-year humans reach 120; 1000-year elves reach 2000). Years,
    // the species row's own unit — 3.4 compares NpcState::ageYears().
    // Widest shipped product: 1,000,000 (the "unbounded" player/goo rows)
    // * 20,000 = 2e10, far inside u64.
    const std::uint64_t lifespan    = data.species(c.species).lifespan_years;
    const std::uint64_t levelFactor = SCALE + static_cast<std::uint64_t>(c.progression.level) * 100ull;
    return lifespan * levelFactor / SCALE;
}

std::int64_t speciesBiasToward(
    std::uint64_t selfXenophobiaBias, std::int64_t territoryCultureBias
)
{
    // The data-driven core of the doc's bias read ("xenophobiaBias +
    // territory culture bias"): the self species row's xenophobiaBias term
    // plus the territory's cultureBias term toward the other species
    // (laws.json cultureBias, resolved by the caller's territory — the
    // lawsAt walk is 3.9's). Both terms are SCALE-grid; the sum keeps the
    // grid (i64 so a negative culture term can pull the bias down). Task
    // 3.9 wraps this with the NpcState adapter (DEVIATIONS.md 2026-10-31
    // row: the card's NpcState-taking signature joins 3.4's NpcState).
    return static_cast<std::int64_t>(selfXenophobiaBias) + territoryCultureBias;
}

} // namespace harrowmere
