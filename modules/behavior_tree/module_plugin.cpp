// The module plugin seam of 'behavior_tree': one exported entry point that
// hands the kernel this binary's static descriptor (engine/kernel/module_api.hpp).
// The descriptor carries the module's name, its factory and its static
// capability declaration; the kernel reads all three without creating an
// instance, and only instantiates the module when the demand closure requires
// one of its capabilities. The descriptor name must equal the name the
// module's manifest reports - the kernel verifies the agreement at startup
// and refuses a drifting plugin with a precise diagnostic.
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "behavior_tree.hpp"

namespace
{
const char *const behaviorTreeProvidedCapabilities[]
    = {"behavior_tree.basic",
        // The Lua bridge extension member: declares this module as a provider of
        // the "scripting.lua_bridge" family. The lua_scripting host acquires every
        // active member through ComponentContext::acquireEachCapability and asks
        // each one to install its own bindings into the host's engine table. Adding
        // this capability here is the ONLY change the behavior_tree module makes to
        // opt into the Lua bridge: no lua_scripting source file, manifest, or
        // build target ever references the behavior_tree module.
        "scripting.lua_bridge.behavior_tree"};
const char *const behaviorTreeRequiredCapabilities[] = {"logging"};

engine::ModuleInterface *createbehaviorTreePluginModule()
{
    return new engine::BehaviorTreeModule();
}

const engine::EngineModulePluginEntry behaviorTreePluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "behavior_tree",
    .createModule            = &createbehaviorTreePluginModule,
    .providedCapabilities    = behaviorTreeProvidedCapabilities,
    .providedCapabilityCount = std::size(behaviorTreeProvidedCapabilities),
    .requiredCapabilities    = behaviorTreeRequiredCapabilities,
    .requiredCapabilityCount = std::size(behaviorTreeRequiredCapabilities),
    .optionalCapabilities    = nullptr,
    .optionalCapabilityCount = 0,
    .declaredPermissions     = nullptr,
    .declaredPermissionCount = 0
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &behaviorTreePluginEntry;
}
