// The module plugin seam of 'pbr': one exported entry point that hands the
// kernel this binary's static descriptor (engine/kernel/module_api.hpp). The
// descriptor carries the module's name, its factory and its static capability
// declaration; the kernel reads all three without creating an instance, and
// only instantiates the module when the demand closure requires one of its
// capabilities.
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "pbr.hpp"

namespace
{
// "lighting.pbr" is the implementation-bound member of the "lighting" family:
// the scene light state this module owns (consumers demand the family, a run
// that needs THIS provider names the member exactly).
// "pbr_direct_diffuse" is the technique's demand marker: the run demands it
// when the analytic direct pass should attach to the renderer. The marker
// names the technique and the transport it activates (not the abstract
// radiance layer it fills) - see the module README, "Wiring".
const char *const pbrProvidedCapabilities[]
    = {"lighting.pbr",
        "pbr_direct_diffuse",
        // The Lua bridge extension member: declares this module as a provider of
        // the "scripting.lua_bridge" family. The lua_scripting host acquires
        // every active member through ComponentContext::acquireEachCapability
        // and asks each one to install its own bindings into the host's engine
        // table. Adding this capability here is the ONLY change the pbr module
        // makes to opt into the Lua bridge.
        "scripting.lua_bridge.lighting"};
const char *const pbrRequiredCapabilities[] = {"logging", "renderer"};

engine::ModuleInterface *createpbrPluginModule()
{
    return new engine::PbrLightingModule();
}

const engine::EngineModulePluginEntry pbrPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "pbr",
    .createModule            = &createpbrPluginModule,
    .providedCapabilities    = pbrProvidedCapabilities,
    .providedCapabilityCount = std::size(pbrProvidedCapabilities),
    .requiredCapabilities    = pbrRequiredCapabilities,
    .requiredCapabilityCount = std::size(pbrRequiredCapabilities),
    .optionalCapabilities    = nullptr,
    .optionalCapabilityCount = 0,
    .declaredPermissions     = nullptr,
    .declaredPermissionCount = 0
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &pbrPluginEntry;
}
