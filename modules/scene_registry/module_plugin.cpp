// The module plugin seam of 'scene_registry': one exported entry point that hands the
// kernel this binary's static descriptor (engine/kernel/module_api.hpp).
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "scene.hpp"

namespace
{
const char *const sceneProvidedCapabilities[]
    = {"scene.registry",
        // The Lua bridge extension member: declares this module as a provider of
        // the "scripting.lua_bridge" family. The lua_scripting host acquires
        // every active member through ComponentContext::acquireEachCapability
        // and asks each one to install its own bindings into the host's engine
        // table. Adding this capability here is the ONLY change the scene module
        // makes to opt into the Lua bridge.
        "scripting.lua_bridge.scene"};
const char *const sceneRequiredCapabilities[] = {"logging"};

engine::ModuleInterface *createscenePluginModule()
{
    return new engine::SceneModule();
}

const engine::EngineModulePluginEntry scenePluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "scene",
    .createModule            = &createscenePluginModule,
    .providedCapabilities    = sceneProvidedCapabilities,
    .providedCapabilityCount = std::size(sceneProvidedCapabilities),
    .requiredCapabilities    = sceneRequiredCapabilities,
    .requiredCapabilityCount = std::size(sceneRequiredCapabilities),
    .optionalCapabilities    = nullptr,
    .optionalCapabilityCount = 0,
    .declaredPermissions     = nullptr,
    .declaredPermissionCount = 0
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &scenePluginEntry;
}
