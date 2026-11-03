// test_status.cpp — Task 2.1's pinned test suite.
//
// Implements the six tests pinned in phase-2-alive.md card 2.1's Tests
// block (names verbatim — common.md §3.5: "Test names are pinned; keep
// them"):
//
//   * TestStatus_InEventFollowsEventId      — IN_EVENT derives from eventId
//   * TestStatus_InCombatWindowExpires      — IN_COMBAT = lastCombatTime +
//                                             COMBAT_TIMEOUT_S (A-4), with
//                                             the 0-stamp "never fought" rule
//   * TestStatus_ArrestedReadsJustice       — ARRESTED reads the justice
//                                             seam (null = not arrested)
//   * TestConsent_DuelConsentOnlyBetweenPair— duelOpponent consent is the
//                                             pair's, nobody else's
//   * TestConsent_ArenaSharedId             — shared non-zero arenaId
//                                             consents; 0 never does
//   * TestCooldown_ActiveUntilExpiry        — active strictly before the
//                                             expiry stamp, free at it
//
// Plus two card-Done-when supports: the IN_WAR null-false seam reading (the
// same pattern ArrestedReadsJustice pins for justice) and the statusFlags
// negative (nothing derives a stored flag from the four statuses — the
// grep-review half of the Done-when, pinned at runtime).
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here —
// tests/main.cpp owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <cstdint>

#include "character/character.hpp"
#include "character/status.hpp"
#include "core/constants.hpp" // COMBAT_TIMEOUT_S (A-4)
#include "core/enums.hpp"     // StatusType, CooldownType
#include "core/types.hpp"     // characterId

using harrowmere::Character;
using harrowmere::characterId;
using harrowmere::CooldownType;
using harrowmere::factionId;
using harrowmere::StatusType;
using harrowmere::WorldContext;

namespace
{

// A stub war registry (6.2's seam): a fixed set of combatant factions.
// Deterministic, no RNG, no state beyond the answer.
struct StubWarRegistry final : harrowmere::IWarRegistry
{
    factionId combatant = 0;

    bool isCombatant(
        factionId faction
    ) const override
    {
        return faction == combatant;
    }
};

// A stub justice query (4.3's seam): one jailed character id. JusticeSink is
// the one justice seam face (3.3's D-SINK-1 merge); isArrested rides it, and
// the write-side virtuals keep their null-default bodies.
struct StubJusticeQuery final : harrowmere::JusticeSink
{
    characterId jailed = 0;

    bool isArrested(
        characterId id
    ) const override
    {
        return id == jailed;
    }
};

Character makeCharacter(
    characterId id
)
{
    Character c{};
    c.id = id;
    return c;
}

} // namespace

TEST_CASE(
    "TestStatus_InEventFollowsEventId"
)
{
    Character c = makeCharacter(1);

    WorldContext ctx;
    ctx.now = 1'000;

    // eventId 0 = not in an event (the doc field default).
    CHECK_FALSE(harrowmere::hasStatus(c, StatusType::IN_EVENT, ctx));

    // Any non-zero eventId IS the in-event state (design reference note:
    // "Setting IN_EVENT always pairs with eventId").
    c.eventId = 77;
    CHECK(harrowmere::hasStatus(c, StatusType::IN_EVENT, ctx));

    // Leaving the event clears the id and the derived status follows —
    // no flag writes anywhere (the eventId IS the state).
    c.eventId = 0;
    CHECK_FALSE(harrowmere::hasStatus(c, StatusType::IN_EVENT, ctx));

    // The derived status is never stored: statusFlags stays untouched
    // (D-CHAR-1).
    CHECK(c.statusFlags == 0);
}

TEST_CASE(
    "TestStatus_InCombatWindowExpires"
)
{
    Character c = makeCharacter(1);

    WorldContext ctx;
    ctx.now = 10'000;

    // Never fought (lastCombatTime == 0, the doc default and 3.3's respawn
    // reset): never in combat, even in the epoch's first seconds.
    c.lastCombatTime = 0;
    ctx.now          = 10; // early game-time where 10 - 0 would read as "in combat" without the guard
    CHECK_FALSE(harrowmere::hasStatus(c, StatusType::IN_COMBAT, ctx));

    // A fresh stamp opens the 30-game-second window (A-4).
    ctx.now          = 10'000;
    c.lastCombatTime = 10'000;
    CHECK(harrowmere::hasStatus(c, StatusType::IN_COMBAT, ctx));

    // 29 game-seconds in: still in combat.
    ctx.now = c.lastCombatTime + harrowmere::COMBAT_TIMEOUT_S - 1;
    CHECK(harrowmere::hasStatus(c, StatusType::IN_COMBAT, ctx));

    // At exactly 30 game-seconds the window has expired.
    ctx.now = c.lastCombatTime + harrowmere::COMBAT_TIMEOUT_S;
    CHECK_FALSE(harrowmere::hasStatus(c, StatusType::IN_COMBAT, ctx));

    // Long after: peaceful.
    ctx.now = c.lastCombatTime + harrowmere::COMBAT_TIMEOUT_S * 100;
    CHECK_FALSE(harrowmere::hasStatus(c, StatusType::IN_COMBAT, ctx));

    // A future stamp (clock reset / stale save) is not a combat window:
    // no underflow read, deterministically not in combat.
    ctx.now = c.lastCombatTime - 1;
    CHECK_FALSE(harrowmere::hasStatus(c, StatusType::IN_COMBAT, ctx));
}

TEST_CASE(
    "TestStatus_ArrestedReadsJustice"
)
{
    Character free   = makeCharacter(1);
    Character jailed = makeCharacter(2);

    WorldContext ctx;
    ctx.now = 1'000;

    // Null justice seam (4.3 not landed — D-SINK-1 null-false): nobody is
    // arrested, deterministically.
    CHECK_FALSE(harrowmere::hasStatus(free, StatusType::ARRESTED, ctx));
    CHECK_FALSE(harrowmere::hasStatus(jailed, StatusType::ARRESTED, ctx));

    // With a justice store present, the query reads per character id —
    // jail state lives in justice's map, never on the Character (design
    // rule: "Jail state lives here, not on Player").
    StubJusticeQuery justice;
    justice.jailed = jailed.id;
    ctx.justice    = &justice;

    CHECK_FALSE(harrowmere::hasStatus(free, StatusType::ARRESTED, ctx));
    CHECK(harrowmere::hasStatus(jailed, StatusType::ARRESTED, ctx));

    // The character record itself carries nothing: no flag, no field write.
    CHECK(jailed.statusFlags == 0);
}

TEST_CASE(
    "TestStatus_InWarReadsRegistry_NullFalse"
)
{
    // Not a pinned name — the IN_WAR seam's supporting test (the war-side
    // twin of TestStatus_ArrestedReadsJustice; 6.2 supplies the registry).
    Character c = makeCharacter(1);
    c.faction   = 42;

    WorldContext ctx;
    ctx.now = 1'000;

    // Null war registry (6.2 not landed): nobody is a combatant.
    CHECK_FALSE(harrowmere::hasStatus(c, StatusType::IN_WAR, ctx));

    StubWarRegistry wars;
    ctx.wars = &wars;

    wars.combatant = 42;
    CHECK(harrowmere::hasStatus(c, StatusType::IN_WAR, ctx));

    wars.combatant = 43;
    CHECK_FALSE(harrowmere::hasStatus(c, StatusType::IN_WAR, ctx));
}

TEST_CASE(
    "TestStatus_DerivedStatusesNeverStored"
)
{
    // The runtime half of the Done-when's grep review: deriving all four
    // statuses (positive or negative) leaves statusFlags exactly as it was
    // — D-CHAR-1: "derived, never stored".
    Character c   = makeCharacter(1);
    c.statusFlags = 0x8000'0000u; // a pre-existing non-derived extension bit

    StubWarRegistry wars;
    StubJusticeQuery justice;
    wars.combatant = c.faction;
    justice.jailed = c.id;

    WorldContext ctx;
    ctx.now     = 5'000;
    ctx.wars    = &wars;
    ctx.justice = &justice;

    c.eventId        = 9;
    c.lastCombatTime = 5'000;

    (void)harrowmere::hasStatus(c, StatusType::IN_EVENT, ctx);
    (void)harrowmere::hasStatus(c, StatusType::IN_COMBAT, ctx);
    (void)harrowmere::hasStatus(c, StatusType::IN_WAR, ctx);
    (void)harrowmere::hasStatus(c, StatusType::ARRESTED, ctx);

    CHECK(c.statusFlags == 0x8000'0000u);
}

TEST_CASE(
    "TestConsent_DuelConsentOnlyBetweenPair"
)
{
    Character a        = makeCharacter(1);
    Character b        = makeCharacter(2);
    Character outsider = makeCharacter(3);

    // No duel anywhere: nobody consents to anybody.
    CHECK_FALSE(harrowmere::consentsToPvp(a, b));
    CHECK_FALSE(harrowmere::consentsToPvp(b, a));
    CHECK_FALSE(harrowmere::consentsToPvp(a, outsider));

    // A accepts a duel with b (2.7 sets BOTH sides; the victim-side read
    // means a's own duelOpponent is what makes a consent to b).
    a.duelOpponent = b.id;
    b.duelOpponent = a.id;

    CHECK(harrowmere::consentsToPvp(a, b));
    CHECK(harrowmere::consentsToPvp(b, a));

    // The duel opponent field names ONE partner: the outsider gets nothing.
    CHECK_FALSE(harrowmere::consentsToPvp(a, outsider));
    CHECK_FALSE(harrowmere::consentsToPvp(outsider, a));
    CHECK_FALSE(harrowmere::consentsToPvp(outsider, b));

    // Clearing one side ends that side's consent (a yields / duel ends).
    a.duelOpponent = 0;
    CHECK_FALSE(harrowmere::consentsToPvp(a, b));
    CHECK(harrowmere::consentsToPvp(b, a)); // b still names a until cleared too
}

TEST_CASE(
    "TestConsent_ArenaSharedId"
)
{
    Character a         = makeCharacter(1);
    Character b         = makeCharacter(2);
    Character elsewhere = makeCharacter(3);

    // Both inside arena 7: mutual consent (the doc's shared-id consent).
    a.arenaId = 7;
    b.arenaId = 7;

    CHECK(harrowmere::consentsToPvp(a, b));
    CHECK(harrowmere::consentsToPvp(b, a));

    // A different arena is a different zone: no consent across zones.
    elsewhere.arenaId = 8;
    CHECK_FALSE(harrowmere::consentsToPvp(a, elsewhere));
    CHECK_FALSE(harrowmere::consentsToPvp(elsewhere, a));

    // arenaId 0 = "not in an arena" (the doc's field comment): two
    // characters outside arenas never consent through the shared 0.
    Character outsideA = makeCharacter(4);
    Character outsideB = makeCharacter(5);
    CHECK(outsideA.arenaId == 0);
    CHECK_FALSE(harrowmere::consentsToPvp(outsideA, outsideB));

    // Leaving the arena (2.7 clears the id) ends the consent.
    a.arenaId = 0;
    CHECK_FALSE(harrowmere::consentsToPvp(a, b));

    // The duel channel and the arena channel are independent: a duel with
    // someone outside the arena does not leak into it.
    Character duelist = makeCharacter(6);
    a.duelOpponent    = duelist.id;
    CHECK(harrowmere::consentsToPvp(a, duelist));
    CHECK_FALSE(harrowmere::consentsToPvp(b, duelist));
}

TEST_CASE(
    "TestCooldown_ActiveUntilExpiry"
)
{
    Character c                 = makeCharacter(1);
    constexpr CooldownType type = CooldownType::APPEARANCE;

    const std::uint64_t now = 50'000;

    // Fresh character: no cooldown armed.
    CHECK_FALSE(harrowmere::cooldownActive(c, type, now));

    // setCooldown stores the EXPIRY stamp (card 2.1: "the worldTime at
    // which the cooldown expires"; callers pass now + duration).
    const std::uint64_t duration = 86'400; // 1 game-day, e.g. A-24's appearance cooldown
    harrowmere::setCooldown(c, type, now + duration);

    CHECK(harrowmere::cooldownActive(c, type, now));
    CHECK(harrowmere::cooldownActive(c, type, now + duration - 1));   // last active second
    CHECK_FALSE(harrowmere::cooldownActive(c, type, now + duration)); // free exactly at expiry
    CHECK_FALSE(harrowmere::cooldownActive(c, type, now + duration + 1));

    // Other slots are untouched: cooldowns are per-type.
    CHECK_FALSE(harrowmere::cooldownActive(c, CooldownType::RESPEC_STAT, now));
    CHECK_FALSE(harrowmere::cooldownActive(c, CooldownType::RESPEC_SKILL, now));
}
