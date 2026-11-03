// test_character_store.cpp - Task 2.0's pinned test suite.
//
// Implements the four tests pinned in phase-2-alive.md card 2.0's Tests
// block (names verbatim - common.md section 3.5: "Test names are pinned;
// keep them"):
//
//   * TestCharacter_DefaultsMatchDocStruct  - compile-time reflection table
//     (field presence + doc types, static_assert) plus the review-checked
//     default init list (runtime half)
//   * TestStore_CreatePlayer_SpawnsAtSafeZone - the pinned starting state
//     (A-41/A-42/A-43) at the worldgen hand-off spawn point
//   * TestStore_NpcFromSeed_CarriesSpeciesAgeRole - the seed -> Character
//     mapping and the D-STORE-1 boundary (age/role stay on the seed for
//     NpcState, task 3.4)
//   * TestStore_CurrencyCanGoNegative - the i64 field sanity (R4)
//
// Plus the store's own contracts the card's Done-when leans on: ascending
// id iteration (R7), the id-continuity seam (plan ids honored, post-plan
// seeds draw from the adopted counter), and the creation-record flow.
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here -
// tests/main.cpp owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN. Only non-asserting
// paths are exercised: the store's programmer-error paths (double player,
// duplicate id) abort under debug asserts and are contract docs, not tests.
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

#include "character/character.hpp"
#include "character/store.hpp"
#include "core/constants.hpp" // PLAYER_SPECIES_ID, STARTER_CURRENCY (A-41), BASE_HP (A-42), HP_PER_LEVEL (A-43), MAX_XP (A-2)
#include "core/enums.hpp"     // CooldownType, StatType, SkillType (closed lists, task 1.1)
#include "core/math.hpp"      // applyXpDelta (R4)
#include "core/types.hpp"     // IdGenerator (FIRST_ID), characterId, Position
#include "world/worldgen.hpp" // NpcSeed (task 1.5's population plan)

// ============================================================================
// Compile-time reflection table (the static half of
// TestCharacter_DefaultsMatchDocStruct): one row per design reference field,
// pinned to the doc's EXACT name and type. The table is the card's
// "static_assert field presence via a compile-time reflection table"; the
// runtime default checks live in the TEST_CASE below.
// ============================================================================

namespace
{

// Field presence with the doc's exact spelling.
#define HARROWMERE_DOC_FIELD(type_, field_)                                                                            \
    static_assert(requires(type_ &t) { t.field_; }, "doc field missing on " #type_ ": " #field_)

#define HARROWMERE_FIELD_TYPE(owner_, field_, expected_)                                                               \
    static_assert(                                                                                                     \
        std::is_same_v<decltype(std::declval<owner_ &>().field_), expected_>,                                          \
        "doc field type mismatch on " #owner_ "." #field_ ", expected " #expected_                                     \
    )

// ---- Body (design reference, Conventions) -----------------------------------
HARROWMERE_DOC_FIELD(harrowmere::Body, currentHp);
HARROWMERE_DOC_FIELD(harrowmere::Body, maxHp);
HARROWMERE_DOC_FIELD(harrowmere::Body, position);
HARROWMERE_FIELD_TYPE(harrowmere::Body, currentHp, std::uint64_t); // u64
HARROWMERE_FIELD_TYPE(harrowmere::Body, maxHp, std::uint64_t);     // u64
HARROWMERE_FIELD_TYPE(harrowmere::Body, position, harrowmere::Position);

// ---- Progression (design reference, Conventions) ----------------------------
HARROWMERE_DOC_FIELD(harrowmere::Progression, totalXp);
HARROWMERE_DOC_FIELD(harrowmere::Progression, availableXp);
HARROWMERE_DOC_FIELD(harrowmere::Progression, level);
HARROWMERE_DOC_FIELD(harrowmere::Progression, stats);
HARROWMERE_DOC_FIELD(harrowmere::Progression, skills);
HARROWMERE_DOC_FIELD(harrowmere::Progression, currency);
HARROWMERE_FIELD_TYPE(harrowmere::Progression, totalXp, std::uint64_t);    // u64 - never decreases
HARROWMERE_FIELD_TYPE(harrowmere::Progression, availableXp, std::int64_t); // i64 - negative = XP debt (R4)
HARROWMERE_FIELD_TYPE(harrowmere::Progression, level, std::uint16_t);      // u16
HARROWMERE_FIELD_TYPE(harrowmere::Progression, currency, std::int64_t);    // i64 - negative = debt
static_assert(
    std::tuple_size_v<decltype(std::declval<harrowmere::Progression &>().stats)>
        == static_cast<std::size_t>(harrowmere::StatType::COUNT),
    "Progression.stats must be sized by the closed StatType list (R9)"
);
static_assert(
    std::tuple_size_v<decltype(std::declval<harrowmere::Progression &>().skills)>
        == static_cast<std::size_t>(harrowmere::SkillType::COUNT),
    "Progression.skills must be sized by the closed SkillType list (R9)"
);

// ---- Character (design reference, character.md - doc order) ------------------
HARROWMERE_DOC_FIELD(harrowmere::Character, id);
HARROWMERE_DOC_FIELD(harrowmere::Character, species);
HARROWMERE_DOC_FIELD(harrowmere::Character, statusFlags);
HARROWMERE_DOC_FIELD(harrowmere::Character, duelOpponent);
HARROWMERE_DOC_FIELD(harrowmere::Character, arenaId);
HARROWMERE_DOC_FIELD(harrowmere::Character, appearanceComponents);
HARROWMERE_DOC_FIELD(harrowmere::Character, equippedItems);
HARROWMERE_DOC_FIELD(harrowmere::Character, body);
HARROWMERE_DOC_FIELD(harrowmere::Character, progression);
HARROWMERE_DOC_FIELD(harrowmere::Character, cooldowns);
HARROWMERE_DOC_FIELD(harrowmere::Character, faction);
HARROWMERE_DOC_FIELD(harrowmere::Character, eventId);
HARROWMERE_DOC_FIELD(harrowmere::Character, lastCombatTime);
HARROWMERE_DOC_FIELD(harrowmere::Character, cachedPower);
HARROWMERE_DOC_FIELD(harrowmere::Character, peakPower);
HARROWMERE_DOC_FIELD(harrowmere::Character, powerDirty);

HARROWMERE_FIELD_TYPE(harrowmere::Character, id, harrowmere::characterId);
HARROWMERE_FIELD_TYPE(harrowmere::Character, species, harrowmere::speciesId);
HARROWMERE_FIELD_TYPE(harrowmere::Character, statusFlags, std::uint32_t); // u32 - non-derived flags only (D-CHAR-1)
HARROWMERE_FIELD_TYPE(harrowmere::Character, duelOpponent, harrowmere::characterId);
HARROWMERE_FIELD_TYPE(harrowmere::Character, arenaId, std::uint64_t);
HARROWMERE_FIELD_TYPE(harrowmere::Character, appearanceComponents, std::vector<std::uint64_t>); // array<u64>
HARROWMERE_FIELD_TYPE(harrowmere::Character, equippedItems, std::vector<std::uint64_t>);        // array<u64>
HARROWMERE_FIELD_TYPE(harrowmere::Character, body, harrowmere::Body);
HARROWMERE_FIELD_TYPE(harrowmere::Character, progression, harrowmere::Progression);
HARROWMERE_FIELD_TYPE(harrowmere::Character, faction, harrowmere::factionId);
HARROWMERE_FIELD_TYPE(harrowmere::Character, eventId, std::uint64_t);
HARROWMERE_FIELD_TYPE(harrowmere::Character, lastCombatTime, std::uint64_t);
HARROWMERE_FIELD_TYPE(harrowmere::Character, cachedPower, std::uint64_t);
HARROWMERE_FIELD_TYPE(harrowmere::Character, peakPower, std::uint64_t);
HARROWMERE_FIELD_TYPE(harrowmere::Character, powerDirty, bool);

static_assert(
    std::tuple_size_v<decltype(std::declval<harrowmere::Character &>().cooldowns)>
        == static_cast<std::size_t>(harrowmere::CooldownType::COUNT),
    "Character.cooldowns must be u64[CooldownType::COUNT] (R9, design reference)"
);

// ---- D-STORE-1 negatives: NPC-only state NEVER leaks onto Character ---------
// role/age/needs/brain/LOD/relationships belong to NpcState (task 3.4); jail
// state belongs to justice (design rule: "Jail state lives here, not on
// Player"). The struct stays doc-clean so save/load (7.1) mirrors the doc.
// The probe runs through a concept so the member access stays dependent
// (a requires-expression over a concrete type hard-errors on a missing
// member instead of yielding false).
#define HARROWMERE_NO_FIELD(field_, why_)                                                                              \
    template <typename T>                                                                                              \
    concept HarrowmereHas_##field_ = requires(T &t) { t.field_; };                                                     \
    static_assert(!HarrowmereHas_##field_<harrowmere::Character>, why_)

HARROWMERE_NO_FIELD(name, "D-STORE-1: name lives in NpcState");
HARROWMERE_NO_FIELD(roleKey, "D-STORE-1: role lives in NpcState");
HARROWMERE_NO_FIELD(birthWorldTime, "D-STORE-1: age lives in NpcState");
HARROWMERE_NO_FIELD(needs, "D-STORE-1: needs live in NpcState");
HARROWMERE_NO_FIELD(tier, "D-STORE-1: LOD tier lives in NpcState");
HARROWMERE_NO_FIELD(affinity, "D-STORE-1: relationships live in NpcState");
HARROWMERE_NO_FIELD(jailUntil, "jail state lives in justice, not on Character");

// ---- the doc's HP formula lives exactly once (A-42 + A-43) ------------------
static_assert(harrowmere::maxHpForLevel(1) == 100 + 20, "A-42/A-43: level 1 -> 120 hp");
static_assert(harrowmere::maxHpForLevel(75) == 100 + 75 * 20, "A-42/A-43: level 75 -> 1600 hp");

// ---- test helpers ------------------------------------------------------------

bool samePosition(
    const harrowmere::Position &a, const harrowmere::Position &b
)
{
    // The spawn point is copied verbatim; exact equality is the contract.
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

harrowmere::world::NpcSeed makeSeed(
    harrowmere::characterId id,
    harrowmere::speciesId species,
    const char *role,
    std::uint32_t ageYears,
    std::uint32_t level
)
{
    harrowmere::world::NpcSeed seed{};
    seed.id            = id;
    seed.species       = species;
    seed.roleKey       = role;
    seed.ageYears      = ageYears;
    seed.integrity     = 5000; // mid-range draw stand-in; the store never reads it
    seed.level         = level;
    seed.homeTerritory = 7;
    seed.homeRegion    = 40;
    return seed;
}

} // namespace

// ============================================================================
// Pinned test 1 - the static half runs at compile time (above); the runtime
// half pins the doc's default init list on a fresh Character.
// ============================================================================
TEST_CASE(
    "TestCharacter_DefaultsMatchDocStruct"
)
{
    const harrowmere::Character c{};

    CHECK(c.id == 0);
    CHECK(c.species == harrowmere::PLAYER_SPECIES_ID); // players are species 0
    CHECK(c.statusFlags == 0);                         // non-derived flags only; nothing in v1 writes bits
    CHECK(c.duelOpponent == 0);                        // 0 = not dueling
    CHECK(c.arenaId == 0);                             // 0 = not in an arena
    CHECK(c.appearanceComponents.empty());
    CHECK(c.equippedItems.empty());

    CHECK(c.body.currentHp == 0);
    CHECK(c.body.maxHp == 0);
    CHECK(samePosition(c.body.position, harrowmere::Position{0.0, 0.0, 0.0}));

    CHECK(c.progression.totalXp == 0);
    CHECK(c.progression.availableXp == 0);
    CHECK(c.progression.level == 1); // a fresh character exists at level 1
    for (const std::uint64_t stat : c.progression.stats)
        CHECK(stat == 0); // the docs: arrays start at 0
    for (const std::uint64_t skill : c.progression.skills)
        CHECK(skill == 0);
    CHECK(c.progression.currency == 0);

    for (const std::uint64_t cooldown : c.cooldowns)
        CHECK(cooldown == 0); // every CooldownType slot expired at epoch

    CHECK(c.faction == 0); // 0 = factionless
    CHECK(c.eventId == 0); // 0 = not in an event
    CHECK(c.lastCombatTime == 0);
    CHECK(c.cachedPower == 0);
    CHECK(c.peakPower == 0);
    CHECK(c.powerDirty == true); // docs default true - the cache recomputes on first read
}

// ============================================================================
// Pinned test 2 - the player-creation flow at the safe-zone spawn point.
// The spawn Position handed to adoptWorldGen is worldgen stage 4's product
// (WorldGenResult::playerSpawn: the beginner kingdom's safe village center);
// the store records it verbatim - the safe flag itself lives on Territory
// (task 1.4) and the store stays world-free by design.
// ============================================================================
TEST_CASE(
    "TestStore_CreatePlayer_SpawnsAtSafeZone"
)
{
    harrowmere::character::CharacterStore store;
    const harrowmere::Position spawn{10.0, 0.5, 20.0};
    store.adoptWorldGen(harrowmere::IdGenerator::FIRST_ID, spawn);

    harrowmere::PlayerCreation creation;
    creation.name     = "Mara"; // free text or a name-table draw - recorded verbatim
    creation.classKey = "";     // no class preset

    const harrowmere::characterId pid = store.createPlayer(creation);
    CHECK(pid != 0);
    CHECK(store.isPlayer(pid));
    CHECK(!store.isPlayer(0)); // 0 is reserved (R10) and never issued
    CHECK(store.playerCount() == 1);
    CHECK(store.playerId() == pid);
    CHECK(store.population() == 1);
    CHECK(store.exists(pid));

    const harrowmere::Character &c = store.get(pid);
    CHECK(c.id == pid);
    CHECK(c.species == harrowmere::PLAYER_SPECIES_ID); // players are species 0 ("player")

    // The pinned starting state (card 2.0).
    CHECK(c.progression.level == 1);
    CHECK(c.progression.totalXp == 0);
    CHECK(c.progression.availableXp == 0);
    CHECK(c.progression.currency == harrowmere::STARTER_CURRENCY); // A-41: 100
    for (const std::uint64_t stat : c.progression.stats)
        CHECK(stat == 0); // BASE_*_CAP floor 0
    for (const std::uint64_t skill : c.progression.skills)
        CHECK(skill == 0);
    CHECK(c.body.maxHp == harrowmere::BASE_HP + harrowmere::HP_PER_LEVEL); // A-42 + A-43 at level 1 = 120
    CHECK(c.body.maxHp == harrowmere::maxHpForLevel(1));
    CHECK(c.body.currentHp == c.body.maxHp);     // spawn healthy
    CHECK(samePosition(c.body.position, spawn)); // spawned AT the safe-zone spawn point

    // Consent/state defaults: nothing is running at creation.
    CHECK(c.duelOpponent == 0);
    CHECK(c.arenaId == 0);
    CHECK(c.eventId == 0);
    CHECK(c.faction == 0);
    CHECK(c.lastCombatTime == 0);
    CHECK(c.powerDirty);

    // The v1 default appearance set is empty (no component vocabulary exists
    // in v1); the field carries the creation input verbatim when non-empty.
    CHECK(c.appearanceComponents.empty());
    CHECK(c.equippedItems.empty());

    // The creation record round-trips (the player-creation flow's metadata).
    CHECK(store.playerCreation().name == "Mara");
    CHECK(store.playerCreation().classKey.empty());
    CHECK(store.playerCreation().appearanceComponents.empty());
}

// ============================================================================
// Pinned test 3 - the seed -> Character mapping, and the D-STORE-1 boundary:
// species rides Character; age and role stay on the SEED (NpcState fields,
// task 3.4) - the store preserves the seed<->id binding by honoring
// seed.id, so 3.4 attaches NpcState by the id this call returned.
// ============================================================================
TEST_CASE(
    "TestStore_NpcFromSeed_CarriesSpeciesAgeRole"
)
{
    harrowmere::character::CharacterStore store;
    store.adoptWorldGen(64, harrowmere::Position{0.0, 0.0, 0.0});

    // A plan NPC: beastkin farmer, 30 years old, level 4 (worldgen's
    // age-band level), plan id 3.
    const harrowmere::world::NpcSeed seed = makeSeed(3, 2, "farmer", 30, 4);
    const harrowmere::characterId nid     = store.createNpc(seed);

    CHECK(nid == seed.id); // plan ids are honored verbatim (strict plan order, R10)
    CHECK(!store.isPlayer(nid));
    CHECK(store.playerCount() == 0); // NPCs never enter playerIds_

    const harrowmere::Character &c = store.get(nid);
    CHECK(c.id == nid);
    CHECK(c.species == seed.species);         // species carried (design reference: species exists on every character)
    CHECK(c.progression.level == seed.level); // the seed's age-band level carried

    // HP derives from the seed level through the one formula (A-42/A-43).
    CHECK(c.body.maxHp == harrowmere::maxHpForLevel(seed.level)); // 100 + 4*20 = 180
    CHECK(c.body.currentHp == c.body.maxHp);

    // The doc floor shared by every NPC at creation.
    CHECK(c.progression.totalXp == 0);
    CHECK(c.progression.availableXp == 0);
    CHECK(c.progression.currency == 0); // no starter purse is pinned for NPCs
    for (const std::uint64_t stat : c.progression.stats)
        CHECK(stat == 0);
    for (const std::uint64_t skill : c.progression.skills)
        CHECK(skill == 0);
    CHECK(c.faction == 0); // factionless - faction seeds link in task 6.1
    CHECK(c.powerDirty);
    CHECK(samePosition(c.body.position, harrowmere::Position{0.0, 0.0, 0.0})); // placement is task 3.4's

    // Age and role: NOT on Character (the compile-time negatives above are
    // the hard pin). The seed remains the single source - seed.ageYears and
    // seed.roleKey ride into NpcState (task 3.4) keyed by the id above.
    CHECK(seed.ageYears == 30);
    CHECK(seed.roleKey == "farmer");

    // A whole plan spawns without collisions, and the store iterates it
    // deterministically id-ascending (R7).
    std::set<harrowmere::characterId> seen;
    for (std::uint32_t i = 0; i < 64; ++i)
    {
        const harrowmere::world::NpcSeed planSeed = makeSeed(100 + i, 1, "laborer", 20 + i, 1);
        const harrowmere::characterId spawned     = store.createNpc(planSeed);
        CHECK(spawned == planSeed.id);
        CHECK(seen.insert(spawned).second); // never a duplicate (R10)
    }
    CHECK(store.population() == 65);
    harrowmere::characterId previous = 0;
    for (const auto &[id, character] : store.all())
    {
        CHECK(id > previous); // std::map order: ascending, deterministic
        CHECK(character.id == id);
        previous = id;
    }
}

// ============================================================================
// Pinned test 4 - the i64 field sanity: currency and availableXp hold debt
// (R4), totalXp is unsigned and never goes negative.
// ============================================================================
TEST_CASE(
    "TestStore_CurrencyCanGoNegative"
)
{
    harrowmere::character::CharacterStore store;
    store.adoptWorldGen(harrowmere::IdGenerator::FIRST_ID, harrowmere::Position{0.0, 0.0, 0.0});
    const harrowmere::characterId pid = store.createPlayer(harrowmere::PlayerCreation{});

    // currency is i64: a (future, 4.3-owned) fine writes the debt, the field
    // holds and reads it back through the store.
    harrowmere::Character &mutableCharacter = store.get(pid);
    mutableCharacter.progression.currency   = -25;
    CHECK(store.get(pid).progression.currency == -25);

    // availableXp is i64: spending below zero (XP debt) is legal through the
    // R4 clamp helper - floored at -MAX_XP exactly, never below.
    mutableCharacter.progression.availableXp = applyXpDelta(0, -10);
    CHECK(store.get(pid).progression.availableXp == -10);
    CHECK(applyXpDelta(-harrowmere::MAX_XP, -1) == -harrowmere::MAX_XP);
    CHECK(applyXpDelta(0, -harrowmere::MAX_XP) == -harrowmere::MAX_XP);

    // totalXp is u64 and untouched by debt paths (it never decreases; the
    // mutation owners are 3.3's kill/consume paths - here only the sanity).
    CHECK(store.get(pid).progression.totalXp == 0);
}

// ============================================================================
// Store contract - deterministic iteration & the id-continuity seam.
// adoptWorldGen hands the runtime worldgen's counter (past the plan's ids);
// plan seeds keep their pre-assigned ids, post-plan seeds (id 0 - births
// 3.4, backup NPCs 3.6) draw from the adopted counter, never reusing an id.
// ============================================================================
TEST_CASE(
    "TestStore_All_IteratesAscendingIds"
)
{
    harrowmere::character::CharacterStore store;
    store.adoptWorldGen(5, harrowmere::Position{1.0, 2.0, 3.0});

    // Plan ids below the adopted counter are honored...
    CHECK(store.createNpc(makeSeed(1, 1, "guard", 25, 3)) == 1);
    CHECK(store.createNpc(makeSeed(2, 3, "farmer", 40, 5)) == 2);
    // ...the player draws the adopted counter value itself...
    const harrowmere::characterId pid = store.createPlayer(harrowmere::PlayerCreation{});
    CHECK(pid == 5);
    // ...and post-plan NPCs continue the sequence (ids never reused, R10).
    CHECK(store.createNpc(makeSeed(0, 1, "", 0, 2)) == 6);
    CHECK(store.createNpc(makeSeed(0, 2, "", 0, 1)) == 7);

    CHECK(store.population() == 5);
    CHECK(store.playerCount() == 1);
    CHECK(store.playerId() == pid);

    std::size_t visits               = 0;
    harrowmere::characterId previous = 0;
    for (const auto &[id, character] : store.all())
    {
        (void)character;
        CHECK(id > previous);
        previous = id;
        ++visits;
    }
    CHECK(visits == store.population());

    // The const overload iterates the same map (one storage truth, R7).
    const harrowmere::character::CharacterStore &readonly = std::as_const(store);
    CHECK(readonly.all().size() == store.population());
    CHECK(readonly.exists(2));
    CHECK(!readonly.exists(999));
}

// ============================================================================
// Store contract - the player-creation flow records its input verbatim
// (name from free text OR a name-table draw; optional class preset;
// appearance components carried onto the doc struct).
// ============================================================================
TEST_CASE(
    "TestStore_PlayerCreation_CarriesNameClassAppearance"
)
{
    harrowmere::character::CharacterStore store;
    store.adoptWorldGen(harrowmere::IdGenerator::FIRST_ID, harrowmere::Position{0.0, 0.0, 0.0});

    harrowmere::PlayerCreation creation;
    creation.name                 = "Sael"; // free text and table draws both arrive resolved
    creation.appearanceComponents = {7, 9};
    creation.classKey             = "warrior";

    const harrowmere::characterId pid = store.createPlayer(creation);
    const harrowmere::Character &c    = store.get(pid);

    CHECK(c.appearanceComponents == creation.appearanceComponents); // doc struct carries the input
    CHECK(store.playerCreation().name == creation.name);
    CHECK(store.playerCreation().classKey == "warrior");

    // The class preset grants NOTHING in v1 (pinned starting state is
    // class-independent): stats/skills stay at the floor.
    for (const std::uint64_t stat : c.progression.stats)
        CHECK(stat == 0);
    for (const std::uint64_t skill : c.progression.skills)
        CHECK(skill == 0);
    CHECK(c.progression.currency == harrowmere::STARTER_CURRENCY);
    CHECK(c.body.maxHp == harrowmere::maxHpForLevel(1));
}
