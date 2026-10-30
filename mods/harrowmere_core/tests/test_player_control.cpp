// test_player_control.cpp — Task 3.10's test suite.
//
// Implements the tests pinned in phase-1-walk.md card 3.10's Tests block
// (names verbatim — common.md §3.5: "Test names are pinned; keep them"):
//
//   TestMove_IntentsApplyOnFixedUpdate       Move intents integrate once per
//                                            fixed update (motion state,
//                                            latest wins), at MOVE_SPEED_MPS
//                                            (A-57) with the sprint x2
//   TestInteract_RangeGated                  the proximity gate (INCLUSIVE at
//                                            INTERACT_RANGE_M) plus the
//                                            resolver/registry seams
//   TestAfk_TimeoutAndClear                  D-AFK-1: A-58 real seconds
//                                            without intents flags AFK until
//                                            the next intent
//   TestCrouchMask_ExposeSuspicionActs       the acts are exposed on the
//                                            action log; 4.5 reads them from
//                                            Phase 3
//   TestActionLog_CountersIncrement          the record* helpers are the
//                                            single write path of the
//                                            reqActions counters
//
// The remaining cases pin the structural rules the card's prose and the
// common contract name: the queue's FIFO order (R7), the clock being
// read-only (the clocks advance at step 0 — 3.8's Phase-1 slice; a
// double-advance here would drift every Phase-1 slice), the deterministic
// drain of the Attack intent (step 10's applyDamage is the consumer, Phase
// 2), the zero-direction fallback (R2), y staying cosmetic (card 1.6), and
// the end-to-end slice shape the headless CI drives.
//
// Headless, per the card's Done-when: the tests drive the controller
// WITHOUT the UI mod — intents are injected into IntentQueue directly. No
// presentation code exists here (input devices are 7.4's job, Phase 2);
// this suite owns semantics only.
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here —
// tests/main.cpp owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include "core/clock.hpp"     // WorldClock — the one heartbeat (D-TIME-1)
#include "core/constants.hpp" // A-57 / A-58 / INTERACT_RANGE_M
#include "core/enums.hpp"     // SuspicionAct
#include "core/types.hpp"     // Position
#include "player/controller.hpp"
#include "player/interactions.hpp"

using namespace harrowmere;
using namespace harrowmere::player;

namespace
{

// A fake target resolver: id -> position (std::map, R7-ordered even though
// lookups never iterate — the fixture keeps the deterministic container
// discipline out of principle). Phase 2's store-backed resolver replaces
// this at the step-9 registration.
class FakeResolver final : public InteractionResolver
{
public:
    std::map<std::uint64_t, Position> targets;

    const Position *positionOf(
        std::uint64_t target
    ) const override
    {
        const auto it = targets.find(target);
        return it == targets.end() ? nullptr : &it->second;
    }
};

// A handler capture: what the routed request delivered (action, target,
// position, worldTime) and how many times it ran.
struct HandlerCapture
{
    int calls                                     = 0;
    InteractIntent::Action action                 = InteractIntent::Action::TALK;
    std::uint64_t target                          = 0;
    Position position{};
    std::uint64_t worldTime                       = 0;

    void record(
        const InteractionRequest &request
    )
    {
        ++calls;
        action        = request.action;
        target        = request.target;
        position      = request.targetPosition;
        worldTime     = request.worldTime;
    }
};

// Intent builders — the exact shapes 7.4P1's input mapper will produce.
SimCommand moveIntent(
    double dirX, double dirZ, bool sprint = false
)
{
    SimCommand cmd;
    cmd.kind        = SimCommand::Kind::Move;
    cmd.move.dirX   = dirX;
    cmd.move.dirZ   = dirZ;
    cmd.move.sprint = sprint;
    return cmd;
}

SimCommand attackIntent(
    std::uint64_t target
)
{
    SimCommand cmd;
    cmd.kind         = SimCommand::Kind::Attack;
    cmd.attack.target = target;
    return cmd;
}

SimCommand interactIntent(
    std::uint64_t target, InteractIntent::Action action
)
{
    SimCommand cmd;
    cmd.kind            = SimCommand::Kind::Interact;
    cmd.interact.target = target;
    cmd.interact.action = action;
    return cmd;
}

SimCommand emoteIntent(
    EmoteIntent::Act act
)
{
    SimCommand cmd;
    cmd.kind      = SimCommand::Kind::Emote;
    cmd.emote.act = act;
    return cmd;
}

// One fixed update at the assembly's pinned pacing (D-TICK-1:
// fixed_update_hz = 10 -> the constant step delta is 0.1 real seconds).
constexpr double kFixedStepS = 0.1;

} // namespace

// ---------------------------------------------------------------------------
// TestMove_IntentsApplyOnFixedUpdate (pinned)
// Move is motion state: the latest Move intent in the slice wins and ONE
// kinematic integration (speed x dt) runs per fixed update — frame-rate
// independent, at A-57's 4 m/s, sprint x2.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestMove_IntentsApplyOnFixedUpdate"
)
{
    // A-57, spelled in constants.hpp: 4 m/s walk, sprint x2.
    CHECK(MOVE_SPEED_MPS == doctest::Approx(4.0));
    CHECK(MOVE_SPRINT_FACTOR == doctest::Approx(2.0));

    SUBCASE("an empty queue integrates nothing")
    {
        PlayerController player;
        IntentQueue queue;
        core::WorldClock clock;

        applyIntents(player, queue, clock, kFixedStepS);

        CHECK(queue.empty());
        CHECK(player.position.x == doctest::Approx(0.0));
        CHECK(player.position.z == doctest::Approx(0.0));
    }

    SUBCASE("one Move intent advances one step of speed x dt")
    {
        PlayerController player;
        IntentQueue queue;
        core::WorldClock clock;

        queue.push(moveIntent(1.0, 0.0)); // east
        applyIntents(player, queue, clock, kFixedStepS);

        // 4 m/s x 0.1 s = 0.4 m per fixed update at 10 Hz.
        CHECK(player.position.x == doctest::Approx(0.4));
        CHECK(player.position.z == doctest::Approx(0.0));

        // The next update integrates again ONLY when a fresh intent
        // arrives: motion state does not coast (the input layer re-submits
        // polled state every frame while the key is held — 7.4P1).
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK(player.position.x == doctest::Approx(0.4));

        queue.push(moveIntent(1.0, 0.0));
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK(player.position.x == doctest::Approx(0.8));
    }

    SUBCASE("directions normalize — the diagonal is not faster")
    {
        PlayerController player;
        IntentQueue queue;
        core::WorldClock clock;

        queue.push(moveIntent(3.0, 4.0)); // length 5
        applyIntents(player, queue, clock, kFixedStepS);

        // 0.4 m along the unit direction (0.6, 0.8).
        CHECK(player.position.x == doctest::Approx(0.4 * 0.6));
        CHECK(player.position.z == doctest::Approx(0.4 * 0.8));
    }

    SUBCASE("the zero direction is the pinned no-motion fallback (R2)")
    {
        PlayerController player;
        IntentQueue queue;
        core::WorldClock clock;

        player.position = Position{1.0, 2.0, 3.0};
        queue.push(moveIntent(0.0, 0.0));
        applyIntents(player, queue, clock, kFixedStepS);

        // No NaN, no movement — and y is cosmetic, never touched (card 1.6).
        CHECK(player.position.x == doctest::Approx(1.0));
        CHECK(player.position.y == doctest::Approx(2.0));
        CHECK(player.position.z == doctest::Approx(3.0));
        CHECK(std::isfinite(player.position.x));
        CHECK(std::isfinite(player.position.z));
    }

    SUBCASE("sprint doubles the step (A-57: sprint x2, free in Phase 1)")
    {
        PlayerController player;
        IntentQueue queue;
        core::WorldClock clock;

        queue.push(moveIntent(1.0, 0.0, true));
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK(player.position.x == doctest::Approx(0.8));
    }

    SUBCASE("the latest Move intent in a burst wins — one integration per update")
    {
        PlayerController player;
        IntentQueue queue;
        core::WorldClock clock;

        // A 60 fps input layer submits ~6 intents per 10 Hz tick: six
        // per-intent integrations would make speed frame-rate dependent.
        // The burst collapses to the latest state, integrated once.
        queue.push(moveIntent(1.0, 0.0));
        queue.push(moveIntent(1.0, 0.0));
        queue.push(moveIntent(1.0, 0.0));
        queue.push(moveIntent(0.0, 1.0));
        applyIntents(player, queue, clock, kFixedStepS);

        CHECK(player.position.x == doctest::Approx(0.0));
        CHECK(player.position.z == doctest::Approx(0.4));

        // A burst ending in the zero direction stops the player.
        queue.push(moveIntent(0.0, 1.0));
        queue.push(moveIntent(0.0, 0.0));
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK(player.position.z == doctest::Approx(0.4));
    }
}

// ---------------------------------------------------------------------------
// TestInteract_RangeGated (pinned)
// "Interactions by proximity + target type": the gate is INCLUSIVE at
// INTERACT_RANGE_M on the x/z plane (D-WORLD-1 helper), unknown targets
// reject before any range math, and routing dispatches to the registered
// owner handler (D-SINK-1's null default until one lands).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestInteract_RangeGated"
)
{
    // 3.0 m, pinned pending Appendix A promotion (constants.hpp +
    // DEVIATIONS.md) — strictly inside every pinned social radius.
    CHECK(INTERACT_RANGE_M == doctest::Approx(3.0));

    SUBCASE("a known target beyond the range rejects without dispatching")
    {
        PlayerController player;
        FakeResolver resolver;
        resolver.targets[7] = Position{10.0, 0.0, 0.0}; // 10 m away
        InteractionHandlers handlers;
        HandlerCapture talk;
        handlers.registerHandler(InteractIntent::Action::TALK, [&talk](const InteractionRequest &request) { talk.record(request); });

        const InteractionGate gate = routeInteraction(player.position, 7, InteractIntent::Action::TALK, 0, resolver, handlers);

        CHECK(gate == InteractionGate::OutOfRange);
        CHECK(talk.calls == 0);
    }

    SUBCASE("a known target within range dispatches to the owner handler")
    {
        PlayerController player;
        player.position = Position{8.5, 0.0, 0.0};
        FakeResolver resolver;
        resolver.targets[7] = Position{10.0, 0.0, 0.0}; // 1.5 m away
        InteractionHandlers handlers;
        HandlerCapture talk;
        handlers.registerHandler(InteractIntent::Action::TALK, [&talk](const InteractionRequest &request) { talk.record(request); });

        const InteractionGate gate = routeInteraction(player.position, 7, InteractIntent::Action::TALK, 4321, resolver, handlers);

        CHECK(gate == InteractionGate::Dispatched);
        REQUIRE(talk.calls == 1);
        CHECK(talk.action == InteractIntent::Action::TALK);
        CHECK(talk.target == 7);
        CHECK(talk.position.x == doctest::Approx(10.0));
        CHECK(talk.position.z == doctest::Approx(0.0));
        CHECK(talk.worldTime == 4321);
    }

    SUBCASE("the boundary is inclusive — exactly at the range dispatches")
    {
        PlayerController player;
        player.position = Position{8.0, 0.0, 0.0};
        FakeResolver resolver;
        resolver.targets[7] = Position{11.0, 0.0, 0.0}; // exactly 3.0 m
        InteractionHandlers handlers;
        HandlerCapture loot;
        handlers.registerHandler(InteractIntent::Action::LOOT, [&loot](const InteractionRequest &request) { loot.record(request); });

        const InteractionGate gate = routeInteraction(player.position, 7, InteractIntent::Action::LOOT, 0, resolver, handlers);

        CHECK(gate == InteractionGate::Dispatched);
        CHECK(loot.calls == 1);
    }

    SUBCASE("an unknown target rejects before the range math")
    {
        PlayerController player;
        FakeResolver resolver; // no targets at all
        InteractionHandlers handlers;
        HandlerCapture talk;
        handlers.registerHandler(InteractIntent::Action::TALK, [&talk](const InteractionRequest &request) { talk.record(request); });

        CHECK(routeInteraction(player.position, 7, InteractIntent::Action::TALK, 0, resolver, handlers) == InteractionGate::UnknownTarget);
        // id 0 is the reserved id (R10) — the resolver's nullptr rules it out too.
        CHECK(routeInteraction(player.position, 0, InteractIntent::Action::TALK, 0, resolver, handlers) == InteractionGate::UnknownTarget);
        CHECK(talk.calls == 0);
    }

    SUBCASE("an unregistered action consumes as the D-SINK-1 null default")
    {
        PlayerController player;
        player.position = Position{0.0, 0.0, 0.0};
        FakeResolver resolver;
        resolver.targets[3] = Position{1.0, 0.0, 0.0};

        // The shared empty registry: every action unclaimed — the exact
        // Phase-1 shape. No crash, visible outcome, intent consumed.
        CHECK(routeInteraction(player.position, 3, InteractIntent::Action::SURRENDER, 0, resolver, InteractionHandlers::empty())
              == InteractionGate::NoHandler);
    }

    SUBCASE("end to end: an Interact intent through applyIntents")
    {
        PlayerController player;
        player.position = Position{9.0, 0.0, 0.0};
        IntentQueue queue;
        core::WorldClock clock;
        clock.worldTime = 777;
        FakeResolver resolver;
        resolver.targets[9] = Position{10.0, 0.0, 0.0}; // 1 m away
        InteractionHandlers handlers;
        HandlerCapture talk;
        handlers.registerHandler(InteractIntent::Action::TALK, [&talk](const InteractionRequest &request) { talk.record(request); });

        queue.push(interactIntent(9, InteractIntent::Action::TALK));
        applyIntents(player, queue, clock, kFixedStepS, resolver, handlers);

        CHECK(queue.empty());
        REQUIRE(talk.calls == 1);
        CHECK(talk.worldTime == 777);
        CHECK(talk.target == 9);
    }
}

// ---------------------------------------------------------------------------
// TestAfk_TimeoutAndClear (pinned)
// D-AFK-1: no UI intents for A-58's 120 REAL seconds -> afk = true until
// the next intent. RealTime (D-TIME-1), never worldTime, never the wall
// clock (R8).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestAfk_TimeoutAndClear"
)
{
    CHECK(AFK_TIMEOUT_REAL_S == 120);

    SUBCASE("a fresh session is not AFK and stays awake inside the window")
    {
        PlayerController player;
        IntentQueue queue;
        core::WorldClock clock;

        applyIntents(player, queue, clock, kFixedStepS);
        CHECK_FALSE(player.afk);

        clock.realTime = 119; // one real second shy of A-58
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK_FALSE(player.afk);
    }

    SUBCASE("A-58 real seconds without intents flags AFK")
    {
        PlayerController player;
        IntentQueue queue;
        core::WorldClock clock;

        clock.realTime = AFK_TIMEOUT_REAL_S; // exactly 120: "no intents FOR 120 s"
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK(player.afk);

        clock.realTime = AFK_TIMEOUT_REAL_S + 500; // long past it — still AFK
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK(player.afk);
    }

    SUBCASE("the next intent clears AFK — and any intent counts")
    {
        PlayerController player;
        IntentQueue queue;
        core::WorldClock clock;
        clock.realTime = AFK_TIMEOUT_REAL_S;
        applyIntents(player, queue, clock, kFixedStepS);
        REQUIRE(player.afk);

        // A rejected interaction (unknown target) is still an intent: the
        // player is present and pressing keys (D-AFK-1: "without intents").
        FakeResolver resolver; // resolves nothing
        queue.push(interactIntent(42, InteractIntent::Action::TALK));
        applyIntents(player, queue, clock, kFixedStepS, resolver);
        CHECK_FALSE(player.afk);
        CHECK(player.lastIntentRealTime == AFK_TIMEOUT_REAL_S);
    }

    SUBCASE("AFK tracks realTime, never worldTime (D-TIME-1 / R8)")
    {
        PlayerController player;
        IntentQueue queue;
        core::WorldClock clock;

        // Game time racing far ahead (TIME_SCALE 60) must not wake or flag
        // the player on its own: only realTime and the intents decide.
        clock.worldTime = 100'000'000;
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK_FALSE(player.afk);
    }
}

// ---------------------------------------------------------------------------
// TestCrouchMask_ExposeSuspicionActs (pinned)
// The acts are exposed on the action log (the card's Phase-3 hand-off:
// "4.5 reads them from Phase 3"); the acts 3.10 does not own read false.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestCrouchMask_ExposeSuspicionActs"
)
{
    SUBCASE("the fresh log exposes UNLIT only — no torch burns yet")
    {
        PlayerActions log;
        CHECK_FALSE(log.exposes(SuspicionAct::CROUCH));
        CHECK(log.exposes(SuspicionAct::UNLIT)); // the absence of light
        CHECK_FALSE(log.exposes(SuspicionAct::MASKED));

        // The boundary: these acts are not player-toggled — their owners
        // set them (TRESPASS 4.5, BLOODSTAINED 4.1, FLEE 3.2).
        CHECK_FALSE(log.exposes(SuspicionAct::TRESPASS));
        CHECK_FALSE(log.exposes(SuspicionAct::BLOODSTAINED));
        CHECK_FALSE(log.exposes(SuspicionAct::FLEE));
    }

    SUBCASE("crouch toggles the CROUCH act")
    {
        PlayerController player;
        IntentQueue queue;
        core::WorldClock clock;

        queue.push(emoteIntent(EmoteIntent::Act::CROUCH));
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK(player.actions.exposes(SuspicionAct::CROUCH));

        queue.push(emoteIntent(EmoteIntent::Act::CROUCH));
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK_FALSE(player.actions.exposes(SuspicionAct::CROUCH));
    }

    SUBCASE("light_torch counters the UNLIT act")
    {
        PlayerController player;
        IntentQueue queue;
        core::WorldClock clock;

        queue.push(emoteIntent(EmoteIntent::Act::LIGHT_TORCH));
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK_FALSE(player.actions.exposes(SuspicionAct::UNLIT)); // the torch burns

        queue.push(emoteIntent(EmoteIntent::Act::LIGHT_TORCH));
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK(player.actions.exposes(SuspicionAct::UNLIT)); // doused again
    }

    SUBCASE("mask toggles the MASKED act (free in Phase 1, act logged)")
    {
        PlayerController player;
        IntentQueue queue;
        core::WorldClock clock;

        queue.push(emoteIntent(EmoteIntent::Act::MASK));
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK(player.actions.exposes(SuspicionAct::MASKED));

        queue.push(emoteIntent(EmoteIntent::Act::MASK));
        applyIntents(player, queue, clock, kFixedStepS);
        CHECK_FALSE(player.actions.exposes(SuspicionAct::MASKED));
    }

    SUBCASE("appendActiveActs lists the exposed acts in enum order (R7)")
    {
        PlayerActions log;
        log.crouched = true;
        log.masked   = true; // torch unlit: UNLIT also exposed

        std::vector<SuspicionAct> acts;
        log.appendActiveActs(acts);

        REQUIRE(acts.size() == 3);
        CHECK(acts[0] == SuspicionAct::CROUCH);
        CHECK(acts[1] == SuspicionAct::UNLIT);
        CHECK(acts[2] == SuspicionAct::MASKED);

        // A fresh log is quiet about everything but the dark: no torch
        // burns yet, so UNLIT is the one exposed act.
        PlayerActions fresh;
        std::vector<SuspicionAct> freshActs;
        fresh.appendActiveActs(freshActs);
        REQUIRE(freshActs.size() == 1);
        CHECK(freshActs[0] == SuspicionAct::UNLIT);

        // With the torch lit, the log appends nothing at all.
        PlayerActions quiet;
        quiet.torchLit = true;
        std::vector<SuspicionAct> none;
        quiet.appendActiveActs(none);
        CHECK(none.empty());
    }
}

// ---------------------------------------------------------------------------
// TestActionLog_CountersIncrement (pinned)
// The record* helpers are the single write path of the reqActions inputs
// (2.3) and quest objectives (6.4); their owner systems call them when they
// land (kills 3.3, crafts 2.4, purchases 3.7).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestActionLog_CountersIncrement"
)
{
    PlayerActions log;

    CHECK(log.killsSapient == 0);
    CHECK(log.killsNonSapient == 0);
    CHECK(log.crafts == 0);
    CHECK(log.purchases == 0);

    log.recordKill(true); // a sapient kill (person)
    log.recordKill(true);
    log.recordKill(false); // an animal/monster kill
    log.recordCraft();
    log.recordCraft();
    log.recordCraft();
    log.recordPurchase();

    CHECK(log.killsSapient == 2);
    CHECK(log.killsNonSapient == 1);
    CHECK(log.crafts == 3);
    CHECK(log.purchases == 1);

    // The counters and the act surface share the log but not state space:
    // recording does not move the acts, toggling does not move the counts.
    PlayerActions both;
    both.recordKill(true);
    both.recordCraft();
    CHECK_FALSE(both.exposes(SuspicionAct::CROUCH));
    CHECK(both.killsSapient == 1);
    both.crouched = true;
    CHECK(both.killsSapient == 1);
    CHECK(both.crafts == 1);
}

// ---------------------------------------------------------------------------
// Structural extras (the card's prose and the common contract, pinned)
// ---------------------------------------------------------------------------

// The queue is FIFO — the input layer's intent order is the application
// order (R7), across kinds.
TEST_CASE(
    "TestPlayerQueue_FifoOrderAcrossKinds"
)
{
    PlayerController player;
    player.position = Position{9.0, 0.0, 0.0};
    IntentQueue queue;
    core::WorldClock clock;
    clock.worldTime = 500;

    FakeResolver resolver;
    resolver.targets[1] = Position{10.0, 0.0, 0.0}; // in range
    InteractionHandlers handlers;
    HandlerCapture talk;
    handlers.registerHandler(InteractIntent::Action::TALK, [&talk](const InteractionRequest &request) { talk.record(request); });

    queue.push(moveIntent(1.0, 0.0)); // east first...
    queue.push(emoteIntent(EmoteIntent::Act::CROUCH)); // ...then crouch...
    queue.push(interactIntent(1, InteractIntent::Action::TALK)); // ...then talk

    applyIntents(player, queue, clock, kFixedStepS, resolver, handlers);

    // The move applied (east), the emote applied, the interact routed — in
    // one slice, order preserved, queue fully drained.
    CHECK(queue.empty());
    CHECK(player.position.x == doctest::Approx(9.4));
    CHECK(player.actions.exposes(SuspicionAct::CROUCH));
    CHECK(talk.calls == 1);
}

// The clocks are READ-ONLY here: they advance at step 0 (0.3 / 3.8's
// Phase-1 slice) — a double-advance would drift every composed slice.
TEST_CASE(
    "TestPlayerSlice_NeverAdvancesTheClocks"
)
{
    PlayerController player;
    IntentQueue queue;
    queue.push(moveIntent(1.0, 0.0));
    queue.push(emoteIntent(EmoteIntent::Act::MASK));

    core::WorldClock clock;
    clock.worldTime = 1000;
    clock.realTime  = 30;

    applyIntents(player, queue, clock, kFixedStepS);

    CHECK(clock.worldTime == 1000);
    CHECK(clock.realTime == 30);
    CHECK(clock.worldCarry == 0);
    CHECK(clock.realCarry == 0);
}

// The Attack intent drains deterministically in Phase 1: step 10's
// applyDamage is its consumer (Phase 2, 3.2/3.3) — the drop is pinned so
// that Phase 2's arrival is a conscious change, not a surprise.
TEST_CASE(
    "TestPlayerSlice_AttackDrainsUntilCombatLands"
)
{
    PlayerController player;
    player.position = Position{1.0, 0.0, 2.0};
    IntentQueue queue;
    core::WorldClock clock;

    queue.push(attackIntent(5));
    queue.push(attackIntent(0)); // 0 = none — equally harmless here

    applyIntents(player, queue, clock, kFixedStepS);

    CHECK(queue.empty());
    CHECK(player.position.x == doctest::Approx(1.0)); // no motion, no state touched
    CHECK(player.position.z == doctest::Approx(2.0));
    CHECK_FALSE(player.afk);
    CHECK(player.actions.killsSapient == 0); // the action log is combat-aftermath's to write
}

// The default arguments are the Phase-1 shape: no resolver wiring, no
// handler registrations — applyIntents runs standalone and interactions
// consume as NoHandler/UnknownTarget (D-SINK-1).
TEST_CASE(
    "TestPlayerSlice_DefaultsRunStandalone"
)
{
    PlayerController player;
    player.position = Position{100.0, 0.0, 100.0};
    IntentQueue queue;
    core::WorldClock clock;

    queue.push(interactIntent(11, InteractIntent::Action::REPORT_CRIME));
    queue.push(interactIntent(0, InteractIntent::Action::BRIBE));

    applyIntents(player, queue, clock, kFixedStepS); // both defaults (null resolver, empty registry)

    CHECK(queue.empty()); // consumed either way — the sim never re-delivers
    CHECK(player.position.x == doctest::Approx(100.0)); // no interaction moved the player
}
