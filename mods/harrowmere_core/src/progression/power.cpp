// power.cpp — the cached power pipeline and the matchmaking score (task 2.5).
//
// The A-20/A-21 weights live in constants.hpp as named scalars (the leaf-
// header rule); this file builds the enum-indexed arrays from them and
// static_asserts the pairing so a constants edit that forgets a stat fails
// at compile time, not in a soak.

#include "progression/power.hpp"

#include <array>
#include <cassert>

#include "character/species.hpp"       // getStatEffectiveness (2.2)
#include "data/registry.hpp"           // DataRegistry (species rows, item rows)
#include "items/gear.hpp"              // getGearScore (2.4)
#include "progression/progression.hpp" // getSkillEffectiveness (2.3, §Skills block)

namespace harrowmere
{

namespace
{

// A-20: statWeight in StatType declaration order (STR 12000, DEF 10000,
// AGI 8000, INT 10000, END 6000, PER 6000) — built from the constants.hpp
// rows so the registry stays the single home of the numbers.
constexpr std::array<std::uint64_t, static_cast<std::size_t>(StatType::COUNT)> kStatWeight = {
    STAT_WEIGHT_STRENGTH,
    STAT_WEIGHT_DEFENCE,
    STAT_WEIGHT_AGILITY,
    STAT_WEIGHT_INTELLIGENCE,
    STAT_WEIGHT_ENDURANCE,
    STAT_WEIGHT_PERCEPTION,
};
static_assert(
    kStatWeight[static_cast<std::size_t>(StatType::STRENGTH)] == 12000
        && kStatWeight[static_cast<std::size_t>(StatType::PERCEPTION)] == 6000,
    "A-20 statWeight must stay paired with the constants.hpp rows (the enum order is the array order)"
);

// A-21: skillWeight — 6000 for all eight skills.
constexpr std::array<std::uint64_t, static_cast<std::size_t>(SkillType::COUNT)> kSkillWeight = [] {
    std::array<std::uint64_t, static_cast<std::size_t>(SkillType::COUNT)> w{};
    w.fill(SKILL_WEIGHT_ALL);
    return w;
}();

} // namespace

std::uint64_t getCurrentPower(
    Character &c, const data::DataRegistry &data, const items::ItemInstanceRegistry &instances
)
{
    // The cache read FIRST — a clean flag returns the stored total without
    // touching anything (TestPower_CacheReturnsUntilDirty).
    if (!c.powerDirty)
        return c.cachedPower;

    // design reference verbatim:
    //   total = getGearScore(player)
    //         + Σ statWeight[st]  * getStatEffectiveness(player, st)  / SCALE
    //         + Σ skillWeight[sk] * getSkillEffectiveness(player, sk) / SCALE
    std::uint64_t total = getGearScore(data, instances, c);

    for (std::size_t st = 0; st < static_cast<std::size_t>(StatType::COUNT); ++st)
        total += kStatWeight[st] * getStatEffectiveness(data, c, static_cast<StatType>(st)) / SCALE;

    for (std::size_t sk = 0; sk < static_cast<std::size_t>(SkillType::COUNT); ++sk)
        total += kSkillWeight[sk] * getSkillEffectiveness(c, static_cast<SkillType>(sk)) / SCALE;

    // The watermark write-back: peak only grows (the anti-sandbagging
    // guarantee TestPower_PeakNeverDecreases pins), the cache stores, the
    // flag clears — in exactly the doc's order.
    c.cachedPower = total;
    if (total > c.peakPower)
        c.peakPower = total;
    c.powerDirty = false;
    return total;
}

std::uint64_t getMatchmakingScore(
    const Character &c, const data::DataRegistry &data
)
{
    // design reference §Matchmaking, verbatim:
    //   getMatchmakingScore(player):
    //       return (8 * player.peakPower + 2 * getPotentialPower(player)) / 10
    // The 8:2 split is the whole formula — the weights are the doc's, the
    // denominator their sum (TestMatchmaking_WeightsEightToTwo pins the
    // blend, not the divisor).
    (void)data; // reserved for data-driven tuning (see power.hpp); v1 reads the character only
    return (8 * c.peakPower + 2 * getPotentialPower(c)) / 10;
}

} // namespace harrowmere
