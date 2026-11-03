// Unit tests for the sim event channels (task 0.7). The two pinned tests
// exercise the engine-bus behaviors the game's systems rely on - publish-
// after-mutation ordering, and the RAII subscription contract around
// unsubscribing during a dispatch - through the game's own event structs, so
// a future kernel sync that changes either behavior fails here before any
// system code stands on it. They are also the compile check that the pinned
// structs work as engine channel payloads (each game event type is one
// EventChannel keyed by type).

#include <algorithm>
#include <cstdint>
#include <vector>

#include <doctest.h>

#include "engine/kernel/event_bus.hpp"

#include "game_events.hpp"

namespace
{

using harrowmere::characterId;

// Opaque, meaningless fixture ids (R10: ids are never meaningful; these are
// test fixtures, not game constants - no Appendix A row applies).
constexpr characterId testCulpritId          = 4021;
constexpr characterId testVillageTerritoryId = 17;
constexpr std::uint64_t testEvidenceId       = 9001;
constexpr characterId testVictimId           = 777;
constexpr characterId testEnvironmentKiller  = 0; // the docs' 0 = environment convention

// Reads the live manhunt target set the way a §4.4 step handler would: the
// handler is dispatched while the simulation state around it already carries
// the mutation that preceded the publish.
struct CrimeHandler
{
    const std::vector<characterId> *manhuntTargetIds = nullptr;
    bool observedPostState                           = false;
    std::uint64_t observedEvidenceId                 = 0;

    void onCrimeRecorded(
        const harrowmere::CrimeRecorded &event
    )
    {
        observedEvidenceId = event.evidenceId;
        observedPostState
            = std::find(manhuntTargetIds->begin(), manhuntTargetIds->end(), event.culprit) != manhuntTargetIds->end();
    }
};

struct DeathObserver
{
    std::vector<characterId> seenVictims;
    std::vector<engine::EventSubscription *> releaseOnDeath; // test hook: release subscriptions mid-dispatch

    void onCharacterDied(
        const harrowmere::CharacterDied &event
    )
    {
        seenVictims.push_back(event.victim);
        for (engine::EventSubscription *subscription : releaseOnDeath)
        {
            subscription->release();
        }
    }
};

} // namespace

// The ordering rule (task 0.7, rule 1): events are facts that already
// happened. The mutation completes BEFORE the publish, so the handler
// observes the post-state (the §4.6 worked trace: a manhunt handler must see
// the culprit already inside its target set), and the event carries the
// post-mutation values. Also pins the engine's synchronous dispatch: the
// handler's effects are observable the moment publish returns.
TEST_CASE(
    "TestEvents_PublishAfterMutationOrdering"
)
{
    engine::EventBus bus;
    engine::EventChannel<harrowmere::CrimeRecorded> &channel = bus.channel<harrowmere::CrimeRecorded>();

    // Stand-in for the justice/manhunt state: the mutating system owns this
    // and finishes the mutation first, then publishes the fact.
    std::vector<characterId> manhuntTargetIds;

    CrimeHandler handler;
    handler.manhuntTargetIds = &manhuntTargetIds;
    channel.subscribe<CrimeHandler, &CrimeHandler::onCrimeRecorded>(&handler);

    // The mutation completes first: the culprit is inserted into the manhunt
    // target set (and the evidence id is assigned by the store stand-in
    // below), exactly the state later systems must see.
    manhuntTargetIds.push_back(testCulpritId);

    // Then the fact is published.
    bus.publish(
        harrowmere::CrimeRecorded{testEvidenceId, testCulpritId, harrowmere::CrimeType::ASSAULT, testVillageTerritoryId}
    );

    // The handler observed the post-state...
    CHECK(handler.observedPostState);
    // ...and the event carried the post-mutation value. This check also pins
    // synchronous dispatch: observedEvidenceId is set only inside the
    // handler, so it can carry the published value here only if the dispatch
    // completed before publish() returned (a deferred dispatch would leave
    // the field at 0).
    CHECK(handler.observedEvidenceId == testEvidenceId);
}

// The engine guarantee the subscription model relies on (task 0.7, rule 3):
// subscriptions are RAII, and an unsubscribe that lands during a dispatch
// never produces a dangling dispatch. The kernel's exact contract, pinned
// here so a kernel sync that changes it fails this test and not a system:
//
//   - The STRUCTURAL removal is deferred: the handler vector is not mutated
//     mid-scan (the DispatchGuard erases the deferred removals after the
//     scan completes), so an already-visited handler keeps its in-flight
//     delivery.
//   - The LOGICAL removal is immediate: the publish scan skips
//     pendingRemoval entries, so a handler released before the scan reaches
//     it is not invoked for the current event either - once release() ran,
//     the stopped component is never invoked again.
//
// Three observers at equal priority (dispatch = subscription order); the
// releaser sits BETWEEN them, so one dispatch pins both halves of the
// contract: `visited` is already scanned when the releaser runs (keeps its
// in-flight event), `pending` is still ahead of the scan (never sees it), and
// the releaser itself keeps receiving.
TEST_CASE(
    "TestEvents_UnsubscribeDuringDispatchDeferred"
)
{
    engine::EventBus bus;
    engine::EventChannel<harrowmere::CharacterDied> &channel = bus.channel<harrowmere::CharacterDied>();

    DeathObserver visited;
    DeathObserver releaser;
    DeathObserver pending;

    engine::EventSubscription visitedSubscription{
        &bus,
        typeid(harrowmere::CharacterDied),
        channel.subscribe<DeathObserver, &DeathObserver::onCharacterDied>(&visited)
    };
    engine::EventSubscription releaserSubscription{
        &bus,
        typeid(harrowmere::CharacterDied),
        channel.subscribe<DeathObserver, &DeathObserver::onCharacterDied>(&releaser)
    };
    engine::EventSubscription pendingSubscription{
        &bus,
        typeid(harrowmere::CharacterDied),
        channel.subscribe<DeathObserver, &DeathObserver::onCharacterDied>(&pending)
    };

    // The releaser runs second and releases both other subscriptions
    // mid-dispatch: one behind the scan, one ahead of it.
    releaser.releaseOnDeath = {&visitedSubscription, &pendingSubscription};

    bus.publish(harrowmere::CharacterDied{testVictimId, testEnvironmentKiller});

    // `visited` was scanned before its release landed: the deferred
    // structural removal keeps its in-flight delivery.
    REQUIRE(visited.seenVictims.size() == 1);
    CHECK(visited.seenVictims[0] == testVictimId);
    // `pending` was released before the scan reached it: the immediate
    // logical removal means no dangling dispatch.
    CHECK(pending.seenVictims.empty());
    // The releaser itself completed its dispatch normally.
    REQUIRE(releaser.seenVictims.size() == 1);

    bus.publish(harrowmere::CharacterDied{testVictimId, testEnvironmentKiller});

    // From the next publish on, both released handlers are silent and the
    // scan is uncorrupted.
    CHECK(visited.seenVictims.size() == 1);
    CHECK(pending.seenVictims.empty());
    REQUIRE(releaser.seenVictims.size() == 2);

    // RAII teardown: an explicit release unsubscribes the remaining handler;
    // releasing an already-released guard is a no-op.
    releaserSubscription.release();
    visitedSubscription.release();
    pendingSubscription.release();

    bus.publish(harrowmere::CharacterDied{testVictimId, testEnvironmentKiller});

    CHECK(releaser.seenVictims.size() == 2);
}
