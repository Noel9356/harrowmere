#ifndef HARROWMERE_CORE_BRIDGE_CHARACTER_LAB_HPP
#define HARROWMERE_CORE_BRIDGE_CHARACTER_LAB_HPP

// rev 34 - the character-customisation integration (the owner's architecture
// document, first implementation test): the parts-library loader (JSON ->
// engine::character structs via the vendored nlohmann, the game's data
// mandate) and the character_lab scenario's two proof assemblies.
//
// The lab stages the doc's hybrid character (humanoid torso + pelvis core,
// canine head, digitigrade legs, humanoid arms, avian wings, reptilian-
// dyed tail) beside its VARIANT (the test operations applied through data:
// the avian head swapped in, both wings removed, the torso widened, the
// legs lengthened, a second tail added) - one scenario, two recipes, so
// the assembly system's replace/remove/proportion/second-appendage ops are
// visible in a single frame.
//
// Deviations from the doc are pinned in the module README (skinning,
// morph targets, retargeting, physics: the engine has no skeletal pipeline;
// mesh instances carry translate + rotateY + scale only).

#include <filesystem>
#include <memory>
#include <string>

#include "bridge/scene_bridge.hpp"
#include "character_capability.hpp"

namespace harrowmere
{
namespace bridge
{

// Parses data/characters/parts.json into a library and installs it on the
// capability. Returns false with a precise error on malformed content.
bool loadCharacterLibrary(const std::filesystem::path &dataDirectory,
    engine::CharacterCustomisationCapability *capability, std::string &error);

// Parses one recipe file (data/characters/recipes/<name>.json).
bool loadCharacterRecipe(const std::filesystem::path &dataDirectory, const std::string &name,
    engine::character::AssemblyRecipe &recipe, std::string &error);

class SceneBridge;

// The lab's visual state: two built assemblies and their per-part object
// handles. Owned by the SceneBridge (the chamber pattern - the plugin pins
// the site, the bridge drains it on its first frame and updates the
// animated parts every frame after).
class CharacterLab
{
public:
    struct Site
    {
        double x = 0.0;
        double z = 0.0;
        double groundY = 0.0;
    };

    void setSite(const Site &site);
    void setCapability(engine::CharacterCustomisationCapability *capability);
    void setDataDirectory(const std::filesystem::path &dataDirectory)
    {
        dataDirectory_ = dataDirectory;
    }
    // One-shot: builds both assemblies (through the capability, so the
    // module's validation gates apply), resolves at t0 and spawns every
    // part through the sink. Safe to call every frame; spawns once.
    void ensureSpawned(SceneRegistrySink &sink);
    // The animated parts (tails, wings) re-resolve and re-pose per frame.
    void updateFrame(SceneRegistrySink &sink, double worldTimeSeconds);
    // The count of live part objects (the diagnostics' evidence).
    std::size_t liveParts() const
    {
        std::size_t total = 0;
        for (const AssemblyVisual &a : assemblies_)
        {
            total += a.handles.size();
        }
        return total;
    }

private:
    struct AssemblyVisual
    {
        engine::character::Assembly assembly;
        double originX = 0.0;
        double originZ = 0.0;
        float headingYaw = 0.0f;
        std::map<std::string, SceneRegistrySink::Handle> handles;
    };

    bool spawnAssembly(SceneRegistrySink &sink, const engine::character::AssemblyRecipe &recipe, double x, double z,
        float headingYaw);

    Site site_{};
    std::filesystem::path dataDirectory_;
    engine::CharacterCustomisationCapability *capability_ = nullptr;
    bool spawned_ = false;
    std::vector<AssemblyVisual> assemblies_;
    std::vector<std::string> animatedParts_;
};

} // namespace bridge
} // namespace harrowmere

#endif // HARROWMERE_CORE_BRIDGE_CHARACTER_LAB_HPP
