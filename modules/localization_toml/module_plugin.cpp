// The module plugin seam of 'localization_toml': one exported entry point
// that hands the kernel this binary's static descriptor
// (engine/kernel/module_api.hpp). The descriptor carries the module's name,
// its factory and its static capability declaration; the kernel reads all
// three without creating an instance, and only instantiates the module when
// the demand closure requires one of its capabilities. The descriptor name
// must equal the name the module's manifest reports - the kernel verifies
// the agreement at startup and refuses a drifting plugin with a precise
// diagnostic.
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "localization.hpp"

namespace
{
const char *const localizationProvidedCapabilities[]
    = {"localization.toml",
        // The Lua bridge extension member: declares this module as a provider of
        // the "scripting.lua_bridge" family. The lua_scripting host acquires every
        // active member through ComponentContext::acquireEachCapability and asks
        // each one to install its own bindings into the host's engine table. Adding
        // this capability here is the ONLY change the localization module makes to
        // opt into the Lua bridge: no lua_scripting source file, manifest, or
        // build target ever references the localization module.
        "scripting.lua_bridge.localization"};
const char *const localizationRequiredCapabilities[] = {"logging"};

engine::ModuleInterface *createlocalizationPluginModule()
{
    return new engine::LocalizationModule();
}

// The module's environment-scope permission requests (everything outside
// the capability.* namespace), declared statically so the kernel can
// risk-assess this module before its binary is instantiated (see
// engine/kernel/module_api.hpp, declaredPermissions).
const char *const localization_tomlDeclaredPermissions[] = {"filesystem.read"};

const engine::EngineModulePluginEntry localizationPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "localization",
    .createModule            = &createlocalizationPluginModule,
    .providedCapabilities    = localizationProvidedCapabilities,
    .providedCapabilityCount = std::size(localizationProvidedCapabilities),
    .requiredCapabilities    = localizationRequiredCapabilities,
    .requiredCapabilityCount = std::size(localizationRequiredCapabilities),
    .optionalCapabilities    = nullptr,
    .optionalCapabilityCount = 0,
    .declaredPermissions     = localization_tomlDeclaredPermissions,
    .declaredPermissionCount = std::size(localization_tomlDeclaredPermissions)
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &localizationPluginEntry;
}
