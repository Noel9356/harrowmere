// ui_harrowmere.hpp -- harrowmere_ui mod component (Phase-1 slice of task 7.3).
//
// The harrowmere_ui mod *provides* ui.harrowmere (family ui) and consumes the
// harrowmere (harrowmere.core) family + input + renderer.vulkan.overlay
// capabilities directly (D-UI-1, common.md §4.1). The engine's own ui_imgui
// module is restored at build per §3.6's build-edge rule but is runtime-inert
// -- no active component demands the `ui` capability, so the widget tree
// (including the debug panel this class drives) belongs to us.
//
// This component owns the ImGuiBackend (its GLFW+Vulkan seam) and the per-frame
// draw pass. It subscribes EngineFrameEvent (the engine's per-iteration
// heartbeat, common.md §4.0) -- not EngineFixedUpdateEvent: the overlay is
// frame-coupled by design (common.md §4.4 step 13 runs per FRAME, not per
// tick). The sim advances on the fixed-update lane; we read its views at frame
// time. The bridge syncs on the same EngineFrameEvent (7.2P1).
//
// Phase-1 scope (task 7.3P1 spec):
//   * F1 toggles the debug overlay (config [harrowmere_ui].key, default F1).
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
#include "engine/capability/component_context.hpp"      // ComponentContext, acquireCapability
#include "engine/capability/event_bus.hpp"               // EventChannel, EventSubscription
#include "engine/capability/input.hpp"                   // InputCapability
#include "engine/capability/renderer_vulkan_overlay.hpp" // RendererVulkanOverlayCapability
#include "engine/events/engine_frame_event.hpp"          // EngineFrameEvent
#include "engine/events/key_event.hpp"                   // KeyPressedEvent

// The sim capability contract -- the ONLY header from harrowmere_core this mod
// compiles against (D-ARCH-1, common.md §4.2). It is published by
// harrowmere_core and consumed by harrowmere_ui; no harrowmere_core internal
// header is included from this mod (test TestUi_DebugPanelBindsWorldView pins
// the compile-time view binding; review grep pins the include policy).
#include "harrowmere_capability.hpp"  // SimCapability, WorldView, PlayerView, ...

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
void drawDebugPanel(const harrowmere::WorldView&    world,
                    const harrowmere::PlayerView&  player,
                    const harrowmere::JusticeView& justice,
                    const harrowmere::EventsView&  events,
                    const harrowmere::UiSignals&   signals,
                    int                             fps,
                    bool*                           pOpen);

// The harrowmere_ui mod component. One instance per run.
//
// Engine component lifecycle (common.md §4.0, task 0.1):
//   construct  -- acquire capabilities (input, renderer.vulkan.overlay, the
//                 harrowmere family); subscribe to EngineFrameEvent and
//                 KeyPressedEvent; read the F1 keybind from config.
//   start      -- nothing eager; the overlay waits for the first *visible*
//                 frame to init the ImGui backend (ImGui init before the
//                 backend handle bundle completes fails -- exactly as
//                 ImGui's own example backend does).
//   stop       -- ImGuiBackend::shutdown; release subscriptions (RAII).
class HarrowmereUi {
public:
    HarrowmereUi();
    ~HarrowmereUi();

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
    // The debug panel takes the WorldView + PlayerView + other views by const
    // reference -- including harrowmere_capability.hpp and calling drawDebugPanel
    // with the view types is itself the binding proof (a future change to the
    // view shape would fail to compile this TU). The function is forward-declared
    // above and defined in panels/debug.cpp.

private:
    // EngineFrameEvent handler -- the per-frame draw pass. Subscribed in
    // construct; dispatched by the engine's event bus on the main thread
    // (single-threaded contract, common.md §4.0).
    void onFrameBegin(const engine::EngineFrameEvent& ev);

    // KeyPressedEvent handler -- F1 toggle for the overlay. The keybind is
    // configurable through [harrowmere_ui].key (default "F1"); task 7.6 will
    // expose it through the settings.toml-backed options screen.
    void onKeyPressed(const engine::KeyPressedEvent& ev);

    // Pulls the renderer.vulkan.overlay handle bundle for ImGuiBackend::init.
    // Returns false (and clears errOut) if any required handle is unavailable
    // this frame; HarrowmereUi retries on the next visible frame.
    bool collectOverlayHandles(ImGuiBackendHandles& out, std::string& errOut) const;

    // --- acquired capabilities (held for the component's lifetime) ---
    std::shared_ptr<engine::InputCapability> input_;
    std::shared_ptr<engine::RendererVulkanOverlayCapability> overlay_;
    std::shared_ptr<harrowmere::SimCapability> sim_;  // harrowmere.core (family harrowmere)

    // --- subscriptions (RAII; released on stop) ---
    std::unique_ptr<engine::EventSubscription<engine::EngineFrameEvent>> frameSub_;
    std::unique_ptr<engine::EventSubscription<engine::KeyPressedEvent>> keySub_;

    // --- ImGui backend (lazy-init on first visible frame) ---
    ImGuiBackend backend_;
    bool backendInitAttempted_ = false;  // true after the first init attempt (success or fail)

    // --- overlay state ---
    bool visible_ = true;          // F1 toggles; default visible (the 7.3P1 Done-when requires F1 shows/hides)
    std::uint32_t toggleKey_ = 0;  // [harrowmere_ui].key; default GLFW_KEY_F1 (set in construct)

    // --- fps tracking (debug panel only; not a sim value) ---
    std::uint64_t frameCount_ = 0;
    double lastFpsTime_ = 0.0;
    int fps_ = 0;
};

}  // namespace harrowmere::ui
