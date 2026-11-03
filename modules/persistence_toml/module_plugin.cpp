// The module plugin seam of 'persistence_toml': one exported entry point that
// hands the kernel this binary's static descriptor (engine/kernel/module_api.hpp).
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "persistence.hpp"

namespace
{
const char *const persistenceProvidedCapabilities[]
    = {"persistence.toml",
        // The Lua bridge extension member: declares this module as a provider of
        // the "scripting.lua_bridge" family. The lua_scripting host acquires
        // every active member through ComponentContext::acquireEachCapability
        // and asks each one to install its own bindings into the host's engine
        // table. Adding this capability here is the ONLY change the persistence
        // module makes to opt into the Lua bridge.
        "scripting.lua_bridge.persistence"};
const char *const persistenceRequiredCapabilities[] = {"logging", "scene"};

engine::ModuleInterface *createpersistencePluginModule()
{
    return new engine::PersistenceModule();
}

// The module's environment-scope permission requests (everything outside
// the capability.* namespace), declared statically so the kernel can
// risk-assess this module before its binary is instantiated (see
// engine/kernel/module_api.hpp, declaredPermissions).
const char *const persistence_tomlDeclaredPermissions[] = {"filesystem.write", "filesystem.read"};

const engine::EngineModulePluginEntry persistencePluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "persistence",
    .createModule            = &createpersistencePluginModule,
    .providedCapabilities    = persistenceProvidedCapabilities,
    .providedCapabilityCount = std::size(persistenceProvidedCapabilities),
    .requiredCapabilities    = persistenceRequiredCapabilities,
    .requiredCapabilityCount = std::size(persistenceRequiredCapabilities),
    .optionalCapabilities    = nullptr,
    .optionalCapabilityCount = 0,
    .declaredPermissions     = persistence_tomlDeclaredPermissions,
    .declaredPermissionCount = std::size(persistence_tomlDeclaredPermissions)
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &persistencePluginEntry;
}
