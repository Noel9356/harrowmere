// The character_customisation module implementation: the library store, the
// config knobs and the capability surface. The whole data model + assembly
// engine lives in character_assembly.hpp (std-only) - this translation unit
// only adds the lifecycle, the logging and the knob plumbing.
#include "character_customisation.hpp"

#include <utility>

namespace engine
{
namespace
{

// The module's knobs ([character.customisation]); every key mirrored in
// config.toml and documented in README.md.
struct ModuleKnobs
{
    std::uint32_t maxPartsPerAssembly = 48;
    bool strictParameters = true;
};

} // namespace

// The capability implementation - OUTSIDE the anonymous namespace: the
// hpp's member unique_ptr names engine::CharacterCustomisationModuleImpl,
// and an anonymous-namespace class would be a different, incomplete type.
class CharacterCustomisationModuleImpl final : public CharacterCustomisationCapability
{
public:
    bool installLibrary(character::Library library, std::string &error) override
    {
        std::vector<character::BuildError> defects;
        if (!character::validateLibrary(library, defects))
        {
            error = defects.empty() ? "library validation failed" : defects.front().message;
            return false;
        }
        library_ = std::move(library);
        return true;
    }

    const character::Library &library() const override
    {
        return library_;
    }

    std::optional<character::Assembly> build(const character::AssemblyRecipe &recipe,
                                             std::vector<character::BuildError> &errors) override
    {
        character::BuildOptions options;
        options.strictParameters = knobs_.strictParameters;
        options.maxPartsPerAssembly = knobs_.maxPartsPerAssembly;
        return character::buildAssembly(library_, recipe, options, errors);
    }

    ModuleKnobs knobs_;
    character::Library library_;
};

CharacterCustomisationModule::CharacterCustomisationModule()
{
    componentManifest = ComponentManifest{
        .name    = "character_customisation",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        // The modular-character service's capability (the demand marker the
        // game mod's manifest carries; the kernel's required-capability
        // closure pulls this plugin in).
        .providedCapabilities = {"character_customisation"},
        .requiredCapabilities = {"logging"},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {},
    };
}

CharacterCustomisationModule::~CharacterCustomisationModule() = default;

void CharacterCustomisationModule::construct(ComponentContext &context)
{
    logger = static_cast<LoggerCapability *>(context.acquireCapability("logging"));

    impl = std::make_unique<CharacterCustomisationModuleImpl>();
    impl->knobs_.maxPartsPerAssembly = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(context.configurationIntegerInSection("character", "customisation.max_parts_per_assembly").value_or(48), 4, 256));
    impl->knobs_.strictParameters
        = context.configurationBooleanInSection("character", "customisation.strict_parameters").value_or(true);

    if (logger != nullptr)
    {
        logger->logMessage(LogLevel::Information,
            "character_customisation module constructed: max_parts_per_assembly=" + std::to_string(impl->knobs_.maxPartsPerAssembly)
                + " strict_parameters=" + (impl->knobs_.strictParameters ? "true" : "false")
                + " (the content library arrives from the game mod at start)");
    }
}

void CharacterCustomisationModule::start()
{
    if (logger != nullptr && impl != nullptr)
    {
        logger->logMessage(LogLevel::Information,
            "character_customisation library live: " + std::to_string(impl->library().parts.size()) + " part assets, "
                + std::to_string(impl->library().rules.size()) + " compatibility rules, "
                + std::to_string(impl->library().profiles.size()) + " assembly profiles, "
                + std::to_string(impl->library().parameters.size()) + " parameters");
    }
}

void CharacterCustomisationModule::stop()
{
    impl.reset();
}

void *CharacterCustomisationModule::capabilityPointerFor(const std::string &capabilityName)
{
    if (capabilityName == CharacterCustomisationCapability::capabilityName && impl != nullptr)
    {
        return static_cast<CharacterCustomisationCapability *>(impl.get());
    }
    return nullptr;
}

} // namespace engine
