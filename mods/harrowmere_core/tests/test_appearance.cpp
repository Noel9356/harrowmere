// test_appearance.cpp — Task 2.6's pinned test suite.
//
// Implements the four tests pinned in phase-2-alive.md card 2.6's Tests
// block (names verbatim — common.md §3.5):
//
//   * TestAppearance_CostGrowsWithLevel          — 50 + 10*level (A-22/A-23)
//   * TestAppearance_CooldownBlocks              — the A-24 1-game-day slot
//   * TestAppearance_InsufficientXpBlocks        — availableXp, NOT currency
//                                                  (the doc's own comparison;
//                                                  the card's classic
//                                                  misread, pinned here)
//   * TestAppearance_FrequentChangesEmitSuspicion— 3 changes within 10
//                                                  game-days (A-25) fire the
//                                                  suspicion seam
//
// Plus the Done-when's guard as TestAppearance_PowerUnchangedAfterChange:
// a change never touches stats/skills/equipment — the power cache stays
// CLEAN and the score does not move (the doc's hard rule, both sides).
//
// Test harness: doctest v2.4.12; no main here — tests/main.cpp owns the
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "character/appearance.hpp"
#include "character/character.hpp"
#include "character/status.hpp"
#include "core/constants.hpp"
#include "core/enums.hpp"
#include "core/types.hpp"
#include "core/world_context.hpp"
#include "data/registry.hpp"
#include "items/gear.hpp"
#include "items/inventory.hpp"

namespace
{

std::string appearanceDataDir()
{
#ifdef HARROWMERE_CORE_DATA_DIR
    return std::string(HARROWMERE_CORE_DATA_DIR);
#else
    return std::string("mods/harrowmere_core/data");
#endif
}

// A recording suspicion sink: captures the A-25 fires (4.5's future home).
struct RecordingSuspicionSink final : harrowmere::ISuspicionSink
{
    struct Signal
    {
        harrowmere::characterId who;
        std::uint64_t at;
    };
    std::vector<Signal> signals;

    void onAppearanceFrequent(
        harrowmere::characterId who, std::uint64_t at
    ) override
    {
        signals.push_back({who, at});
    }
};

// A recording appearance sink: captures the post-mutation facts (the 0.7
// AppearanceChanged publication).
struct RecordingAppearanceSink final : harrowmere::IAppearanceSink
{
    std::vector<harrowmere::characterId> published;

    void onAppearanceChanged(
        harrowmere::characterId who
    ) override
    {
        published.push_back(who);
    }
};

harrowmere::Character makeCharacter(
    harrowmere::characterId id, std::int64_t xp
)
{
    harrowmere::Character c{};
    c.id                      = id;
    c.progression.availableXp = xp;
    c.body.maxHp              = harrowmere::maxHpForLevel(1);
    c.body.currentHp          = c.body.maxHp;
    return c;
}

// A full-seam context: history + both recording sinks wired.
struct AppearanceFixture
{
    RecordingSuspicionSink suspicion;
    RecordingAppearanceSink appearance;
    harrowmere::AppearanceHistory history;
    harrowmere::WorldContext ctx;

    AppearanceFixture()
    {
        ctx.suspicion         = &suspicion;
        ctx.appearance        = &appearance;
        ctx.appearanceHistory = &history;
        ctx.now               = 1'000'000;
    }
};

} // namespace

// ---- pinned: TestAppearance_CostGrowsWithLevel ------------------------------------

TEST_CASE(
    "TestAppearance_CostGrowsWithLevel"
)
{
    // 50 + 10 * level, hand-computed at the doc's own points: level 1 pays
    // 60, level 10 pays 150, level 75 pays 800 (A-23's "anti-griefing by
    // escalation").
    const auto atLevel = [](std::uint16_t level) {
        harrowmere::Character c = makeCharacter(1, 0);
        c.progression.level     = level;
        return harrowmere::appearanceChangeCost(c);
    };
    CHECK(harrowmere::BASE_APPEARANCE_COST == 50); // A-22
    CHECK(harrowmere::APPEARANCE_COST_RATE == 10); // A-23
    CHECK(atLevel(1) == 60);
    CHECK(atLevel(10) == 150);
    CHECK(atLevel(26) == 310);
    CHECK(atLevel(75) == 800);
}

// ---- pinned: TestAppearance_CooldownBlocks -----------------------------------------

TEST_CASE(
    "TestAppearance_CooldownBlocks"
)
{
    harrowmere::Character c = makeCharacter(1, 10'000);
    AppearanceFixture fx;
    harrowmere::WorldContext &ctx = fx.ctx;

    const std::vector<std::uint64_t> firstLook{1, 2, 3};
    const std::vector<std::uint64_t> secondLook{9, 9};

    // The first change succeeds and arms the A-24 cooldown (the slot holds
    // the expiry stamp: now + 1 game-day).
    REQUIRE(harrowmere::changeAppearance(c, firstLook, ctx).isOk());
    CHECK(c.appearanceComponents == firstLook);
    CHECK(
        c.cooldowns[static_cast<std::size_t>(harrowmere::CooldownType::APPEARANCE)]
        == ctx.now + harrowmere::APPEARANCE_COOLDOWN_GAME_S
    );
    CHECK(harrowmere::cooldownActive(c, harrowmere::CooldownType::APPEARANCE, ctx.now));

    // While the cooldown runs: canChangeAppearance is false and the change
    // refuses with CooldownActive — nothing else moves.
    ctx.now = ctx.now + harrowmere::APPEARANCE_COOLDOWN_GAME_S - 1; // the last blocked second
    CHECK_FALSE(harrowmere::canChangeAppearance(c, ctx.now));
    const auto blocked = harrowmere::changeAppearance(c, secondLook, ctx);
    REQUIRE(blocked.isErr());
    CHECK(blocked.error() == harrowmere::AppearanceError::CooldownActive);
    CHECK(c.appearanceComponents == firstLook);
    CHECK(fx.appearance.published.size() == 1); // no second publication

    // Free exactly AT the expiry stamp (cooldowns hold expiries; the slot is
    // active strictly before it — 2.1's rule).
    ctx.now = ctx.now + 1;
    CHECK(harrowmere::canChangeAppearance(c, ctx.now));
    const std::vector<std::uint64_t> thirdLook{4, 5};
    REQUIRE(harrowmere::changeAppearance(c, thirdLook, ctx).isOk());
    CHECK(c.appearanceComponents == thirdLook);
    CHECK(fx.appearance.published.size() == 2);
}

// ---- pinned: TestAppearance_InsufficientXpBlocks ------------------------------------

TEST_CASE(
    "TestAppearance_InsufficientXpBlocks"
)
{
    // THE card's classic misread, pinned: the cost is paid from
    // availableXp (the doc's own comparison) — a purse FULL of currency and
    // an empty XP balance must STILL refuse.
    harrowmere::Character c = makeCharacter(1, /*xp=*/0);
    c.progression.currency  = 999'999; // tempting, and irrelevant
    harrowmere::WorldContext ctx;
    ctx.now = 50'000;

    CHECK_FALSE(harrowmere::canChangeAppearance(c, ctx.now));
    const std::vector<std::uint64_t> look{7, 7, 7};
    const auto refused = harrowmere::changeAppearance(c, look, ctx);
    REQUIRE(refused.isErr());
    CHECK(refused.error() == harrowmere::AppearanceError::InsufficientXp);
    CHECK(c.appearanceComponents.empty());    // nothing applied
    CHECK(c.progression.currency == 999'999); // currency untouched
    CHECK(c.progression.availableXp == 0);    // XP untouched

    // One XP short refuses; exactly the cost passes and pays FROM availableXp.
    const std::uint64_t cost  = harrowmere::appearanceChangeCost(c); // level 1 -> 60
    c.progression.availableXp = static_cast<std::int64_t>(cost) - 1;
    CHECK_FALSE(harrowmere::canChangeAppearance(c, ctx.now));
    c.progression.availableXp = static_cast<std::int64_t>(cost);
    REQUIRE(harrowmere::changeAppearance(c, look, ctx).isOk());
    CHECK(c.progression.availableXp == 0); // the cost came out of XP, never currency
    CHECK(c.progression.currency == 999'999);

    // And the R4 floor holds: an appearance cannot be paid out of debt —
    // a player at -MAX_XP refuses even though the cost is trivially small.
    harrowmere::Character debtor   = makeCharacter(2, 0);
    debtor.progression.availableXp = -harrowmere::MAX_XP;
    CHECK_FALSE(harrowmere::canChangeAppearance(debtor, ctx.now));
    const std::vector<std::uint64_t> tiny{1};
    CHECK(harrowmere::changeAppearance(debtor, tiny, ctx).error() == harrowmere::AppearanceError::InsufficientXp);
    CHECK(debtor.progression.availableXp == -harrowmere::MAX_XP); // clamped, never below
}

// ---- pinned: TestAppearance_FrequentChangesEmitSuspicion -----------------------------

TEST_CASE(
    "TestAppearance_FrequentChangesEmitSuspicion"
)
{
    harrowmere::Character c = makeCharacter(1, 1'000'000);
    AppearanceFixture fx;
    harrowmere::WorldContext &ctx = fx.ctx;

    const std::uint64_t day    = harrowmere::APPEARANCE_COOLDOWN_GAME_S;        // 86'400
    const std::uint64_t window = harrowmere::APPEARANCE_FREQUENT_WINDOW_GAME_S; // 10 days
    const std::vector<std::uint64_t> look{1};

    // Three changes at one-game-day intervals (the A-24 cooldown is exactly
    // one game-day, so back-to-back changes ride its expiry): all three sit
    // inside the 10-game-day A-25 window.
    REQUIRE(harrowmere::changeAppearance(c, look, ctx).isOk()); // T0
    ctx.now += day;
    REQUIRE(harrowmere::changeAppearance(c, look, ctx).isOk()); // T0 + 1d
    CHECK(fx.suspicion.signals.empty());                        // two changes: not yet "frequent" (A-25 wants 3)

    ctx.now += day;
    REQUIRE(harrowmere::changeAppearance(c, look, ctx).isOk()); // T0 + 2d — the third inside the window
    REQUIRE(fx.suspicion.signals.size() == 1);
    CHECK(fx.suspicion.signals[0].who == c.id);
    CHECK(fx.suspicion.signals[0].at == ctx.now); // the crossing change's stamp

    // The trigger stays honest while the window stays full: a fourth change
    // inside the window fires again (4.5 weighs each signal; no silent
    // latch, and nothing in the plan pins one).
    ctx.now += day;
    REQUIRE(harrowmere::changeAppearance(c, look, ctx).isOk()); // T0 + 3d — window now holds 4
    REQUIRE(fx.suspicion.signals.size() == 2);
    CHECK(fx.suspicion.signals[1].at == ctx.now);

    // Slide past the window with 6-day gaps: the first bundle's stamps age
    // out two at a time. At T0+9d the window still holds 5 stamps (fires);
    // at T0+15d it holds exactly 2 — below the count, NO signal; same at
    // T0+21d.
    ctx.now += day * 6; // T0 + 9d
    REQUIRE(harrowmere::changeAppearance(c, look, ctx).isOk());
    REQUIRE(fx.suspicion.signals.size() == 3);
    CHECK(fx.history.changesWithin(c.id, ctx.now, window) == 5);

    ctx.now += day * 6; // T0 + 15d — T0..T0+3d are 12..15 days old: outside
    REQUIRE(harrowmere::changeAppearance(c, look, ctx).isOk());
    CHECK(fx.suspicion.signals.size() == 3); // the count fell below the trigger: silent
    CHECK(fx.history.changesWithin(c.id, ctx.now, window) == 2);

    ctx.now += day * 6; // T0 + 21d
    const std::uint64_t lastStamp = ctx.now;
    REQUIRE(harrowmere::changeAppearance(c, look, ctx).isOk());
    CHECK(fx.suspicion.signals.size() == 3); // still only 2 in the window
    CHECK(fx.history.changesWithin(c.id, ctx.now, window) == 2);

    // The window arithmetic itself (A-25 counts within 10 game-days):
    // stamps exactly window-old sit OUTSIDE (the window is (now - w, now]).
    ctx.now += day * 8; // T0 + 29d — the T0+15d stamp is 14d old, T0+21d is 8d old
    CHECK(fx.history.changesWithin(c.id, ctx.now, window) == 1);
    CHECK(fx.history.changesWithin(c.id, ctx.now + day * 3, window) == 0);
    CHECK(fx.history.lastChangeTime(c.id) == lastStamp); // the newest stamp, not the reading time
    CHECK(fx.history.lastChangeTime(999) == 0);          // never changed: the "never" sentinel

    // Sparse changes never fire: a fresh character changing once every 9
    // game-days keeps exactly 2 stamps in any 10-day window.
    harrowmere::Character sparse = makeCharacter(2, 1'000'000);
    ctx.now                      = 5'000'000;
    for (int i = 0; i < 6; ++i)
    {
        REQUIRE(harrowmere::changeAppearance(sparse, look, ctx).isOk());
        ctx.now += day * 9;
    }
    CHECK(fx.suspicion.signals.size() == 3); // unchanged by the sparse burst
    for (const auto &signal : fx.suspicion.signals)
        CHECK(signal.who == 1); // no signal names the sparse character
}

// ---- Done-when: appearance never touches stats/skills/equipment ------------------------

TEST_CASE(
    "TestAppearance_PowerUnchangedAfterChange"
)
{
    // The doc's hard rule, pinned from the power side: "Appearance never
    // affects stats, skills, or equipment." A change must not even dirty the
    // cache — the power formula reads none of the fields it touches.
    harrowmere::data::DataRegistry registry;
    std::vector<harrowmere::data::DataError> errors;
    REQUIRE(registry.load(appearanceDataDir(), errors));
    harrowmere::items::ItemInstanceRegistry instances;
    harrowmere::items::InventoryStore bags;

    harrowmere::Character c           = makeCharacter(1, 1'000'000);
    harrowmere::items::Inventory &bag = bags.bag(c.id);
    const std::uint64_t sword         = instances.spawn(registry.itemByKey("iron_sword")->id);

    AppearanceFixture fx;
    harrowmere::WorldContext &ctx = fx.ctx;

    // Some real combat state so the "unchanged" check has something to bite.
    c.progression.stats[static_cast<std::size_t>(harrowmere::StatType::STRENGTH)] = 6;
    c.progression.skills[static_cast<std::size_t>(harrowmere::SkillType::MELEE)]  = 4;
    harrowmere::items::addLooseInstance(bag, sword);
    REQUIRE(harrowmere::equip(c, bag, instances, registry, sword).isOk());

    const std::uint64_t powerBefore    = harrowmere::getCurrentPower(c, registry, instances);
    const std::uint64_t strengthBefore = c.progression.stats[static_cast<std::size_t>(harrowmere::StatType::STRENGTH)];
    const std::uint64_t meleeBefore    = c.progression.skills[static_cast<std::size_t>(harrowmere::SkillType::MELEE)];
    REQUIRE_FALSE(c.powerDirty); // the cache is clean and current

    const std::vector<std::uint64_t> newLook{77, 88, 99};
    REQUIRE(harrowmere::changeAppearance(c, newLook, ctx).isOk());
    CHECK(c.appearanceComponents == newLook); // the look changed...

    CHECK_FALSE(c.powerDirty); // ...and the cache did not even notice
    CHECK(harrowmere::getCurrentPower(c, registry, instances) == powerBefore);
    CHECK(c.progression.stats[static_cast<std::size_t>(harrowmere::StatType::STRENGTH)] == strengthBefore);
    CHECK(c.progression.skills[static_cast<std::size_t>(harrowmere::SkillType::MELEE)] == meleeBefore);
    REQUIRE(c.equippedItems.size() == 1); // equipment untouched
    CHECK(c.equippedItems[0] == sword);
}
