// The character-customisation integration (see character_lab.hpp for the
// contract). The loader speaks the game's JSON data mandate (nlohmann,
// vendored); the lab drives the module's validated builder and the sink's
// scaled mesh-instance path.
#include "bridge/character_lab.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <iostream>

namespace harrowmere
{
namespace bridge
{

namespace
{

constexpr float kPartUvScale = 1.0f;

std::array<float, 3> vec3(const nlohmann::json &j, const char *field, std::array<float, 3> fallback = {0.0f, 0.0f, 0.0f})
{
    if (!j.contains(field) || !j[field].is_array() || j[field].size() < 3)
    {
        return fallback;
    }
    return {j[field][0].get<float>(), j[field][1].get<float>(), j[field][2].get<float>()};
}

bool loadLibraryBody(const nlohmann::json &root, engine::character::Library &library, std::string &error)
{
    if (!root.contains("parameters") || !root.contains("parts"))
    {
        error = "parts.json misses 'parameters' or 'parts'";
        return false;
    }
    for (const auto &p : root["parameters"])
    {
        engine::character::ParameterDef def;
        def.name = p.at("name").get<std::string>();
        def.minValue = p.value("min", 0.0f);
        def.maxValue = p.value("max", 1.0f);
        def.defaultValue = p.value("default", 1.0f);
        library.parameters[def.name] = def;
    }
    for (const auto &r : root.value("rules", nlohmann::json::array()))
    {
        engine::character::CompatibilityRule rule;
        rule.parentRole = r.at("parent").get<std::string>();
        rule.childRole = r.at("child").get<std::string>();
        rule.allowed = r.value("allowed", true);
        rule.yawToleranceRadians = r.value("yaw_tolerance", 3.14159265f);
        rule.maxChildScale = r.value("max_child_scale", 0.0f);
        library.rules.push_back(rule);
    }
    for (const auto &a : root["parts"])
    {
        engine::character::PartAsset asset;
        asset.id = a.at("id").get<std::string>();
        asset.role = a.at("role").get<std::string>();
        asset.poseMeshes[0] = a.at("mesh").get<std::string>();
        asset.defaultAlbedo = vec3(a, "albedo", {0.75f, 0.72f, 0.68f});
        asset.restExtentBelow = a.value("rest_extent_below", 0.0f);
        const std::string anim = a.value("animation", "none");
        asset.animation = anim == "tail_wag"   ? engine::character::PartAnimation::TailWag
            : anim == "wing_idle"              ? engine::character::PartAnimation::WingIdle
            : anim == "gait"                   ? engine::character::PartAnimation::Gait
                                               : engine::character::PartAnimation::None;
        asset.animationAmplitudeRadians = a.value("amplitude", 0.35f);
        asset.animationSpeedHz = a.value("speed", 0.9f);
        asset.seamLevel = a.value("seam", "overlap");
        for (const auto &c : a.value("connectors", nlohmann::json::array()))
        {
            engine::character::ConnectorDef def;
            def.id = c.at("id").get<std::string>();
            def.role = c.at("role").get<std::string>();
            def.anchor.position = vec3(c, "position");
            def.anchor.yawRadians = c.value("yaw", 0.0f);
            def.maxConnections = static_cast<std::uint32_t>(c.value("max", 1u));
            if (c.contains("param"))
            {
                def.anchor.paramOffset.parameter = c["param"].at("name").get<std::string>();
                def.anchor.paramOffset.offsetPerUnit = vec3(c["param"], "offset_per_unit");
            }
            asset.connectors.push_back(def);
        }
        for (const auto &b : a.value("bindings", nlohmann::json::array()))
        {
            engine::character::ParameterBinding binding;
            binding.parameter = b.at("parameter").get<std::string>();
            binding.scaleCoeffs = vec3(b, "coeffs");
            asset.bindings.push_back(binding);
        }
        library.parts[asset.id] = asset;
    }
    for (const auto &p : root.value("profiles", nlohmann::json::array()))
    {
        engine::character::AssemblyProfile profile;
        profile.id = p.at("id").get<std::string>();
        for (const auto &rc : p.value("roles", nlohmann::json::array()))
        {
            profile.roleCounts.push_back({rc.at("role").get<std::string>(),
                static_cast<std::uint32_t>(rc.value("min", 1u)), static_cast<std::uint32_t>(rc.value("max", 1u))});
        }
        library.profiles[profile.id] = profile;
    }
    return true;
}

} // namespace

bool loadCharacterLibrary(const std::filesystem::path &dataDirectory,
    engine::CharacterCustomisationCapability *capability, std::string &error)
{
    if (capability == nullptr)
    {
        error = "no character_customisation capability";
        return false;
    }
    const auto path = dataDirectory / "characters" / "parts.json";
    std::ifstream in(path);
    if (!in)
    {
        error = "cannot open " + path.string();
        return false;
    }
    try
    {
        const nlohmann::json root = nlohmann::json::parse(in);
        engine::character::Library library;
        if (!loadLibraryBody(root, library, error))
        {
            return false;
        }
        return capability->installLibrary(std::move(library), error);
    }
    catch (const std::exception &e)
    {
        error = std::string("parts.json parse: ") + e.what();
        return false;
    }
}

bool loadCharacterRecipe(const std::filesystem::path &dataDirectory, const std::string &name,
    engine::character::AssemblyRecipe &recipe, std::string &error)
{
    const auto path = dataDirectory / "characters" / "recipes" / (name + ".json");
    std::ifstream in(path);
    if (!in)
    {
        error = "cannot open " + path.string();
        return false;
    }
    try
    {
        const nlohmann::json root = nlohmann::json::parse(in);
        recipe.versionRead = static_cast<std::uint32_t>(root.value("version", 1u));
        recipe.profileId = root.at("profile").get<std::string>();
        if (root.contains("parameters"))
        {
            for (auto it = root["parameters"].begin(); it != root["parameters"].end(); ++it)
            {
                recipe.parameters[it.key()] = it.value().get<float>();
            }
        }
        for (const auto &p : root.value("parts", nlohmann::json::array()))
        {
            engine::character::PartInstance inst;
            inst.id = p.at("id").get<std::string>();
            inst.assetId = p.at("asset").get<std::string>();
            if (p.contains("albedo"))
            {
                inst.albedo = vec3(p, "albedo");
                inst.albedoOverridden = true;
            }
            recipe.parts.push_back(inst);
        }
        for (const auto &c : root.value("connections", nlohmann::json::array()))
        {
            engine::character::PartConnection conn;
            conn.parentInstanceId = c.at("parent").get<std::string>();
            conn.parentConnectorId = c.at("parent_connector").get<std::string>();
            conn.childInstanceId = c.at("child").get<std::string>();
            conn.childConnectorId = c.at("child_connector").get<std::string>();
            conn.offset = vec3(c, "offset");
            conn.offsetYawRadians = c.value("offset_yaw", 0.0f);
            recipe.connections.push_back(conn);
        }
        return true;
    }
    catch (const std::exception &e)
    {
        error = std::string("recipe parse: ") + e.what();
        return false;
    }
}

// ---- the lab ---------------------------------------------------------------

void CharacterLab::setSite(const Site &site)
{
    site_ = site;
    spawned_ = false; // a new site re-stages (the capture rig's only path)
}

void CharacterLab::setCapability(engine::CharacterCustomisationCapability *capability)
{
    capability_ = capability;
}

bool CharacterLab::spawnAssembly(SceneRegistrySink &sink, const engine::character::AssemblyRecipe &recipe, double x,
    double z, float headingYaw)
{
    std::vector<engine::character::BuildError> errors;
    std::optional<engine::character::Assembly> assembly = capability_ != nullptr
        ? capability_->build(recipe, errors)
        : std::nullopt;
    if (!assembly.has_value())
    {
        for (const auto &e : errors)
        {
            std::cerr << "[character_lab] build error: " << e.message << "\n";
        }
        return false;
    }
    AssemblyVisual visual;
    visual.assembly = std::move(*assembly);
    visual.originX = x;
    visual.originZ = z;
    visual.headingYaw = headingYaw;

    engine::character::ResolveInput input;
    input.groundPosition = {static_cast<float>(x), static_cast<float>(site_.groundY), static_cast<float>(z)};
    input.headingYawRadians = headingYaw;
    input.timeSeconds = 0.0f;
    for (const engine::character::ResolvedPart &part : engine::character::resolveAssembly(visual.assembly, input))
    {
        const SceneRegistrySink::Handle handle = sink.spawnPropMeshInstanceScaled(part.meshName.c_str(),
            {part.position[0], part.position[1], part.position[2]}, part.yawRadians, part.albedo, "",
            kPartUvScale, part.scale);
        visual.handles[part.instanceId] = handle;
    }
    assemblies_.push_back(std::move(visual));
    return true;
}

void CharacterLab::ensureSpawned(SceneRegistrySink &sink)
{
    if (spawned_ || capability_ == nullptr
        || (site_.x == 0.0 && site_.z == 0.0))
    {
        return;
    }
    // The stone pad: a thin slab the two assemblies stand on (the same
    // ground the GI chamber pours for itself), lifted clear of the terrain.
    const SceneRegistrySink::Handle pad
        = sink.spawnBox({site_.x, site_.groundY + 0.01, site_.z}, 3.4f, 0.10f, {0.42f, 0.40f, 0.37f});
    if (pad != SceneRegistrySink::NULL_HANDLE)
    {
        sink.setObjectPose(pad, {site_.x, site_.groundY + 0.06, site_.z}, 0.0f);
    }

    std::string error;
    engine::character::AssemblyRecipe hybrid;
    engine::character::AssemblyRecipe variant;
    const bool loaded = loadCharacterRecipe(dataDirectory_, "hybrid_chimera", hybrid, error)
        && loadCharacterRecipe(dataDirectory_, "hybrid_variant", variant, error);
    if (!loaded)
    {
        spawned_ = true; // one precise attempt; a broken recipe must not spam
        std::cerr << "[character_lab] recipe load failed: " << error << "\n";
        return;
    }

    // The hybrid faces the camera side (-X, the app's staged eye); the
    // variant mirrors it - the pair reads as a before/after of the ops.
    spawnAssembly(sink, hybrid, site_.x - 1.15, site_.z, 1.57079637f);
    spawnAssembly(sink, variant, site_.x + 1.15, site_.z, 1.57079637f);
    spawned_ = true;
}

void CharacterLab::updateFrame(SceneRegistrySink &sink, double worldTimeSeconds)
{
    if (assemblies_.empty())
    {
        return;
    }
    for (AssemblyVisual &visual : assemblies_)
    {
        engine::character::ResolveInput input;
        input.groundPosition = {static_cast<float>(visual.originX), static_cast<float>(site_.groundY),
            static_cast<float>(visual.originZ)};
        input.headingYawRadians = visual.headingYaw;
        input.timeSeconds = static_cast<float>(worldTimeSeconds);
        for (const engine::character::ResolvedPart &part : engine::character::resolveAssembly(visual.assembly, input))
        {
            const auto handle = visual.handles.find(part.instanceId);
            if (handle == visual.handles.end() || handle->second == SceneRegistrySink::NULL_HANDLE)
            {
                continue;
            }
            // The static parts' poses never change; the animated ones ride
            // the per-frame yaw. Re-posing all is one call per part - the
            // registry coalesces.
            sink.setObjectPose(handle->second,
                {part.position[0], part.position[1], part.position[2]}, part.yawRadians);
        }
    }
}

} // namespace bridge
} // namespace harrowmere
