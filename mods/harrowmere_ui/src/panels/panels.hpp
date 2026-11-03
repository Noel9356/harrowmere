#ifndef HARROWMERE_UI_SRC_PANELS_PANELS_HPP
#define HARROWMERE_UI_SRC_PANELS_PANELS_HPP

// panels.hpp - the shared surface of task 7.3's panel set (hud, character,
// inventory, justice, events, world_inspector, map, time_controls).
//
// Every panel:
//   * reads the world EXCLUSIVELY through SimCapability views (D-ARCH-1,
//     common.md §4.2) - the view types come from harrowmere_capability.hpp,
//     the ONLY harrowmere_core header this mod compiles against (§4.2 rule
//     5; the Done-when grep pins it),
//   * writes EXCLUSIVELY through SimCapability::submit (§4.2 rule 2: queued;
//     tick step 9 applies) - the submit* helpers below are the single write
//     path for buttons AND for the pinned tests (TestUi_CommandsQueueNotApply
//     drives them against a recording capability),
//   * never mutates sim state, never holds a sim pointer beyond the frame.
//
// The binding table (kPanelBindings) is the compile-time view list card
// 7.3's pinned test names ("TestUi_AllPanelsBindViews"): each panel names the
// views it reads as bits; a panel that starts reading a view without
// declaring it fails the test, and a view shape change that drops a field a
// panel reads fails to compile the panel's TU (the debug.cpp precedent).
//
// Panel layout/ergonomics are §5's "yours" - positions, columns and colors
// here are presentation defaults, not pins.

#include <array>
#include <cstdint>
#include <string>

#include "harrowmere_capability.hpp" // SimCapability + views (the only core header)

#include <imgui.h>

namespace harrowmere::ui
{

// ---- the panel registry -------------------------------------------------------

enum class PanelId : std::uint32_t
{
    Hud = 0,
    Character,
    Inventory,
    Justice,
    Events,
    WorldInspector,
    Map,
    TimeControls,
    Debug, // the 7.3P1 panel (panels/debug.cpp), registered here so the F1
           // toggle and the visibility model treat every surface the same
    COUNT
};

// Which published views a panel reads (the binding bits).
enum ViewBits : std::uint32_t
{
    kViewWorld      = 1u << 0,
    kViewCharacters = 1u << 1,
    kViewPlayer     = 1u << 2,
    kViewJustice    = 1u << 3,
    kViewEvents     = 1u << 4,
    kViewSignals    = 1u << 5,
};

struct PanelBinding
{
    PanelId id;
    const char *title;     // the ImGui window title (and the log name)
    std::uint32_t views;   // ViewBits - the compile-time view list
    const char *toggleKey; // the pinned keyboard toggle ("" = always-on)
    bool alwaysOn;         // true = HUD: drawn whenever the overlay is visible
};

// The nine-panel binding table. View masks are pinned per panel; the
// WorldInspector reads characters (the present set) + the player row for the
// trust/age framing; TimeControls reads the player's worldTime next to the
// pacing control (it is the only panel bound to the presentation-pacing
// surface rather than a view alone).
inline constexpr std::array<PanelBinding, static_cast<std::size_t>(PanelId::COUNT)> kPanelBindings{{
    {PanelId::Hud, "Harrowmere HUD", kViewWorld | kViewPlayer | kViewSignals, "", true},
    {PanelId::Character, "Character###harrowmere_character", kViewPlayer, "C", false},
    {PanelId::Inventory, "Inventory###harrowmere_inventory", kViewPlayer, "Tab", false},
    {PanelId::Justice, "Justice###harrowmere_justice", kViewJustice | kViewSignals, "J", false},
    {PanelId::Events, "Events###harrowmere_events", kViewEvents, "K", false},
    {PanelId::WorldInspector, "World Inspector###harrowmere_inspector", kViewCharacters | kViewPlayer, "", false},
    {PanelId::Map, "Map###harrowmere_map", kViewWorld, "M", false},
    {PanelId::TimeControls, "Time###harrowmere_time", kViewPlayer, "", false},
    {PanelId::Debug,
        "Harrowmere Debug [F1]###harrowmere_debug",
        kViewWorld | kViewPlayer | kViewJustice | kViewEvents | kViewSignals,
        "",
        false},
}};

// ---- the frame context ---------------------------------------------------------

// One frame's draw context: the view snapshots (const refs - the panels never
// hold them past the call) + the capability (submit() only) + the UI's own
// selection state. Everything is rebuilt per frame by the overlay host
// (ui_harrowmere.cpp's recordOverlay).
struct PanelContext
{
    // The view snapshots (post-tick for this frame; §4.2 rule 1).
    const harrowmere::views::WorldView *world           = nullptr;
    const harrowmere::views::CharactersView *characters = nullptr;
    const harrowmere::views::PlayerView *player         = nullptr;
    const harrowmere::views::JusticeView *justice       = nullptr;
    const harrowmere::views::EventsView *events         = nullptr;
    const harrowmere::views::UiSignals *signals         = nullptr;

    // The sim capability: the panels' ONLY member call is submit() (and
    // timeControls/setTimeControls on the Time panel - the presentation
    // pacing surface, see harrowmere_capability.hpp's note). Never a view
    // write, never a store handle.
    harrowmere::SimCapability *sim = nullptr;

    // Presentation-only state (the UI's own; not sim state).
    int fps                           = 0;
    std::uint64_t selectedCharacterId = 0;          // the World inspector's selection
    harrowmere::views::TimeControls timeControls{}; // the pacing readback
};

// ---- panel visibility (the overlay's own state; F1/Tab/C/J/K/M/Esc toggle) ---

struct PanelVisibility
{
    std::array<bool, static_cast<std::size_t>(PanelId::COUNT)> open{};
};

// The defaults: HUD on (always-on semantics regardless), everything else off
// - the player opens panels with the pinned keys; the debug overlay keeps
// its 7.3P1 [harrowmere_ui].visible_on_start seed.
PanelVisibility defaultPanelVisibility();

// ---- the draw functions (one per panel, defined in panels/<name>.cpp) ---------

void drawHudPanel(PanelContext &ctx, bool *pOpen);
void drawCharacterPanel(PanelContext &ctx, bool *pOpen);
void drawInventoryPanel(PanelContext &ctx, bool *pOpen);
void drawJusticePanel(PanelContext &ctx, bool *pOpen);
void drawEventsPanel(PanelContext &ctx, bool *pOpen);
void drawWorldInspectorPanel(PanelContext &ctx, bool *pOpen);
void drawMapPanel(PanelContext &ctx, bool *pOpen);
void drawTimeControlsPanel(PanelContext &ctx, bool *pOpen);
// drawDebugPanel lives in panels/debug.cpp (the 7.3P1 signature; unchanged).

// ---- the submit helpers (the panels' single write path) ------------------------
//
// Each helper builds exactly one SimCommand and hands it to submit(). The
// buttons call these; TestUi_CommandsQueueNotApply calls these against a
// recording capability and then drains the recording through the step-9
// application - the queue-not-apply contract, end to end.

void submitMove(PanelContext &ctx, const harrowmere::MoveIntent &move);
void submitAttack(PanelContext &ctx, std::uint64_t target);
void submitBuy(PanelContext &ctx, std::uint32_t good, std::uint32_t qty);
void submitSell(PanelContext &ctx, std::uint32_t good, std::uint32_t qty);
void submitCraft(PanelContext &ctx, const std::string &recipeKey);
void submitEquip(PanelContext &ctx, std::uint64_t itemInstanceId);
void submitUnequip(PanelContext &ctx, std::uint64_t itemInstanceId);
void submitConsume(PanelContext &ctx, const std::string &itemKey);
void submitBuyStat(PanelContext &ctx, std::uint32_t stat);
void submitBuySkill(PanelContext &ctx, std::uint32_t skill);
void submitBuyLevel(PanelContext &ctx);
void submitRespecStat(PanelContext &ctx, std::uint32_t stat);
void submitRespecSkill(PanelContext &ctx, std::uint32_t skill);
void submitChangeAppearance(PanelContext &ctx, const std::uint64_t *components, std::uint32_t count);
void submitRequestDuel(PanelContext &ctx, std::uint64_t target);
void submitYieldDuel(PanelContext &ctx);
void submitJoinEvent(PanelContext &ctx, std::uint64_t eventId);
void submitLeaveEvent(PanelContext &ctx, std::uint64_t eventId);
void submitReportCrime(PanelContext &ctx, std::uint64_t target);
void submitBribe(PanelContext &ctx, std::uint64_t target);
void submitSurrender(PanelContext &ctx);
void submitServeSentence(PanelContext &ctx);
void submitAttemptEscape(PanelContext &ctx);
void submitClaimTerritory(PanelContext &ctx, std::uint64_t territory);
void submitInstallLaws(PanelContext &ctx, std::uint64_t territory, const std::string &lawSetKey);
void submitFoundFaction(PanelContext &ctx, const std::string &name);
void submitDissolveFaction(PanelContext &ctx);
void submitSetRelation(PanelContext &ctx, std::uint32_t faction, std::int32_t relation);
void submitDeclareWar(PanelContext &ctx, std::uint32_t faction);
void submitDonate(PanelContext &ctx, std::uint64_t target, std::int64_t amount);

// The Time panel's pacing write (NOT a SimCommand - the presentation pacing
// surface on the capability; see harrowmere_capability.hpp's TimeControls
// note for the decision record).
void applyTimeControls(PanelContext &ctx, bool paused, std::uint32_t ticksPerDispatch);

// ---- small shared formatters ---------------------------------------------------

// Game clock as HH:MM + game-day (the HUD/Time panels; A-3's day math).
void formatGameClock(std::uint64_t worldTime, char *buf, std::size_t bufSize);

// ---- the Map panel's row facets (7.3b) -----------------------------------------
//
// The classification one territory row renders (safe zone / lawless / one of
// my claims). The draw renders exactly these facets; TestUi_
// MapShowsSafeZonesAndClaims pins the classification so the pinned test name
// has a real readback (a draw's glyphs are not readable — the classification
// is the panel's contract with the view data).
enum class MapRowFacets : std::uint32_t
{
    None        = 0,
    Safe        = 1u << 0, // the green SAFE tag
    Lawless     = 1u << 1, // the amber LAWLESS tag
    ClaimedByMe = 1u << 2, // the CLAIM tag (6.3 fills the view; the facet renders from day one)
};

constexpr MapRowFacets operator|(
    MapRowFacets a, MapRowFacets b
)
{
    return static_cast<MapRowFacets>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

constexpr MapRowFacets operator&(
    MapRowFacets a, MapRowFacets b
)
{
    return static_cast<MapRowFacets>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

MapRowFacets mapRowFacets(const harrowmere::views::TerritorySummary &t);

} // namespace harrowmere::ui

#endif // HARROWMERE_UI_SRC_PANELS_PANELS_HPP
