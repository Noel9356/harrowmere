// test_consent.cpp — Task 2.7's pinned test suite.
//
// Implements the five tests pinned in phase-2-alive.md card 2.7's Tests
// block (names verbatim — common.md §3.5):
//
//   * TestConsent_DuelSetsBothSides           — acceptance writes BOTH
//                                               duelOpponent fields; 2.1's
//                                               consentsToPvp (the ONE consent
//                                               check) agrees on both sides
//   * TestConsent_BroadcastReachesRadiusOnly  — the A-26 gather over the
//                                               midpoint, PLUS the pinned
//                                               order asserted from inside
//                                               the broadcast: witnesses hear
//                                               the consent BEFORE the fields
//                                               enable damage
//   * TestConsent_DuelEndClearsBoth           — yield clears both sides and
//                                               closes the witness windows
//   * TestArena_SharedIdConsentsAllOccupants  — one zone id, everyone inside
//                                               consents to everyone
//   * TestArena_LeaveClearsId                 — walking out clears the field
//
// Supporting tests: the sweep's two end conditions (A-27 idle timeout,
// DUEL_MAX_SEPARATION_M range separation) and the death path
// (endDuelOnDeath closes the duel AND the pending requests touching the
// dead).
//
// Test harness: doctest v2.4.12; no main here — tests/main.cpp owns the
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "character/store.hpp"
#include "character/status.hpp" // consentsToPvp — THE consent check (2.1's Done-when)
#include "combat/consent.hpp"
#include "core/types.hpp"
#include "data/registry.hpp"
#include "world/spatial_grid.hpp"
#include "world/world.hpp"

#include <filesystem>

namespace
{

std::string consentDataDir()
{
#ifdef HARROWMERE_CORE_DATA_DIR
    return std::string(HARROWMERE_CORE_DATA_DIR);
#else
    return std::string("mods/harrowmere_core/data");
#endif
}

// A recording broadcast sink: captures the observer sets AND asserts the
// pinned order from inside the callback (the duelStarted deliver point is
// the one place that can observe the pre-consent world).
struct RecordingBroadcast final : harrowmere::combat::IDuelBroadcast
{
    struct Entry
    {
        harrowmere::characterId a, b;
        std::uint64_t at;
        std::vector<harrowmere::characterId> observers;
    };
    std::vector<Entry> starts;
    std::vector<Entry> ends;

    // Non-owning peek at the character store, set per test (the pinned-order
    // assertion reads the consent fields mid-broadcast).
    const harrowmere::character::CharacterStore *characters = nullptr;

    void duelStarted(
        harrowmere::characterId a,
        harrowmere::characterId b,
        std::uint64_t at,
        const std::vector<harrowmere::characterId> &observers
    ) override
    {
        // THE PINNED ORDER: broadcast BEFORE enable damage. If either field
        // is already set here, the first blow could land before any witness
        // heard the consent — the exact bug the card's pitfall warns about.
        if (characters != nullptr)
        {
            CHECK(characters->get(a).duelOpponent == 0);
            CHECK(characters->get(b).duelOpponent == 0);
        }
        starts.push_back({a, b, at, observers});
    }

    void duelEnded(
        harrowmere::characterId a,
        harrowmere::characterId b,
        std::uint64_t at,
        const std::vector<harrowmere::characterId> &observers
    ) override
    {
        // The close is a post-state fact: the fields are ALREADY clear
        // (0.7's rule — a handler reads the truth).
        if (characters != nullptr)
        {
            CHECK(characters->get(a).duelOpponent == 0);
            CHECK(characters->get(b).duelOpponent == 0);
        }
        ends.push_back({a, b, at, observers});
    }
};

// The consent flow lives in harrowmere::combat; the tests read it card-verbatim.
using harrowmere::combat::acceptDuel;
using harrowmere::combat::ArenaTable;
using harrowmere::combat::ConsentRegistry;
using harrowmere::combat::DuelError;
using harrowmere::combat::endDuelOnDeath;
using harrowmere::combat::refreshArenaPresence;
using harrowmere::combat::requestDuel;
using harrowmere::combat::sweepDuels;
using harrowmere::combat::yieldDuel;

bool contains(
    const std::vector<harrowmere::characterId> &ids, harrowmere::characterId id
)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// The shared fixture: a committed registry, a character store with four
// people, and their spots in the spatial grid.
struct ConsentFixture
{
    harrowmere::data::DataRegistry registry;
    harrowmere::character::CharacterStore characters;
    harrowmere::world::SpatialGrid grid;
    harrowmere::combat::ConsentRegistry duels;
    RecordingBroadcast broadcast;

    static constexpr harrowmere::characterId kA  = 1;
    static constexpr harrowmere::characterId kB  = 2;
    static constexpr harrowmere::characterId kW1 = 3; // near the midpoint
    static constexpr harrowmere::characterId kW2 = 4; // far from it

    ConsentFixture()
    {
        std::vector<harrowmere::data::DataError> errors;
        REQUIRE(registry.load(consentDataDir(), errors));
        REQUIRE(errors.empty());

        harrowmere::PlayerCreation creation;
        creation.name = "duelist";
        characters.adoptWorldGen(10, harrowmere::Position{0.0, 0.0, 0.0});
        characters.createPlayer(creation); // id 10, unused by the duel tests

        make(kA, {500.0, 0.0, 500.0});
        make(kB, {520.0, 0.0, 500.0});
        make(kW1, {530.0, 0.0, 500.0}); // 20 m from the a/b midpoint
        make(kW2, {600.0, 0.0, 500.0}); // 90 m from it
    }

    harrowmere::characterId make(
        harrowmere::characterId forceId, const harrowmere::Position &p
    )
    {
        // Seeds with a forced plan id keep the ids the tests read.
        harrowmere::world::NpcSeed seed{};
        seed.id                          = forceId;
        seed.species                     = 1; // human
        seed.level                       = 1;
        const harrowmere::characterId id = characters.createNpc(seed);
        REQUIRE(id == forceId);
        characters.get(id).body.position = p;
        grid.insert(id, p);
        return id;
    }
};

// A committed registry as a first member (WorldStore's defaultLaws needs
// the loaded snapshot BEFORE the store member initializes).
struct CommittedRegistry
{
    harrowmere::data::DataRegistry registry;

    CommittedRegistry()
    {
        std::vector<harrowmere::data::DataError> errors;
        REQUIRE(registry.load(consentDataDir(), errors));
        REQUIRE(errors.empty());
    }
};

// An arena world: one CITY territory anchored at region 0 (whose center is
// (512, 0, 512) — regionCenter's formula), registry-declared arena radius.
struct ArenaFixture
{
    CommittedRegistry committed;
    harrowmere::world::WorldStore world;
    harrowmere::combat::ArenaTable arenas;

    static constexpr harrowmere::territoryId kCity = 7;
    static constexpr harrowmere::regionId kRegion  = 0;

    ArenaFixture() : world(committed.registry.defaultLaws())
    {
        harrowmere::data::DataRegistry &registry = committed.registry;
        (void)registry;

        harrowmere::world::Territory city{};
        city.type   = harrowmere::TerritoryType::CITY;
        city.id     = kCity;
        city.region = kRegion;
        world.addTerritory(city);
        world.setRegionTerritory(kRegion, kCity);

        arenas.build(registry, world);
        REQUIRE(arenas.size() == 1); // one CITY -> one zone (v1)
    }
};

} // namespace

// ---- pinned: TestConsent_DuelSetsBothSides -----------------------------------------

TEST_CASE(
    "TestConsent_DuelSetsBothSides"
)
{
    ConsentFixture fx;

    // The request alone consents to NOTHING: fields still zero, 2.1's check
    // reads no consent either way.
    REQUIRE(requestDuel(ConsentFixture::kA, ConsentFixture::kB, fx.duels, fx.characters, 1'000).isOk());
    CHECK(fx.duels.hasRequest(ConsentFixture::kA, ConsentFixture::kB));
    CHECK(fx.characters.get(ConsentFixture::kA).duelOpponent == 0);
    CHECK(fx.characters.get(ConsentFixture::kB).duelOpponent == 0);
    CHECK_FALSE(
        harrowmere::consentsToPvp(fx.characters.get(ConsentFixture::kA), fx.characters.get(ConsentFixture::kB))
    );

    // Acceptance flips BOTH fields (the victim-side read of 2.1's check
    // needs each side to name the other) and inserts the record.
    REQUIRE(acceptDuel(ConsentFixture::kB, ConsentFixture::kA, fx.duels, fx.characters, fx.grid, &fx.broadcast, 2'000)
            .isOk());
    CHECK(fx.characters.get(ConsentFixture::kA).duelOpponent == ConsentFixture::kB);
    CHECK(fx.characters.get(ConsentFixture::kB).duelOpponent == ConsentFixture::kA);
    CHECK(fx.duels.duelOf(ConsentFixture::kA) != nullptr);
    CHECK(fx.duels.duelOf(ConsentFixture::kB) != nullptr); // either side finds it

    // The ONE consent check agrees on both sides now — and nobody else
    // gained anything.
    CHECK(harrowmere::consentsToPvp(fx.characters.get(ConsentFixture::kA), fx.characters.get(ConsentFixture::kB)));
    CHECK(harrowmere::consentsToPvp(fx.characters.get(ConsentFixture::kB), fx.characters.get(ConsentFixture::kA)));
    CHECK_FALSE(
        harrowmere::consentsToPvp(fx.characters.get(ConsentFixture::kW1), fx.characters.get(ConsentFixture::kA))
    );
    CHECK_FALSE(
        harrowmere::consentsToPvp(fx.characters.get(ConsentFixture::kA), fx.characters.get(ConsentFixture::kW1))
    );

    // The request bookkeeping is consumed: no double acceptance.
    CHECK_FALSE(fx.duels.hasRequest(ConsentFixture::kA, ConsentFixture::kB));
    auto again
        = acceptDuel(ConsentFixture::kB, ConsentFixture::kA, fx.duels, fx.characters, fx.grid, &fx.broadcast, 3'000);
    REQUIRE(again.isErr());
    CHECK(again.error() == DuelError::NotRequested);

    // Duel-state gates: a duelist cannot open a second front...
    auto busy = requestDuel(ConsentFixture::kA, ConsentFixture::kW1, fx.duels, fx.characters, 3'100);
    REQUIRE(busy.isErr());
    CHECK(busy.error() == DuelError::AlreadyDueling);
    // ...nobody duels themselves, unknown ids refuse, and acceptance needs
    // its request.
    auto self = requestDuel(ConsentFixture::kA, ConsentFixture::kA, fx.duels, fx.characters, 3'200);
    CHECK(self.error() == DuelError::SelfDuel);
    auto ghost = requestDuel(ConsentFixture::kA, 999, fx.duels, fx.characters, 3'300);
    CHECK(ghost.error() == DuelError::UnknownCharacter);
}

// ---- pinned: TestConsent_BroadcastReachesRadiusOnly ----------------------------------

TEST_CASE(
    "TestConsent_BroadcastReachesRadiusOnly"
)
{
    ConsentFixture fx;
    fx.broadcast.characters = &fx.characters; // the pinned-order assertion reads the fields

    REQUIRE(requestDuel(ConsentFixture::kA, ConsentFixture::kB, fx.duels, fx.characters, 1'000).isOk());
    REQUIRE(acceptDuel(ConsentFixture::kB, ConsentFixture::kA, fx.duels, fx.characters, fx.grid, &fx.broadcast, 2'000)
            .isOk());

    // Exactly one start broadcast, for the right pair at the right time.
    REQUIRE(fx.broadcast.starts.size() == 1);
    const auto &start = fx.broadcast.starts[0];
    CHECK(start.a == ConsentFixture::kA);
    CHECK(start.b == ConsentFixture::kB);
    CHECK(start.at == 2'000);

    // The A-26 gather: the midpoint of a(500,500) and b(520,500) is
    // (510, 500); the radius is CONSENT_BROADCAST_M = 50 m.
    //   kA (10 m), kB (10 m), kW1 (20 m) — inside; kW2 (90 m) — outside.
    const std::vector<harrowmere::characterId> &observers = start.observers;
    CHECK(contains(observers, ConsentFixture::kA));
    CHECK(contains(observers, ConsentFixture::kB));
    CHECK(contains(observers, ConsentFixture::kW1));
    CHECK_FALSE(contains(observers, ConsentFixture::kW2));
    // Ascending id order (the grid's sorted query, R7).
    CHECK(std::is_sorted(observers.begin(), observers.end()));

    // The broadcast carries the CONSENT, not the fight: the fields flipped
    // after the delivery (asserted inside the callback above) — from NOW the
    // damage gate is open and 2.1's check agrees.
    CHECK(harrowmere::consentsToPvp(fx.characters.get(ConsentFixture::kA), fx.characters.get(ConsentFixture::kB)));
}

// ---- pinned: TestConsent_DuelEndClearsBoth -------------------------------------------

TEST_CASE(
    "TestConsent_DuelEndClearsBoth"
)
{
    ConsentFixture fx;
    fx.broadcast.characters = &fx.characters;

    REQUIRE(requestDuel(ConsentFixture::kA, ConsentFixture::kB, fx.duels, fx.characters, 1'000).isOk());
    REQUIRE(acceptDuel(ConsentFixture::kB, ConsentFixture::kA, fx.duels, fx.characters, fx.grid, &fx.broadcast, 2'000)
            .isOk());

    // Mutual damage stamps the timeout window (3.2's report path).
    fx.duels.noteDuelDamage(ConsentFixture::kA, 2'500);
    CHECK(fx.duels.duelOf(ConsentFixture::kA) != nullptr);
    CHECK(fx.duels.duelOf(ConsentFixture::kA)->lastDamage == 2'500);

    // The yield command: ONE side concedes — BOTH fields clear.
    REQUIRE(yieldDuel(ConsentFixture::kA, fx.duels, fx.characters, fx.grid, &fx.broadcast, 3'000).isOk());
    CHECK(fx.characters.get(ConsentFixture::kA).duelOpponent == 0);
    CHECK(fx.characters.get(ConsentFixture::kB).duelOpponent == 0);
    CHECK(fx.duels.duelOf(ConsentFixture::kA) == nullptr);
    CHECK(fx.duels.duelOf(ConsentFixture::kB) == nullptr);

    // The close broadcast is a post-state fact (the fields read 0 inside the
    // callback — the recording sink asserted it) with the CURRENT watchers.
    REQUIRE(fx.broadcast.ends.size() == 1);
    CHECK(fx.broadcast.ends[0].a == ConsentFixture::kA);
    CHECK(fx.broadcast.ends[0].b == ConsentFixture::kB);
    CHECK(contains(fx.broadcast.ends[0].observers, ConsentFixture::kW1));

    // Yielding with no duel standing is a rule refusal.
    auto nothing = yieldDuel(ConsentFixture::kB, fx.duels, fx.characters, fx.grid, &fx.broadcast, 3'100);
    REQUIRE(nothing.isErr());
    CHECK(nothing.error() == DuelError::NotDueling);

    // Re-acceptance is impossible (the request went with the duel) — a fresh
    // duel needs a fresh request.
    auto stale
        = acceptDuel(ConsentFixture::kB, ConsentFixture::kA, fx.duels, fx.characters, fx.grid, &fx.broadcast, 3'200);
    CHECK(stale.error() == DuelError::NotRequested);
}

// ---- pinned: TestArena_SharedIdConsentsAllOccupants -----------------------------------

TEST_CASE(
    "TestArena_SharedIdConsentsAllOccupants"
)
{
    ArenaFixture fx;
    ConsentFixture people;

    // Region 0's center is (512, 0, 512); the CITY's arena radius is 60
    // (territory_templates.json). Two characters inside, one 100 m out.
    const harrowmere::Position insideA{512.0 + 10.0, 0.0, 512.0};
    const harrowmere::Position insideB{512.0 - 20.0, 0.0, 512.0 + 15.0};
    const harrowmere::Position outside{512.0 + 100.0, 0.0, 512.0};

    harrowmere::Character &a   = people.characters.get(ConsentFixture::kA);
    harrowmere::Character &b   = people.characters.get(ConsentFixture::kB);
    harrowmere::Character &far = people.characters.get(ConsentFixture::kW2);
    a.body.position            = insideA;
    b.body.position            = insideB;
    far.body.position          = outside;

    // Everyone starts outside the shared id.
    CHECK(a.arenaId == 0);
    CHECK(b.arenaId == 0);
    CHECK(far.arenaId == 0);

    // Entering sets arenaId = zoneId — the SAME value for all occupants (the
    // doc's shared-id consent; the zone's territory id).
    refreshArenaPresence(a, fx.arenas);
    refreshArenaPresence(b, fx.arenas);
    refreshArenaPresence(far, fx.arenas);
    CHECK(a.arenaId == ArenaFixture::kCity);
    CHECK(b.arenaId == ArenaFixture::kCity);
    CHECK(far.arenaId == 0);

    // The ONE consent check (2.1) reads the shared non-zero id: every
    // occupant consents to every other — and to nobody outside.
    CHECK(harrowmere::consentsToPvp(a, b));
    CHECK(harrowmere::consentsToPvp(b, a));
    CHECK_FALSE(harrowmere::consentsToPvp(a, far));
    CHECK_FALSE(harrowmere::consentsToPvp(far, a));
    CHECK_FALSE(harrowmere::consentsToPvp(b, far));

    // A duel inside an arena is the other channel working independently:
    // consent for the pair holds even after the arena field clears.
    a.duelOpponent = far.id; // a fenced exhibition bout with the outsider
    CHECK(harrowmere::consentsToPvp(a, far));
    CHECK_FALSE(harrowmere::consentsToPvp(b, far)); // the arena never leaks the pair's consent
    a.duelOpponent = 0;
}

// ---- pinned: TestArena_LeaveClearsId ---------------------------------------------------

TEST_CASE(
    "TestArena_LeaveClearsId"
)
{
    ArenaFixture fx;
    ConsentFixture people;

    harrowmere::Character &a = people.characters.get(ConsentFixture::kA);
    a.body.position          = {512.0 + 5.0, 0.0, 512.0};
    refreshArenaPresence(a, fx.arenas);
    CHECK(a.arenaId == ArenaFixture::kCity);
    CHECK(harrowmere::consentsToPvp(a, people.characters.get(ConsentFixture::kB)) == false); // b is elsewhere

    // Walk out (past the 60 m radius) and refresh: the field clears to the
    // doc's "0 = not in an arena" and the consent goes with it.
    harrowmere::Character &b = people.characters.get(ConsentFixture::kB);
    b.body.position          = {512.0 + 5.0, 0.0, 512.0};
    refreshArenaPresence(b, fx.arenas);
    CHECK(b.arenaId == ArenaFixture::kCity);
    CHECK(harrowmere::consentsToPvp(a, b));

    a.body.position = {512.0 + 200.0, 0.0, 512.0};
    refreshArenaPresence(a, fx.arenas);
    CHECK(a.arenaId == 0);
    // The shared-id consent needs BOTH sides inside: the leaver stopped
    // consenting AND nobody consents to a character standing outside.
    CHECK_FALSE(harrowmere::consentsToPvp(a, b));
    CHECK_FALSE(harrowmere::consentsToPvp(b, a));

    // Re-entry re-arms the shared id.
    a.body.position = {512.0, 0.0, 512.0};
    refreshArenaPresence(a, fx.arenas);
    CHECK(a.arenaId == ArenaFixture::kCity);
    CHECK(harrowmere::consentsToPvp(a, b));
}

// ---- supporting: the sweep's two end conditions -----------------------------------------

TEST_CASE(
    "TestConsent_SweepTimesOutIdleDuels"
)
{
    ConsentFixture fx;
    fx.broadcast.characters = &fx.characters;

    REQUIRE(requestDuel(ConsentFixture::kA, ConsentFixture::kB, fx.duels, fx.characters, 1'000).isOk());
    REQUIRE(acceptDuel(ConsentFixture::kB, ConsentFixture::kA, fx.duels, fx.characters, fx.grid, &fx.broadcast, 2'000)
            .isOk());
    fx.duels.noteDuelDamage(ConsentFixture::kA, 2'500); // the only mutual damage

    // A-27: 600 game-s of no mutual damage ends the duel. At 3'000 the idle
    // window is 500 — alive; at 3'100 it is exactly 600 — dead.
    sweepDuels(fx.duels, fx.characters, fx.grid, &fx.broadcast, 3'000);
    CHECK(fx.duels.duelOf(ConsentFixture::kA) != nullptr);

    sweepDuels(fx.duels, fx.characters, fx.grid, &fx.broadcast, 3'100);
    CHECK(fx.duels.duelOf(ConsentFixture::kA) == nullptr);
    CHECK(fx.characters.get(ConsentFixture::kA).duelOpponent == 0);
    CHECK(fx.characters.get(ConsentFixture::kB).duelOpponent == 0);
    REQUIRE(fx.broadcast.ends.size() == 1); // one close, not one per condition

    // Fresh damage keeps re-arming the window: a duel that trades blows
    // every few minutes never times out.
    REQUIRE(requestDuel(ConsentFixture::kA, ConsentFixture::kB, fx.duels, fx.characters, 10'000).isOk());
    REQUIRE(acceptDuel(ConsentFixture::kB, ConsentFixture::kA, fx.duels, fx.characters, fx.grid, &fx.broadcast, 11'000)
            .isOk());
    for (std::uint64_t t = 11'500; t <= 30'000; t += 500)
    {
        fx.duels.noteDuelDamage(ConsentFixture::kA, t);
        sweepDuels(fx.duels, fx.characters, fx.grid, &fx.broadcast, t + 400);
        REQUIRE(fx.duels.duelOf(ConsentFixture::kA) != nullptr);
    }
    sweepDuels(fx.duels, fx.characters, fx.grid, &fx.broadcast, 30'700);
    CHECK(fx.duels.duelOf(ConsentFixture::kA) == nullptr); // ...but a quiet 700 s ends it
}

TEST_CASE(
    "TestConsent_SweepEndsSeparatedDuels"
)
{
    ConsentFixture fx;
    fx.broadcast.characters = &fx.characters;

    REQUIRE(requestDuel(ConsentFixture::kA, ConsentFixture::kB, fx.duels, fx.characters, 1'000).isOk());
    REQUIRE(acceptDuel(ConsentFixture::kB, ConsentFixture::kA, fx.duels, fx.characters, fx.grid, &fx.broadcast, 2'000)
            .isOk());
    fx.duels.noteDuelDamage(ConsentFixture::kA, 2'100);

    // The pair stands 20 m apart: well inside the 100 m separation limit —
    // an otherwise-idle sweep leaves the duel alone.
    sweepDuels(fx.duels, fx.characters, fx.grid, &fx.broadcast, 2'200);
    CHECK(fx.duels.duelOf(ConsentFixture::kA) != nullptr);

    // One duelist walks beyond DUEL_MAX_SEPARATION_M (100 m: 520 -> 700 while
    // a stays at 500): the next sweep ends the duel — a duel is a duel
    // because both stand in it.
    fx.characters.get(ConsentFixture::kB).body.position = {700.0, 0.0, 500.0};
    fx.grid.move(ConsentFixture::kB, {520.0, 0.0, 500.0}, {700.0, 0.0, 500.0});
    sweepDuels(fx.duels, fx.characters, fx.grid, &fx.broadcast, 2'300);
    CHECK(fx.duels.duelOf(ConsentFixture::kA) == nullptr);
    CHECK(fx.characters.get(ConsentFixture::kA).duelOpponent == 0);
    CHECK(fx.characters.get(ConsentFixture::kB).duelOpponent == 0);
}

// ---- supporting: the death path ------------------------------------------------------------

TEST_CASE(
    "TestConsent_DeathEndsDuelAndPendingRequests"
)
{
    ConsentFixture fx;
    fx.broadcast.characters = &fx.characters;

    // An active duel...
    REQUIRE(requestDuel(ConsentFixture::kA, ConsentFixture::kB, fx.duels, fx.characters, 1'000).isOk());
    REQUIRE(acceptDuel(ConsentFixture::kB, ConsentFixture::kA, fx.duels, fx.characters, fx.grid, &fx.broadcast, 2'000)
            .isOk());
    // A request naming a duelist as its target refuses upfront (the target
    // could not accept it anyway — the same AlreadyDueling gate the
    // acceptance path uses).
    auto busyTarget = requestDuel(ConsentFixture::kW2, ConsentFixture::kB, fx.duels, fx.characters, 2'200);
    REQUIRE(busyTarget.isErr());
    CHECK(busyTarget.error() == DuelError::AlreadyDueling);

    // ...and a pending request BETWEEN BYSTANDERS (touches neither duelist)
    // sits beside the duel untouched.
    REQUIRE(requestDuel(ConsentFixture::kW2, ConsentFixture::kW1, fx.duels, fx.characters, 2'300).isOk());
    CHECK(fx.duels.pendingCount() == 1);

    // kB dies (3.3's death path calls this): the duel ends with the close
    // broadcast, the survivor's field clears — and the bystander request
    // (touching neither duelist) SURVIVES the death.
    endDuelOnDeath(ConsentFixture::kB, fx.duels, fx.characters, fx.grid, &fx.broadcast, 3'000);
    CHECK(fx.characters.get(ConsentFixture::kA).duelOpponent == 0);
    CHECK(fx.duels.duelOf(ConsentFixture::kA) == nullptr);
    CHECK(fx.duels.pendingCount() == 1);
    CHECK(fx.duels.hasRequest(ConsentFixture::kW2, ConsentFixture::kW1));

    // And the dead duelist's own consent field is cleared too (the store
    // record still exists until the corpse path removes it).
    CHECK(fx.characters.get(ConsentFixture::kB).duelOpponent == 0);

    // kW2 dies as well: every pending request touching the dead goes with
    // them — no posthumous acceptances.
    endDuelOnDeath(ConsentFixture::kW2, fx.duels, fx.characters, fx.grid, &fx.broadcast, 3'100);
    CHECK(fx.duels.pendingCount() == 0);
    CHECK_FALSE(fx.duels.hasRequest(ConsentFixture::kW2, ConsentFixture::kW1));
}

// ---- supporting: the request bookkeeping is replaceable, never ambiguous --------------------

TEST_CASE(
    "TestConsent_RequestBookkeeping"
)
{
    ConsentFixture fx;

    // A target with one challenger: replacing refreshes, not duplicates.
    REQUIRE(requestDuel(ConsentFixture::kA, ConsentFixture::kW1, fx.duels, fx.characters, 1'000).isOk());
    REQUIRE(requestDuel(ConsentFixture::kA, ConsentFixture::kW1, fx.duels, fx.characters, 1'500).isOk());
    CHECK(fx.duels.pendingCount() == 1);

    // A second challenger for the same target coexists (the target picks).
    REQUIRE(requestDuel(ConsentFixture::kB, ConsentFixture::kW1, fx.duels, fx.characters, 2'000).isOk());
    CHECK(fx.duels.pendingCount() == 2);

    // Acceptance picks exactly the requested pair.
    REQUIRE(acceptDuel(ConsentFixture::kW1, ConsentFixture::kB, fx.duels, fx.characters, fx.grid, &fx.broadcast, 2'500)
            .isOk());
    CHECK(fx.characters.get(ConsentFixture::kB).duelOpponent == ConsentFixture::kW1);
    CHECK(fx.characters.get(ConsentFixture::kW1).duelOpponent == ConsentFixture::kB);
    CHECK(fx.duels.pendingCount() == 1); // kA -> kW1 survives (kW1's fields are kB's business)

    // ...but kW1, now dueling, cannot accept the other request.
    auto busy
        = acceptDuel(ConsentFixture::kW1, ConsentFixture::kA, fx.duels, fx.characters, fx.grid, &fx.broadcast, 2'600);
    REQUIRE(busy.isErr());
    CHECK(busy.error() == DuelError::AlreadyDueling);
}
