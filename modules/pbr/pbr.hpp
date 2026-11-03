#ifndef ENGINE_MODULES_PBR_PBR_HPP
#define ENGINE_MODULES_PBR_PBR_HPP

#include "lighting_capability.hpp"
#include "logging_capability.hpp"
#include "lua_bridge_extension.hpp"
#include "engine/kernel/component_interface.hpp"

#include "light_state.hpp"

#include <memory>
#include <string>

namespace engine
{

// Internal Vulkan state of the pass (defined in pbr.cpp).
class PbrLightingPass;

// Module "pbr": a lighting TECHNIQUE module named after its algorithm, not
// after a rendering category. It owns the scene light state (the
// LightingCapability contract: sun + point lights, mutable by C++ mods at
// runtime; the descriptor carries the family member name "lighting.pbr") and
// provides the pbr_direct_diffuse demand marker: when the run demands it, the
// module attaches the analytic PBR pass, which evaluates the
// lights with a Cook-Torrance GGX BRDF and analytic SDF soft shadows and
// writes the direct radiance target. A module that is activated only for its
// light state stays an idle provider: mods can still place lights, and
// another lighting technique (or none) consumes them.
//
// The module additionally provides the "scripting.lua_bridge.lighting" Lua
// bridge extension: it installs engine.set_sun_light, engine.clear_sun_light,
// engine.add_point_light and engine.remove_point_light into every script state
// through a dedicated LightingLuaBridge object owned by this module (see
// pbr.cpp). The host discovers this extension through the multi-provider
// acquisition of the "scripting.lua_bridge" family, so adding this capability
// to the module never touches the lua_scripting source.
class PbrLightingModule final : public ModuleInterface, public LightingCapability
{
public:
    PbrLightingModule();
    ~PbrLightingModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    // LightingCapability (single threaded by the engine contract).
    void setSunLight(const DirectionalLightDescription &light) override;
    void clearSunLight() override;
    bool hasSunLight() const override;
    DirectionalLightDescription sunLight() const override;
    PointLightHandle addPointLight(const PointLightDescription &light) override;
    void updatePointLight(PointLightHandle handle, const PointLightDescription &light) override;
    void removePointLight(PointLightHandle handle) override;
    void clearPointLights() override;
    std::uint32_t pointLightCount() const override;
    PointLightDescription pointLight(std::uint32_t index) const override;

private:
    ComponentManifest componentManifest;
    LoggerCapability *logger = nullptr;
    bool passAttached        = false;
    std::shared_ptr<PbrLightingPass> pass;
    std::shared_ptr<LightState> state;
    // The Lua bridge extension object owned by this module. Constructed in
    // construct() and returned from capabilityPointerFor for the
    // "scripting.lua_bridge.lighting" member. Held as the base
    // LuaBridgeExtension pointer: the concrete LightingLuaBridge type
    // (defined in pbr.cpp) stays private to this module's translation unit.
    std::unique_ptr<LuaBridgeExtension> luaBridge;
};

} // namespace engine

#endif // ENGINE_MODULES_PBR_PBR_HPP
