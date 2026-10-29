// The module plugin seam of 'logging_stdout': one exported entry point that hands the
// kernel this binary's static descriptor (engine/kernel/module_api.hpp). The
// descriptor carries the module's name, its factory and its static capability
// declaration; the kernel reads all three without creating an instance, and
// only instantiates the module when the demand closure requires one of its
// capabilities.
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "logging.hpp"

namespace
{
// The implementation-bound member of the "logging" family: the console
// (stdout) sink implementation. Consumers demand the family ("logging"); a
// run that needs THIS sink names the member exactly.
const char *const loggingProvidedCapabilities[] = {"logging.stdout"};

engine::ModuleInterface *createloggingPluginModule()
{
    return new engine::LoggingModule();
}

const engine::EngineModulePluginEntry loggingPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "logging",
    .createModule            = &createloggingPluginModule,
    .providedCapabilities    = loggingProvidedCapabilities,
    .providedCapabilityCount = std::size(loggingProvidedCapabilities),
    .requiredCapabilities    = nullptr,
    .requiredCapabilityCount = 0,
    .optionalCapabilities    = nullptr,
    .optionalCapabilityCount = 0,
    .declaredPermissions     = nullptr,
    .declaredPermissionCount = 0
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &loggingPluginEntry;
}
