#ifndef ENGINE_MODULES_CHARACTER_CUSTOMISATION_CHARACTER_CUSTOMISATION_HPP
#define ENGINE_MODULES_CHARACTER_CUSTOMISATION_CHARACTER_CUSTOMISATION_HPP

#include "character_capability.hpp"
#include "logging_capability.hpp"
#include "engine/kernel/component_interface.hpp"

#include <memory>
#include <string>

namespace engine
{

// Internal state (defined in character_customisation.cpp).
class CharacterCustomisationModuleImpl;

// Module "character_customisation": a CONTENT-SERVICE module - the parts
// library, the compatibility-rule registry, the validated assembly builder
// and the resolve math live in character_assembly.hpp (std-only, unit-test-
// compiled); this class wraps them with the module lifecycle, the config
// knobs ([character.customisation]) and the published capability. The mod
// installs its parsed content library at start() and builds assemblies on
// demand; resolve happens at the call site (resolveAssembly is free).
class CharacterCustomisationModule final : public ModuleInterface
{
public:
    CharacterCustomisationModule();
    ~CharacterCustomisationModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

private:
    ComponentManifest componentManifest;
    LoggerCapability *logger = nullptr;
    std::unique_ptr<CharacterCustomisationModuleImpl> impl;
};

} // namespace engine

#endif // ENGINE_MODULES_CHARACTER_CUSTOMISATION_CHARACTER_CUSTOMISATION_HPP
