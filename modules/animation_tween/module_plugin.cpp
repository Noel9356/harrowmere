// The module plugin seam of 'animation_tween': one exported entry point that hands
// the kernel this binary's static descriptor (engine/kernel/module_api.hpp).
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "animation.hpp"

namespace
{
const char *const animationProvidedCapabilities[]
    = {"animation.tweens",
        // The Lua bridge extension member: declares this module as a provider of
        // the "scripting.lua_bridge" family. The lua_scripting host acquires
        // every active member through ComponentContext::acquireEachCapability
        // and asks each one to install its own bindings into the host's engine
        // table. Adding this capability here is the ONLY change the animation
        // module makes to opt into the Lua bridge.
        "scripting.lua_bridge.animation"};
const char *const animationRequiredCapabilities[] = {"logging", "scene"};
const char *const animationOptionalCapabilities[] = {"lighting"};

engine::ModuleInterface *createanimationPluginModule()
{
    return new engine::AnimationModule();
}

const engine::EngineModulePluginEntry animationPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "animation",
    .createModule            = &createanimationPluginModule,
    .providedCapabilities    = animationProvidedCapabilities,
    .providedCapabilityCount = std::size(animationProvidedCapabilities),
    .requiredCapabilities    = animationRequiredCapabilities,
    .requiredCapabilityCount = std::size(animationRequiredCapabilities),
    .optionalCapabilities    = animationOptionalCapabilities,
    .optionalCapabilityCount = std::size(animationOptionalCapabilities),
    .declaredPermissions     = nullptr,
    .declaredPermissionCount = 0
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &animationPluginEntry;
}
