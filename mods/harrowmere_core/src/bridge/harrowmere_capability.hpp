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
// the command vocabulary (applied at step 9); neither the surface nor the
// family/member names change.

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
{
}; // regions, territories, laws-at, safe zones, season/weather (Phase 2)

struct CharactersView
{
}; // presence, species, role, power, status, needs-as-visible (Phase 2)

struct PlayerView
{
}; // the local player's full state (Phase 2)

struct JusticeView
{
}; // reputation, notoriety, warrants against you, jail status (Phase 2)

struct EventsView
{
}; // joinable/active events, own contribution (Phase 2)

// The UI-visible signal queue (§4.2): rejection reasons and world notices
// surface back through these, never through exceptions. Task 0.7 pins the
// event-side PlayerSignal struct; the capability-side queue lives here.
struct UiSignals
{
};

// ---- write path (v0.1 stub) --------------------------------------------------
//
// Commands are the ONLY write path from outside harrowmere_core: submit
// queues, tick step 9 applies (or rejects with a logged reason that surfaces
// as a PlayerSignal). Phase 2 pins the concrete command variants; v0.1
// carries an empty vocabulary so the seam compiles.

struct SimCommand
{
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
    virtual ~SimCapability() = default;

    // ---- reads: stable, cheap view snapshots (rebuilt at step 13 each frame) ----
    virtual const WorldView &world() const;      // regions, territories, laws-at, safe zones, season/weather
    virtual const CharactersView &characters() const; // presence, species, role, power, status, needs (as visible)
    virtual const PlayerView &player() const;    // the local player's full state
    virtual const JusticeView &justice() const;  // reputation, notoriety, warrants against you, jail status
    virtual const EventsView &events() const;    // joinable/active events, own contribution
    virtual const UiSignals &signals() const;    // PlayerSignals queue (investigated / wanted / poster / greeting)

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

inline void SimCapability::submit(SimCommand)
{
    // v0.1: no command vocabulary exists yet; nothing to queue. Phase 2 fills
    // the queue here and tick step 9 drains it (§4.4).
}

} // namespace harrowmere

#endif // HARROWMERE_CORE_BRIDGE_HARROWMERE_CAPABILITY_HPP
