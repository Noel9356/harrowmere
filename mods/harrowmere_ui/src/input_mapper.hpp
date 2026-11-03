// input_mapper.hpp - the game's input layer, Phase-1 slice (task 7.4P1)
// extended by the full card: the complete interaction loop.
//
// The input mapper owns the binding tables and turns device state into sim
// intents, per the cards:
//
//   "Movement uses polled InputCapability state each frame (is_key_down);
//    toggles (F1) subscribe to KeyPressedEvent/KeyReleasedEvent on the bus
//    ... Mouse deltas via the input capability's consumed-delta query, gated
//    on the UI's wantsMouse() (7.3P1) ... Movement submits SimCommand::Move
//    (4.2) - no input path mutates sim state directly."
//
//   Task 7.4: "Devices -> intents; the playable loop from keyboard/mouse ...
//   Every binding maps to exactly one SimCommand/intent - no binding mutates
//   sim state directly (R-rule: only step 9 applies)."
//
// ---- Ownership map (who handles which device signal) -------------------------
//
//   W / A / S / D        POLLED  - walk (camera-relative), this mapper, each frame
//   LeftShift            POLLED  - sprint (the Move intent's sprint flag), this mapper
//   mouse motion         DRAINED - look, this mapper, via consumeMouseDelta()
//                                  gated on the overlay's wantsMouse() (7.3P1)
//   LMB                  EDGE    - Attack{target} (this mapper; rising edge)
//   RMB                  EDGE    - Interact{aimed target, TALK} ("alt-interact":
//                                  the aimed-target interact; E takes the nearest)
//   E                    EDGE    - Interact{nearest, TALK} (the sim re-gates the
//                                  range - A-121 lives in core/constants.hpp and
//                                  the sim's gate is authoritative)
//   G / B / X            EDGE    - Interact{nearest, REPORT_CRIME / BRIBE /
//                                  SURRENDER} - the targeted intents (their
//                                  owners land in Phase 3; the gate reports
//                                  NoHandler meanwhile - 3.10's null default)
//   Space                EDGE    - cosmetic jump in v1: NO sim command exists
//                                  (documented N/A row; Appendix E's re-entry)
//   Z / T / Y            EDGE    - Emote{CROUCH / LIGHT_TORCH / MASK} toggles
//                                  (the sim flips the act; one submit per press)
//   F1                   EVENT   - the overlay toggle. The subscription is the
//                                  overlay component's own (7.3P1 subscribes
//                                  KeyPressedEvent directly on the bus and
//                                  reads [harrowmere_ui].key). This mapper NEVER
//                                  polls the toggle key and NEVER subscribes a
//                                  second handler for it - the card's pitfall
//                                  ("mixing polled state with event
//                                  subscriptions double-fires toggles") made
//                                  single-ownership the design: one binding,
//                                  one owner, one flip per press.
//   Tab / C / J / K / M  EVENT   - the 7.3 panel toggles (the overlay's own
//                                  subscription; see the F1 row)
//   Esc                  EVENT   - "menu": closes every panel (the overlay)
//
// Edge-triggered keys here are POLLED rising edges (isKeyDown now && !then):
// they submit intents (sim writes), so they belong to the mapper by the
// ownership rule above, and the polled edge keeps them testable headless -
// the P1 harness has no event bus. The TRUE event bus toggles are the
// UI-local ones (F1/panels), owned by the overlay - never mixed (the
// pitfall's exact split).
//
// ---- Targeting (7.4's "LMB attack/interact-target, E interact (nearest)") ----
//
// The mapper resolves intent TARGETS from the capability's CharactersView
// (the present set - the same characters the 7.2 bridge renders):
//   * nearest  - the closest present non-player character (planar distance,
//     R5) within INTERACT_RANGE_UI_M;
//   * aimed    - the closest such character to the LOOK DIRECTION (a
//     cone-free dot-product pick: max dot above a floor, nearest on ties).
// View reads happen ONLY on an intent edge (lazy) - movement-only frames
// never touch the read side (the P1 NoDirectSimMutation contract holds
// unmodified). The sim remains authoritative: its interact gate re-checks
// the range, its combat resolves the swing.
//
// ---- House shape (the scene_bridge pattern, task 7.2P1) ----------------------
//
// The mapper compiles against ONLY published capability headers
// (input_capability.hpp, harrowmere_capability.hpp) plus two local seams, so
// the pinned tests drive it headless with fakes and the runtime wiring lives
// in the plugin TU (ui_harrowmere_plugin.cpp), which adapts the engine's
// real capabilities onto the seams:
//
//   UiInputGate - the overlay side (7.3's HarrowmereUi adapts this when it
//                 integrates): wantsMouse()/wantsKeyboard() gate the camera
//                 look and the movement intents while the overlay is
//                 interactive (the ui_harrowmere.hpp contract, verbatim).
//   CameraRig   - the presentation side: where look is applied. The plugin
//                 adapts the renderer capability's camera pose (the free-fly
//                 controller's state, which the capability doc names as "the
//                 one camera truth"); a null rig degrades look to tracked
//                 heading only (headless, minimal builds) - the feature
//                 degrades, never the run (common.md section 4.0).
//
// ---- Look conventions (the engine free-fly camera's own, camera_controller.hpp)
//
//   forward = (cos(pitch) sin(yaw), sin(pitch), -cos(pitch) cos(yaw))
//   yaw wraps the world Y axis starting from -Z; pitch positive upwards,
//   clamped just under +/-89 degrees; screen x grows right -> turn right,
//   screen y grows down -> look up when the cursor moves up. The mapper
//   re-states them here because it is now a producer of that state: it
//   drains the cursor accumulator first (the frame event dispatches before
//   the renderer's camera integration), applies look through the rig, and
//   the engine's controller - reading a drained accumulator - keeps the pose
//   the mapper set. One delta consumer per frame, by construction.
//
//   Look integrates ONLY while the cursor is captured AND the overlay does
//   not claim it - the same window the engine's camera integration uses, so
//   the mapper and the controller can never disagree about who is looking.
//   While the overlay claims the pointer the mapper releases the capture
//   (one-directional: it hands the cursor over, never steals it back - the
//   engine controller's own F / Esc / left-click flow remains the way a
//   dev run re-captures).
//
// ---- Sim write path (4.2, D-ARCH-3) ------------------------------------------
//
// The mapper's ONLY sim references are the published SimCapability methods:
// submit(SimCommand) for every intent, and the two read accessors
// (player(), characters()) ON EDGE FRAMES for targeting. The per-frame
// cadence is deliberate: Move is MOTION STATE (the latest intent in a fixed
// update's slice wins, one kinematic integration per tick - the drain-side
// rule task 3.10 pinned in controller.hpp), so the mapper re-submits the
// polled direction every frame while a movement key is held and submits
// NOTHING while idle - idle frames stay silent so D-AFK-1's "no intents for
// 120 real seconds" stays true while the player is idle.
//
// ---- Determinism (R5-R8) ------------------------------------------------------
//
// f64 lives only in look/heading/aim math (R5's presentation carve-out - the
// same carve-out the engine's camera controller uses); the Move intent
// carries a direction, and the SCALE-grid-free integration happens sim-side
// in controller.cpp. No RNG, no wall clock, no unordered containers.

#pragma once

#include <array>
#include <cstdint>
#include <string>

// Published capability vocabulary (the providers own their headers - the
// engine include paths arrive through the engine_capability_input /
// harrowmere_capability_headers INTERFACE targets).
#include "input_capability.hpp"      // engine::InputCapability, KeyCode, Key{Pressed,Released}Event, MouseButton
#include "harrowmere_capability.hpp" // harrowmere::SimCapability, SimCommand, MoveIntent

namespace engine
{
class SettingsCapability;
}

namespace harrowmere::ui
{

// ---- P1 binding table (the card's Goal, as data) ------------------------------
//
// The pinned P1 bindings in one table so the review grep and the pinning test
// (TestInput_P1BindingsComplete) read the same rows the code polls. Extending
// the table is Phase 2's card 7.4 (interaction intents) - which ships the
// SEPARATE fullBindingTable below and leaves this one untouched (the P1
// pinned test pins its size and rows; "the Phase-1 suite still passes
// unmodified").

// How a binding's device signal arrives.
enum class BindingKind : std::uint32_t
{
    PolledKey,   // polled each frame through InputCapability::isKeyDown
    MouseDelta,  // drained each frame through InputCapability::consumeMouseDelta
    EventToggle, // edge-triggered through KeyPressedEvent/KeyReleasedEvent on the bus
};

struct P1BindingRow
{
    const char *action;  // stable name (logs, tests, the 7.6 keybinds screen)
    engine::KeyCode key; // the polled/edge key; Unknown for the mouse row
    BindingKind kind;    // how the signal arrives
    const char *owner;   // the component that executes the binding
};

inline constexpr std::uint32_t p1BindingCount = 7;
inline constexpr std::array<P1BindingRow, p1BindingCount> p1BindingTable{{
    {"walk_forward", engine::KeyCode::W, BindingKind::PolledKey, "harrowmere_ui input mapper (7.4P1)"},
    {"walk_back", engine::KeyCode::S, BindingKind::PolledKey, "harrowmere_ui input mapper (7.4P1)"},
    {"walk_left", engine::KeyCode::A, BindingKind::PolledKey, "harrowmere_ui input mapper (7.4P1)"},
    {"walk_right", engine::KeyCode::D, BindingKind::PolledKey, "harrowmere_ui input mapper (7.4P1)"},
    {"sprint", engine::KeyCode::LeftShift, BindingKind::PolledKey, "harrowmere_ui input mapper (7.4P1)"},
    {"look", engine::KeyCode::Unknown, BindingKind::MouseDelta, "harrowmere_ui input mapper (7.4P1)"},
    {"toggle_overlay",
        engine::KeyCode::F1,
        BindingKind::EventToggle,
        "harrowmere_ui overlay (7.3P1) - event-driven; this mapper never polls it"},
}};

// The polled keys as named constants (the hot path reads these, not the table):
// single definitions so the table above and update() cannot drift.
inline constexpr engine::KeyCode kWalkForwardKey = engine::KeyCode::W;
inline constexpr engine::KeyCode kWalkBackKey    = engine::KeyCode::S;
inline constexpr engine::KeyCode kWalkLeftKey    = engine::KeyCode::A;
inline constexpr engine::KeyCode kWalkRightKey   = engine::KeyCode::D;
inline constexpr engine::KeyCode kSprintKey      = engine::KeyCode::LeftShift;
// Event-owned (see the ownership map above): the mapper reads this constant
// ONLY to document the table row - never in a poll, never in a subscription.
inline constexpr engine::KeyCode kOverlayToggleKey = engine::KeyCode::F1;

// ---- Input-layer presentation constants (7.4P1-pinned, pending Appendix A) ----
//
// The scene_bridge precedent (7.2P1): presentation values the card does not
// pin stay local to the deliverable so it is drop-in and does not edit an
// existing constants.hpp (which lives in harrowmere_core - the UI mod cannot
// include core internals, section 4.2 rule 5). Merge on integrate: the lead
// promotes them into Appendix A with the next deviation sync
// (DEVIATIONS.md 2026-10-30 carries the row).

// Radians of look rotation per cursor pixel. Matches the engine camera
// integration's shipped default (renderer_vulkan scene_description.hpp
// defaultLookSensitivity = 0.0022) so the game's look feel is the camera
// feel the engine tuned - the mapper is a producer of the same state.
inline constexpr double MOUSE_LOOK_RADIANS_PER_PIXEL = 0.0022; // A-123 (lives in the UI mod per A.15)

// The follow-cam's eye height above the sim player transform, metres. The
// sim-side player transform is a ground point (y is cosmetic, card 1.6); the
// view needs an eye. 1.7 m is the standing eye-height convention; Phase 2's
// avatar (card 7.2) replaces the number with the avatar's real eye line.
inline constexpr double PLAYER_EYE_HEIGHT_M = 1.7; // 7.4P1 presentation (follow-cam, DEVIATIONS 2026-10-31)

// The pitch clamp, just under +/-89 degrees - the engine camera controller's
// own maxPitch. Keeps the forward vector out of the straight-up/down
// degeneracy (the horizontal component of the walk heading can never vanish).
inline constexpr double MAX_LOOK_PITCH_RADIANS = 1.55334; // A-124 (lives in the UI mod per A.15)

// A-121's UI-side mirror (core/constants.hpp owns the real value; the UI mod
// cannot include core internals - §4.2 rule 5). The mapper uses it ONLY to
// pick a target for an intent; the sim's interact gate re-checks the range
// authoritatively (3.10's routeInteraction, boundary inclusive).
inline constexpr double INTERACT_RANGE_UI_M = 3.0; // A-121 mirror (sim re-gates)

// The v1 attack targeting window (the mapper's aim pick). The pinned combat
// formulas (A-28..A-30) resolve the swing; v1 ships no ranged weapons, so
// the target pick rides the same 3 m touch window as the interact intent.
inline constexpr double ATTACK_RANGE_UI_M = 3.0; // v1 melee pick window (presentation)

// The aimed pick's dot-product floor (cos of the acceptance cone). ~0.9
// (~25 degrees) keeps LMB from grabbing a bystander at the frame's edge.
inline constexpr double AIM_CONE_MIN_DOT = 0.9; // presentation (aim pick)

// ---- The full binding table (task 7.4's pinned set, as data) ------------------
//
// Every binding of card 7.4's Spec in one table; each row names the ONE
// SimCommand/intent it produces (or None + why). The pinned test
// (TestInput_BindingTableComplete) reads these rows.

struct FullBindingRow
{
    const char *action;       // stable name (logs, tests, 7.6's screen)
    engine::KeyCode key;      // the polled/edge key; Unknown for mouse rows
    BindingKind kind;         // how the signal arrives
    SimCommand::Kind command; // the ONE command it submits (None below)
    bool hasCommand;          // false = the row submits nothing (documented)
    const char *owner;        // the component that executes the binding
};

inline constexpr SimCommand::Kind kNoCommand = static_cast<SimCommand::Kind>(0xFFFFFFFFu); // the N/A sentinel

inline constexpr std::uint32_t fullBindingCount = 21;
inline constexpr std::array<FullBindingRow, fullBindingCount> fullBindingTable{{
    {"walk_forward",
        engine::KeyCode::W,
        BindingKind::PolledKey,
        SimCommand::Kind::Move,
        true,
        "harrowmere_ui input mapper (polled, each frame)"},
    {"walk_back",
        engine::KeyCode::S,
        BindingKind::PolledKey,
        SimCommand::Kind::Move,
        true,
        "harrowmere_ui input mapper (polled, each frame)"},
    {"walk_left",
        engine::KeyCode::A,
        BindingKind::PolledKey,
        SimCommand::Kind::Move,
        true,
        "harrowmere_ui input mapper (polled, each frame)"},
    {"walk_right",
        engine::KeyCode::D,
        BindingKind::PolledKey,
        SimCommand::Kind::Move,
        true,
        "harrowmere_ui input mapper (polled, each frame)"},
    {"sprint",
        engine::KeyCode::LeftShift,
        BindingKind::PolledKey,
        SimCommand::Kind::Move,
        true,
        "harrowmere_ui input mapper (the Move intent's sprint flag)"},
    {"look",
        engine::KeyCode::Unknown,
        BindingKind::MouseDelta,
        kNoCommand,
        false,
        "harrowmere_ui input mapper (presentation look; no sim command)"},
    {"toggle_ui",
        engine::KeyCode::F1,
        BindingKind::EventToggle,
        kNoCommand,
        false,
        "harrowmere_ui overlay (event-driven; the mapper never polls it)"},
    {"toggle_inventory",
        engine::KeyCode::Tab,
        BindingKind::EventToggle,
        kNoCommand,
        false,
        "harrowmere_ui overlay (event-driven; the mapper never polls it)"},
    {"toggle_character",
        engine::KeyCode::C,
        BindingKind::EventToggle,
        kNoCommand,
        false,
        "harrowmere_ui overlay (event-driven; the mapper never polls it)"},
    {"toggle_justice",
        engine::KeyCode::J,
        BindingKind::EventToggle,
        kNoCommand,
        false,
        "harrowmere_ui overlay (event-driven; the mapper never polls it)"},
    {"toggle_events",
        engine::KeyCode::K,
        BindingKind::EventToggle,
        kNoCommand,
        false,
        "harrowmere_ui overlay (event-driven; the mapper never polls it)"},
    {"toggle_map",
        engine::KeyCode::M,
        BindingKind::EventToggle,
        kNoCommand,
        false,
        "harrowmere_ui overlay (event-driven; the mapper never polls it)"},
    {"menu",
        engine::KeyCode::Escape,
        BindingKind::EventToggle,
        kNoCommand,
        false,
        "harrowmere_ui overlay (closes every panel; event-driven)"},
    {"jump",
        engine::KeyCode::Space,
        BindingKind::PolledKey,
        kNoCommand,
        false,
        "cosmetic v1 - no sim command exists (documented N/A; Appendix E re-entry)"},
    {"attack",
        engine::KeyCode::Unknown,
        BindingKind::PolledKey,
        SimCommand::Kind::Attack,
        true,
        "harrowmere_ui input mapper (LMB rising edge; aimed target)"},
    {"alt_interact",
        engine::KeyCode::Unknown,
        BindingKind::PolledKey,
        SimCommand::Kind::Interact,
        true,
        "harrowmere_ui input mapper (RMB rising edge; aimed target, TALK)"},
    {"interact",
        engine::KeyCode::E,
        BindingKind::PolledKey,
        SimCommand::Kind::Interact,
        true,
        "harrowmere_ui input mapper (rising edge; NEAREST target, TALK; the sim re-gates range)"},
    {"crouch",
        engine::KeyCode::Z,
        BindingKind::PolledKey,
        SimCommand::Kind::Emote,
        true,
        "harrowmere_ui input mapper (rising edge; Emote{CROUCH} toggle)"},
    {"torch",
        engine::KeyCode::T,
        BindingKind::PolledKey,
        SimCommand::Kind::Emote,
        true,
        "harrowmere_ui input mapper (rising edge; Emote{LIGHT_TORCH} toggle)"},
    {"mask",
        engine::KeyCode::Y,
        BindingKind::PolledKey,
        SimCommand::Kind::Emote,
        true,
        "harrowmere_ui input mapper (rising edge; Emote{MASK} toggle)"},
    {"report_crime",
        engine::KeyCode::G,
        BindingKind::PolledKey,
        SimCommand::Kind::Interact,
        true,
        "harrowmere_ui input mapper (rising edge; Interact{nearest, REPORT_CRIME}; owner lands Phase 3)"},
}};

// The edge-polled keys as named constants (the hot path reads these).
inline constexpr engine::KeyCode kInteractKey    = engine::KeyCode::E;
inline constexpr engine::KeyCode kCrouchKey      = engine::KeyCode::Z;
inline constexpr engine::KeyCode kTorchKey       = engine::KeyCode::T;
inline constexpr engine::KeyCode kMaskKey        = engine::KeyCode::Y;
inline constexpr engine::KeyCode kReportCrimeKey = engine::KeyCode::G;
inline constexpr engine::KeyCode kBribeKey       = engine::KeyCode::B;
inline constexpr engine::KeyCode kSurrenderKey   = engine::KeyCode::X;
inline constexpr engine::KeyCode kJumpKey        = engine::KeyCode::Space;

// Runtime bindings are loaded from the engine settings store at startup.
// Defaults remain the pinned 7.4 bindings above; the options panel writes
// dotted names that map to the engine's [input.actions] TOML table.
struct InputBindings
{
    engine::KeyCode walk_forward = kWalkForwardKey;
    engine::KeyCode walk_back    = kWalkBackKey;
    engine::KeyCode walk_left    = kWalkLeftKey;
    engine::KeyCode walk_right   = kWalkRightKey;
    engine::KeyCode sprint       = kSprintKey;
    engine::KeyCode interact     = kInteractKey;
    engine::KeyCode crouch       = kCrouchKey;
    engine::KeyCode torch        = kTorchKey;
    engine::KeyCode mask         = kMaskKey;
    engine::KeyCode report_crime = kReportCrimeKey;
    engine::KeyCode bribe        = kBribeKey;
    engine::KeyCode surrender    = kSurrenderKey;
    engine::KeyCode jump         = kJumpKey;
};

InputBindings loadInputBindings(const engine::SettingsCapability &settings);
// Bribe/surrender ride the same EdgeIntent table as report_crime (the rows
// above pin report_crime; these two are the remaining targeted intents).
inline constexpr engine::MouseButton kAttackMouseButton      = engine::MouseButton::Left;
inline constexpr engine::MouseButton kAltInteractMouseButton = engine::MouseButton::Right;

// ---- Command coverage (card 7.4's pinned test reads this) ---------------------
//
// "Every SimCommand reachable or documented N/A": one row per pinned command
// naming its producer. `via` spells the surface: a key binding, a panel
// (7.3's panels), or N/A until the owner phase lands.
struct CommandCoverageRow
{
    SimCommand::Kind kind;
    const char *via;
};

inline constexpr std::uint32_t commandCoverageCount = 32;
inline constexpr std::array<CommandCoverageRow, commandCoverageCount> commandCoverageTable{{
    {SimCommand::Kind::Move, "keys: W/A/S/D (walk)"},
    {SimCommand::Kind::Attack, "mouse: LMB (attack)"},
    {SimCommand::Kind::Interact, "keys: E / RMB / G / B / X (interact, report, bribe, surrender)"},
    {SimCommand::Kind::Emote, "keys: Z / T / Y (crouch, torch, mask toggles)"},
    {SimCommand::Kind::Buy, "panel: trade row (3.7 wiring pending - documented N/A until then)"},
    {SimCommand::Kind::Sell, "panel: trade row (3.7 wiring pending - documented N/A until then)"},
    {SimCommand::Kind::Craft, "panel: inventory (crafting list)"},
    {SimCommand::Kind::Equip, "panel: inventory (loot/instance flow; button seam in place)"},
    {SimCommand::Kind::Unequip, "panel: inventory (equipped list)"},
    {SimCommand::Kind::Consume, "panel: inventory (bag list)"},
    {SimCommand::Kind::BuyStat, "panel: character (stats table)"},
    {SimCommand::Kind::BuySkill, "panel: character (skills table)"},
    {SimCommand::Kind::BuyLevel, "panel: character (level button)"},
    {SimCommand::Kind::RespecStat, "panel: character (stat respec)"},
    {SimCommand::Kind::RespecSkill, "panel: character (skill respec)"},
    {SimCommand::Kind::ChangeAppearance, "panel: character (appearance presets)"},
    {SimCommand::Kind::RequestDuel, "panel: world inspector (duel the selection)"},
    {SimCommand::Kind::YieldDuel, "panel: character (dueling row)"},
    {SimCommand::Kind::JoinEvent, "panel: events (join button)"},
    {SimCommand::Kind::LeaveEvent, "panel: events (leave button)"},
    {SimCommand::Kind::ReportCrime, "keys: G (targeted) + panel: world inspector"},
    {SimCommand::Kind::Bribe, "panel: world inspector (targeted; 4.1 owner pending)"},
    {SimCommand::Kind::Surrender, "keys: X (targeted) + panel: justice (4.6 owner pending)"},
    {SimCommand::Kind::ServeSentence, "panel: justice + time (D-JAIL-1; 4.3 owner pending)"},
    {SimCommand::Kind::AttemptEscape, "panel: justice (4.3 owner pending)"},
    {SimCommand::Kind::ClaimTerritory, "N/A - Phase 4 owner (6.3); submit seam exists"},
    {SimCommand::Kind::InstallLaws, "N/A - Phase 4 owner (6.3); submit seam exists"},
    {SimCommand::Kind::FoundFaction, "N/A - Phase 4 owner (6.1); submit seam exists"},
    {SimCommand::Kind::DissolveFaction, "N/A - Phase 4 owner (6.1); submit seam exists"},
    {SimCommand::Kind::SetRelation, "N/A - Phase 4 owner (6.1); submit seam exists"},
    {SimCommand::Kind::DeclareWar, "N/A - Phase 4 owner (6.2); submit seam exists"},
    {SimCommand::Kind::Donate, "N/A - Phase 4 owner (4.5/6.x); submit seam exists"},
}};

// ---- The two local seams -------------------------------------------------------

// The overlay side of the input contract (7.3's ui_harrowmere.hpp, verbatim
// semantics): what the input mapper reads each frame to know whether the
// overlay is interactive. The plugin's adapter forwards to wantsMouse()/
// wantsKeyboard() and the mapper's behavior is unchanged.
class UiInputGate
{
public:
    virtual ~UiInputGate() = default;

    // True when the overlay claims the pointer this frame (visible AND a
    // panel has mouse focus). The camera look gate - card 7.4P1 verbatim.
    virtual bool wantsMouse() const = 0;

    // True when the overlay claims the keyboard this frame (a text field has
    // focus). The movement-intent gate: WASD must not walk the player while
    // the user types into a panel.
    virtual bool wantsKeyboard() const = 0;
};

// The presentation side: where look is applied. The plugin adapts the
// renderer capability's camera pose onto this (the free-fly controller's
// state, which the renderer capability doc names as "the one camera truth").
// A null InputMapper rig pointer degrades look to tracked-heading-only.
class CameraRig
{
public:
    virtual ~CameraRig() = default;

    // The camera eye position, world space, metres.
    virtual std::array<double, 3> position() const = 0;

    // The current look target, world space (a direction handle from the
    // position - the engine camera keeps it one unit along the forward).
    virtual std::array<double, 3> lookAt() const = 0;

    // Re-aims the camera at a new look target from the current position. The
    // engine convention (camera_controller.hpp): the pose is the free-fly
    // controller's state, and external setters mutate IT - the mapper's look
    // therefore survives the controller's own per-frame write-back.
    virtual void setLookAt(const std::array<double, 3> &lookAt) = 0;

    // Teleports the camera eye to a new position, KEEPING the current look
    // direction (the renderer capability's documented setter contract:
    // "a set position keeps the current look direction"). The follow-cam's
    // only write - the walk stays observable without touching the heading the
    // look integration owns.
    virtual void setPosition(const std::array<double, 3> &position) = 0;
};

// ---- The follow-cam (the presentation half of "the player walks") -------------

// The sim-side Move intent moves the PLAYER (a 10 Hz kinematic integration in
// harrowmere_core); with no avatar in the scene until Phase 2 (card 7.2), a
// camera that stays put makes the walk unobservable - the exit checklist's
// "the player walks, sprints and looks around" would be invisible. The
// follow-cam closes that gap: each frame it mirrors the sim's authoritative
// player view into the camera EYE position (never the look direction - the
// mapper owns that), presentation-only, one view read and one rig write per
// frame.
//
// It is deliberately NOT part of InputMapper: the pinned
// TestInput_NoDirectSimMutation keeps the input layer off the capability's
// read side, and the follow-cam is not input - it is the renderer-side view
// of sim state (the same split as the 7.2 scene bridge: reads a view,
// pushes presentation, mutates nothing). The plugin wires it after the
// mapper's frame update; a null rig (renderer absent) skips it entirely.
// [harrowmere_ui].follow_camera (default on) turns it off for a dev fly run.
class PlayerFollowCamera
{
public:
    // All references the caller keeps alive for this object's lifetime (the
    // plugin owns them as members).
    PlayerFollowCamera(const harrowmere::SimCapability &sim, CameraRig &rig);

    // One frame: read the player view, write the eye position. The look
    // direction is never touched (the mapper owns it), so the framing the
    // configured [scene.camera] seeded survives the teleport, and mouse look
    // keeps working exactly as the mapper built it.
    void update();

private:
    const harrowmere::SimCapability &sim_;
    CameraRig &rig_;
};

// ---- The mapper ----------------------------------------------------------------

// The input mapper. One instance per run, owned by the harrowmere_ui
// plugin component; update() runs once per EngineFrameEvent (the per-frame
// heartbeat - input is frame-coupled by design, common.md section 4.4: the
// SIM advances on the fixed-update lane, the input layer reads devices at
// frame rate and submits intents).
//
// The frame-event dispatch strictly precedes the renderer's camera
// integration inside the composition root's loop (publish -> renderFrame),
// so the mapper's delta drain is always first: the engine controller reads a
// settled accumulator, and look is single-consumer by construction.
class InputMapper
{
public:
    // All seams are references the caller keeps alive for the mapper's
    // lifetime (the plugin owns them as members). cameraRig may be null
    // (optional capability absent - headless, minimal builds): look heading
    // stays tracked, nothing is applied - the feature degrades, never the
    // run (common.md section 4.0).
    InputMapper(
        engine::InputCapability &input,
        harrowmere::SimCapability &sim,
        const UiInputGate &uiGate,
        CameraRig *cameraRig,
        InputBindings bindings = {}
    );

    // One frame of input. Order is load-bearing:
    //   1. seed the look state from the rig once (the configured [scene.camera]
    //      framing survives until the first look - the controller's own
    //      syncFromCamera contract),
    //   2. release the cursor to the overlay when it claims the pointer
    //      (one-directional),
    //   3. drain the cursor accumulator ALWAYS (the mapper is the one delta
    //      consumer; draining while gated keeps the accumulator settled - the
    //      engine ui capability's own modal guarantee, ui_capability.hpp) and
    //      integrate look while captured and unclaimed, applying through the
    //      rig,
    //   4. poll the movement keys and submit ONE SimCommand::Move when a key
    //      is held (silent when idle - the D-AFK-1 contract),
    //   5. poll the EDGE bindings (7.4's loop: LMB/RMB/E/G/B/X/Z/T/Y) and
    //      submit their ONE command per rising edge; targets resolve lazily
    //      from the CharactersView on an edge only (movement-only frames
    //      never touch the read side).
    void update();

    // Replace persisted bindings after a settings-file reload. Edge history is
    // cleared so a key held across the remap cannot synthesize a press.
    void setBindings(InputBindings bindings) noexcept;

    // ---- readbacks (the debug panel and tests; presentation state, not sim
    // state) ---------------------------------------------------------------

    // The tracked look heading, radians. yaw wraps the world Y axis starting
    // from -Z (the engine camera convention); pitch is positive upwards and
    // clamped to MAX_LOOK_PITCH_RADIANS.
    double lookYawRadians() const noexcept
    {
        return yaw_;
    }
    double lookPitchRadians() const noexcept
    {
        return pitch_;
    }

    // The last intent target the mapper resolved (0 = none). Readback for
    // the debug panel and tests; the sim re-gates everything.
    std::uint64_t lastIntentTarget() const noexcept
    {
        return lastTarget_;
    }

private:
    // The camera-relative walk direction for the current heading, ground
    // plane (Move is a 2D intent; y is cosmetic, card 1.6). forward is the
    // look heading projected on x/z and normalized; right is its level
    // perpendicular (the engine camera's own strafe frame).
    std::array<double, 2> groundFrameForward() const noexcept;
    std::array<double, 2> groundFrameRight() const noexcept;

    // The unit forward vector for the tracked yaw/pitch (the engine camera's
    // parameterization; camera_controller.hpp verbatim).
    std::array<double, 3> lookDirection() const noexcept;

    // ---- 7.4's edge-intent helpers ----------------------------------------

    // Polls one edge key; returns true exactly once per press (rising edge).
    bool risingEdge(engine::KeyCode key);
    bool risingEdgeMouseButton(engine::MouseButton button);

    // Target resolution over the CharactersView (lazy: edge frames only).
    // nearest = minimal planar distance from the player within rangeM;
    // aimed = the look-direction cone pick (AIM_CONE_MIN_DOT floor, nearest
    // on ties). Returns 0 when nothing qualifies (the sim-side gate then
    // reports UnknownTarget - a deterministic no-op, never a crash).
    std::uint64_t resolveNearestTarget(double rangeM);
    std::uint64_t resolveAimedTarget(double rangeM);

    // Submits one Interact intent with the resolved target + action.
    void submitInteract(std::uint64_t target, harrowmere::InteractIntent::Action action);

    engine::InputCapability &input_;
    harrowmere::SimCapability &sim_;
    const UiInputGate &uiGate_;
    CameraRig *cameraRig_; // nullable by design (degrade-if-absent)
    InputBindings bindings_;

    // Look state (presentation, R5 carve-out). Seeded from the rig's
    // configured framing on the first update, then integrated from cursor
    // deltas. With a null rig the seed is the convention's default (-Z).
    double yaw_   = 0.0;
    double pitch_ = 0.0;
    bool seeded_  = false;

    // The edge key state (previous-frame poll; rising edges submit). The
    // keys the mapper owns as edges: E, Space, Z, T, Y, G, B, X + LMB/RMB.
    bool prevInteract_    = false;
    bool prevJump_        = false;
    bool prevCrouch_      = false;
    bool prevTorch_       = false;
    bool prevMask_        = false;
    bool prevReportCrime_ = false;
    bool prevBribe_       = false;
    bool prevSurrender_   = false;
    bool prevAttack_      = false;
    bool prevAltInteract_ = false;

    // The last resolved target (readback only).
    std::uint64_t lastTarget_ = 0;
};

} // namespace harrowmere::ui
