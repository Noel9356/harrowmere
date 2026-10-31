// ui_harrowmere.hpp -- harrowmere_ui mod component (Phase-1 slice of task 7.3).
//
// The harrowmere_ui mod *provides* ui.harrowmere (family ui) and consumes the
// harrowmere (harrowmere.core) family + input + renderer.vulkan.overlay
// capabilities directly (D-UI-1, common.md §4.1). The engine's own ui_imgui
// module is restored at build per §3.6's build-edge rule but is runtime-inert
// -- no active component demands the `ui` capability, so the widget tree
// (including the debug panel this class drives) belongs to us.
//
// The overlay is FRAME-COUPLED by design (common.md §4.4 step 13): the engine's
// renderer calls this component's recordOverlay once per rendered frame (the
// renderer.vulkan.overlay seam, engine::OverlayRecorder) -- that call is the
// ImGui frame. The sim advances on the fixed-update lane; we read its views at
// frame time. The scene bridge (7.2P1) syncs on the same per-frame path.
//
// Phase-1 scope (task 7.3P1 spec):
//   * F1 toggles the debug overlay (config [harrowmere_ui] key, default F1,
//     resolved through the input capability's own key-name table).
//   * The overlay shows: region / territory / law set / safe-zone flag /
//     season / weather / current tick number / fps.
//   * Input mapper (7.4P1) reads wantsMouse()/wantsKeyboard() to gate the
//     camera look and movement intents when the overlay is interactive.
//   * This is the plumbing Phase-2's real panels ride on (card 7.2/7.3 full).
//
// Not this phase (do not build ahead):
//   * HUD, character sheet, inventory, world inspector, time controls (7.3 full).
//   * Settings / keybinds UI (7.6 -- uses settings.toml).
//   * Modding surface (7.5).
//
// Conventions (common.md §3.3): snake_case file, PascalCase type, camelCase
// function. Namespace harrowmere::ui so a third-party native mod consuming
// ui.harrowmere can never collide with the harrowmere_ prefix reserved for
// game modules.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Engine capability headers (published by their owning modules -- common.md
// §3.6/§4.0). The capability headers are platform-free by the engine's
// configure-time guard; this header keeps that property -- it includes only
// the capability contracts (virtual interfaces + plain data) and std types.
#include "engine/kernel/component_interface.hpp" // ComponentContext, EventBus, EngineFrameEvent
#include "input_capability.hpp"                  // engine::InputCapability, KeyPressedEvent, KeyCode
#include "renderer_overlay_capability.hpp"       // engine::RendererOverlayHostCapability, OverlayRecorder
#include "window_capability.hpp"                 // engine::WindowCapability (optional probe: the GLFW handle)

// The sim capability contract -- the ONLY header from harrowmere_core this mod
// compiles against (D-ARCH-1, common.md §4.2). It is published by
// harrowmere_core and consumed by harrowmere_ui; no harrowmere_core internal
// header is included from this mod (test TestUi_DebugPanelBindsWorldView pins
// the compile-time view binding; review grep pins the include policy).
#include "harrowmere_capability.hpp"  // SimCapability, views::WorldView, views::PlayerView, ...

#include "backend/imgui_backend.hpp"

namespace harrowmere::ui {

// Forward declaration of the debug-panel draw function (defined in
// panels/debug.cpp). Forward-declaring here (rather than including a separate
// panels/debug.hpp) keeps the file set on task 7.3P1's Files section minimal;
// Phase-2 panels will get their own headers when they land.
//
// Draws the Phase-1 debug overlay: region / territory / law set / safe-zone
// flag underfoot, season, weather, current tick number, fps. The view types
// come from harrowmere_capability.hpp (included above) -- taking them by
// const-ref here is the compile-time binding proof (test
// TestUi_DebugPanelBindsWorldView).
//
// `pOpen` follows ImGui's Begin() convention: when the panel is closed via
// the close button, *pOpen is set to false and the caller treats it as
// "hide overlay" (equivalent to pressing F1).
void drawDebugPanel(const harrowmere::views::WorldView&    world,
                    const harrowmere::views::PlayerView&  player,
                    const harrowmere::views::JusticeView& justice,
                    const harrowmere::views::EventsView&  events,
                    const harrowmere::views::UiSignals&   signals,
                    int                             fps,
                    bool*                           pOpen);

// The harrowmere_ui mod component. One instance per run. It IS the overlay
// recorder: the renderer.vulkan.overlay host calls recordOverlay once per
// rendered frame, inside its command recording (the seam's contract -- main
// thread, no bus publishes, no window-system calls from there).
//
// Engine component lifecycle (common.md §4.0, task 0.1):
//   construct  -- acquire capabilities (input, the overlay host, the
//                 harrowmere family; probe the window capability for the
//                 GLFW handle); subscribe KeyPressedEvent (F1); read the
//                 [harrowmere_ui] key config.
//   start      -- try the backend init + recorder registration once (the
//                 renderer starts before this mod - the resolver orders
//                 them); the per-frame path retries while the renderer has
//                 not published its handle bundle yet.
//   stop       -- unregister the recorder, ImGuiBackend::shutdown, release
//                 subscriptions (RAII).
class HarrowmereUi final : public engine::OverlayRecorder {
public:
    HarrowmereUi();
    ~HarrowmereUi() override;

    HarrowmereUi(const HarrowmereUi&) = delete;
    HarrowmereUi& operator=(const HarrowmereUi&) = delete;

    // Engine component lifecycle. The mod's plugin entry (engine_query_mod_
    // entry_table -> createMod/destroyMod, task 0.1) constructs this class
    // and calls these in order; HarrowmereUi does not own its own lifetime.
    void construct(engine::ComponentContext& ctx);
    void start();
    void stop();

    // Called by 7.4P1's input mapper each frame to know whether to suppress
    // camera look / movement intents. Returns false when the overlay is hidden
    // OR ImGui has no panel with focus. Cheap (cached from the last endFrame).
    bool wantsMouse() const noexcept;
    bool wantsKeyboard() const noexcept;

    // True when the overlay is visible (toggled by F1). 7.4P1 reads this for
    // cursor-claim semantics (wantsMouse is the stronger gate; this is the
    // coarse visibility flag).
    bool overlayVisible() const noexcept { return visible_; }

    // Compile-time view binding (test TestUi_DebugPanelBindsWorldView).
    //
    // The debug panel takes the WorldView + PlayerView + other view types by
    // const reference -- including harrowmere_capability.hpp and calling
    // drawDebugPanel with the view types is itself the binding proof (a
    // future change to the view shape would fail to compile this TU).

private:
    // engine::OverlayRecorder -- the renderer.vulkan.overlay host calls this
    // once per RENDERED frame (after the composite wrote the swapchain image,
    // before present; main thread, inside command recording). This IS the
    // ImGui frame: lazy init on the first visible frame, then beginFrame ->
    // draw the panels from the sim's view snapshots -> endFrame into the
    // frame's overlay command buffer.
    void recordOverlay(const engine::OverlayFrameContext& context) override;

    // KeyPressedEvent handler -- F1 toggle for the overlay. The keybind is
    // configurable through [harrowmere_ui] key (default "F1"); task 7.6 will
    // expose it through the settings.toml-backed options screen.
    void onKeyPressed(const engine::KeyPressedEvent& ev);

    // Pulls the renderer's published backend handle bundle (plus the window
    // capability's GLFW handle) into the wrapper's bundle. Returns false (and
    // clears errOut) if any required handle is unavailable this frame;
    // HarrowmereUi retries on the next visible frame.
    bool collectOverlayHandles(ImGuiBackendHandles& out, std::string& errOut) const;

    // --- acquired capabilities (held for the component's lifetime) ---
    engine::InputCapability *input_ = nullptr;                    // required
    engine::RendererOverlayHostCapability *overlay_ = nullptr;    // required (renderer.vulkan.overlay)
    engine::WindowCapability *window_ = nullptr;                  // optional probe (the GLFW handle)
    harrowmere::SimCapability *sim_ = nullptr;                    // harrowmere.core (family harrowmere)

    // --- subscriptions (RAII; released on stop) ---
    engine::EventSubscription keySub_;

    // --- ImGui backend (lazy-init on the first visible frame) ---
    ImGuiBackend backend_;
    bool recorderRegistered_ = false; // the host's setOverlayRecorder ran

    // --- overlay state ---
    bool visible_ = true;          // F1 toggles; default visible (the 7.3P1 Done-when requires F1 shows/hides);
                                   // [harrowmere_ui] visible_on_start seeds it (a debug panel covering the
                                   // world is a dev convenience, not a content promise - automated visual
                                   // runs and players who want a clean first frame start it hidden)
    engine::KeyCode toggleKey_ = engine::KeyCode::Unknown; // [harrowmere_ui] key; default F1 (set in construct)

    // --- fps tracking (debug panel only; not a sim value) ---
    std::uint64_t frameCount_ = 0;
    double lastFpsTime_ = 0.0;
    int fps_ = 0;
};

}  // namespace harrowmere::ui
