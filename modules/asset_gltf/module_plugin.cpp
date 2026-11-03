// The module plugin seam of 'asset_gltf': one exported entry point that hands the
// kernel this binary's static descriptor (engine/kernel/module_api.hpp).
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "asset.hpp"

namespace
{
const char *const assetProvidedCapabilities[]
    = {"asset.gltf",
        // The Lua bridge extension member: declares this module as a provider of
        // the "scripting.lua_bridge" family. The lua_scripting host acquires
        // every active member through ComponentContext::acquireEachCapability
        // and asks each one to install its own bindings into the host's engine
        // table. Adding this capability here is the ONLY change the asset module
        // makes to opt into the Lua bridge.
        "scripting.lua_bridge.asset"};
const char *const assetRequiredCapabilities[] = {"logging", "scene"};

engine::ModuleInterface *createassetPluginModule()
{
    return new engine::AssetModule();
}

// The module's environment-scope permission requests (everything outside
// the capability.* namespace), declared statically so the kernel can
// risk-assess this module before its binary is instantiated (see
// engine/kernel/module_api.hpp, declaredPermissions).
const char *const asset_gltfDeclaredPermissions[] = {"filesystem.read"};

const engine::EngineModulePluginEntry assetPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "asset",
    .createModule            = &createassetPluginModule,
    .providedCapabilities    = assetProvidedCapabilities,
    .providedCapabilityCount = std::size(assetProvidedCapabilities),
    .requiredCapabilities    = assetRequiredCapabilities,
    .requiredCapabilityCount = std::size(assetRequiredCapabilities),
    .optionalCapabilities    = nullptr,
    .optionalCapabilityCount = 0,
    .declaredPermissions     = asset_gltfDeclaredPermissions,
    .declaredPermissionCount = std::size(asset_gltfDeclaredPermissions)
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &assetPluginEntry;
}
