// The native-mod plugin seam of 'harrowmere_ui': the same contract
// harrowmere_core answers (mod_api.hpp). v0.1 is an empty shell with inert
// lifecycle - the manifest wiring (requires harrowmere + input +
// renderer.vulkan.overlay, provides ui.harrowmere) is what this task
// validates; the ImGui backend and panels arrive at 7.3P1/7.4P1.
#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/component_manifest.hpp"
#include "engine/kernel/mod_api.hpp"

#include <string>

#include "ui_harrowmere_capability.hpp"

namespace
{

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
    }

    const engine::ComponentManifest &manifest() const override
    {
        return manifest_;
    }

    // Inert lifecycle until 7.3P1: no panels exist yet. The requirements
    // (the sim capability, input, the renderer overlay seam) are all resolved
    // and started before this construct runs - the graph orders them - so
    // Phase 1's later tasks can start registering surfaces here.
    void construct(engine::ComponentContext &) override
    {
    }

    void start() override
    {
    }

    void stop() override
    {
    }

    void *capabilityPointerFor(const std::string &capabilityName) override
    {
        if (capabilityName == harrowmere::UiHarrowmereCapability::capabilityName)
        {
            return &uiCapability_;
        }
        return nullptr;
    }

private:
    engine::ComponentManifest manifest_;
    harrowmere::UiHarrowmereCapability uiCapability_{};
};

engine::ModInterface *createHarrowmereUiMod()
{
    return new HarrowmereUiMod();
}

void destroyHarrowmereUiMod(engine::ModInterface *mod)
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
