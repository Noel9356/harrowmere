#ifndef ENGINE_MODULES_ASSET_HPP
#define ENGINE_MODULES_ASSET_HPP

#include "asset_capability.hpp"
#include "logging_capability.hpp"
#include "lua_bridge_extension.hpp"
#include "scene_capability.hpp"
#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/event_bus.hpp"

#include <memory>
#include <string>

namespace engine
{

// Module "asset_gltf": content import into the scene registry. The first source is
// glTF 2.0 (see the module README for the mapping contract): node hierarchy,
// world transforms, per-primitive bounds and material factors become scene
// registry objects, and every consumer of the registry - renderer, physics,
// animation, scripts - sees the imported level exactly like any other
// runtime-spawned content.
//
// The module additionally provides the "scripting.lua_bridge.asset" Lua
// bridge extension: it installs engine.import_gltf into every script state
// through a dedicated AssetLuaBridge object owned by this module (see
// asset.cpp). The host discovers this extension through the multi-provider
// acquisition of the "scripting.lua_bridge" family, so adding this capability
// to the module never touches the lua_scripting source.
class AssetModule final : public ModuleInterface, public AssetCapability
{
public:
    AssetModule();
    ~AssetModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    // AssetCapability.
    std::uint32_t
    importGltfIntoScene(const std::string &path, std::uint32_t fidelity, std::string &errorReport) override;

private:
    ComponentManifest componentManifest;
    ComponentContext *boundContext   = nullptr;
    LoggerCapability *logger         = nullptr;
    SceneCapability *sceneCapability = nullptr;
    // The Lua bridge extension object owned by this module. Constructed in
    // construct() and returned from capabilityPointerFor for the
    // "scripting.lua_bridge.asset" member. Held as the base LuaBridgeExtension
    // pointer: the concrete AssetLuaBridge type (defined in asset.cpp) stays
    // private to this module's translation unit.
    std::unique_ptr<LuaBridgeExtension> luaBridge;
};

} // namespace engine

#endif // ENGINE_MODULES_ASSET_HPP
