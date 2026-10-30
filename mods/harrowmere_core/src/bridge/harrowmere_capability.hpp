#ifndef HARROWMERE_CORE_BRIDGE_HARROWMERE_CAPABILITY_HPP
#define HARROWMERE_CORE_BRIDGE_HARROWMERE_CAPABILITY_HPP

// The published capability surface of the harrowmere_core native mod - the
// ONLY header harrowmere_ui (and any third-party native mod) compiles against.
// The provider owns its vocabulary, engine-style: like every published
// capability header this stays implementation-, platform- and vendor-agnostic
// (plain data + virtuals; the purity guard in the mod's CMakeLists enforces it
// mechanically). See mods/README.md, section "Portability".
//
// v0.1 (task 0.1): the surface below is the CONTRACT (common.md §4.2). The
// views are inert stubs - empty snapshot types and a no-op command queue -
// and that is deliberate: the header's shape is what later phases plug into,
// so it exists and compiles from day one while carrying zero game behavior.
// Phase 2 fills the view contents (rebuilt at tick step 13 each frame) and
// wires the command queue (applied at step 9); neither the surface nor the
// family/member names change. The write-path VOCABULARY (the SimCommand
// intent envelope) landed with task 3.10 - see the write-path block below.

#include <cstdint>
#include <string>

namespace harrowmere
{

// ---- read views (v0.1 stubs; Phase 2 fills the contents) --------------------
//
// Views are SNAPSHOTS, not live references: rebuilt once per frame (tick step
// 13), read by a UI frame between ticks. No view ever exposes a store
// iterator or a raw Character pointer - ids and copies only (R10, §4.5).
// Fog-of-war is structural: views expose exactly what a player can observe
// (justice: reputation, warrants against you, signals - never caseScore or
// hidden evidence; §4.6); TestUi_NoHiddenEvidenceExposed (7.3) pins the
// whitelist.

struct WorldView
{}; // regions, territories, laws-at, safe zones, season/weather (Phase 2)

struct CharactersView
{}; // presence, species, role, power, status, needs-as-visible (Phase 2)

struct PlayerView
{}; // the local player's full state (Phase 2)

struct JusticeView
{}; // reputation, notoriety, warrants against you, jail status (Phase 2)

struct EventsView
{}; // joinable/active events, own contribution (Phase 2)

// The UI-visible signal queue (§4.2): rejection reasons and world notices
// surface back through these, never through exceptions. Task 0.7 pins the
// event-side PlayerSignal struct; the capability-side queue lives here.
struct UiSignals
{};

// ---- write path ---------------------------------------------------------------
//
// Commands are the ONLY write path from outside harrowmere_core: submit
// queues, tick step 9 applies (or rejects with a logged reason that surfaces
// as a PlayerSignal).
//
// v0.1 (task 0.1) shipped an empty command envelope. Task 3.10 (player
// control & interaction intents) pins the intent vocabulary the input layer
// produces (card 3.10 Spec: "Input layer (7.4, UI mod) produces intents
// (Move{dir}, Attack{target}, Interact{target, action}, Emote{act})") - it
// lives HERE because this header is the only vocabulary the input mod may
// compile against (DEVIATIONS.md carries the row; the shapes stay plain
// data, no new includes, purity guard untouched).
//
// The envelope carries every variant side by side (no std::variant): `kind`
// names the active one. That keeps the header POD-simple and lets later
// phases extend the set the card's own way - "consent-gated intents extend
// the intent set (2.7) - the queue must take new variants without breaking
// the contract": a new Kind value plus a new payload field is source-
// compatible for producers and consumers alike.
//
// submit() itself stays the v0.1 stub on purpose: there is no runtime sim
// instance yet (the plugin carries no world/clock/queue - the same Phase-1
// line task 3.8's DEVIATIONS.md row records), so the queue submit() fills
// and the step-9 drain (harrowmere_core::player::applyIntents over
// player::IntentQueue) are wired by the runtime bootstrap / 3.1's Scheduler.
// The vocabulary and the drain-side contract are already pinned here.

// Move{dir} - the polled motion state. The input layer re-submits its
// camera-relative direction every frame (card 7.4P1), so a 10 Hz fixed
// update receives bursts; the sim consumes a Move as MOTION STATE (the
// latest intent in the slice wins, one kinematic integration per fixed
// update - frame-rate independent; see controller.hpp). dirX/dirZ are a
// world-space direction (the input layer owns camera-relative mapping);
// the controller normalizes. sprint: A-57's "sprint x2" - free in Phase 1,
// priced with the rest need when 2.1's player mirror lands.
struct MoveIntent
{
    double dirX = 0.0;
    double dirZ = 0.0;
    bool sprint = false;
};

// Attack{target} - a combat action against the named character (0 = none).
// Step 9 consumes the intent; the application is step 10's applyDamage
// (common.md §4.6 worked trace) - the consumer lands with 3.2/3.3 (Phase 2);
// Phase 1 drains the intent and leaves the state untouched.
struct AttackIntent
{
    std::uint64_t target = 0; // characterId space (R10); 0 = none
};

// Interact{target, action} - a context interaction with a nearby character.
// The card pins the action list; COUNT exists because the handler registry
// is array-indexed by it (R9). The per-action owners: talk -> trade 3.7 /
// title dialog 6.3, loot -> corpses 3.3, report_crime + bribe -> 4.1,
// surrender -> 4.3, pick_up/drop -> inventory 2.4. The range gate and the
// handler registry are task 3.10's (src/player/interactions.hpp).
struct InteractIntent
{
    enum class Action : std::uint32_t
    {
        TALK,
        LOOT,
        REPORT_CRIME,
        BRIBE,
        SURRENDER,
        PICK_UP,
        DROP,
        COUNT
    };

    std::uint64_t target = 0; // characterId space (R10); 0 = none
    Action action        = Action::TALK;
};

// Emote{act} - a self-state toggle the world can observe (card 3.10:
// crouch toggle, light_torch - UNLIT's counter-act, mask toggle - the
// MASKED act; the item-ownership check activates with 2.4's inventory, the
// Phase-1 toggle is free but the act is logged). The suspicion acts these
// expose are core/enums.hpp's SuspicionAct; the mapping lives on the
// player-side action log (player::PlayerActions::exposes).
struct EmoteIntent
{
    enum class Act : std::uint32_t
    {
        CROUCH,
        LIGHT_TORCH,
        MASK
    };

    Act act = Act::CROUCH;
};

struct SimCommand
{
    enum class Kind : std::uint32_t
    {
        Move,
        Attack,
        Interact,
        Emote
    };

    Kind kind            = Kind::Move;
    MoveIntent move{};      // kind == Move
    AttackIntent attack{};  // kind == Attack
    InteractIntent interact{}; // kind == Interact
    EmoteIntent emote{};    // kind == Emote
};

// Capability "harrowmere": the game's authoritative simulation. Consumers
// acquire through the family name; the descriptor declares the
// implementation-bound member ("harrowmere.core") exactly as an engine module
// does. Mods consume it through the family ("harrowmere"); the app acquires
// it as a hard requirement (a run without its sim is not Harrowmere).
class SimCapability
{
public:
    static constexpr const char *capabilityName = "harrowmere.core";
    virtual ~SimCapability()                    = default;

    // ---- reads: stable, cheap view snapshots (rebuilt at step 13 each frame) ----
    virtual const WorldView &world() const;           // regions, territories, laws-at, safe zones, season/weather
    virtual const CharactersView &characters() const; // presence, species, role, power, status, needs (as visible)
    virtual const PlayerView &player() const;         // the local player's full state
    virtual const JusticeView &justice() const;       // reputation, notoriety, warrants against you, jail status
    virtual const EventsView &events() const;         // joinable/active events, own contribution
    virtual const UiSignals &signals() const;         // PlayerSignals queue (investigated / wanted / poster / greeting)

    // ---- writes: commands, never direct state mutation ----
    // Queued; applied at step 9 next tick (§4.4) or rejected with a logged
    // reason that surfaces back through a PlayerSignal.
    virtual void submit(SimCommand cmd);

private:
    // v0.1 stub storage: one inert snapshot per view. Phase 2 replaces these
    // with rebuilt-per-frame view data; the accessor signatures do not move.
    WorldView world_{};
    CharactersView characters_{};
    PlayerView player_{};
    JusticeView justice_{};
    EventsView events_{};
    UiSignals signals_{};
};

// ---- inline definitions (v0.1 inert stubs) ----------------------------------

inline const WorldView &SimCapability::world() const
{
    return world_;
}

inline const CharactersView &SimCapability::characters() const
{
    return characters_;
}

inline const PlayerView &SimCapability::player() const
{
    return player_;
}

inline const JusticeView &SimCapability::justice() const
{
    return justice_;
}

inline const EventsView &SimCapability::events() const
{
    return events_;
}

inline const UiSignals &SimCapability::signals() const
{
    return signals_;
}

inline void SimCapability::submit(
    SimCommand
)
{
    // v0.1: no command vocabulary exists yet; nothing to queue. Phase 2 fills
    // the queue here and tick step 9 drains it (§4.4).
}

} // namespace harrowmere

#endif // HARROWMERE_CORE_BRIDGE_HARROWMERE_CAPABILITY_HPP
