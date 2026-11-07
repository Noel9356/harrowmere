#ifndef ENGINE_MODULES_CHARACTER_CUSTOMISATION_CHARACTER_CAPABILITY_HPP
#define ENGINE_MODULES_CHARACTER_CUSTOMISATION_CHARACTER_CAPABILITY_HPP

// The published capability surface of the character_customisation module.
// Every consumer - other modules, native mods, the app - includes THIS
// header, from the module that provides the capability. Like every
// published capability header this stays implementation-, platform- and
// vendor-agnostic: the data model (engine::character::*) lives in
// character_assembly.hpp beside this header and is std-only.

#include <optional>
#include <string>
#include <vector>

#include "character_assembly.hpp"

namespace engine
{

// Capability "character_customisation": the parts library + validated
// assembly builder. The content owner (the game mod) parses its own data
// files into engine::character::Library / AssemblyRecipe structs and drives
// this surface; the module owns the validation knobs (the config's part
// ceiling and parameter strictness) and the library's one live copy.
//
// The render contract is documented in character_assembly.hpp: assemblies
// resolve into per-part mesh instances (registry mesh name, world position,
// yaw, axis scale, albedo) the consumer spawns through the scene registry -
// mesh instances carry translate + rotateY + scale only, so the doc's
// skeletal layers are documented deviations (module README), not silent
// omissions.
class CharacterCustomisationCapability
{
public:
    static constexpr const char *capabilityName = "character_customisation";

    virtual ~CharacterCustomisationCapability() = default;

    // Installs (replaces) the parts library. The library is validated first
    // (validateLibrary); an invalid library is refused with one precise
    // message per defect and the PREVIOUS library stays live.
    virtual bool installLibrary(character::Library library, std::string &error) = 0;

    // The live library (read-only view for content tooling).
    virtual const character::Library &library() const = 0;

    // Builds + validates one assembly under the module's configured knobs
    // (the part ceiling, the parameter strictness). Errors carry one precise
    // message per validation failure.
    virtual std::optional<character::Assembly> build(const character::AssemblyRecipe &recipe,
                                                     std::vector<character::BuildError> &errors) = 0;
};

} // namespace engine

#endif // ENGINE_MODULES_CHARACTER_CUSTOMISATION_CHARACTER_CAPABILITY_HPP
