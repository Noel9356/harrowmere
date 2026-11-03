// test_progression.cpp — Task 2.3's pinned test suite.
//
// Implements the eleven tests pinned in phase-2-alive.md card 2.3's Tests
// block (names verbatim — common.md §3.5: "Test names are pinned; keep
// them"):
//
//   * TestProgression_CostTablesMatchClosedForm        — costs[i] = max(base,
//     costs[i-1]*num/den), multiply BEFORE divide (literal spot values)
//   * TestProgression_CapsGrowWithLevel                — caps[i] = 10 + i/5
//   * TestEffectiveness_SoftCapSqrtBeyondThreshold     — the A-12 knee
//   * TestBuyLevel_DeductsAndIncreases_TotalXpUnchanged
//   * TestBuyStat_RefusesAboveCap                      — statCaps[level]
//   * TestRespec_RefundsLiteralLastCost                — D-REFUND-1
//   * TestRespec_BlockedInCombat                       — isPeaceful gate
//   * TestRespec_CooldownActive                        — the cooldown gate
//   * TestMana_EnhancementAboveThresholdNeedsArcana    — D-LORE-1
//   * TestMana_FailureDealsPainDamage                  — the pain formula
//   * TestXpDelta_FloorAtNegativeMaxXp                 — R4's -MAX_XP floor
//
// Plus the skill-side supports (requirements from classes.json, reqActions
// counters through the WorldContext seam) the card's Spec block pins and
// its Done-when leans on. The mana tests reach above the knee through the
// public buyStat by constructing the character state directly — the v1 caps
// bind below the knee (DEVIATIONS.md 2026-10-31), so no legitimate play
// path gets there.
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here.
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <string>
#include <vector>

#include "character/character.hpp"
#include "character/status.hpp"
#include "character/store.hpp"
#include "core/constants.hpp"
#include "core/enums.hpp"
#include "core/math.hpp" // applyXpDelta
#include "core/rng.hpp"  // SeedRng (D-LORE-1's "combat" stream)
#include "core/types.hpp"
#include "core/world_context.hpp"
#include "data/registry.hpp" // DataRegistry (classes.json requirements)
#include "progression/progression.hpp"
#include "world/worldgen.hpp" // NpcSeed (post-plan draw in the requirement test)

namespace fs = std::filesystem;

using harrowmere::Character;
using harrowmere::characterId;
using harrowmere::CooldownType;
using harrowmere::PlayerCreation;
using harrowmere::PurchaseError;
using harrowmere::SkillType;
using harrowmere::StatType;
using harrowmere::WorldContext;
using harrowmere::character::CharacterStore;
using harrowmere::core::SeedRng;
using harrowmere::data::DataRegistry;

namespace
{

// The shipped data directory (test_registry.cpp's resolution order: env,
// baked define, source-tree walk).
fs::path resolveDataDir()
{
    if (const char *env = std::getenv("HARROWMERE_TEST_DATA"))
    {
        if (env[0] != '\0')
            return fs::path(env);
    }
#ifdef HARROWMERE_CORE_DATA_DIR
    return fs::path(HARROWMERE_CORE_DATA_DIR);
#endif
    fs::path probe = fs::current_path();
    for (int i = 0; i < 6; ++i)
    {
        const fs::path candidate = probe / "mods" / "harrowmere_core" / "data";
        if (fs::exists(candidate / "species.json"))
            return candidate;
        probe = probe.parent_path();
    }
    return fs::path("mods") / "harrowmere_core" / "data";
}

// A writable copy of the shipped package with classes.json replaced — the
// requirement tests pin their own class data instead of shipped content.
fs::path makePackageWithClasses(
    const std::string &classesJson, const std::string &tag
)
{
    const fs::path dir = fs::temp_directory_path() / ("harrowmere_progression_" + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::copy(resolveDataDir(), dir, fs::copy_options::recursive);
    std::ofstream out(dir / "classes.json", std::ios::trunc);
    out << classesJson;
    return dir;
}

Character makeCharacter(
    std::uint64_t id, std::uint16_t species
)
{
    Character c{};
    c.id                = id;
    c.species           = species;
    c.progression.level = 1;
    c.body.maxHp        = harrowmere::maxHpForLevel(1);
    c.body.currentHp    = c.body.maxHp;
    return c;
}

WorldContext makeCtx()
{
    WorldContext ctx{};
    ctx.now = 100'000;
    return ctx;
}

} // namespace

// ============================================================================
// Cost & cap tables (design reference §Progression, verbatim)
// ============================================================================

TEST_CASE(
    "TestProgression_CostTablesMatchClosedForm"
)
{
    // The closed form: costs[0] = base; costs[i] = max(base, costs[i-1] *
    // num / den) — multiply BEFORE divide (the plan's #1 progression
    // pitfall: divide-first collapses the 1.15 growth to integers early).
    // Every table spans [0, MAX_LEVEL] — MAX_LEVEL + 1 entries.
    const auto &level = harrowmere::levelCosts();
    const auto &stat  = harrowmere::statCosts();
    const auto &skill = harrowmere::skillCosts();

    CHECK(level.size() == harrowmere::kProgressionTableSize);
    CHECK(stat.size() == harrowmere::kProgressionTableSize);
    CHECK(skill.size() == harrowmere::kProgressionTableSize);

    // Literal spot values (truncating division on the multiply-first form).
    // Level: 100, 115, 132.25->132, 151.8->151, 173.65->173, 198.95->198, ...
    CHECK(level[0] == 100);
    CHECK(level[1] == 115);
    CHECK(level[2] == 132);
    CHECK(level[3] == 151);
    CHECK(level[4] == 173);
    CHECK(level[5] == 198);
    CHECK(level[6] == 227);
    CHECK(level[7] == 261);
    CHECK(level[8] == 300);
    CHECK(level[9] == 345);

    // Stat: 50, 56, 62.72->62, 69.44->69, 77.28->77, 86.24->86, ...
    CHECK(stat[0] == 50);
    CHECK(stat[1] == 56);
    CHECK(stat[2] == 62);
    CHECK(stat[3] == 69);
    CHECK(stat[4] == 77);
    CHECK(stat[5] == 86);
    CHECK(stat[6] == 96);
    CHECK(stat[7] == 107);
    CHECK(stat[8] == 119);
    CHECK(stat[9] == 133);

    // Skill: 80, 92, 105.8->105, 120.75->120, 138, 158.7->158, ...
    CHECK(skill[0] == 80);
    CHECK(skill[1] == 92);
    CHECK(skill[2] == 105);
    CHECK(skill[3] == 120);
    CHECK(skill[4] == 138);
    CHECK(skill[5] == 158);
    CHECK(skill[6] == 181);
    CHECK(skill[7] == 208);
    CHECK(skill[8] == 239);
    CHECK(skill[9] == 274);

    // The full closed form over every entry of every table.
    const auto checkClosedForm
        = [](const std::vector<std::uint64_t> &costs, std::uint64_t base, std::uint64_t num, std::uint64_t den) {
              for (std::size_t i = 1; i < costs.size(); ++i)
              {
                  const std::uint64_t grown    = costs[i - 1] * num / den;
                  const std::uint64_t expected = grown > base ? grown : base;
                  CHECK_MESSAGE(costs[i] == expected, "costs[" << i << "] diverges from the closed form");
              }
          };
    checkClosedForm(
        level, harrowmere::BASE_LEVEL_COST, harrowmere::LEVEL_COST_RATE_NUM, harrowmere::LEVEL_COST_RATE_DEN
    );
    checkClosedForm(stat, harrowmere::BASE_STAT_COST, harrowmere::STAT_COST_RATE_NUM, harrowmere::STAT_COST_RATE_DEN);
    checkClosedForm(
        skill, harrowmere::BASE_SKILL_COST, harrowmere::SKILL_COST_RATE_NUM, harrowmere::SKILL_COST_RATE_DEN
    );

    // A-8's growth sanity: the 1.15 rate keeps growing (never stuck at base).
    CHECK(level[harrowmere::kProgressionTableSize - 1] > level[harrowmere::kProgressionTableSize - 2]);
}

TEST_CASE(
    "TestProgression_CapsGrowWithLevel"
)
{
    // caps[i] = base + i / divisor, i = the LEVEL (design reference §Stats:
    // "stats[statId] never exceeds statCaps[level]"). +1 per 5 levels, 30
    // at cap-level 100 (A-13..A-16).
    const auto &statCaps  = harrowmere::statCaps();
    const auto &skillCaps = harrowmere::skillCaps();

    CHECK(statCaps.size() == harrowmere::kProgressionTableSize);
    CHECK(skillCaps.size() == harrowmere::kProgressionTableSize);

    CHECK(statCaps[0] == 10);
    CHECK(statCaps[1] == 10); // the level-1 starting cap
    CHECK(statCaps[4] == 10);
    CHECK(statCaps[5] == 11);
    CHECK(statCaps[9] == 11);
    CHECK(statCaps[10] == 12);
    CHECK(statCaps[99] == 29);
    CHECK(statCaps[100] == 30);

    for (std::size_t i = 0; i < statCaps.size(); ++i)
    {
        CHECK(statCaps[i] == 10 + i / 5);
        CHECK(skillCaps[i] == 10 + i / 5); // "as stats" (A-15/A-16)
    }

    // Caps never shrink and always allow at least the previous level's cap.
    for (std::size_t i = 1; i < statCaps.size(); ++i)
        CHECK(statCaps[i] >= statCaps[i - 1]);
}

TEST_CASE(
    "TestEffectiveness_SoftCapSqrtBeyondThreshold"
)
{
    // getEffectiveness verbatim: point <= threshold -> point; else
    // threshold + isqrt(point - threshold). STAT_THRESHOLD = 50 (A-12).
    constexpr std::uint64_t t = harrowmere::STAT_THRESHOLD;

    CHECK(harrowmere::getEffectiveness(0, t) == 0);
    CHECK(harrowmere::getEffectiveness(10, t) == 10);
    CHECK(harrowmere::getEffectiveness(t, t) == 50);     // at the knee: face value
    CHECK(harrowmere::getEffectiveness(t + 1, t) == 51); // isqrt(1) = 1
    CHECK(harrowmere::getEffectiveness(59, t) == 53);    // isqrt(9) = 3
    CHECK(harrowmere::getEffectiveness(66, t) == 54);    // isqrt(16) = 4
    CHECK(harrowmere::getEffectiveness(100, t) == 57);   // isqrt(50) = 7
    CHECK(harrowmere::getEffectiveness(150, t) == 60);   // isqrt(100) = 10
    CHECK(harrowmere::getEffectiveness(2500, t) == 99);  // isqrt(2450) = 49

    // Monotone non-decreasing across the knee.
    std::uint64_t prev = 0;
    for (std::uint64_t p = 0; p <= 200; ++p)
    {
        const std::uint64_t eff = harrowmere::getEffectiveness(p, t);
        CHECK(eff >= prev);
        prev = eff;
    }

    // getSkillEffectiveness is the same knee over the SKILL_THRESHOLD.
    Character c                                                       = makeCharacter(1, harrowmere::PLAYER_SPECIES_ID);
    c.progression.skills[static_cast<std::size_t>(SkillType::ARCANA)] = 60;
    CHECK(harrowmere::getSkillEffectiveness(c, SkillType::ARCANA) == 53); // isqrt(10) = 3
}

// ============================================================================
// Levels (design reference §Levels)
// ============================================================================

TEST_CASE(
    "TestBuyLevel_DeductsAndIncreases_TotalXpUnchanged"
)
{
    Character c               = makeCharacter(1, harrowmere::PLAYER_SPECIES_ID);
    c.progression.totalXp     = 5'000;
    c.progression.availableXp = 1'000;
    c.powerDirty              = false; // level changes must NOT dirty the cache (2.5's pin)

    // Manual buy: levelCosts[1] = 115 is the XP from level 1 to 2 (design
    // reference §Levels; the card spec's availableXp -= levelCosts[level]).
    const auto first = harrowmere::buyLevel(c, false);
    REQUIRE(first.isOk());
    CHECK(c.progression.level == 2);
    CHECK(c.progression.availableXp == 1'000 - 115);
    CHECK(c.progression.totalXp == 5'000); // never decreases, never moves on a purchase
    CHECK_FALSE(c.powerDirty);             // markPowerDirty deliberately not fired

    // Another manual buy: levelCosts[2] = 132.
    REQUIRE(harrowmere::buyLevel(c, false).isOk());
    CHECK(c.progression.level == 3);
    CHECK(c.progression.availableXp == 1'000 - 115 - 132);

    // Insufficient XP refuses and changes nothing (levelCosts[3] = 151).
    c.progression.availableXp   = 100;
    const std::int64_t beforeXp = c.progression.availableXp;
    const auto broke            = harrowmere::buyLevel(c, false);
    REQUIRE(broke.isErr());
    CHECK(broke.error() == PurchaseError::InsufficientXp);
    CHECK(c.progression.level == 3);
    CHECK(c.progression.availableXp == beforeXp);
}

TEST_CASE(
    "TestBuyLevel_AutoLoopsAndRespectsCaps"
)
{
    // automatic = the NPC auto-buy loop: buys while affordable, stops at
    // the exact level where the next cost no longer fits.
    Character player               = makeCharacter(1, harrowmere::PLAYER_SPECIES_ID);
    player.progression.availableXp = 115; // exactly the 1->2 cost
    REQUIRE(harrowmere::buyLevel(player, true).isOk());
    CHECK(player.progression.level == 2); // levelCosts[2] = 132 > 0 remaining: loop stopped
    CHECK(player.progression.availableXp == 0);

    // Running out of XP is the loop's normal end (Ok — 3.4 re-reads the
    // level); only the manual path reports InsufficientXp.
    player.progression.availableXp = 10;
    REQUIRE(harrowmere::buyLevel(player, true).isOk());
    CHECK(player.progression.level == 2);

    // The player's lore cap (A-6): late-stage mortal at 75.
    Character capped               = makeCharacter(2, harrowmere::PLAYER_SPECIES_ID);
    capped.progression.level       = harrowmere::PLAYER_MAX_LEVEL - 1;
    capped.progression.availableXp = 1'000'000'000;
    REQUIRE(harrowmere::buyLevel(capped, false).isOk());
    CHECK(capped.progression.level == harrowmere::PLAYER_MAX_LEVEL);
    const auto refused = harrowmere::buyLevel(capped, true);
    REQUIRE(refused.isOk()); // the automatic end at the cap is a success end
    CHECK(capped.progression.level == harrowmere::PLAYER_MAX_LEVEL);
    const auto manualRefused = harrowmere::buyLevel(capped, false);
    REQUIRE(manualRefused.isErr()); // the manual path reports the refusal
    CHECK(manualRefused.error() == PurchaseError::AtLevelCap);

    // NPCs cap at peak-stage 100 (A-6) — species != 0 buys past 75.
    Character npc               = makeCharacter(3, 1);          // human
    npc.progression.level       = harrowmere::PLAYER_MAX_LEVEL; // a player would be done here
    npc.progression.availableXp = 10'000'000'000;
    const auto npcBought        = harrowmere::buyLevel(npc, true);
    REQUIRE(npcBought.isOk());
    CHECK(npc.progression.level == harrowmere::MAX_LEVEL); // bought up to peak-stage
}

// ============================================================================
// Stats (design reference §Stats)
// ============================================================================

TEST_CASE(
    "TestBuyStat_RefusesAboveCap"
)
{
    Character c               = makeCharacter(1, harrowmere::PLAYER_SPECIES_ID);
    c.progression.availableXp = 10'000;
    WorldContext ctx          = makeCtx();

    // statCosts[9] = 133 buys the 10th point; statCaps[1] = 10 (A-13).
    c.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 9;
    c.powerDirty                                                      = false;
    const auto toCap                                                  = harrowmere::buyStat(c, StatType::STRENGTH, ctx);
    REQUIRE(toCap.isOk());
    CHECK(c.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] == 10);
    CHECK(c.progression.availableXp == 10'000 - 133);
    CHECK(c.powerDirty); // purchases mark the power cache dirty

    // One more point would exceed statCaps[level] = 10: refused, nothing moves.
    const auto overCap = harrowmere::buyStat(c, StatType::STRENGTH, ctx);
    REQUIRE(overCap.isErr());
    CHECK(overCap.error() == PurchaseError::AtCap);
    CHECK(c.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] == 10);
    CHECK(c.progression.availableXp == 10'000 - 133);

    // A higher level raises the cap (caps grow with level, A-14).
    c.progression.level = 5; // statCaps[5] = 11
    REQUIRE(harrowmere::buyStat(c, StatType::STRENGTH, ctx).isOk());
    CHECK(c.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] == 11);

    // Insufficient XP refuses before any side effect (statCosts[11] = 165).
    c.progression.availableXp = 100;
    const auto broke          = harrowmere::buyStat(c, StatType::STRENGTH, ctx);
    REQUIRE(broke.isErr());
    CHECK(broke.error() == PurchaseError::InsufficientXp);
    CHECK(c.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] == 11);
    CHECK(c.progression.availableXp == 100);
}

// ============================================================================
// Respec (D-REFUND-1 + the peaceful/cooldown gates)
// ============================================================================

TEST_CASE(
    "TestRespec_RefundsLiteralLastCost"
)
{
    // D-REFUND-1: the refund for point N is exactly costs[N-1] — the LAST
    // point's cost, NOT the sum of the ladder. Suspected design gap,
    // implemented as written, pinned here; DEVIATIONS.md carries the flag.
    Character c               = makeCharacter(1, harrowmere::PLAYER_SPECIES_ID);
    c.progression.availableXp = 10'000;
    WorldContext ctx          = makeCtx();

    // Buy STRENGTH 0 -> 3: costs 50 + 56 + 62 = 168.
    for (int i = 0; i < 3; ++i)
        REQUIRE(harrowmere::buyStat(c, StatType::STRENGTH, ctx).isOk());
    CHECK(c.progression.availableXp == 10'000 - 168);

    // Respec refunds ONLY statCosts[2] = 62 (not 168).
    const auto res = harrowmere::respecStat(c, StatType::STRENGTH, ctx);
    REQUIRE(res.isOk());
    CHECK(c.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] == 0);
    CHECK(c.progression.availableXp == 10'000 - 168 + 62);
    CHECK(c.powerDirty);

    // The cooldown armed (RESPEC_COOLDOWN_GAME_S, constants.hpp).
    CHECK(
        c.cooldowns[static_cast<std::size_t>(CooldownType::RESPEC_STAT)] == ctx.now + harrowmere::RESPEC_COOLDOWN_GAME_S
    );

    // Skills refund through the same literal rule ("getRefund is used on
    // stat and skill"): MELEE 0 -> 2 costs 80 + 92 = 172; refund = 92.
    Character s               = makeCharacter(2, harrowmere::PLAYER_SPECIES_ID);
    s.progression.availableXp = 10'000;
    for (int i = 0; i < 2; ++i)
        REQUIRE(harrowmere::buySkill(s, SkillType::MELEE, ctx).isOk());
    REQUIRE(harrowmere::respecSkill(s, SkillType::MELEE, ctx).isOk());
    CHECK(s.progression.skills[static_cast<std::size_t>(SkillType::MELEE)] == 0);
    CHECK(s.progression.availableXp == 10'000 - 172 + 92);

    // getRefund itself: point 0 refunds 0 (the doc's own guard).
    CHECK(harrowmere::getRefund(harrowmere::statCosts(), 0) == 0);
    CHECK(harrowmere::getRefund(harrowmere::statCosts(), 1) == 50);
    CHECK(harrowmere::getRefund(harrowmere::statCosts(), 3) == 62);
}

TEST_CASE(
    "TestRespec_BlockedInCombat"
)
{
    Character c                                                      = makeCharacter(1, harrowmere::PLAYER_SPECIES_ID);
    c.progression.stats[static_cast<std::size_t>(StatType::AGILITY)] = 3;
    c.progression.availableXp                                        = 10'000;

    WorldContext ctx = makeCtx();
    ctx.now          = 100'000;
    c.lastCombatTime = ctx.now - 5; // inside the 30-game-second window (A-4)

    const auto res = harrowmere::respecStat(c, StatType::AGILITY, ctx);
    REQUIRE(res.isErr());
    CHECK(res.error() == PurchaseError::NotPeaceful);
    CHECK(c.progression.stats[static_cast<std::size_t>(StatType::AGILITY)] == 3); // untouched
    CHECK(c.progression.availableXp == 10'000);                                   // no refund, no charge
    CHECK(c.cooldowns[static_cast<std::size_t>(CooldownType::RESPEC_STAT)] == 0); // nothing armed

    // Outside events but arrested (4.3's seam) is equally not peaceful.
    c.lastCombatTime = 0;
    struct StubJustice final : harrowmere::JusticeSink
    {
        bool isArrested(
            characterId
        ) const override
        {
            return true;
        }
    } justice;
    ctx.justice       = &justice;
    const auto jailed = harrowmere::respecSkill(c, SkillType::MELEE, ctx);
    REQUIRE(jailed.isErr());
    CHECK(jailed.error() == PurchaseError::NotPeaceful);
}

TEST_CASE(
    "TestRespec_CooldownActive"
)
{
    Character c                                                      = makeCharacter(1, harrowmere::PLAYER_SPECIES_ID);
    c.progression.stats[static_cast<std::size_t>(StatType::AGILITY)] = 1;
    c.progression.availableXp                                        = 10'000;

    WorldContext ctx = makeCtx();
    ctx.now          = 100'000;

    // An armed cooldown blocks even a peaceful respec.
    c.cooldowns[static_cast<std::size_t>(CooldownType::RESPEC_STAT)] = ctx.now + 1'000;
    const auto blocked = harrowmere::respecStat(c, StatType::AGILITY, ctx);
    REQUIRE(blocked.isErr());
    CHECK(blocked.error() == PurchaseError::CooldownActive);
    CHECK(c.progression.stats[static_cast<std::size_t>(StatType::AGILITY)] == 1);

    // At the expiry stamp the cooldown no longer blocks (design reference:
    // "cooldowns[cooldownType] <= worldTime" may respec).
    c.cooldowns[static_cast<std::size_t>(CooldownType::RESPEC_STAT)] = ctx.now;
    REQUIRE(harrowmere::respecStat(c, StatType::AGILITY, ctx).isOk());

    // The respec armed its own cooldown: an immediate second one blocks.
    c.progression.stats[static_cast<std::size_t>(StatType::AGILITY)] = 1;
    const auto again = harrowmere::respecStat(c, StatType::AGILITY, ctx);
    REQUIRE(again.isErr());
    CHECK(again.error() == PurchaseError::CooldownActive);

    // The skill cooldown is an independent slot.
    CHECK_FALSE(harrowmere::cooldownActive(c, CooldownType::RESPEC_SKILL, ctx.now));
    c.progression.skills[static_cast<std::size_t>(SkillType::MELEE)] = 1;
    REQUIRE(harrowmere::respecSkill(c, SkillType::MELEE, ctx).isOk());
    CHECK(harrowmere::cooldownActive(c, CooldownType::RESPEC_SKILL, ctx.now));
    CHECK_FALSE(harrowmere::cooldownActive(c, CooldownType::RESPEC_STAT, ctx.now + harrowmere::RESPEC_COOLDOWN_GAME_S));
}

// ============================================================================
// D-LORE-1: the mana rule (card 2.3)
// ============================================================================

TEST_CASE(
    "TestMana_EnhancementAboveThresholdNeedsArcana"
)
{
    // Above STAT_THRESHOLD the ARCANA effectiveness must cover
    // point - STAT_THRESHOLD. The v1 caps bind below the knee, so the test
    // constructs the above-knee state directly (a future cap raise reaches
    // it in play — see DEVIATIONS.md 2026-10-31).
    Character c                 = makeCharacter(1, harrowmere::PLAYER_SPECIES_ID);
    const auto intIdx           = static_cast<std::size_t>(StatType::INTELLIGENCE);
    c.progression.stats[intIdx] = 55; // target point 56 > 50: required arcana 6
    c.progression.availableXp   = 10'000'000;
    WorldContext ctx            = makeCtx();

    SeedRng rng;
    rng.seedAll(42);
    ctx.rng = &rng;

    // Shortfall 6 -> failure chance min(SCALE, 6*2000) = SCALE: ALWAYS
    // fails, whatever the draw. The XP for the attempted point is consumed
    // and the pain lands — the purchase does not complete.
    const std::int64_t beforeXp  = c.progression.availableXp;
    const std::uint64_t beforeHp = c.body.currentHp;
    const auto res               = harrowmere::buyStat(c, StatType::INTELLIGENCE, ctx);
    REQUIRE(res.isErr());
    CHECK(res.error() == PurchaseError::ManaBacklash);
    CHECK(c.progression.stats[intIdx] == 55); // the point was NOT bought
    CHECK(c.progression.availableXp == beforeXp - static_cast<std::int64_t>(harrowmere::statCosts()[55]));
    CHECK(c.body.currentHp < beforeHp); // pain dealt (exact value in the next test)

    // With sufficient ARCANA there is no backlash at all: no damage, no XP
    // movement (the purchase then runs into the v1 cap, which refuses
    // cleanly — the knee's risk, not the cap, is what this test pins).
    Character learned                 = makeCharacter(2, harrowmere::PLAYER_SPECIES_ID);
    learned.progression.stats[intIdx] = 55;
    learned.progression.skills[static_cast<std::size_t>(SkillType::ARCANA)] = 6; // eff 6 == required 6
    learned.progression.availableXp                                         = 10'000'000;
    const auto clean = harrowmere::buyStat(learned, StatType::INTELLIGENCE, ctx);
    REQUIRE(clean.isErr());
    CHECK(clean.error() == PurchaseError::AtCap);         // the cap refuses AFTER a clean enhancement
    CHECK(learned.body.currentHp == learned.body.maxHp);  // no pain
    CHECK(learned.progression.availableXp == 10'000'000); // no XP consumed

    // Below the knee no arcana is required at all (covered by the plain
    // purchase tests above; asserted here against the pure helper too).
    CHECK(harrowmere::requiredArcana(50) == 0);
    CHECK(harrowmere::requiredArcana(56) == 6);
}

TEST_CASE(
    "TestMana_FailureDealsPainDamage"
)
{
    // The pain formula: maxHp * (5 + shortfall/500) / 100 — integer
    // division inside the parens is the doc's own form.
    CHECK(harrowmere::manaPainDamage(120, 6) == 6); // 120 * 5 / 100 (6/500 -> +0%)
    CHECK(harrowmere::manaPainDamage(120, 0) == 0);
    CHECK(harrowmere::manaPainDamage(1600, 500) == 96); // (5 + 500/500)% = 6% of 1600
    CHECK(harrowmere::manaFailureChance(1) == 2'000);
    CHECK(harrowmere::manaFailureChance(5) == 10'000);
    CHECK(harrowmere::manaFailureChance(6) == 10'000);      // min(SCALE, ...) saturates
    CHECK(harrowmere::manaFailureChance(50'000) == 10'000); // no overflow, saturation

    // End to end through buyStat: shortfall 6, maxHp 120 -> exactly 6 pain.
    Character c                 = makeCharacter(1, harrowmere::PLAYER_SPECIES_ID);
    const auto intIdx           = static_cast<std::size_t>(StatType::INTELLIGENCE);
    c.progression.stats[intIdx] = 55;
    c.progression.availableXp   = 10'000'000;
    c.body.currentHp            = c.body.maxHp; // 120

    WorldContext ctx = makeCtx();
    SeedRng rng;
    rng.seedAll(7);
    ctx.rng = &rng;

    REQUIRE(harrowmere::buyStat(c, StatType::INTELLIGENCE, ctx).isErr());
    CHECK(c.body.currentHp == c.body.maxHp - 6);

    // Pain can KILL (the doc: "failing can cause pain to death") — currentHp
    // floors at 0 (u64, never wraps); the death routing itself is 3.3's
    // environment-death path.
    Character doomed                 = makeCharacter(2, harrowmere::PLAYER_SPECIES_ID);
    doomed.progression.stats[intIdx] = 55;
    doomed.progression.availableXp   = 10'000'000;
    doomed.body.currentHp            = 3;
    REQUIRE(harrowmere::buyStat(doomed, StatType::INTELLIGENCE, ctx).isErr());
    CHECK(doomed.body.currentHp == 0);
}

TEST_CASE(
    "TestXpDelta_FloorAtNegativeMaxXp"
)
{
    // R4: applyXpDelta floors at exactly -MAX_XP, never below (A-2).
    // applyXpDelta is a global-scope helper (core/math.hpp, task 0.2).
    constexpr std::int64_t MAX_XP = harrowmere::MAX_XP;

    CHECK(applyXpDelta(0, -100) == -100);
    CHECK(applyXpDelta(100, -1'000) == -900);    // a plain negative balance is legal (debt)
    CHECK(applyXpDelta(-MAX_XP, -1) == -MAX_XP); // the floor holds
    CHECK(applyXpDelta(-MAX_XP + 5, -100) == -MAX_XP);
    CHECK(applyXpDelta(0, -MAX_XP - 5) == -MAX_XP); // an oversized step clamps
    CHECK(applyXpDelta(-MAX_XP, 1) == -MAX_XP + 1); // climbing back out works
    CHECK(applyXpDelta(50, 50) == 100);             // the upward side has no game clamp
}

// ============================================================================
// Skill requirements (§Skills: "stats, other skills, or actions")
// ============================================================================

namespace
{

// One warrior row carrying the card's own example: MELEE needs STRENGTH 5;
// one artisan row requiring two crafts via the reqActions counter.
constexpr const char *kRequirementClassesJson = R"([
    { "id": 1, "key": "commoner", "statWeights": {}, "skillWeights": {}, "startingSkills": {} },
    { "id": 2, "key": "warrior",
      "statWeights": {}, "skillWeights": {}, "startingSkills": {},
      "skillRequirements": {
          "MELEE": { "reqStats": { "STRENGTH": 5 }, "reqSkills": {}, "reqActions": [] }
      } },
    { "id": 3, "key": "hunter",
      "statWeights": {}, "skillWeights": {}, "startingSkills": {},
      "skillRequirements": {
          "SURVIVAL": { "reqStats": {}, "reqSkills": { "MELEE": 2 }, "reqActions": ["kill_non_sapient_10", "craft_2"] }
      } }
])";

// Builds the temp package and a fresh DataRegistry over it (DataRegistry is
// non-copyable and each test needs its own instance).
std::unique_ptr<DataRegistry> loadRequirementRegistry(
    const std::string &tag
)
{
    const fs::path pkg = makePackageWithClasses(kRequirementClassesJson, tag);
    auto registry      = std::make_unique<DataRegistry>();
    std::vector<harrowmere::data::DataError> errors;
    REQUIRE_MESSAGE(registry->load(pkg.string(), errors), "requirement registry load failed");
    return registry;
}

} // namespace

TEST_CASE(
    "TestProgression_SkillRequirementsGatePurchase"
)
{
    // The character's classes.json row gates the purchase: the card's own
    // example — MELEE requires STRENGTH 5.
    auto registry = loadRequirementRegistry("gates");

    CharacterStore store;
    store.adoptWorldGen(1, harrowmere::Position{0.0, 0.0, 0.0});
    PlayerCreation creation;
    creation.name       = "tester";
    creation.classKey   = "warrior";
    const auto playerId = store.createPlayer(creation);
    REQUIRE(playerId != 0);

    Character &player              = store.get(playerId);
    player.progression.availableXp = 10'000;

    WorldContext ctx = makeCtx();
    ctx.data         = registry.get();
    ctx.characters   = &store;

    // STR 4 < 5: requirement unmet, nothing moves.
    player.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 4;
    const auto blocked = harrowmere::buySkill(player, SkillType::MELEE, ctx);
    REQUIRE(blocked.isErr());
    CHECK(blocked.error() == PurchaseError::RequirementUnmet);
    CHECK(player.progression.skills[static_cast<std::size_t>(SkillType::MELEE)] == 0);
    CHECK(player.progression.availableXp == 10'000);

    // STR 5: the gate opens; the purchase costs skillCosts[0] = 80 and
    // marks the power cache dirty.
    player.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 5;
    player.powerDirty                                                      = false;
    const auto bought = harrowmere::buySkill(player, SkillType::MELEE, ctx);
    REQUIRE(bought.isOk());
    CHECK(player.progression.skills[static_cast<std::size_t>(SkillType::MELEE)] == 1);
    CHECK(player.progression.availableXp == 10'000 - 80);
    CHECK(player.powerDirty);

    // A classless character (no class key) is unconstrained in v1: the NPC
    // buys the same skill with STR 0.
    harrowmere::world::NpcSeed seed;
    seed.id                     = 0; // post-plan draw
    seed.species                = 1;
    seed.level                  = 1;
    const auto npcId            = store.createNpc(seed);
    Character &npc              = store.get(npcId);
    npc.progression.availableXp = 10'000;
    const auto npcBought        = harrowmere::buySkill(npc, SkillType::MELEE, ctx);
    REQUIRE(npcBought.isOk());
    CHECK(npc.progression.skills[static_cast<std::size_t>(SkillType::MELEE)] == 1);

    // Skill caps still bind (skillCaps[1] = 10) — the gate is additional.
    npc.progression.skills[static_cast<std::size_t>(SkillType::MELEE)] = 10;
    const auto capped = harrowmere::buySkill(npc, SkillType::MELEE, ctx);
    REQUIRE(capped.isErr());
    CHECK(capped.error() == PurchaseError::AtCap);
}

TEST_CASE(
    "TestProgression_SkillRequirements_ReqActionsCounters"
)
{
    // reqActions read the counter map through WorldContext::actionCounters
    // (NpcState::actionCounters for NPCs / the player action log for the
    // player). A "<counter>_<count>" entry needs the counter at >= count;
    // a null counter map reads as all zeros (fail closed).
    auto registry = loadRequirementRegistry("reqactions");

    CharacterStore store;
    store.adoptWorldGen(1, harrowmere::Position{0.0, 0.0, 0.0});
    PlayerCreation creation;
    creation.name       = "hunter";
    creation.classKey   = "hunter";
    const auto playerId = store.createPlayer(creation);

    Character &player                                                     = store.get(playerId);
    player.progression.availableXp                                        = 10'000;
    player.progression.skills[static_cast<std::size_t>(SkillType::MELEE)] = 2; // reqSkills met

    WorldContext ctx = makeCtx();
    ctx.data         = registry.get();
    ctx.characters   = &store;

    // No counters wired: both reqActions entries unmet.
    CHECK_FALSE(harrowmere::skillRequirementsMet(player, SkillType::SURVIVAL, ctx));

    std::map<std::string, std::uint64_t> counters;
    ctx.actionCounters = &counters;

    // Partial: kills met, crafts not.
    counters["kill_non_sapient"] = 10;
    CHECK_FALSE(harrowmere::skillRequirementsMet(player, SkillType::SURVIVAL, ctx));
    const auto blocked = harrowmere::buySkill(player, SkillType::SURVIVAL, ctx);
    REQUIRE(blocked.isErr());
    CHECK(blocked.error() == PurchaseError::RequirementUnmet);

    // Both met: the gate opens (kill_non_sapient_10 + craft_2).
    counters["craft"] = 2;
    CHECK(harrowmere::skillRequirementsMet(player, SkillType::SURVIVAL, ctx));
    REQUIRE(harrowmere::buySkill(player, SkillType::SURVIVAL, ctx).isOk());
    CHECK(player.progression.skills[static_cast<std::size_t>(SkillType::SURVIVAL)] == 1);
}
