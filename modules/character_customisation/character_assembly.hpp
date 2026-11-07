#ifndef ENGINE_MODULES_CHARACTER_CUSTOMISATION_CHARACTER_ASSEMBLY_HPP
#define ENGINE_MODULES_CHARACTER_CUSTOMISATION_CHARACTER_ASSEMBLY_HPP

// The character-customisation data model and assembly engine (rev 34), the
// owner's architecture document mapped onto this engine's contract:
//
//   attachment tree  - rooted, acyclic, exactly one structural parent per
//                      non-root instance; transforms compose down the tree.
//   connectors       - data-defined anchors with semantic roles; compatibili-
//                      ty comes from a rule REGISTRY, never species
//                      conditionals (parent role x child role -> allowed,
//                      orientation tolerance, scale ceiling, capacity).
//   rig fragments    - pose-variant SETS per part asset (rest/strideA/
//                      strideB) - this engine's honest analogue of rig
//                      fragments while mesh instances carry translate +
//                      rotateY + scale only (no skeletal pipeline): a gait
//                      part swaps whole meshes, an appendage part wags.
//   parameters       - a global registry with ranges; each asset implements
//                      its OWN bindings (axis-scale coefficients, and
//                      parameter-dependent connector anchor offsets so
//                      connectors stay synchronized with deformation).
//   geometry junctions - overlapping components with hidden junctions (the
//                      default), seam metadata recorded per asset;
//                      transition meshes and topology merging are future
//                      work (the parts library carries the seam field).
//   recipes          - versioned, declarative, serialization-agnostic (the
//                      content owner parses/serializes; this layer speaks
//                      structs only).
//
// Deliberately NOT here: skeletal skinning, morph targets, retargeting,
// physics articulation, a Lua surface (each documented as a deviation in
// the module README - the engine has no skeletal pipeline; mesh instances
// are the assembly's render contract). Header-only and std-only so the
// mod's unit battery compiles it directly; the plugin wraps it unchanged.
//
// Lifetime contract: a built Assembly borrows the recipe's connections and
// the library's assets/connectors (non-owning pointers). The owner keeps
// both alive for the assembly's whole life.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine
{
namespace character
{

// ---- the parameter registry ------------------------------------------------

struct ParameterDef
{
    std::string name;   // "body.height", "torso.width", "legs.length", ...
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float defaultValue = 1.0f;
};

// One asset's own mapping for one parameter: partScale[i] multiplies
// (1 + scaleCoeffs[i] * (value - 1)). Coefficients are the asset author's
// decision - "body.height" may scale a torso's Y strongly and its XZ
// barely, while a head binds only its Y.
struct ParameterBinding
{
    std::string parameter;
    std::array<float, 3> scaleCoeffs = {0.0f, 0.0f, 0.0f};
};

// A connector anchor's parameter-dependent offset (the doc's requirement
// that connector positions stay synchronized with deformation): the anchor
// slides by offsetPerUnit * (value - 1) meters per axis, in PART space.
struct AnchorParamOffset
{
    std::string parameter;   // empty = none
    std::array<float, 3> offsetPerUnit = {0.0f, 0.0f, 0.0f};
};

// ---- connectors and compatibility ------------------------------------------

// Where a part attaches/detaches, in PART space (meters, Y up, -Z forward).
// yawRadians is the socket's facing inside the part (0 = the part's -Z).
struct ConnectorAnchor
{
    std::array<float, 3> position = {0.0f, 0.0f, 0.0f};
    float yawRadians = 0.0f;
    AnchorParamOffset paramOffset;
};

struct ConnectorDef
{
    std::string id;      // unique within the owning part asset
    std::string role;    // semantic id, e.g. "spine.mount", "head.root"
    ConnectorAnchor anchor;
    std::uint32_t maxConnections = 1;
};

// The compatibility registry: data-defined, extensible (the doc's rule
// registry). A connection (parent connector role -> child connector role)
// is legal only when a rule names the pair, allows it, and the child's
// parameter-driven scale and the relative socket yaw stay inside the rule.
struct CompatibilityRule
{
    std::string parentRole;
    std::string childRole;
    bool allowed = false;
    float yawToleranceRadians = 3.14159265f; // free orientation by default
    float maxChildScale = 0.0f;              // 0 = no ceiling
};

// ---- part assets ------------------------------------------------------------

enum class PartAnimation : std::uint8_t
{
    None = 0,     // static geometry
    TailWag = 1,  // yaw oscillation around the part origin (its connector)
    WingIdle = 2, // TailWag's gentler sibling (the folded-wing sway)
    Gait = 3,     // whole-mesh pose swap on the gait bucket (rest/A/B)
};

// The doc's BodyPartAsset, mapped onto the render contract: geometry is a
// named registry mesh (or three - the pose variants ARE the rig fragment),
// material slots are flat dyes (albedo, one slot per part in v1), the seam
// field records the junction strategy the asset was authored for.
struct PartAsset
{
    std::string id;
    std::string role;   // anatomical role, e.g. "core.torso", "head", "leg"
    // Pose variants (the rig fragment): [0] rest, [1] stride A, [2] stride B.
    // Gait parts must fill all three; other parts use [0] only.
    std::array<std::string, 3> poseMeshes;
    std::vector<ConnectorDef> connectors;
    std::vector<ParameterBinding> bindings;
    std::array<float, 3> defaultAlbedo = {0.75f, 0.72f, 0.68f};
    // How far the part's geometry reaches below its origin at rest (meters),
    // authored into the mesh. The ground solver uses the deepest chain.
    float restExtentBelow = 0.0f;
    PartAnimation animation = PartAnimation::None;
    // Animation feel (TailWag/WingIdle): yaw amplitude (radians) and rate.
    float animationAmplitudeRadians = 0.35f;
    float animationSpeedHz = 0.9f;
    // Geometry junction strategy: "overlap" (hidden junctions, the default),
    // "transition" (authored transition geometry - future), "merge"
    // (topology-merged - future). Validation records, resolve ignores.
    std::string seamLevel = "overlap";
};

// ---- assembly profiles ------------------------------------------------------

// The validated combination contract: a profile names the anatomical roles
// an assembly of this topology must carry, with min/max counts ("one core
// torso, one head, two legs, zero to two wings, zero to two tails").
struct ProfileRoleCount
{
    std::string role;
    std::uint32_t minimum = 1;
    std::uint32_t maximum = 1;
};

struct AssemblyProfile
{
    std::string id;
    std::vector<ProfileRoleCount> roleCounts;
};

// ---- recipes ----------------------------------------------------------------

struct PartInstance
{
    std::string id;       // unique within the assembly
    std::string assetId;
    // Per-slot dye override; the flag gates it (the default is the asset's).
    std::array<float, 3> albedo = {0.0f, 0.0f, 0.0f};
    bool albedoOverridden = false;
};

struct PartConnection
{
    std::string parentInstanceId;
    std::string parentConnectorId;
    std::string childInstanceId;
    std::string childConnectorId;
    // The explicit correction transform (the doc's BodyPartConnection
    // offset): applied in the parent socket's frame.
    std::array<float, 3> offset = {0.0f, 0.0f, 0.0f};
    float offsetYawRadians = 0.0f;
};

struct AssemblyRecipe
{
    static inline constexpr std::uint32_t version = 1u;
    std::uint32_t versionRead = version;
    std::string profileId;
    std::map<std::string, float> parameters; // absent -> the registry default
    std::vector<PartInstance> parts;
    std::vector<PartConnection> connections;
};

// ---- the library ------------------------------------------------------------

struct Library
{
    std::map<std::string, ParameterDef> parameters;
    std::vector<CompatibilityRule> rules;
    std::map<std::string, PartAsset> parts;
    std::map<std::string, AssemblyProfile> profiles;

    const CompatibilityRule *findRule(const std::string &parentRole, const std::string &childRole) const
    {
        for (const CompatibilityRule &r : rules)
        {
            if (r.parentRole == parentRole && r.childRole == childRole)
            {
                return &r;
            }
        }
        return nullptr;
    }
};

// ---- assembly (built + validated) -------------------------------------------

struct BuiltPart
{
    std::string instanceId;
    const PartAsset *asset = nullptr;
    std::array<float, 3> albedo = {0.75f, 0.72f, 0.68f};
    std::array<float, 3> scale = {1.0f, 1.0f, 1.0f};
    std::size_t parentIndex = kNoParent; // index into Assembly::parts
    const PartConnection *viaConnection = nullptr;
    const ConnectorDef *parentConnector = nullptr;
    const ConnectorDef *childConnector = nullptr;
    std::uint32_t animationPhaseIndex = 0; // wag phase offset, per sibling

    static inline constexpr std::size_t kNoParent = static_cast<std::size_t>(-1);
};

struct Assembly
{
    std::string profileId;
    std::map<std::string, float> parameters;
    std::vector<BuiltPart> parts;   // topological order (root first)
};

// ---- build + validation (the doc's mandatory checks) ------------------------

struct BuildError
{
    std::string message; // one precise message per failure
};

struct BuildOptions
{
    // strict = an out-of-range parameter REJECTS the assembly; relaxed clamps.
    bool strictParameters = true;
    // The per-assembly part ceiling (the module config's knob).
    std::uint32_t maxPartsPerAssembly = 48;
};

// Validates the library itself (bindings reference registered parameters,
// gait assets carry all three poses, ids agree). A broken library fails
// every assembly, so it is checked once at load.
bool validateLibrary(const Library &library, std::vector<BuildError> &errors);

// Builds + validates one assembly. The checks, in order: recipe version,
// part ceiling, unique instance ids, known assets, parameter ranges,
// exactly one parent per non-root + acyclicity (the root is the one
// parentless instance), connector ids exist on both ends, rule exists +
// allowed, orientation tolerance, capacity, child scale ceiling, the
// profile's role-count contract.
std::optional<Assembly> buildAssembly(const Library &library, const AssemblyRecipe &recipe,
                                      const BuildOptions &options, std::vector<BuildError> &errors);

// ---- resolve ------------------------------------------------------------------

struct ResolveInput
{
    std::array<float, 3> groundPosition = {0.0f, 0.0f, 0.0f}; // the feet point
    float headingYawRadians = 0.0f;
    float timeSeconds = 0.0f;
    std::uint8_t gaitBucket = 0; // 0 rest, 1 stride A, 2 stride B
};

struct ResolvedPart
{
    std::string instanceId;
    std::string meshName;
    std::array<float, 3> position = {0.0f, 0.0f, 0.0f};
    float yawRadians = 0.0f;
    std::array<float, 3> scale = {1.0f, 1.0f, 1.0f};
    std::array<float, 3> albedo = {0.75f, 0.72f, 0.68f};
};

// ---- implementation ------------------------------------------------------------

namespace detail
{

inline constexpr float kPi = 3.14159265f;

inline float wrapPi(float a)
{
    while (a > kPi)
    {
        a -= 2.0f * kPi;
    }
    while (a < -kPi)
    {
        a += 2.0f * kPi;
    }
    return a;
}

inline std::array<float, 3> paramScale(const PartAsset &asset, const std::map<std::string, float> &params)
{
    std::array<float, 3> scale = {1.0f, 1.0f, 1.0f};
    for (const ParameterBinding &b : asset.bindings)
    {
        const auto found = params.find(b.parameter);
        const float v = found != params.end() ? found->second : 1.0f;
        for (std::size_t i = 0; i < 3; ++i)
        {
            scale[i] *= (1.0f + b.scaleCoeffs[i] * (v - 1.0f));
        }
    }
    for (float &s : scale)
    {
        s = std::clamp(s, 0.1f, 10.0f);
    }
    return scale;
}

inline std::array<float, 3> rotateY(const std::array<float, 3> &p, float yaw)
{
    const float s = std::sin(yaw);
    const float c = std::cos(yaw);
    return {c * p[0] + s * p[2], p[1], -s * p[0] + c * p[2]};
}

// The anchor's (parameter-adjusted) position in its part's space.
inline std::array<float, 3> anchorPosition(const ConnectorAnchor &a, const std::map<std::string, float> &params)
{
    std::array<float, 3> p = a.position;
    if (!a.paramOffset.parameter.empty())
    {
        const auto found = params.find(a.paramOffset.parameter);
        const float v = found != params.end() ? found->second : 1.0f;
        for (std::size_t i = 0; i < 3; ++i)
        {
            p[i] += a.paramOffset.offsetPerUnit[i] * (v - 1.0f);
        }
    }
    return p;
}

// The doc's attachment-transform chain, yaw-only edition (column-vector
// convention, parent-to-child):
//   T_child = T_parent * T_parentSocket * T_offset * (T_childSocket)^-1
// Walks the tree in topological order (root first) and fills world
// positions/yaws. The root sits at rootPos with rootYaw.
inline void walkTransforms(const Assembly &assembly, const std::array<float, 3> &rootPos, float rootYaw,
                           std::vector<std::array<float, 3>> &positions, std::vector<float> &yaws)
{
    positions.assign(assembly.parts.size(), {0.0f, 0.0f, 0.0f});
    yaws.assign(assembly.parts.size(), 0.0f);
    for (std::size_t i = 0; i < assembly.parts.size(); ++i)
    {
        const BuiltPart &p = assembly.parts[i];
        float yaw = 0.0f;
        std::array<float, 3> pos{0.0f, 0.0f, 0.0f};
        if (p.parentIndex == BuiltPart::kNoParent)
        {
            yaw = rootYaw;
            pos = rootPos;
        } else
        {
            const float parentYaw = yaws[p.parentIndex];
            const std::array<float, 3> paPos = anchorPosition(p.parentConnector->anchor, assembly.parameters);
            const std::array<float, 3> caPos = anchorPosition(p.childConnector->anchor, assembly.parameters);
            const float socketYaw = parentYaw + p.parentConnector->anchor.yawRadians;
            const std::array<float, 3> paRot = detail::rotateY(paPos, parentYaw);
            const std::array<float, 3> socket = {positions[p.parentIndex][0] + paRot[0],
                positions[p.parentIndex][1] + paRot[1], positions[p.parentIndex][2] + paRot[2]};
            const float offYaw = socketYaw + p.viaConnection->offsetYawRadians;
            const std::array<float, 3> offPos = detail::rotateY(p.viaConnection->offset, socketYaw);
            const std::array<float, 3> aligned = {socket[0] + offPos[0], socket[1] + offPos[1],
                socket[2] + offPos[2]};
            yaw = offYaw - p.childConnector->anchor.yawRadians;
            const std::array<float, 3> caRot = detail::rotateY(caPos, yaw);
            pos = {aligned[0] - caRot[0], aligned[1] - caRot[1], aligned[2] - caRot[2]};
        }
        // Animated parts: the yaw oscillation rides AROUND the part's own
        // origin (its connector), so the attachment stays put and the
        // appendage swings. The phase splits siblings (two tails wag out of
        // phase - the doc's "animate the tail independently" and "add a
        // second tail" both observable).
        if (p.asset->animation == PartAnimation::TailWag || p.asset->animation == PartAnimation::WingIdle)
        {
            const float amplitude = p.asset->animation == PartAnimation::WingIdle
                ? p.asset->animationAmplitudeRadians * 0.35f
                : p.asset->animationAmplitudeRadians;
            const float phase = static_cast<float>(p.animationPhaseIndex) * 0.9f;
            yaw += amplitude * std::sin(6.28318531f * p.asset->animationSpeedHz * 0 /*time filled by caller*/
                                        + phase);
        }
        positions[i] = pos;
        yaws[i] = yaw;
    }
}

} // namespace detail

inline bool validateLibrary(const Library &library, std::vector<BuildError> &errors)
{
    bool ok = true;
    const auto fail = [&ok, &errors](const std::string &m) {
        errors.push_back({m});
        ok = false;
    };
    for (const auto &[id, asset] : library.parts)
    {
        if (asset.id != id)
        {
            fail("part asset '" + id + "' carries mismatched id '" + asset.id + "'");
        }
        if (asset.poseMeshes[0].empty())
        {
            fail("part asset '" + id + "' carries no rest mesh");
        }
        if (asset.animation == PartAnimation::Gait)
        {
            if (asset.poseMeshes[1].empty() || asset.poseMeshes[2].empty())
            {
                fail("gait part asset '" + id + "' must carry all three pose meshes");
            }
            if (asset.restExtentBelow <= 0.0f)
            {
                fail("gait part asset '" + id + "' must carry a positive restExtentBelow (the ground solver reads it)");
            }
        }
        std::map<std::string, int> connectorSeen;
        for (const ConnectorDef &c : asset.connectors)
        {
            if (c.id.empty())
            {
                fail("part asset '" + id + "' carries a connector with an empty id");
            }
            connectorSeen[c.id] += 1;
            if (!c.anchor.paramOffset.parameter.empty()
                && library.parameters.count(c.anchor.paramOffset.parameter) == 0)
            {
                fail("part asset '" + id + "' connector '" + c.id + "' binds unknown parameter '"
                     + c.anchor.paramOffset.parameter + "'");
            }
        }
        for (const auto &[cid, count] : connectorSeen)
        {
            if (count > 1)
            {
                fail("part asset '" + id + "' declares connector '" + cid + "' twice");
            }
        }
        for (const ParameterBinding &b : asset.bindings)
        {
            if (library.parameters.count(b.parameter) == 0)
            {
                fail("part asset '" + id + "' binds unknown parameter '" + b.parameter + "'");
            }
        }
    }
    for (const CompatibilityRule &r : library.rules)
    {
        if (r.parentRole.empty() || r.childRole.empty())
        {
            fail("compatibility rule with an empty role pair");
        }
    }
    for (const auto &[id, profile] : library.profiles)
    {
        if (profile.id != id)
        {
            fail("assembly profile '" + id + "' carries mismatched id '" + profile.id + "'");
        }
        for (const ProfileRoleCount &rc : profile.roleCounts)
        {
            if (rc.minimum > rc.maximum)
            {
                fail("assembly profile '" + id + "' role '" + rc.role + "' has minimum > maximum");
            }
        }
    }
    return ok;
}

inline std::optional<Assembly> buildAssembly(const Library &library, const AssemblyRecipe &recipe,
                                             const BuildOptions &options, std::vector<BuildError> &errors)
{
    const auto fail = [&errors](const std::string &m) {
        errors.push_back({m});
    };
    if (recipe.versionRead != AssemblyRecipe::version)
    {
        fail("recipe version " + std::to_string(recipe.versionRead) + " unsupported (expected "
             + std::to_string(AssemblyRecipe::version) + ")");
        return std::nullopt;
    }
    if (recipe.parts.size() > options.maxPartsPerAssembly)
    {
        fail("recipe carries " + std::to_string(recipe.parts.size()) + " parts, above the ceiling "
             + std::to_string(options.maxPartsPerAssembly));
        return std::nullopt;
    }

    // Parameters: defaults, then the recipe's values (range-checked).
    Assembly assembly;
    assembly.profileId = recipe.profileId;
    for (const auto &[name, def] : library.parameters)
    {
        assembly.parameters[name] = def.defaultValue;
    }
    for (const auto &[name, value] : recipe.parameters)
    {
        const auto def = library.parameters.find(name);
        if (def == library.parameters.end())
        {
            fail("recipe sets unknown parameter '" + name + "'");
            return std::nullopt;
        }
        if (value < def->second.minValue || value > def->second.maxValue)
        {
            if (options.strictParameters)
            {
                fail("parameter '" + name + "' value " + std::to_string(value) + " outside ["
                     + std::to_string(def->second.minValue) + ", " + std::to_string(def->second.maxValue) + "]");
                return std::nullopt;
            }
            assembly.parameters[name] = std::clamp(value, def->second.minValue, def->second.maxValue);
        } else
        {
            assembly.parameters[name] = value;
        }
    }

    // Instances: unique ids, known assets, per-instance scale from bindings.
    std::map<std::string, std::size_t> indexById;
    for (std::size_t i = 0; i < recipe.parts.size(); ++i)
    {
        const PartInstance &inst = recipe.parts[i];
        if (indexById.count(inst.id) != 0)
        {
            fail("duplicate instance id '" + inst.id + "'");
            return std::nullopt;
        }
        const auto asset = library.parts.find(inst.assetId);
        if (asset == library.parts.end())
        {
            fail("instance '" + inst.id + "' references unknown part asset '" + inst.assetId + "'");
            return std::nullopt;
        }
        BuiltPart part;
        part.instanceId = inst.id;
        part.asset = &asset->second;
        part.albedo = inst.albedoOverridden ? inst.albedo : asset->second.defaultAlbedo;
        part.scale = detail::paramScale(asset->second, assembly.parameters);
        part.animationPhaseIndex = static_cast<std::uint32_t>(assembly.parts.size());
        indexById.emplace(inst.id, assembly.parts.size());
        assembly.parts.push_back(part);
    }

    // Structural parents: exactly one per non-root instance.
    std::map<std::string, std::size_t> parentOf;
    std::map<std::string, std::size_t> connectionIndexByChild;
    for (std::size_t ci = 0; ci < recipe.connections.size(); ++ci)
    {
        const PartConnection &c = recipe.connections[ci];
        const auto parent = indexById.find(c.parentInstanceId);
        const auto child = indexById.find(c.childInstanceId);
        if (parent == indexById.end() || child == indexById.end())
        {
            fail("connection references unknown instance '" + c.parentInstanceId + "'/'" + c.childInstanceId + "'");
            return std::nullopt;
        }
        if (parent == child)
        {
            fail("connection '" + c.childInstanceId + "' attaches to itself");
            return std::nullopt;
        }
        if (parentOf.count(c.childInstanceId) != 0)
        {
            fail("instance '" + c.childInstanceId
                 + "' carries a second structural parent (attachment trees have exactly one)");
            return std::nullopt;
        }
        parentOf[c.childInstanceId] = parent->second;
        connectionIndexByChild[c.childInstanceId] = ci;
    }
    std::string rootId;
    for (const auto &[id, idx] : indexById)
    {
        if (parentOf.count(id) == 0)
        {
            if (!rootId.empty())
            {
                fail("assembly carries a second parentless instance '" + id + "' (root '" + rootId
                     + "' already exists)");
                return std::nullopt;
            }
            rootId = id;
        }
    }
    if (rootId.empty())
    {
        fail("assembly carries no root (every instance has a parent - the tree is cyclic)");
        return std::nullopt;
    }

    // Connector validity + compatibility + orientation + scale + capacity.
    std::map<std::pair<std::string, std::string>, int> capacityUsed;
    for (const auto &[childId, parentIdx] : parentOf)
    {
        const std::size_t ci = connectionIndexByChild[childId];
        const PartConnection &c = recipe.connections[ci];
        const BuiltPart &parent = assembly.parts[parentIdx];
        const auto childIdx = indexById.find(childId)->second;
        const auto pc = std::find_if(parent.asset->connectors.begin(), parent.asset->connectors.end(),
                                     [&](const ConnectorDef &d) { return d.id == c.parentConnectorId; });
        const auto childAsset = assembly.parts[childIdx].asset;
        const auto cc = std::find_if(childAsset->connectors.begin(), childAsset->connectors.end(),
                                     [&](const ConnectorDef &d) { return d.id == c.childConnectorId; });
        if (pc == parent.asset->connectors.end())
        {
            fail("parent '" + parent.instanceId + "' (" + parent.asset->id + ") has no connector '"
                 + c.parentConnectorId + "'");
            return std::nullopt;
        }
        if (cc == childAsset->connectors.end())
        {
            fail("child '" + childId + "' (" + childAsset->id + ") has no connector '" + c.childConnectorId + "'");
            return std::nullopt;
        }
        const CompatibilityRule *rule = library.findRule(pc->role, cc->role);
        if (rule == nullptr)
        {
            fail("no compatibility rule for role pair '" + pc->role + "' -> '" + cc->role + "'");
            return std::nullopt;
        }
        if (!rule->allowed)
        {
            fail("role pair '" + pc->role + "' -> '" + cc->role + "' is disallowed by the registry");
            return std::nullopt;
        }
        const float relativeYaw = detail::wrapPi(c.offsetYawRadians + cc->anchor.yawRadians - pc->anchor.yawRadians);
        if (std::fabs(relativeYaw) > rule->yawToleranceRadians)
        {
            fail("connection '" + childId + "' yaws " + std::to_string(relativeYaw)
                 + " rad past the rule's tolerance " + std::to_string(rule->yawToleranceRadians));
            return std::nullopt;
        }
        const BuiltPart &child = assembly.parts[childIdx];
        const float scaleMagnitude = std::max({child.scale[0], child.scale[1], child.scale[2]});
        if (rule->maxChildScale > 0.0f && scaleMagnitude > rule->maxChildScale)
        {
            fail("child '" + childId + "' scale " + std::to_string(scaleMagnitude)
                 + " exceeds the rule's ceiling " + std::to_string(rule->maxChildScale));
            return std::nullopt;
        }
        const int used = ++capacityUsed[{parent.instanceId, c.parentConnectorId}];
        if (used > static_cast<int>(pc->maxConnections))
        {
            fail("connector '" + c.parentConnectorId + "' on '" + parent.instanceId + "' exceeds its capacity "
                 + std::to_string(pc->maxConnections));
            return std::nullopt;
        }
        BuiltPart &mutableChild = assembly.parts[childIdx];
        mutableChild.parentIndex = parentIdx;
        mutableChild.viaConnection = &recipe.connections[ci];
        mutableChild.parentConnector = &*pc;
        mutableChild.childConnector = &*cc;
    }

    // The profile's role-count contract.
    const auto profile = library.profiles.find(recipe.profileId);
    if (profile == library.profiles.end())
    {
        fail("unknown assembly profile '" + recipe.profileId + "'");
        return std::nullopt;
    }
    std::map<std::string, std::uint32_t> roleCounts;
    for (const BuiltPart &p : assembly.parts)
    {
        roleCounts[p.asset->role] += 1;
    }
    for (const ProfileRoleCount &rc : profile->second.roleCounts)
    {
        const std::uint32_t count = roleCounts[rc.role];
        if (count < rc.minimum || count > rc.maximum)
        {
            fail("profile '" + recipe.profileId + "' wants " + std::to_string(rc.minimum) + ".."
                 + std::to_string(rc.maximum) + " of role '" + rc.role + "', recipe carries "
                 + std::to_string(count));
            return std::nullopt;
        }
    }

    // Topological order (root first, parents before children) - the recipe
    // order is data, the resolve order is the tree. Iterative peel: while a
    // pass places at least one ready part, keep peeling; a full pass with
    // zero placements means a cycle the parent-map could not see (it can
    // not happen with exactly-one-parent enforcement, the check stays as
    // the defense in depth).
    Assembly ordered;
    ordered.profileId = assembly.profileId;
    ordered.parameters = assembly.parameters;
    std::vector<std::size_t> oldToNew(assembly.parts.size(), BuiltPart::kNoParent);
    std::vector<char> placed(assembly.parts.size(), 0);
    const std::size_t total = assembly.parts.size();
    for (std::size_t placedCount = 0; placedCount < total;)
    {
        bool progress = false;
        for (std::size_t i = 0; i < total; ++i)
        {
            if (placed[i])
            {
                continue;
            }
            const BuiltPart &p = assembly.parts[i];
            const bool ready = p.parentIndex == BuiltPart::kNoParent || placed[p.parentIndex] != 0;
            if (!ready)
            {
                continue;
            }
            BuiltPart copy = p;
            copy.parentIndex = p.parentIndex == BuiltPart::kNoParent ? BuiltPart::kNoParent : oldToNew[p.parentIndex];
            oldToNew[i] = ordered.parts.size();
            ordered.parts.push_back(std::move(copy));
            placed[i] = 1;
            placedCount += 1;
            progress = true;
        }
        if (!progress)
        {
            fail("assembly is cyclic (unresolved instances remain after the topological pass)");
            return std::nullopt;
        }
    }
    return ordered;
}

// The deepest rest extent below the root origin for the current parameters:
// the root's height above the feet point (the ground solve).
inline float groundToRootHeight(const Assembly &assembly, float headingYawRadians)
{
    std::vector<std::array<float, 3>> positions;
    std::vector<float> yaws;
    detail::walkTransforms(assembly, {0.0f, 0.0f, 0.0f}, headingYawRadians, positions, yaws);
    // The LOWEST rest point below the root (the deepest foot) - a max here
    // would read the root itself (extent 0) and never lift the figure. The
    // seed is the FIRST part's foot (a 0 seed would read "ground" even when
    // every part hangs above it).
    float lowest = positions[0][1] - assembly.parts[0].asset->restExtentBelow * assembly.parts[0].scale[1];
    for (std::size_t i = 1; i < assembly.parts.size(); ++i)
    {
        const BuiltPart &p = assembly.parts[i];
        lowest = std::min(lowest, positions[i][1] - p.asset->restExtentBelow * p.scale[1]);
    }
    return -lowest;
}

inline std::vector<ResolvedPart> resolveAssembly(const Assembly &assembly, const ResolveInput &input)
{
    // One walk with the root at ground level, then the vertical shift: the
    // attachment chain is translation-invariant along Y, so lifting the root
    // lifts every part equally - the deepest foot lands on the ground point.
    std::vector<std::array<float, 3>> positions;
    std::vector<float> yaws;
    detail::walkTransforms(assembly, {input.groundPosition[0], input.groundPosition[1], input.groundPosition[2]},
                           input.headingYawRadians, positions, yaws);
    float lowest = positions[0][1] - assembly.parts[0].asset->restExtentBelow * assembly.parts[0].scale[1];
    for (std::size_t i = 1; i < assembly.parts.size(); ++i)
    {
        const BuiltPart &p = assembly.parts[i];
        lowest = std::min(lowest, positions[i][1] - p.asset->restExtentBelow * p.scale[1]);
    }
    // The walk started with the root AT the ground height; the deepest foot
    // dangles lowest meters below it - lift the whole (translation-
    // invariant) tree so the foot lands on the ground point.
    const float lift = input.groundPosition[1] - lowest;

    std::vector<ResolvedPart> out;
    out.reserve(assembly.parts.size());
    for (std::size_t i = 0; i < assembly.parts.size(); ++i)
    {
        const BuiltPart &p = assembly.parts[i];
        ResolvedPart r;
        r.instanceId = p.instanceId;
        r.meshName = p.asset->poseMeshes[0];
        if (p.asset->animation == PartAnimation::Gait)
        {
            const std::uint8_t bucket = std::min<std::uint8_t>(input.gaitBucket, 2u);
            r.meshName = p.asset->poseMeshes[bucket];
        }
        r.position = {positions[i][0], positions[i][1] + lift, positions[i][2]};
        r.yawRadians = yaws[i];
        // The time-varying appendage yaw (see detail::walkTransforms' phase
        // note): applied here, where the resolve owns time.
        if (p.asset->animation == PartAnimation::TailWag || p.asset->animation == PartAnimation::WingIdle)
        {
            const float amplitude = p.asset->animation == PartAnimation::WingIdle
                ? p.asset->animationAmplitudeRadians * 0.35f
                : p.asset->animationAmplitudeRadians;
            const float phase = static_cast<float>(p.animationPhaseIndex) * 0.9f;
            r.yawRadians += amplitude * std::sin(6.28318531f * p.asset->animationSpeedHz * input.timeSeconds + phase);
        }
        r.scale = p.scale;
        r.albedo = p.albedo;
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace character
} // namespace engine

#endif // ENGINE_MODULES_CHARACTER_CUSTOMISATION_CHARACTER_ASSEMBLY_HPP
