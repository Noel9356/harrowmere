// The module plugin seam of 'input_glfw': one exported entry point that hands the
// kernel this binary's static descriptor (engine/kernel/module_api.hpp). The
// descriptor carries the module's name, its factory and its static capability
// declaration; the kernel reads all three without creating an instance, and
// only instantiates the module when the demand closure requires one of its
// capabilities.
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "input.hpp"

namespace
{
const char *const inputProvidedCapabilities[]
    = {"input.glfw",
        // The Lua bridge extension member: declares this module as a provider of
        // the "scripting.lua_bridge" family. The lua_scripting host acquires
        // every active member through ComponentContext::acquireEachCapability
        // and asks each one to install its own bindings into the host's engine
        // table. Adding this capability here is the ONLY change the input module
        // makes to opt into the Lua bridge: no lua_scripting source file,
        // manifest, or build target ever references the input module's bridge.
        "scripting.lua_bridge.input"};
const char *const inputRequiredCapabilities[] = {"logging", "window"};
// "ui" is deliberately NOT declared optional (the ui module requires the
// input capability, so an optional edge back to "ui" would close a
// construct-order cycle - see the manifest in input.cpp for the full
// rationale); the delta bridges resolve it lazily instead. The descriptor
// therefore carries the nullptr/0 empty list the other modules without
// optionals use.

engine::ModuleInterface *createinputPluginModule()
{
    return new engine::InputModule();
}

const engine::EngineModulePluginEntry inputPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "input",
    .createModule            = &createinputPluginModule,
    .providedCapabilities    = inputProvidedCapabilities,
    .providedCapabilityCount = std::size(inputProvidedCapabilities),
    .requiredCapabilities    = inputRequiredCapabilities,
    .requiredCapabilityCount = std::size(inputRequiredCapabilities),
    .optionalCapabilities    = nullptr,
    .optionalCapabilityCount = 0,
    .declaredPermissions     = nullptr,
    .declaredPermissionCount = 0
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &inputPluginEntry;
}
