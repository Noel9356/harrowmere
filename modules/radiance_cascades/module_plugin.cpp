// The module plugin seam of 'radiance_cascades': one exported entry point that
// hands the kernel this binary's static descriptor (engine/kernel/module_api.hpp).
// The descriptor carries the module's name, its factory and its static
// capability declaration; the kernel reads all three without creating an
// instance, and only instantiates the module when the demand closure requires
// one of its capabilities.
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "radiance_cascades.hpp"

namespace
{
// One demand marker per probe placement: the two names are alternative
// transports for the composite's indirect diffuse radiance layer, and the run
// selects the placement by demanding exactly one of them (see the module
// README, "Placement"). The markers carry no call interface - the
// implementation behind either is the lighting technique pass the module
// attaches to the renderer.
const char *const radiance_cascadesProvidedCapabilities[]
    = {"screen_space_radiance_cascades", "world_space_radiance_cascades"};
const char *const radiance_cascadesRequiredCapabilities[] = {"logging", "renderer"};
// "lighting" is used when a provider is active and the pass degrades to
// emissive + sky transport without one (see the module manifest).
const char *const radiance_cascadesOptionalCapabilities[] = {"lighting"};

engine::ModuleInterface *createRadianceCascadesPluginModule()
{
    return new engine::RadianceCascadesModule();
}

const engine::EngineModulePluginEntry radiance_cascadesPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "radiance_cascades",
    .createModule            = &createRadianceCascadesPluginModule,
    .providedCapabilities    = radiance_cascadesProvidedCapabilities,
    .providedCapabilityCount = std::size(radiance_cascadesProvidedCapabilities),
    .requiredCapabilities    = radiance_cascadesRequiredCapabilities,
    .requiredCapabilityCount = std::size(radiance_cascadesRequiredCapabilities),
    .optionalCapabilities    = radiance_cascadesOptionalCapabilities,
    .optionalCapabilityCount = std::size(radiance_cascadesOptionalCapabilities),
    .declaredPermissions     = nullptr,
    .declaredPermissionCount = 0
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &radiance_cascadesPluginEntry;
}
