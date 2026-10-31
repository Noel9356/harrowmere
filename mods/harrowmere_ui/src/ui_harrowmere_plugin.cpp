// The native-mod plugin seam of 'harrowmere_ui': the same contract
// harrowmere_core answers (mod_api.hpp). v0.1 was an empty shell with inert
// lifecycle - the manifest wiring (requires harrowmere + input +
// renderer.vulkan.overlay, provides ui.harrowmere) is what that task
// validated; the ImGui backend and panels arrive at 7.3P1.
//
// Task 7.4P1 wires the game's input layer here: the mod acquires its
// requirements, adapts them onto the input mapper's two seams (UiInputGate,
// CameraRig - see input_mapper.hpp), and drives one mapper update per
// EngineFrameEvent. The mapper is the frame-coupled half of the input path
// (the sim advances on the fixed-update lane; this layer reads devices and
// submits intents). The F1 edge is NOT wired here: the overlay component's
// own KeyPressedEvent subscription owns that toggle (7.3P1) - single
// ownership is the card's double-fire pitfall answered by construction, and
// the mapper's binding table pins the row without polling it.
//
// Capability notes:
//   * input + harrowmere are manifest REQUIREMENTS - a missing one fails
//     startup with a precise ComponentAccessError (a UI mod that cannot read
//     devices or reach the sim is not a UI mod).
//   * renderer is an OPTIONAL capability (mod.toml optional_capabilities):
//     the P1 camera rig degrades to tracked-heading-only when the renderer
//     is absent (headless, minimal builds) - the feature degrades, never the
//     run (common.md section 4.0). In the game assembly the renderer module
//     is always active (harrowmere_ui's own renderer.vulkan.overlay
//     requirement pulls it), so the probe resolves in every real run.
#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/component_manifest.hpp"
#include "engine/kernel/mod_api.hpp"

#include <memory>
#include <string>
#include <typeindex>

#include "input_capability.hpp"       // engine::InputCapability (required)
#include "renderer_capability.hpp"    // engine::RendererCapability (optional camera rig)
#include "renderer_overlay_capability.hpp" // engine::RendererOverlayHostCapability (required, 7.3P1)
#include "window_capability.hpp"      // engine::WindowCapability (optional probe: the GLFW handle)
#include "harrowmere_capability.hpp"  // harrowmere::SimCapability (required, family harrowmere)

#include "ui_harrowmere_capability.hpp"
#include "input_mapper.hpp"           // the 7.4P1 deliverable: seams + mapper
#include "ui_harrowmere.hpp"          // the 7.3P1 deliverable: the overlay component

// Logging shim (see imgui_backend.cpp for the rationale): the UI mod does not
// require the logging capability in its manifest, so lifecycle lines go to
// stderr unless the project's macros are on the include path.
#ifndef HARROWMERE_LOG_INFO
#  include <cstdio>
#  define HARROWMERE_LOG_INFO(cat, fmt, ...)  std::fprintf(stderr, "[ui][INFO] " fmt "\n", ##__VA_ARGS__)
#  define HARROWMERE_LOG_WARN(cat, fmt, ...)  std::fprintf(stderr, "[ui][WARN] " fmt "\n", ##__VA_ARGS__)
#endif

#define HARROWMERE_LOG_CATEGORY ui

namespace
{

// ---- the two seams, adapted onto the engine capabilities ----------------------

// The overlay gate: forwards to the 7.3P1 overlay component's claim methods
// (the seams' semantics are HarrowmereUi's method contract - ui_harrowmere.hpp
// documents them for exactly this consumer). The mapper gates camera look and
// movement intents on these so the overlay never fights the cursor.
class OverlayUiGate final : public harrowmere::ui::UiInputGate
{
public:
    explicit OverlayUiGate(harrowmere::ui::HarrowmereUi &overlay)
        : overlay_(overlay)
    {
    }

    bool wantsMouse() const override
    {
        return overlay_.wantsMouse();
    }

    bool wantsKeyboard() const override
    {
        return overlay_.wantsKeyboard();
    }

private:
    harrowmere::ui::HarrowmereUi &overlay_;
};

// The P1 camera rig: the renderer capability's camera pose (the free-fly
// controller's state - the renderer capability doc names it "the one camera
// truth", and its setters mutate that state, so the mapper's look survives
// the controller's per-frame write-back). Floats at the capability boundary,
// doubles inside the mapper (R5's presentation carve-out).
class RendererCameraRig final : public harrowmere::ui::CameraRig
{
public:
    explicit RendererCameraRig(
        engine::RendererCapability &renderer
    )
        : renderer_(&renderer)
    {
    }

    std::array<double, 3> position() const override
    {
        const std::array<float, 3> position = renderer_->cameraPosition();
        return {static_cast<double>(position[0]), static_cast<double>(position[1]), static_cast<double>(position[2])};
    }

    std::array<double, 3> lookAt() const override
    {
        const std::array<float, 3> lookAt = renderer_->cameraLookAt();
        return {static_cast<double>(lookAt[0]), static_cast<double>(lookAt[1]), static_cast<double>(lookAt[2])};
    }

    void setLookAt(
        const std::array<double, 3> &lookAt
    ) override
    {
        renderer_->setCameraLookAt({static_cast<float>(lookAt[0]), static_cast<float>(lookAt[1]),
                                    static_cast<float>(lookAt[2])});
    }

    void setPosition(
        const std::array<double, 3> &position
    ) override
    {
        // The renderer capability's documented teleport semantics: the pose
        // is the free-fly controller's state, and a set position keeps the
        // current look direction - exactly the follow-cam's contract.
        renderer_->setCameraPosition({static_cast<float>(position[0]), static_cast<float>(position[1]),
                                      static_cast<float>(position[2])});
    }

private:
    engine::RendererCapability *renderer_;
};

// ---- the mod component ----------------------------------------------------------

class HarrowmereUiMod final : public engine::ModInterface
{
public:
    HarrowmereUiMod()
    {
        // Must agree with mod.toml on identity and on `provides` - the same
        // drift check the core mod keeps (and the kernel enforces).
        manifest_.name                 = "harrowmere_ui";
        manifest_.kind                 = engine::ComponentKind::SharedLibraryMod;
        manifest_.version              = "0.1.0";
        manifest_.providedCapabilities = {"ui.harrowmere"};
        manifest_.requiredCapabilities = {"harrowmere", "input", "renderer.vulkan.overlay"};
        manifest_.optionalCapabilities = {"renderer", "window"};
        manifest_.knownConfigKeys      = {"harrowmere_ui.key", "harrowmere_ui.visible_on_start",
                                          "harrowmere_ui.follow_camera"};
    }

    const engine::ComponentManifest &manifest() const override
    {
        return manifest_;
    }

    void construct(
        engine::ComponentContext &context
    ) override
    {
        // The two required capabilities: the devices and the sim (the graph
        // orders them before this construct runs - the resolver's work).
        //
        // The sim acquire is BY FAMILY NAME ("harrowmere" - the manifest's
        // requirement spelling, D-UI-1): the kernel's derived grants are one
        // "capability.<name>" per declared requirement, so the family
        // requirement grants capability.harrowmere, and the resolver answers
        // a family request with the single active member's pointer
        // (harrowmere.core - engine_kernel.cpp: "exact declarations first,
        // then - for a family request - the single active member"). The typed
        // template form would request capability.harrowmere.core, which no
        // family requirement grants; the app's template-form acquire in
        // app/harrowmere_app.cpp works because the application context is the
        // trusted composition root (granted everything), a mod is not.
        engine::InputCapability &input = context.acquireCapability<engine::InputCapability>();
        harrowmere::SimCapability &sim = *static_cast<harrowmere::SimCapability *>(
            context.acquireCapability(harrowmere::SimCapability::capabilityFamily));

        // The optional camera rig: present in every real run of the game
        // assembly (this mod's own renderer.vulkan.overlay requirement pulls
        // the renderer module), absent in degraded builds - the mapper then
        // degrades to tracked-heading-only, never the run.
        engine::RendererCapability *renderer = context.tryAcquireCapability<engine::RendererCapability>();
        cameraRig_                           = renderer != nullptr ? std::make_unique<RendererCameraRig>(*renderer)
                                                                   : nullptr;

        // The follow-cam (the presentation half of "the player walks", see
        // input_mapper.hpp): ON by default - the exit checklist's walk is
        // observable through it. [harrowmere_ui].follow_camera = 0 restores
        // the pure dev fly camera (the camera then stays wherever the
        // configured pose and the gamepad axes put it). Degrades off with the
        // rig (renderer absent), like every camera feature here.
        const bool followCamera = context.configurationIntegerInSection("harrowmere_ui", "follow_camera").value_or(1) != 0;
        if (cameraRig_ != nullptr && followCamera)
        {
            followCamera_ = std::make_unique<harrowmere::ui::PlayerFollowCamera>(sim, *cameraRig_);
        }

        // The 7.3P1 overlay component (construct acquires its own
        // capabilities, subscribes the F1 toggle, reads [harrowmere_ui] key).
        overlay_ = std::make_unique<harrowmere::ui::HarrowmereUi>();
        overlay_->construct(context);

        // The 7.4P1 input mapper over the seams. The gate and the rig are
        // members (the mapper keeps references; they outlive it - declaration
        // order below reverses destruction correctly). The gate forwards to
        // the overlay's claim methods.
        gate_ = std::make_unique<OverlayUiGate>(*overlay_);
        mapper_ = std::make_unique<harrowmere::ui::InputMapper>(input, sim, *gate_, cameraRig_.get());

        // One mapper update per EngineFrameEvent - the per-frame heartbeat
        // (common.md section 4.0). The dispatch strictly precedes the
        // renderer's camera integration inside the composition root's loop
        // (publish, then renderFrame), so the mapper's cursor-delta drain is
        // always first and look stays single-consumer by construction
        // (input_mapper.hpp). The overlay draw (7.3P1) and the scene bridge
        // (7.2P1) ride the same event; none of them consumes cursor deltas,
        // so there is no ordering hazard with either.
        engine::EventChannel<engine::EngineFrameEvent> &frameChannel =
            context.eventBus().channel<engine::EngineFrameEvent>();
        frameSubscription_ = engine::EventSubscription(
            &context.eventBus(), typeid(engine::EngineFrameEvent),
            frameChannel.subscribe<HarrowmereUiMod, &HarrowmereUiMod::onFrame>(this)
        );
    }

    void start() override
    {
        // The overlay component starts after the mapper's seams are bound
        // (its recorder registration probes the renderer's handle bundle).
        if (overlay_ != nullptr)
        {
            overlay_->start();
        }
        HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "harrowmere_ui 0.1.0 up (7.3P1 overlay + 7.4P1 input mapper active)");
    }

    void stop() override
    {
        // Mirror of construct in reverse: the subscription first (no in-flight
        // frame dispatch lands on a tearing-down mapper), then the mapper,
        // then the seams. The RAII guard would release anyway; the explicit
        // reset keeps the same shape the engine modules keep.
        frameSubscription_.release();
        mapper_.reset();
        followCamera_.reset();
        gate_.reset();
        if (overlay_ != nullptr)
        {
            overlay_->stop();
            overlay_.reset();
        }
        cameraRig_.reset();
    }

    void *capabilityPointerFor(
        const std::string &capabilityName
    ) override
    {
        if (capabilityName == harrowmere::UiHarrowmereCapability::capabilityName)
        {
            return &uiCapability_;
        }
        return nullptr;
    }

private:
    // The per-frame dispatch: one input-mapper update, then the follow-cam
    // (the mapper is input - device reads and intent submission; the
    // follow-cam is presentation - one view read, one eye write. Both ride
    // the same frame event; the order makes the walk land on the same frame
    // the intent was submitted from). Input is frame-coupled by design
    // (input_mapper.hpp); the sim reads the submitted intents on the
    // fixed-update lane.
    void onFrame(
        const engine::EngineFrameEvent & /*event*/
    )
    {
        if (mapper_ != nullptr)
        {
            mapper_->update();
        }
        if (followCamera_ != nullptr)
        {
            followCamera_->update();
        }
    }

    engine::ComponentManifest manifest_;
    harrowmere::UiHarrowmereCapability uiCapability_{};

    // The 7.3P1 + 7.4P1 wiring. Declaration order is destruction order: the
    // mapper (which holds references into the gate and the rig) dies before
    // them, and the overlay outlives both (the gate forwards into it).
    std::unique_ptr<harrowmere::ui::HarrowmereUi> overlay_;
    std::unique_ptr<OverlayUiGate> gate_;
    std::unique_ptr<RendererCameraRig> cameraRig_;
    std::unique_ptr<harrowmere::ui::PlayerFollowCamera> followCamera_;
    std::unique_ptr<harrowmere::ui::InputMapper> mapper_;
    engine::EventSubscription frameSubscription_;
};

engine::ModInterface *createHarrowmereUiMod()
{
    return new HarrowmereUiMod();
}

void destroyHarrowmereUiMod(
    engine::ModInterface *mod
)
{
    delete mod;
}

const EngineModEntryTable harrowmereUiEntryTable{
    .interfaceVersion = engine::engineModInterfaceVersion,
    .createMod        = &createHarrowmereUiMod,
    .destroyMod       = &destroyHarrowmereUiMod,
};

} // namespace

extern "C" const EngineModEntryTable *engine_query_mod_entry_table()
{
    return &harrowmereUiEntryTable;
}
