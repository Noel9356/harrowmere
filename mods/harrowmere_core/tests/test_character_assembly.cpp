// rev 34 - the character-customisation assembly engine (the owner's
// architecture document, first implementation test): the validation gates,
// the resolve math (attachment chain, parameter bindings, connector sync,
// ground solve), the doc's test operations as recipe edits, and the parts
// library loader round-trip.
#include "bridge/character_lab.hpp"
#include "character_assembly.hpp"

#include <doctest/doctest.h>

#include <cmath>

namespace
{
using namespace engine::character;

Library testLibrary()
{
    Library lib;
    lib.parameters["legs.length"] = {"legs.length", 0.7f, 1.35f, 1.0f};
    lib.parameters["torso.width"] = {"torso.width", 0.7f, 1.4f, 1.0f};
    PartAsset torso;
    torso.id = "torso";
    torso.role = "core.torso";
    torso.poseMeshes = {"mesh_torso", "", ""};
    torso.connectors.push_back({"spine", "spine.mount", {{0, 0, 0}, 0.0f, {}}, 1});
    torso.connectors.push_back({"neck", "neck.mount", {{0, 0.5, 0}, 0.0f, {"torso.width", {0, 0.1f, 0}}}, 1});
    torso.bindings.push_back({"torso.width", {0.5f, 0.0f, 0.25f}});
    torso.restExtentBelow = 0.0f;
    lib.parts["torso"] = torso;
    PartAsset head;
    head.id = "head";
    head.role = "head";
    head.poseMeshes = {"mesh_head", "", ""};
    head.connectors.push_back({"neck", "neck.root", {{0, 0, 0}, 0.0f, {}}, 1});
    lib.parts["head"] = head;
    lib.rules.push_back({"spine.mount", "spine.root", true, 3.14159265f, 0.0f});
    lib.rules.push_back({"neck.mount", "neck.root", true, 3.14159265f, 1.3f});
    lib.profiles["humanoid"] = {"humanoid", {{"core.torso", 1, 1}, {"head", 0, 1}}};
    return lib;
}

AssemblyRecipe twoPartRecipe()
{
    AssemblyRecipe r;
    r.profileId = "humanoid";
    r.parts.push_back({"root", "torso", {}, false});
    r.parts.push_back({"skull", "head", {}, false});
    r.connections.push_back({"root", "neck", "skull", "neck", {0, 0, 0}, 0.0f});
    return r;
}
} // namespace

TEST_CASE("library validation catches broken content")
{
    Library lib = testLibrary();
    std::vector<BuildError> errors;
    CHECK(validateLibrary(lib, errors));
    lib.parts["head"].poseMeshes[0].clear();
    CHECK_FALSE(validateLibrary(lib, errors));
}

TEST_CASE("build validates the doc's mandatory checks")
{
    Library lib = testLibrary();
    BuildOptions opt;
    std::vector<BuildError> errors;
    CHECK(buildAssembly(lib, twoPartRecipe(), opt, errors).has_value());
    // duplicate instance id
    auto r = twoPartRecipe();
    r.parts[1].id = "root";
    CHECK_FALSE(buildAssembly(lib, r, opt, errors).has_value());
    // second structural parent
    r = twoPartRecipe();
    r.parts.push_back({"skull2", "head", {}, false});
    r.connections.push_back({"root", "spine", "skull2", "neck", {0, 0, 0}, 0.0f});
    r.connections.push_back({"skull", "neck", "skull2", "neck", {0, 0, 0}, 0.0f});
    CHECK_FALSE(buildAssembly(lib, r, opt, errors).has_value());
    // unknown connector
    r = twoPartRecipe();
    r.connections[0].parentConnectorId = "missing";
    CHECK_FALSE(buildAssembly(lib, r, opt, errors).has_value());
    // missing rule
    r = twoPartRecipe();
    lib.rules.clear();
    CHECK_FALSE(buildAssembly(lib, r, opt, errors).has_value());
    // capacity: neck.root max 1 -> two heads on one neck.mount reject
    lib = testLibrary();
    r = twoPartRecipe();
    r.parts.push_back({"skull2", "head", {}, false});
    r.connections.push_back({"root", "neck", "skull2", "neck", {0, 0, 0}, 0.0f});
    CHECK_FALSE(buildAssembly(lib, r, opt, errors).has_value());
    // out-of-range parameter: strict rejects, relaxed clamps
    r = twoPartRecipe();
    r.parameters["torso.width"] = 9.0f;
    CHECK_FALSE(buildAssembly(lib, r, opt, errors).has_value());
    opt.strictParameters = false;
    const auto clamped = buildAssembly(lib, r, opt, errors);
    REQUIRE(clamped.has_value());
    CHECK(clamped->parameters.at("torso.width") == doctest::Approx(1.4f));
}

TEST_CASE("resolve: attachment chain, connector sync, ground solve")
{
    Library lib = testLibrary();
    lib.parts["torso"].poseMeshes = {"m", "", ""};
    lib.parts["head"].poseMeshes = {"h", "", ""};
    lib.parts["head"].restExtentBelow = 0.1f;
    BuildOptions opt;
    std::vector<BuildError> errors;
    // The recipe's root sits at the origin; the head's connector rides the
    // neck.mount (0, 0.5, 0) adjusted by torso.width's +0.1/unit offset.
    auto r = twoPartRecipe();
    r.parameters["torso.width"] = 1.2f; // in range (the strict gate rejects 1.5 below)
    const auto assembly = buildAssembly(lib, r, opt, errors);
    REQUIRE(assembly.has_value());
    ResolveInput in;
    in.groundPosition = {0.0f, 4.0f, 0.0f};
    const auto parts = resolveAssembly(*assembly, in);
    REQUIRE(parts.size() == 2);
    // torso width binding 0.5/0.25: scale x = 1.10, z = 1.05
    CHECK(parts[0].scale[0] == doctest::Approx(1.10f));
    CHECK(parts[0].scale[2] == doctest::Approx(1.05f));
    // connector sync: neck anchor y = 0.5 + 0.1 * 0.2 = 0.52
    CHECK(parts[1].position[1] - parts[0].position[1] == doctest::Approx(0.52f).epsilon(1e-4));
    // ground solve: nothing hangs below the torso base (the root, extent 0),
    // so the root STANDS on the ground and the head's foot rides 0.42 above
    // it - the lift zeroes when no part dangles below the root.
    CHECK((parts[1].position[1] - 0.1f) == doctest::Approx(4.42f).epsilon(1e-4));
    // determinism: the same inputs resolve bit-identically
    const auto again = resolveAssembly(*assembly, in);
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        CHECK(parts[i].position == again[i].position);
        CHECK(parts[i].yawRadians == again[i].yawRadians);
    }
}

TEST_CASE("the doc's test operations via recipe edits")
{
    Library lib = testLibrary();
    lib.parts["torso"].poseMeshes = {"m", "", ""};
    lib.parts["head"].poseMeshes = {"h", "", ""};
    BuildOptions opt;
    std::vector<BuildError> errors;
    // REPLACE the head: another head asset on the same connector resolves.
    lib.parts["head2"] = lib.parts["head"];
    lib.parts["head2"].id = "head2";
    lib.parts["head2"].poseMeshes = {"h2", "", ""};
    auto r = twoPartRecipe();
    r.parts[1].assetId = "head2";
    const auto swapped = buildAssembly(lib, r, opt, errors);
    REQUIRE(swapped.has_value());
    ResolveInput in;
    const auto parts = resolveAssembly(*swapped, in);
    CHECK(parts[1].meshName == "h2");
    // REMOVE a part: the head connection dropped - the profile allows 0 heads.
    r = twoPartRecipe();
    r.parts.pop_back();
    r.connections.pop_back();
    CHECK(buildAssembly(lib, r, opt, errors).has_value());
    // SCALE ceiling: the neck rule caps the head at 1.3 - a giant head rejects.
    lib.parameters["head.size"] = {"head.size", 0.8f, 2.0f, 1.0f};
    lib.parts["head"].bindings.push_back({"head.size", {0.5f, 0.5f, 0.5f}});
    r = twoPartRecipe();
    r.parameters["head.size"] = 2.0f; // scale 1.5 > 1.3 ceiling (out-of-range + ceiling)
    CHECK_FALSE(buildAssembly(lib, r, opt, errors).has_value());
}

TEST_CASE("parts.json loads through the game's data loader")
{
    // The shipped library must parse and install (the plugin's start path).
    std::string error;
    static engine::CharacterCustomisationCapability *probe = nullptr;
    // A recording capability: installLibrary is what the loader calls.
    struct RecordingCapability final : engine::CharacterCustomisationCapability
    {
        bool installLibrary(Library library, std::string &) override
        {
            installed = std::move(library);
            ++calls;
            return true;
        }
        const Library &library() const override
        {
            return installed;
        }
        std::optional<Assembly> build(const AssemblyRecipe &, std::vector<BuildError> &) override
        {
            return std::nullopt;
        }
        Library installed;
        int calls = 0;
    } cap;
    probe = &cap;
    // The repo layout: tests run from the mod root's build dir - resolve the
    // data path relative to __FILE__ so the check is location-independent.
    const std::filesystem::path dataDir
        = std::filesystem::path(__FILE__).parent_path().parent_path() / "data";
    CHECK(harrowmere::bridge::loadCharacterLibrary(dataDir, probe, error));
    CHECK(cap.installed.parts.size() >= 10);
    CHECK(cap.installed.profiles.count("hybrid_chimera") == 1);
    AssemblyRecipe hybrid;
    CHECK(harrowmere::bridge::loadCharacterRecipe(dataDir, "hybrid_chimera", hybrid, error));
    CHECK(hybrid.parts.size() == 10);
    CHECK(hybrid.connections.size() == 9);
    AssemblyRecipe variant;
    CHECK(harrowmere::bridge::loadCharacterRecipe(dataDir, "hybrid_variant", variant, error));
    CHECK(variant.parameters.at("torso.width") == doctest::Approx(1.35f));
    // The variant IS the doc's test ops: avian head, no wings, two tails.
    std::size_t wings = 0;
    std::size_t tails = 0;
    std::string headAsset;
    for (const auto &p : variant.parts)
    {
        const auto &asset = cap.installed.parts[p.assetId];
        if (asset.role == "wing") ++wings;
        if (asset.role == "tail") ++tails;
        if (asset.role == "head") headAsset = p.assetId;
    }
    CHECK(wings == 0);
    CHECK(tails == 2);
    CHECK(headAsset == "chimera_head_avian");
    // And both recipes BUILD under the real assembly engine.
    BuildOptions opt;
    std::vector<BuildError> errors;
    CHECK(buildAssembly(cap.installed, hybrid, opt, errors).has_value());
    CHECK(buildAssembly(cap.installed, variant, opt, errors).has_value());
}
