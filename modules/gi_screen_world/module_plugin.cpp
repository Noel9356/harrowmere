// The module plugin seam of 'gi_screen_world': one exported entry point
// that hands the kernel this binary's static descriptor
// (engine/kernel/module_api.hpp). The descriptor carries the module's name,
// its factory and its static capability declaration; the kernel reads all
// three without creating an instance, and only instantiates the module when
// the demand closure requires one of its capabilities.
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "gi.hpp"

namespace
{
// "gi_screen_world_diffuse" is the technique's demand marker: the run
// demands it when the SDF hemisphere-trace diffuse GI pass should attach to
// the renderer. The marker names the technique and the transport it activates
// (not the abstract radiance layer it fills) - the same contract
// pbr_direct_diffuse follows. Deliberately NOT a "lighting.*" family member:
// the family demand resolves to ONE provider, and the scene light state
// belongs to the pbr module - this module only READS it through the
// LightingCapability contract.
const char *const giScreenWorldProvidedCapabilities[] = {"gi_screen_world_diffuse"};
// "lighting" is a requirement (the relight reads the scene's light state
// through the pbr-implemented LightingCapability); its capability.lighting
// grant is DERIVED from the requirement (effectivePermissions) - the
// descriptor declares only environment-scope permissions, of which this
// module has none.
const char *const giScreenWorldRequiredCapabilities[] = {"logging", "renderer", "lighting"};

engine::ModuleInterface *creategiScreenWorldPluginModule()
{
    return new engine::GiScreenWorldModule();
}

const engine::EngineModulePluginEntry giScreenWorldPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "gi_screen_world",
    .createModule            = &creategiScreenWorldPluginModule,
    .providedCapabilities    = giScreenWorldProvidedCapabilities,
    .providedCapabilityCount = std::size(giScreenWorldProvidedCapabilities),
    .requiredCapabilities    = giScreenWorldRequiredCapabilities,
    .requiredCapabilityCount = std::size(giScreenWorldRequiredCapabilities),
    .optionalCapabilities    = nullptr,
    .optionalCapabilityCount = 0,
    .declaredPermissions     = nullptr,
    .declaredPermissionCount = 0
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &giScreenWorldPluginEntry;
}
