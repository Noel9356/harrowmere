// The module plugin seam of 'character_customisation': one exported entry
// point that hands the kernel this binary's static descriptor
// (engine/kernel/module_api.hpp) - the same shape every module ships.
#include "engine/kernel/module_api.hpp"

#include <iterator>

#include "character_customisation.hpp"

namespace
{
// "character_customisation" is the capability the game mod demands in its
// manifest (mod.toml capabilities list): the demand pulls this plugin into
// the assembly, the mod acquires the pointer and installs its parsed parts
// library at start.
const char *const characterProvidedCapabilities[] = {"character_customisation"};
const char *const characterRequiredCapabilities[] = {"logging"};

engine::ModuleInterface *createCharacterCustomisationPluginModule()
{
    return new engine::CharacterCustomisationModule();
}

const engine::EngineModulePluginEntry characterCustomisationPluginEntry{
    .abiVersion              = engine::engineModulePluginAbiVersion,
    .moduleName              = "character_customisation",
    .createModule            = &createCharacterCustomisationPluginModule,
    .providedCapabilities    = characterProvidedCapabilities,
    .providedCapabilityCount = std::size(characterProvidedCapabilities),
    .requiredCapabilities    = characterRequiredCapabilities,
    .requiredCapabilityCount = std::size(characterRequiredCapabilities),
    .optionalCapabilities    = nullptr,
    .optionalCapabilityCount = 0,
    .declaredPermissions     = nullptr,
    .declaredPermissionCount = 0
};
} // namespace

ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin()
{
    return &characterCustomisationPluginEntry;
}
