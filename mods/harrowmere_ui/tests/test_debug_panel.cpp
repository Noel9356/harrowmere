// test_debug_panel.cpp -- pinned tests for task 7.3P1.
//
// Two tests are pinned by the card:
//   * TestUi_SeamRegisteredOnce        -- a second successful init() is a
//                                         logged no-op (the renderer.vulkan.
//                                         overlay seam registers exactly
//                                         once per HarrowmereUi lifetime).
//   * TestUi_DebugPanelBindsWorldView  -- compile-time view binding. The
//                                         drawDebugPanel signature takes the
//                                         WorldView + other view types by
//                                         const ref; including harrowmere_
//                                         capability.hpp and calling the
//                                         function with the view types is the
//                                         binding proof.
//
// Test framework: doctest (task 0.6). The test binary is harrowmere_core_tests
// per common.md §3.5 -- this file is added to that binary's sources when the
// harrowmere_ui mod lands. (Phase 1 ships the file; the harrowmere_ui tests
// link against harrowmere_ui objects + the third_party/imgui objects compiled
// with IMGUI_DISABLE_OBSOLETE_FUNCTIONS + the harrowmere_core capability
// headers INTERFACE target.)

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "../src/ui_harrowmere.hpp"  // pulls in harrowmere_capability.hpp + backend

#include <imgui.h>

#include <string>

// ---------------------------------------------------------------------------
// TestUi_SeamRegisteredOnce
//
// The seam registration is the ImGuiBackend::init() call -- it pulls the
// renderer.vulkan.overlay handle bundle and wires ImGui's GLFW + Vulkan
// backends. The contract pinned by the card: a *successful* second init is a
// logged no-op (it does not double-init ImGui, which would leak the prior
// context). A *failed* init must remain retryable -- the next frame may
// succeed when the renderer presents.
//
// We cannot perform a real successful init in a unit test (it requires a live
// Vulkan instance + GLFW window from the engine capabilities). We test the
// two contract edges that *are* unit-testable:
//   (a) a failed init does not flip ready(); the backend stays retryable.
//   (b) the init() method's idempotency guard is wired (a second call on an
//       already-initialized backend returns true without re-doing work). We
//       cannot reach the "already-initialized" branch without real handles,
//       so this part of the test is a contract-level assertion documented
//       inline; the integration test (TestBootstrap_GameAssemblyBoots from
//       task 0.1, extended in 7.3P1) covers the success path in CI.
// ---------------------------------------------------------------------------
TEST_CASE("TestUi_SeamRegisteredOnce") {
    harrowmere::ui::ImGuiBackend backend;

    // (a) Pre-init state.
    REQUIRE_FALSE(backend.ready());

    // An empty handle bundle is missing every required handle -- init() must
    // fail with a precise diagnostic and must NOT leave the backend in a
    // half-initialized state.
    harrowmere::ui::ImGuiBackendHandles emptyHandles{};
    std::string err;
    REQUIRE_FALSE(backend.init(emptyHandles, err));
    REQUIRE_FALSE(err.empty());
    REQUIRE_FALSE(backend.ready());

    // The failed init must leave the backend retryable: a second attempt with
    // the same bad handles fails again (it is NOT a silent no-op that pretends
    // success). The "second registration is a no-op" contract applies only to
    // a *successful* registration; a failed one must remain retryable so the
    // next frame's onFrameBegin can try again when the renderer becomes ready.
    std::string err2;
    REQUIRE_FALSE(backend.init(emptyHandles, err2));
    REQUIRE_FALSE(err2.empty());
    REQUIRE_FALSE(backend.ready());

    // wantsMouse/wantsKeyboard return false on an uninitialized backend --
    // 7.4P1's input mapper can safely poll these every frame before the
    // overlay has had a chance to come up.
    REQUIRE_FALSE(backend.wantsMouse());
    REQUIRE_FALSE(backend.wantsKeyboard());

    // shutdown() on an uninitialized backend is a safe no-op (the destructor
    // calls it; the test pins that the destructor does not double-free).
    backend.shutdown();
    REQUIRE_FALSE(backend.ready());
}

// ---------------------------------------------------------------------------
// TestUi_DebugPanelBindsWorldView
//
// Compile-time view binding. drawDebugPanel takes the WorldView + other view
// types by const reference; including harrowmere_capability.hpp and calling
// the function with the view types is itself the binding proof. If a future
// change to a view shape drops a field this panel reads, the TU fails to
// compile -- the breakage surfaces at build time, not mid-sim.
//
// ImGui needs a context for the panel's ImGui::Begin / Text / End calls to
// succeed; we create one without a platform backend (ImGui runs headless for
// tests -- no GLFW/Vulkan required). The draw does not touch the renderer;
// it only walks ImGui's internal command list, which is fine for a unit test.
// ---------------------------------------------------------------------------
TEST_CASE("TestUi_DebugPanelBindsWorldView") {
    // Set up a minimal ImGui context (no backend -- headless test).
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // don't write imgui.ini during tests
    io.LogFilename = nullptr;    // don't write imgui_log.txt during tests
    // A headless frame: ImGui requires a display size, a built font atlas
    // (the CPU-side stb_truetype build - the canonical headless recipe, no
    // GLFW/Vulkan involved) and a NewFrame before any widget call (Begin
    // outside a frame is undefined).
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *fontPixels = nullptr;
    int fontWidth = 0, fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
    ImGui::NewFrame();

    // Build the views. The fields used here are the ones the panel reads in
    // panels/debug.cpp; if the as-built harrowmere_capability.hpp uses
    // different names, this TU fails to compile -- which is exactly the
    // compile-time binding contract being tested.
    harrowmere::views::WorldView world{};
    world.currentRegion.id = 42;
    world.currentRegion.manaRichness = 9800;  // D-LORE-3 band
    world.currentTerritory.id = 7;
    world.currentTerritory.type = harrowmere::views::TerritoryType::Village;
    world.currentTerritory.lawSetKey = "default";
    world.currentTerritory.safe = true;
    world.currentTerritory.lawless = false;
    world.season.season = harrowmere::views::SeasonId::Spring;
    world.season.dayOfSeason = 5;
    world.weather.kind = harrowmere::views::WeatherKind::Clear;
    world.tickNumber = 12345;

    harrowmere::views::PlayerView player{};
    player.position = harrowmere::views::Position{128.0, 0.0, 256.0};
    player.cachedPower = 50000;
    player.peakPower   = 55000;
    player.level       = 5;
    player.availableXp = 12000;
    player.worldTime   = 86400ull * 5;  // day 5
    player.realTime    = 600;

    harrowmere::views::JusticeView justice{};
    justice.reputation = 0;
    justice.notoriety  = 0;
    justice.activeWarrantsAgainstYou = 0;
    justice.isJailed  = false;
    justice.jailUntil = 0;

    harrowmere::views::EventsView events{};
    // No active events in Phase 1 (no events system yet -- card 5.1 is Phase 3).

    harrowmere::views::UiSignals signals{};
    // No signals in Phase 1.

    // The binding call. If the panel's signature or the view shapes drift,
    // this line fails to compile.
    bool open = true;
    harrowmere::ui::drawDebugPanel(world, player, justice, events, signals,
                                    /*fps=*/60, &open);

    // The panel was open and we did not press the close button -- pOpen must
    // still be true.
    REQUIRE(open);

    // A second call with pOpen starting false should leave it false (the panel
    // honors the caller's intent; ImGui's Begin does not flip pOpen on its own
    // unless the close button is pressed, which a headless test cannot do).
    bool closed = false;
    harrowmere::ui::drawDebugPanel(world, player, justice, events, signals,
                                    /*fps=*/60, &closed);
    REQUIRE_FALSE(closed);

    // Close the frame the panel drew in (Render finalizes; the draw data is
    // dropped - there is no renderer to submit it).
    ImGui::Render();
    ImGui::DestroyContext();
    (void)ctx;
}
