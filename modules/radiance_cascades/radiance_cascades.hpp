#ifndef ENGINE_MODULES_RADIANCE_CASCADES_HPP
#define ENGINE_MODULES_RADIANCE_CASCADES_HPP

#include "logging_capability.hpp"
#include "engine/kernel/component_interface.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace engine
{

class LightingTechniquePass;

class RadianceCascadesModule final : public ModuleInterface
{
public:
    RadianceCascadesModule();
    ~RadianceCascadesModule() override;

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
    bool passAttached        = false;
    bool lightingAvailable   = false;
    // Start-log reporting of the DEMANDED placement (the run selects the
    // placement by demanding one of the module's two placement capabilities -
    // screen_space_radiance_cascades or world_space_radiance_cascades): "screen",
    // "world", or "none" when no placement was demanded and the module idles.
    // The pass keeps the authoritative settings.
    std::string configuredPlacement   = "none";
    std::string configuredMappingName = "octahedron";
    std::uint32_t configuredBounces   = 1;
    std::shared_ptr<LightingTechniquePass> pass;
};

} // namespace engine

#endif // ENGINE_MODULES_RADIANCE_CASCADES_HPP
