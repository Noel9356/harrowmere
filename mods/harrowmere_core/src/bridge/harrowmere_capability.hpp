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
// views began as inert stubs - and that is deliberate: the header's shape is
// what later phases plug into. The Phase-1 runtime bootstrap (the exit
// checklist's debug-overlay requirement) filled the Phase-1 slice of
// WorldView/PlayerView with real snapshot data and wired the command queue
// into the fixed-update lane; Phase 2 fills the rest (rebuilt at tick step 13
// each frame) and owns the full command surface. The surface's names and
// shapes only ever extend. The write-path VOCABULARY (the SimCommand intent
// envelope) landed with task 3.10 - see the write-path block below.

#include <cstdint>
#include <string>
#include <vector>

namespace harrowmere
{

// ---- read views (Phase-1 slice; Phase 2 fills the rest) ----------------------
//
// Views are SNAPSHOTS, not live references: rebuilt once per frame (tick step
// 13), read by a UI frame between ticks. No view ever exposes a store
// iterator or a raw Character pointer - ids and copies only (R10, §4.5).
// Fog-of-war is structural: views expose exactly what a player can observe
// (justice: reputation, warrants against you, signals - never caseScore or
// hidden evidence; §4.6); TestUi_NoHiddenEvidenceExposed (7.3) pins the
// whitelist.
//
// Phase 1 fills the fields the phase's own exit checklist names (the F1 debug
// overlay: region, territory, law set, safe-zone flag, season, weather, tick
// number): WorldView carries the world-underfoot summary and the tick, and
// PlayerView carries the player transform + the two clocks. The Phase-2-only
// surfaces (power, justice, events) exist as fields with zero/empty values so
// the published shape does not move when 2.x/3.x fill them.

// The published view vocabulary lives in its own namespace: the capability
// surface's snapshot types must never collide with the sim-internal
// core/... names (core/types.hpp's harrowmere::Position is the doc-pinned
// sim-side struct; views::Position is the wire copy the UI reads).
namespace views
{

struct Position
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// Published vocabulary for the fields below. These are the capability
// surface's own enums (plain data, no includes): they mirror the sim-internal
// closed lists (core/enums.hpp, seasons.hpp) by value - the provider maps
// between them when it rebuilds the views, and the ordinals are pinned in the
// sim so the mapping is a cast-checked switch, never a re-derivation.
enum class SeasonId : std::uint32_t
{
    Spring,
    Summer,
    Autumn,
    Winter
};

enum class WeatherKind : std::uint32_t
{
    Clear,
    Rain,
    Storm,
    Fog,
    Snow
};

// The legal hierarchy type (sim-internal core/enums.hpp has the same closed
// list with the docs' SCREAMING spellings; the published surface uses the
// doc's display names).
enum class TerritoryType : std::uint32_t
{
    Village,
    City,
    Kingdom,
    Empire
};

// The region underfoot (the player's region cell).
struct RegionSummary
{
    std::uint32_t id = 0;           // regionId (u16 space, u32 on the wire)
    std::uint32_t manaRichness = 0; // 0..SCALE (D-LORE-3 band 9500..10000 at worldgen)
};

// The territory underfoot with its observable facets (law/safe/lawless).
struct TerritorySummary
{
    std::uint64_t id = 0; // territoryId; 0 = none (unclaimed frontier)
    TerritoryType type = TerritoryType::Village;
    std::string lawSetKey; // the effective law set's key (lawsAt walk result)
    bool safe = false;     // safe-zone flag
    bool lawless = false;  // 4.6: garrison-less unconquered territory
};

struct SeasonSummary
{
    SeasonId season = SeasonId::Spring;
    std::uint64_t dayOfSeason = 0; // 0-based day within the current season (A-51 length 30)
};

struct WeatherSummary
{
    WeatherKind kind = WeatherKind::Clear;
};

struct WorldView
{
    RegionSummary currentRegion{};
    TerritorySummary currentTerritory{};
    SeasonSummary season{};
    WeatherSummary weather{};
    std::uint64_t tickNumber = 0; // completed sim ticks since session start
};

struct PlayerView
{
    Position position{};          // the sim-side player transform (3.10)
    std::uint64_t worldTime = 0;  // game seconds since world epoch (R8)
    std::uint64_t realTime = 0;   // real seconds since session start (D-TIME-1)
    std::uint64_t cachedPower = 0;  // Phase 2 fills (2.5); zero in Phase 1
    std::uint64_t peakPower = 0;    // Phase 2 fills; zero in Phase 1
    std::uint32_t level = 0;        // Phase 2 fills; zero in Phase 1
    std::int64_t availableXp = 0;   // Phase 2 fills; zero in Phase 1
    bool afk = false;               // D-AFK-1 (3.10)
};

// Fog-of-war whitelist (§4.2 rule 3): reputation/notoriety/warrants-against-
// you/jail only. Phase 1 has no justice activity - the values stay zeroed.
struct JusticeView
{
    std::int64_t reputation = 0;
    std::uint64_t notoriety = 0;
    std::uint32_t activeWarrantsAgainstYou = 0;
    bool isJailed = false;
    std::uint64_t jailUntil = 0;
};

// One UI-visible signal (the capability-side twin of game_events.hpp's
// PlayerSignal event - §4.2 rule 2: rejections surface back through these).
enum class PlayerSignalKind : std::uint32_t
{
    Investigated,
    Wanted,
    Poster,
    Greeting
};

struct PlayerSignalEntry
{
    PlayerSignalKind kind = PlayerSignalKind::Investigated;
    std::string text;
};

struct UiSignals
{
    std::vector<PlayerSignalEntry> entries; // Phase 3 fills; empty in Phase 1
};

// One active event instance as the player may see it (Phase 1: none).
struct EventSummary
{
    std::uint64_t eventId = 0;
    std::string templateKey;
    std::uint64_t stateId = 0;
};

struct EventsView
{
    std::vector<EventSummary> active; // Phase 2 fills (5.x); empty in Phase 1
};

// presence, species, role, power, status, needs-as-visible - Phase 2 fills
// (2.x); the published shape exists now so consumers compile against it.
struct CharactersView
{};

} // namespace views

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
    // The family name a mod acquires through: the manifest requirement
    // spelling, and the grant the kernel derives from it
    // ("capability.harrowmere"). The kernel answers a family request with the
    // single active member's pointer (harrowmere.core) - engine_kernel.cpp,
    // resolveCapabilityPointer: "exact declarations first, then - for a
    // family request - the single active member".
    static constexpr const char *capabilityFamily = "harrowmere";
    // The concrete member the core mod's descriptor declares. The app (the
    // trusted composition root) may acquire it directly; a mod that demands
    // only the family has no grant for the member name and must not.
    static constexpr const char *capabilityName = "harrowmere.core";
    virtual ~SimCapability()                    = default;

    // ---- reads: stable, cheap view snapshots (rebuilt at step 13 each frame) ----
    virtual const views::WorldView &world() const;           // regions, territories, laws-at, safe zones, season/weather
    virtual const views::CharactersView &characters() const; // presence, species, role, power, status, needs (as visible)
    virtual const views::PlayerView &player() const;         // the local player's full state
    virtual const views::JusticeView &justice() const;       // reputation, notoriety, warrants against you, jail status
    virtual const views::EventsView &events() const;         // joinable/active events, own contribution
    virtual const views::UiSignals &signals() const;         // PlayerSignals queue (investigated / wanted / poster / greeting)

    // ---- writes: commands, never direct state mutation ----
    // Queued; applied at step 9 next tick (§4.4) or rejected with a logged
    // reason that surfaces back through a PlayerSignal.
    virtual void submit(SimCommand cmd);

private:
    // v0.1 stub storage: one inert snapshot per view. Phase 2 replaces these
    // with rebuilt-per-frame view data; the accessor signatures do not move.
    views::WorldView world_{};
    views::CharactersView characters_{};
    views::PlayerView player_{};
    views::JusticeView justice_{};
    views::EventsView events_{};
    views::UiSignals signals_{};
};

// ---- inline definitions (v0.1 inert stubs) ----------------------------------

inline const views::WorldView &SimCapability::world() const
{
    return world_;
}

inline const views::CharactersView &SimCapability::characters() const
{
    return characters_;
}

inline const views::PlayerView &SimCapability::player() const
{
    return player_;
}

inline const views::JusticeView &SimCapability::justice() const
{
    return justice_;
}

inline const views::EventsView &SimCapability::events() const
{
    return events_;
}

inline const views::UiSignals &SimCapability::signals() const
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
