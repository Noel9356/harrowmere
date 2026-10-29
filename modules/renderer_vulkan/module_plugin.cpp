// The module plugin seam of 'renderer_vulkan': one exported entry point that hands the
// kernel this binary's static descriptor (engine/kernel/module_api.hpp). The
// descriptor carries the module's name, its factory and its static capability
// declaration; the kernel reads all three without creating an instance, and
// only instantiates the module when the demand closure requires one of its
// capabilities.
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "renderer.hpp"

namespace
{
const char *const rendererProvidedCapabilities[]
    = {"renderer.vulkan",
        "renderer.vulkan.overlay",
        // The Lua bridge extension member: declares this module as a provider of
        // the "scripting.lua_bridge" family. The lua_scripting host acquires
        // every active member through ComponentContext::acquireEachCapability
        // and asks each one to install its own bindings into the host's engine
        // table. Adding this capability here is the ONLY change the renderer
        // module makes to opt into the Lua bridge.
        "scripting.lua_bridge.renderer"};
const char *const rendererRequiredCapabilities[] = {"logging", "window"};
const char *const rendererOptionalCapabilities[] = {"input", "scene"};

engine::ModuleInterface *createrendererPluginModule()
{
    return new engine::RendererModule();
}

const engine::EngineModulePluginEntry rendererPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "renderer",
    .createModule            = &createrendererPluginModule,
    .providedCapabilities    = rendererProvidedCapabilities,
    .providedCapabilityCount = std::size(rendererProvidedCapabilities),
    .requiredCapabilities    = rendererRequiredCapabilities,
    .requiredCapabilityCount = std::size(rendererRequiredCapabilities),
    .optionalCapabilities    = rendererOptionalCapabilities,
    .optionalCapabilityCount = std::size(rendererOptionalCapabilities),
    .declaredPermissions     = nullptr,
    .declaredPermissionCount = 0
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &rendererPluginEntry;
}
