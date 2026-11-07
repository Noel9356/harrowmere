#ifndef ENGINE_MODULES_GI_SCREEN_WORLD_GI_HPP
#define ENGINE_MODULES_GI_SCREEN_WORLD_GI_HPP

#include "gi_capability.hpp"
#include "logging_capability.hpp"
#include "engine/kernel/component_interface.hpp"

#include <memory>
#include <string>

namespace engine
{

// Internal Vulkan state of the pass (defined in gi.cpp).
class GiScreenWorldPass;

// Module "gi_screen_world": a lighting TECHNIQUE module named after its
// algorithm - the per-pixel cosine-HEMISPHERE trace against the analytic
// scene SDF (the name the owner pinned in rev 31: "gi_traced" was too
// generic, any GI is traced; THIS one is a hemisphere gather over the
// distance field). It provides the gi_screen_world_diffuse demand marker:
// when the run demands it, the module attaches the ray-traced diffuse GI
// pass, which fills the renderer's indirectRadiance target (diffuse GI
// irradiance / pi) with a stratified multi-ray SDF hemisphere trace, an
// analytic relight of the hit points, a screen-space second bounce and a
// reprojection-validated temporal accumulation. A module activated only for
// its family member stays an idle provider, exactly like the pbr module's
// light-state contract.
class GiScreenWorldModule final : public ModuleInterface
{
public:
    GiScreenWorldModule();
    ~GiScreenWorldModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

private:
    ComponentManifest componentManifest;
    LoggerCapability *logger = nullptr;
    bool passAttached        = false;
    std::shared_ptr<GiScreenWorldPass> pass;
    GiScreenWorldSettings settings;
};

} // namespace engine

#endif // ENGINE_MODULES_GI_SCREEN_WORLD_GI_HPP
