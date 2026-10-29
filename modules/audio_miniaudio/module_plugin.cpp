// The module plugin seam of 'audio_miniaudio': one exported entry point that hands the
// kernel this binary's static descriptor (engine/kernel/module_api.hpp).
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "audio.hpp"

namespace
{
const char *const audioProvidedCapabilities[]
    = {"audio.miniaudio",
        // The Lua bridge extension member: declares this module as a provider of
        // the "scripting.lua_bridge" family. The lua_scripting host acquires
        // every active member through ComponentContext::acquireEachCapability
        // and asks each one to install its own bindings into the host's engine
        // table. Adding this capability here is the ONLY change the audio module
        // makes to opt into the Lua bridge.
        "scripting.lua_bridge.audio"};
const char *const audioRequiredCapabilities[] = {"logging"};

engine::ModuleInterface *createaudioPluginModule()
{
    return new engine::AudioModule();
}

// The module's environment-scope permission requests (everything outside
// the capability.* namespace), declared statically so the kernel can
// risk-assess this module before its binary is instantiated (see
// engine/kernel/module_api.hpp, declaredPermissions).
const char *const audio_miniaudioDeclaredPermissions[] = {"filesystem.read"};

const engine::EngineModulePluginEntry audioPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "audio",
    .createModule            = &createaudioPluginModule,
    .providedCapabilities    = audioProvidedCapabilities,
    .providedCapabilityCount = std::size(audioProvidedCapabilities),
    .requiredCapabilities    = audioRequiredCapabilities,
    .requiredCapabilityCount = std::size(audioRequiredCapabilities),
    .optionalCapabilities    = nullptr,
    .optionalCapabilityCount = 0,
    .declaredPermissions     = audio_miniaudioDeclaredPermissions,
    .declaredPermissionCount = std::size(audio_miniaudioDeclaredPermissions)
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &audioPluginEntry;
}
