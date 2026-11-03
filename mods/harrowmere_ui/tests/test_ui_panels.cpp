// test_ui_panels.cpp - the pinned tests for task 7.3 (the full panel set).
//
//   * TestUi_AllPanelsBindViews      - the compile-time view list: every
//                                      panel declares the views it reads, the
//                                      masks match the card's panel specs, and
//                                      every draw function runs against a
//                                      headless ImGui frame without touching
//                                      anything but the submit path.
//   * TestUi_CommandsQueueNotApply   - the panels' single write path is
//                                      submit(); between the submit and an
//                                      explicit "step 9" drain NOTHING in the
//                                      views moves (§4.2 rule 2, pinned).
//   * TestUi_NoHiddenEvidenceExposed - the JusticeView whitelist: the struct
//                                      carries exactly the observable surface
//                                      (reputation/notoriety/warrants-against-
//                                      you/jail + signals) and none of the
//                                      hidden fields (caseScore, evidence,
//                                      reports, physicalScore) - the fog-of-war
//                                      rule as a test (§4.2 rule 3).
//
// ImGui runs headless (no GLFW/Vulkan): a bare context with a built font
// atlas and NewFrame, exactly the recipe test_debug_panel.cpp pins for this
// harness. The panels' ImGui calls walk the internal command list; no
// renderer is involved.

#include <doctest.h>

#include "panels/panels.hpp" // the deliverables under test (kPanelBindings + the draw functions + helpers)

#include <imgui.h>

// The step-9 half of the queue test (the same objects the runtime binds; the
// P1 roundtrip's pattern - the UI mod itself stays core-header-free, tests
// may bind the drain side).
#include "core/constants.hpp"    // MOVE_SPEED_MPS (A-57)
#include "player/controller.hpp" // applyIntents (3.10, step 9's home)

#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

// The panels' surface is this suite's subject; the test reads it under its
// own namespace (the same convention the input-mapper suite keeps).
using namespace harrowmere::ui;

// ---- the fog-of-war member-detection traits (namespace scope: templates) ---
template <typename T, typename = void> struct HasCaseScore : std::false_type
{};
template <typename T> struct HasCaseScore<T, std::void_t<decltype(&T::caseScore)>> : std::true_type
{};
template <typename T, typename = void> struct HasEvidence : std::false_type
{};
template <typename T> struct HasEvidence<T, std::void_t<decltype(&T::evidence)>> : std::true_type
{};
template <typename T, typename = void> struct HasReports : std::false_type
{};
template <typename T> struct HasReports<T, std::void_t<decltype(&T::reports)>> : std::true_type
{};
template <typename T, typename = void> struct HasPhysicalScore : std::false_type
{};
template <typename T> struct HasPhysicalScore<T, std::void_t<decltype(&T::physicalScore)>> : std::true_type
{};
template <typename T, typename = void> struct HasAggregateWeight : std::false_type
{};
template <typename T> struct HasAggregateWeight<T, std::void_t<decltype(&T::aggregateReportWeight)>> : std::true_type
{};

// ---------------------------------------------------------------------------
// The panel fake: every view seedable, submit() records, reads counted. The
// panels see exactly the capability surface (the real class is the contract;
// the fake is a subclass overriding the readers).
// ---------------------------------------------------------------------------
namespace
{

class PanelFakeSim final : public harrowmere::SimCapability
{
public:
    mutable std::uint32_t readCount = 0;
    std::vector<harrowmere::SimCommand> submitted;
    std::vector<harrowmere::views::TimeControls> timeWrites;

    harrowmere::views::WorldView worldView{};
    harrowmere::views::CharactersView charactersView{};
    harrowmere::views::PlayerView playerView{};
    harrowmere::views::JusticeView justiceView{};
    harrowmere::views::EventsView eventsView{};
    harrowmere::views::UiSignals signalsView{};

    const harrowmere::views::WorldView &world() const override
    {
        ++readCount;
        return worldView;
    }

    const harrowmere::views::CharactersView &characters() const override
    {
        ++readCount;
        return charactersView;
    }

    const harrowmere::views::PlayerView &player() const override
    {
        ++readCount;
        return playerView;
    }

    const harrowmere::views::JusticeView &justice() const override
    {
        ++readCount;
        return justiceView;
    }

    const harrowmere::views::EventsView &events() const override
    {
        ++readCount;
        return eventsView;
    }

    const harrowmere::views::UiSignals &signals() const override
    {
        ++readCount;
        return signalsView;
    }

    void submit(
        harrowmere::SimCommand command
    ) override
    {
        submitted.push_back(command);
    }

    harrowmere::views::TimeControls timeControls() const override
    {
        return timeControlsState;
    }

    void setTimeControls(
        const harrowmere::views::TimeControls &controls
    ) override
    {
        timeControlsState = controls;
        timeWrites.push_back(controls);
    }

    harrowmere::views::TimeControls timeControlsState{};
};

// The headless ImGui frame guard: one context for the whole binary (the
// debug-panel harness owns its own; doctest runs suites independently, but
// two contexts in one process is fine - the guard just scopes this suite's).
struct HeadlessImGui
{
    ImGuiContext *ctx = nullptr;

    HeadlessImGui()
    {
        IMGUI_CHECKVERSION();
        ctx            = ImGui::CreateContext();
        ImGuiIO &io    = ImGui::GetIO();
        io.IniFilename = nullptr; // don't write imgui.ini during tests
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        // Build the font atlas on the CPU (the canonical headless recipe).
        unsigned char *pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->Build();
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::NewFrame();
    }

    ~HeadlessImGui()
    {
        ImGui::EndFrame();
        ImGui::DestroyContext(ctx);
    }
};

// A PanelContext over the fake, assembled through the capability ACCESSORS
// (the exact path ui_harrowmere.cpp runs) - the readCount proof below rides
// on that.
PanelContext makeContext(
    PanelFakeSim &sim
)
{
    PanelContext ctx{};
    ctx.world        = &sim.world();
    ctx.characters   = &sim.characters();
    ctx.player       = &sim.player();
    ctx.justice      = &sim.justice();
    ctx.events       = &sim.events();
    ctx.signals      = &sim.signals();
    ctx.sim          = &sim;
    ctx.fps          = 60;
    ctx.timeControls = sim.timeControls();
    return ctx;
}

} // namespace

// ---------------------------------------------------------------------------
// TestUi_AllPanelsBindViews (card 7.3, pinned name)
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestUi_AllPanelsBindViews"
)
{
    // ---- the compile-time view list ----------------------------------------
    // Every panel declares a non-empty view set (TimeControls reads the
    // player's worldTime next to its pacing control).
    for (const auto &binding : harrowmere::ui::kPanelBindings)
    {
        CHECK(binding.views != 0);
        CHECK(binding.title != nullptr);
        CHECK(binding.title[0] != '\0');
    }

    // The pinned per-panel masks (card 7.3's panel list, verbatim):
    static_assert(
        harrowmere::ui::kPanelBindings[static_cast<std::size_t>(harrowmere::ui::PanelId::Hud)].views
            == (harrowmere::ui::kViewWorld | harrowmere::ui::kViewPlayer | harrowmere::ui::kViewSignals),
        "the HUD reads world + player + signals (status icons)"
    );
    static_assert(
        harrowmere::ui::kPanelBindings[static_cast<std::size_t>(harrowmere::ui::PanelId::Character)].views
            == harrowmere::ui::kViewPlayer,
        "the Character panel reads the player view only"
    );
    static_assert(
        harrowmere::ui::kPanelBindings[static_cast<std::size_t>(harrowmere::ui::PanelId::Inventory)].views
            == harrowmere::ui::kViewPlayer,
        "the Inventory panel reads the player view only (bag/equipped/recipes)"
    );
    static_assert(
        harrowmere::ui::kPanelBindings[static_cast<std::size_t>(harrowmere::ui::PanelId::Justice)].views
            == (harrowmere::ui::kViewJustice | harrowmere::ui::kViewSignals),
        "the Justice panel reads the whitelist + signals - nothing else"
    );
    static_assert(
        harrowmere::ui::kPanelBindings[static_cast<std::size_t>(harrowmere::ui::PanelId::Events)].views
            == harrowmere::ui::kViewEvents,
        "the Events panel reads the events view only"
    );
    static_assert(
        harrowmere::ui::kPanelBindings[static_cast<std::size_t>(harrowmere::ui::PanelId::WorldInspector)].views
            == (harrowmere::ui::kViewCharacters | harrowmere::ui::kViewPlayer),
        "the World inspector reads the present set + the player framing"
    );
    static_assert(
        harrowmere::ui::kPanelBindings[static_cast<std::size_t>(harrowmere::ui::PanelId::Map)].views
            == harrowmere::ui::kViewWorld,
        "the Map panel reads the world view (the territory tree)"
    );
    static_assert(
        harrowmere::ui::kPanelBindings[static_cast<std::size_t>(harrowmere::ui::PanelId::TimeControls)].views
            == harrowmere::ui::kViewPlayer,
        "the Time panel reads the player's worldTime beside the pacing control"
    );
    static_assert(harrowmere::ui::kPanelBindings.size() == 9, "the eight 7.3 panels + the 7.3P1 debug panel");

    // ---- the runtime binding proof ------------------------------------------
    // Every panel draws once against the fake (headless ImGui); the draw
    // compiles against the exact view shapes (a dropped field fails the TU)
    // and submits nothing without a button press.
    HeadlessImGui imgui;
    PanelFakeSim sim;
    PanelContext ctx = makeContext(sim);

    bool open = true;
    drawHudPanel(ctx, &open);
    CHECK(open);
    drawCharacterPanel(ctx, &open);
    drawInventoryPanel(ctx, &open);
    drawJusticePanel(ctx, &open);
    drawEventsPanel(ctx, &open);
    drawWorldInspectorPanel(ctx, &open);
    drawMapPanel(ctx, &open);
    drawTimeControlsPanel(ctx, &open);
    // (The debug panel's binding proof lives in test_debug_panel.cpp - the
    // 7.3P1 harness that owns that surface.)

    // No button was pressed: no command left the panels.
    CHECK(sim.submitted.empty());
    // The panels read their views (the masks actually got exercised).
    CHECK(sim.readCount > 0);
}

// ---------------------------------------------------------------------------
// TestUi_CommandsQueueNotApply (card 7.3, pinned name)
//
// The panels' write path is submit() and ONLY submit(): between the helper
// calls and an explicit step-9 drain, the views do not move by one bit. The
// drain half proves the command takes effect only when step 9 runs it (the
// Move intent lands in player::applyIntents - the same objects the runtime
// binds; the P1 roundtrip's pattern).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestUi_CommandsQueueNotApply"
)
{
    HeadlessImGui imgui;
    PanelFakeSim sim;
    PanelContext ctx = makeContext(sim);

    // Field snapshots of the views before the submits (the vector members
    // compare by content - a memcpy of the struct would compare heap
    // pointers, which a value copy legitimately changes).
    const harrowmere::views::WorldView worldBefore   = sim.worldView;
    const harrowmere::views::PlayerView playerBefore = sim.playerView;
    const auto worldSame                             = [&]() {
        return worldBefore.currentRegion.id == sim.worldView.currentRegion.id
            && worldBefore.currentTerritory.id == sim.worldView.currentTerritory.id
            && worldBefore.season.season == sim.worldView.season.season
            && worldBefore.weather.kind == sim.worldView.weather.kind
            && worldBefore.tickNumber == sim.worldView.tickNumber
            && worldBefore.territories.size() == sim.worldView.territories.size();
    };

    // The panels' helpers (what the buttons call):
    submitBuyStat(ctx, 0);  // STRENGTH
    submitBuySkill(ctx, 0); // MELEE
    submitBuyLevel(ctx);
    submitRespecStat(ctx, 0);
    submitConsume(ctx, "bandage");
    submitEquip(ctx, 1234);
    submitUnequip(ctx, 1234);
    submitCraft(ctx, "craft_bandage");
    submitRequestDuel(ctx, 42);
    submitYieldDuel(ctx);
    submitJoinEvent(ctx, 1);
    submitLeaveEvent(ctx, 1);
    submitServeSentence(ctx);
    submitAttack(ctx, 42);
    submitMove(ctx, harrowmere::MoveIntent{0.0, -1.0, false});
    applyTimeControls(ctx, false, 8);

    // Fifteen queued commands + the one pacing write (the Time panel's
    // surface - not a SimCommand).
    REQUIRE(sim.submitted.size() == 15);
    CHECK(sim.submitted[0].kind == harrowmere::SimCommand::Kind::BuyStat);
    CHECK(sim.submitted[1].kind == harrowmere::SimCommand::Kind::BuySkill);
    CHECK(sim.submitted[2].kind == harrowmere::SimCommand::Kind::BuyLevel);
    CHECK(sim.submitted[3].kind == harrowmere::SimCommand::Kind::RespecStat);
    CHECK(sim.submitted[4].kind == harrowmere::SimCommand::Kind::Consume);
    CHECK(sim.submitted[5].kind == harrowmere::SimCommand::Kind::Equip);
    CHECK(sim.submitted[6].kind == harrowmere::SimCommand::Kind::Unequip);
    CHECK(sim.submitted[7].kind == harrowmere::SimCommand::Kind::Craft);
    CHECK(sim.submitted[8].kind == harrowmere::SimCommand::Kind::RequestDuel);
    CHECK(sim.submitted[9].kind == harrowmere::SimCommand::Kind::YieldDuel);
    CHECK(sim.submitted[10].kind == harrowmere::SimCommand::Kind::JoinEvent);
    CHECK(sim.submitted[11].kind == harrowmere::SimCommand::Kind::LeaveEvent);
    CHECK(sim.submitted[12].kind == harrowmere::SimCommand::Kind::ServeSentence);
    CHECK(sim.submitted[13].kind == harrowmere::SimCommand::Kind::Attack);
    CHECK(sim.submitted[14].kind == harrowmere::SimCommand::Kind::Move);
    REQUIRE(sim.timeWrites.size() == 1);
    CHECK(sim.timeWrites[0].ticksPerDispatch == 8);
    CHECK_FALSE(sim.timeWrites[0].paused);

    // NOTHING in the views moved - submit queued, nothing applied (§4.2 rule
    // 2; the UI mod has no other write path by construction).
    CHECK(worldSame());
    CHECK(sim.playerView.level == playerBefore.level);
    CHECK(sim.playerView.availableXp == playerBefore.availableXp);
    CHECK(sim.playerView.stats[0] == playerBefore.stats[0]);
    CHECK(sim.playerView.skills[0] == playerBefore.skills[0]);

    // The step-9 half: the queue drains through the fixed-update application
    // and the Move intent (the only one with a Phase-2 consumer) moves the
    // player - the command takes effect ONLY when step 9 runs.
    harrowmere::player::IntentQueue queue;
    for (const auto &command : sim.submitted)
    {
        queue.push(command);
    }
    harrowmere::player::PlayerController player{};
    harrowmere::core::WorldClock clock{};
    harrowmere::player::applyIntents(player, queue, clock, 0.1);
    // The Move was the motion state: 4 m/s x 0.1 s along -Z (A-57). Every
    // other command drained as a deterministic no-op (their owners land in
    // later phases - controller.hpp's drain rule).
    CHECK(player.position.z == doctest::Approx(-harrowmere::MOVE_SPEED_MPS * 0.1));
    // ...and the player view the panels read was NEVER the thing that moved
    // (the sim's own state did, through the sim's own path).
    CHECK(sim.playerView.position.x == doctest::Approx(playerBefore.position.x));
}

// ---------------------------------------------------------------------------
// TestUi_NoHiddenEvidenceExposed (card 7.3, pinned name)
//
// The fog-of-war rule as a test: the JusticeView whitelist is STRUCTURAL.
//   (a) The struct aggregate-initializes from exactly the whitelisted fields
//       in declaration order - a hidden member inserted among them breaks the
//       compile.
//   (b) Member detection proves the classic hidden fields do not exist on
//       the view: caseScore, evidence, reports, physicalScore, aggregate-
//       ReportWeight - the evidence-grade surfaces the doc keeps from the
//       player ("Nothing is visible at the act", design reference).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestUi_NoHiddenEvidenceExposed"
)
{
    // (a) The whitelist shape, designated-init in declaration order (C++20):
    // any new member declared BEFORE the last designator breaks this compile
    // - the view builder cannot grow a hidden field silently.
    const harrowmere::views::JusticeView whitelisted{
        .reputation               = 0,
        .notoriety                = 0,
        .activeWarrantsAgainstYou = 0,
        .isJailed                 = false,
        .jailUntil                = 0,
        .perTerritory             = {},
        .warrants                 = {},
    };
    static_assert(
        std::is_aggregate_v<harrowmere::views::JusticeView>,
        "JusticeView stays a plain aggregate - the fog-of-war whitelist"
    );
    CHECK(whitelisted.activeWarrantsAgainstYou == 0);

    // (b) The hidden fields must not exist, under any spelling the evidence
    // pipeline uses (4.1's Evidence members; 4.2's case score). The traits
    // live at namespace scope (templates); the asserts pin the view here.
    static_assert(
        !HasCaseScore<harrowmere::views::JusticeView>::value,
        "caseScore is hidden evidence - never on the published view"
    );
    static_assert(
        !HasEvidence<harrowmere::views::JusticeView>::value, "evidence lists are never on the published view"
    );
    static_assert(
        !HasReports<harrowmere::views::JusticeView>::value, "witness reports are never on the published view"
    );
    static_assert(
        !HasPhysicalScore<harrowmere::views::JusticeView>::value, "physicalScore is never on the published view"
    );
    static_assert(
        !HasAggregateWeight<harrowmere::views::JusticeView>::value,
        "aggregateReportWeight is never on the published view"
    );

    // The runtime half: the justice panel draws the whitelist values and
    // submits nothing by itself (no button pressed in a draw).
    HeadlessImGui imgui;
    PanelFakeSim sim;
    sim.justiceView.notoriety                = 3000;
    sim.justiceView.activeWarrantsAgainstYou = 1;
    sim.justiceView.warrants.push_back({5ull, 4000, 900000, false});
    sim.justiceView.perTerritory.push_back({5ull, -1200});
    sim.signalsView.entries.push_back({harrowmere::views::PlayerSignalKind::Wanted, "posters name you"});
    PanelContext ctx = makeContext(sim);
    bool open        = true;
    drawJusticePanel(ctx, &open);
    CHECK(open);
    CHECK(sim.submitted.empty()); // a draw is read-only; buttons queue commands
}

// ---------------------------------------------------------------------------
// TestUi_JusticeButtonsMapCommands (card 7.3b, pinned name): each Justice
// panel button submits exactly its SimCommand — the serve-sentence button
// the D-JAIL-1 fast-forward, the escape attempt, the surrender, and the
// targeted report/bribe pair — one command each, kinds verbatim, payloads
// intact (the helpers ARE what the buttons call).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestUi_JusticeButtonsMapCommands"
)
{
    HeadlessImGui imgui;
    PanelFakeSim sim;
    PanelContext ctx = makeContext(sim);

    // The jail block's buttons (4.3; D-JAIL-1):
    submitServeSentence(ctx);
    submitAttemptEscape(ctx);
    // The surrender (4.3's "to any guard of the issuing territory"):
    submitSurrender(ctx);
    // The targeted pair (4.1's report/bribery paths; the panel-path form):
    submitReportCrime(ctx, 42);
    submitBribe(ctx, 43);

    REQUIRE(sim.submitted.size() == 5);
    CHECK(sim.submitted[0].kind == harrowmere::SimCommand::Kind::ServeSentence);
    CHECK(sim.submitted[1].kind == harrowmere::SimCommand::Kind::AttemptEscape);
    CHECK(sim.submitted[2].kind == harrowmere::SimCommand::Kind::Surrender);
    CHECK(sim.submitted[3].kind == harrowmere::SimCommand::Kind::ReportCrime);
    CHECK(sim.submitted[3].target.target == 42);
    CHECK(sim.submitted[4].kind == harrowmere::SimCommand::Kind::Bribe);
    CHECK(sim.submitted[4].target.target == 43);

    // The untargeted forms (the fog-of-war panel never names a hidden
    // witness — the core resolves the nearest qualifying one).
    PanelFakeSim plain;
    PanelContext plainCtx = makeContext(plain);
    submitReportCrime(plainCtx, 0);
    submitBribe(plainCtx, 0);
    submitSurrender(plainCtx);
    REQUIRE(plain.submitted.size() == 3);
    CHECK(plain.submitted[0].target.target == 0);
    CHECK(plain.submitted[1].target.target == 0);
    CHECK(plain.submitted[2].kind == harrowmere::SimCommand::Kind::Surrender);
}

// ---------------------------------------------------------------------------
// TestUi_MapShowsSafeZonesAndClaims (card 7.3b, pinned name): the Map panel's
// row classification renders the view's facets — safe zones flagged, lawless
// territories flagged, the player's own claims flagged — and the panel draws
// the whole tree headless.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestUi_MapShowsSafeZonesAndClaims"
)
{
    // The classification (the panel's contract with the view data — the
    // readback the pinned test name exercises).
    harrowmere::views::TerritorySummary safe{};
    safe.id   = 4;
    safe.safe = true;
    CHECK(mapRowFacets(safe) == MapRowFacets::Safe);

    harrowmere::views::TerritorySummary lawless{};
    lawless.id      = 9;
    lawless.lawless = true;
    CHECK(mapRowFacets(lawless) == MapRowFacets::Lawless);

    harrowmere::views::TerritorySummary mine{};
    mine.id          = 12;
    mine.claimedByMe = true; // 6.3 fills the view; the facet renders from day one
    CHECK(mapRowFacets(mine) == MapRowFacets::ClaimedByMe);

    harrowmere::views::TerritorySummary plain{};
    plain.id = 5;
    CHECK(mapRowFacets(plain) == MapRowFacets::None);

    // A claim on a safe territory shows both facets (the flags are
    // independent bits, not a ladder).
    harrowmere::views::TerritorySummary claimedSafe{};
    claimedSafe.id          = 6;
    claimedSafe.safe        = true;
    claimedSafe.claimedByMe = true;
    CHECK(mapRowFacets(claimedSafe) == (MapRowFacets::Safe | MapRowFacets::ClaimedByMe));

    // The draw: the whole tree renders headless (the same harness the
    // binding test pins), submitting nothing on its own.
    HeadlessImGui imgui;
    PanelFakeSim sim;
    sim.worldView.territories.push_back(safe);
    sim.worldView.territories.push_back(lawless);
    sim.worldView.territories.push_back(mine);
    sim.worldView.territories.push_back(claimedSafe);
    sim.worldView.currentTerritory.id = 4; // the underfoot marker
    PanelContext ctx                  = makeContext(sim);
    bool open                         = true;
    drawMapPanel(ctx, &open);
    CHECK(open);
    CHECK(sim.submitted.empty());
}
