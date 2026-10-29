// The module plugin seam of 'lua_scripting': one exported entry point that
// hands the kernel this binary's static descriptor (engine/kernel/module_api.hpp).
// The descriptor carries the module's name, its factory and its static
// capability declaration; the kernel reads all three without creating an
// instance, and only instantiates the module when the demand closure requires
// one of its capabilities - for this module, that happens exactly when a
// script mod requires the scripting capability it provides.
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "lua_scripting.hpp"

namespace
{
const char *const lua_scriptingProvidedCapabilities[] = {"scripting.lua"};
const char *const lua_scriptingRequiredCapabilities[] = {"logging"};
// The optional capability set is deliberately small and never grows as new
// Lua bridge extensions are added: "scripting.lua_bridge" is the family other
// modules provide members of to install their own Lua bindings without
// touching this module. The resolver orders every active member before this
// host through the optional edge, and acquireEachCapability collects them all
// at construct time. The host never names a capability it does not own, so a
// new module adding its own Lua bridge never appears here. The family edge
// subsumes every per-module optional capability the host used to declare
// (input, scene, lighting, renderer, animation, asset, physics, audio, ui,
// persistence) - each of those modules now declares its own bridge member of
// the family, and the family edge here orders them all before this module
// when active.
//
// "window" is the one concrete optional edge: window_size is the only
// capability-backed call in the host surface, and it degrades to {0, 0} when
// no window module is active. Required, a script host dragged a window into
// every assembly - a headless server, a batch simulation, a CI run could not
// host scripts at all. Optional, the scripted headless run starts exactly
// like a rendered one (the frame-clock work, docs/architecture.md, made the
// script handlers tick without a renderer; this edge completes the story),
// and a run WITH a window keeps the identical surface.
const char *const lua_scriptingOptionalCapabilities[] = {"scripting.lua_bridge", "window"};

engine::ModuleInterface *createlua_scriptingPluginModule()
{
    return new engine::LuaScriptingModule();
}

const engine::EngineModulePluginEntry lua_scriptingPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "lua_scripting",
    .createModule            = &createlua_scriptingPluginModule,
    .providedCapabilities    = lua_scriptingProvidedCapabilities,
    .providedCapabilityCount = std::size(lua_scriptingProvidedCapabilities),
    .requiredCapabilities    = lua_scriptingRequiredCapabilities,
    .requiredCapabilityCount = std::size(lua_scriptingRequiredCapabilities),
    .optionalCapabilities    = lua_scriptingOptionalCapabilities,
    .optionalCapabilityCount = std::size(lua_scriptingOptionalCapabilities),
    .declaredPermissions     = nullptr,
    .declaredPermissionCount = 0
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &lua_scriptingPluginEntry;
}
