// The module plugin seam of 'screenshot_png': one exported entry point that hands
// the kernel this binary's static descriptor (engine/kernel/module_api.hpp).
// The descriptor carries the module's name, its factory and its static
// capability declaration; the kernel reads all three without creating an
// instance, and only instantiates the module when the demand closure requires
// one of its capabilities.
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "screenshot.hpp"

namespace
{
// "screenshot.png" is the implementation-bound member of the "screenshot"
// family: the PNG-encoder capture implementation (JPEG and BMP selectable per
// capture, the PNG codec is the module's own). Consumers demand the family; a
// run that needs THIS implementation names the member exactly.
const char *const screenshotProvidedCapabilities[]
    = {"screenshot.png",
        // The Lua bridge extension member: declares this module as a provider of
        // the "scripting.lua_bridge" family. The lua_scripting host acquires every
        // active member through ComponentContext::acquireEachCapability and asks
        // each one to install its own bindings into the host's engine table. Adding
        // this capability here is the ONLY change the screenshot module makes to
        // opt into the Lua bridge: no lua_scripting source file, manifest, or
        // build target ever references the screenshot module.
        "scripting.lua_bridge.screenshot"};
const char *const screenshotRequiredCapabilities[] = {"logging", "renderer"};

engine::ModuleInterface *createscreenshotPluginModule()
{
    return new engine::ScreenshotModule();
}

// The module's environment-scope permission requests (everything outside
// the capability.* namespace), declared statically so the kernel can
// risk-assess this module before its binary is instantiated (see
// engine/kernel/module_api.hpp, declaredPermissions).
const char *const screenshot_pngDeclaredPermissions[] = {"filesystem.write"};

const engine::EngineModulePluginEntry screenshotPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "screenshot",
    .createModule            = &createscreenshotPluginModule,
    .providedCapabilities    = screenshotProvidedCapabilities,
    .providedCapabilityCount = std::size(screenshotProvidedCapabilities),
    .requiredCapabilities    = screenshotRequiredCapabilities,
    .requiredCapabilityCount = std::size(screenshotRequiredCapabilities),
    .optionalCapabilities    = nullptr,
    .optionalCapabilityCount = 0,
    .declaredPermissions     = screenshot_pngDeclaredPermissions,
    .declaredPermissionCount = std::size(screenshot_pngDeclaredPermissions)
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &screenshotPluginEntry;
}
