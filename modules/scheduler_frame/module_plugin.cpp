// The module plugin seam of 'scheduler_frame': one exported entry point that
// hands the kernel this binary's static descriptor (engine/kernel/module_api.hpp).
// The descriptor carries the module's name, its factory and its static
// capability declaration; the kernel reads all three without creating an
// instance, and only instantiates the module when the demand closure requires
// one of its capabilities. The descriptor name must equal the name the
// module's manifest reports - the kernel verifies the agreement at startup
// and refuses a drifting plugin with a precise diagnostic.
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "scheduler.hpp"

namespace
{
const char *const schedulerProvidedCapabilities[] = {"scheduler.frame"};
const char *const schedulerRequiredCapabilities[] = {"logging"};

engine::ModuleInterface *createschedulerPluginModule()
{
    return new engine::SchedulerModule();
}

const engine::EngineModulePluginEntry schedulerPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "scheduler",
    .createModule            = &createschedulerPluginModule,
    .providedCapabilities    = schedulerProvidedCapabilities,
    .providedCapabilityCount = std::size(schedulerProvidedCapabilities),
    .requiredCapabilities    = schedulerRequiredCapabilities,
    .requiredCapabilityCount = std::size(schedulerRequiredCapabilities),
    .optionalCapabilities    = nullptr,
    .optionalCapabilityCount = 0,
    .declaredPermissions     = nullptr,
    .declaredPermissionCount = 0
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &schedulerPluginEntry;
}
