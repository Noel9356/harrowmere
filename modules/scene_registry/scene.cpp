#include "scene.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sol/sol.hpp>
#include <stdexcept>
#include <string>

namespace engine
{

namespace
{

// Table field helpers for the bridge's object descriptions. Tables
// from Lua: { position = {0, 1, 2}, radius = 0.5 }. Every field is optional -
// the fallbacks match the SceneObjectDescriptor defaults, so a minimal
// { type = "sphere" } table spawns a valid object.
std::array<float, 3> vec3Field(
    const sol::table &table, const char *key, const std::array<float, 3> &fallback
)
{
    const sol::object value = table[key];
    if (value.get_type() == sol::type::table)
    {
        const sol::table components = value;
        return {
            components[1].get_or(0.0f),
            components[2].get_or(0.0f),
            components[3].get_or(0.0f),
        };
    }
    return fallback;
}

double numberField(
    const sol::table &table, const char *key, double fallback
)
{
    const sol::object value = table[key];
    if (value.get_type() == sol::type::number)
    {
        return value.as<double>();
    }
    return fallback;
}

std::string stringField(
    const sol::table &table, const char *key
)
{
    const sol::object value = table[key];
    if (value.get_type() == sol::type::string)
    {
        return value.as<std::string>();
    }
    return {};
}

// The dedicated Lua bridge extension object for the scene module. Owns the
// sol2 wiring that installs engine.spawn_object, engine.remove_object,
// engine.set_object_position, engine.set_object_rotation,
// engine.set_object_material and engine.object_count into every script state.
// Holds a raw pointer to the SceneCapability the module provides.
class SceneLuaBridge final : public LuaBridgeExtension
{
public:
    explicit SceneLuaBridge(
        SceneCapability *capability
    )
        : capability(capability)
    {}

    const char *extensionName() const override
    {
        return "scene";
    }

    void installLuaBridge(
        luaState *lua
    ) override
    {
        // The host (lua_scripting) guarantees the engine table exists when
        // this is called. The bridge functions call the SceneCapability
        // methods directly through the captured pointer - no capability
        // acquisition, no permission check.
        sol::state_view luaView(lua);
        sol::table engine = luaView["engine"];

        engine.set_function("spawn_object", [this](const sol::table &description) -> std::uint32_t {
            SceneObjectDescriptor object;
            const std::string typeName = stringField(description, "type");
            object.type                = typeName == "sphere" ? scene_object_type::sphere : scene_object_type::box;
            object.position            = vec3Field(description, "position", {0.0f, 0.0f, 0.0f});
            if (object.type == scene_object_type::sphere)
            {
                // Spheres keep exact normals under uniform radius: one number,
                // stored in x per the SceneObject convention (y/z ignored by
                // the shaders).
                const float radius = static_cast<float>(numberField(description, "radius", 1.0));
                object.size        = {radius, radius, radius};
            } else
            {
                object.size = vec3Field(description, "half_extents", {1.0f, 1.0f, 1.0f});
            }
            object.rotationYRadians = static_cast<float>(numberField(description, "rotation", 0.0));
            object.albedo           = vec3Field(description, "albedo", {0.8f, 0.8f, 0.8f});
            object.roughness        = static_cast<float>(numberField(description, "roughness", 0.8));
            object.metallic         = static_cast<float>(numberField(description, "metallic", 0.0));
            object.emissiveStrength = static_cast<float>(numberField(description, "emissive", 0.0));
            object.uvScale          = static_cast<float>(numberField(description, "uv_scale", 1.0));
            object.textureSet       = stringField(description, "texture");
            return capability->addObject(object);
        });
        engine.set_function("remove_object", [this](std::uint32_t handle) { return capability->removeObject(handle); });
        engine.set_function("set_object_position", [this](std::uint32_t handle, float x, float y, float z) -> bool {
            // Rotation is preserved: a position move must not reorient anything.
            // The transform-only readback answers with the one field the move
            // needs - the full descriptor copy this bridge used to make spent a
            // texture-set string copy to read one float.
            SceneObjectTransform current;
            if (!capability->objectTransform(handle, current))
            {
                return false;
            }
            return capability->setObjectTransform(handle, {x, y, z}, current.rotationYRadians);
        });
        engine.set_function("set_object_rotation", [this](std::uint32_t handle, float rotationYRadians) -> bool {
            SceneObjectTransform current;
            if (!capability->objectTransform(handle, current))
            {
                return false;
            }
            return capability->setObjectTransform(handle, current.position, rotationYRadians);
        });
        engine.set_function("set_object_material", [this](std::uint32_t handle, const sol::table &material) -> bool {
            SceneObjectDescriptor current;
            if (!capability->object(handle, current))
            {
                return false;
            }
            // Only the fields the table names change: an omitted field keeps
            // its current value (the material edit composes, never resets).
            current.albedo            = vec3Field(material, "albedo", current.albedo);
            current.roughness         = static_cast<float>(numberField(material, "roughness", current.roughness));
            current.metallic          = static_cast<float>(numberField(material, "metallic", current.metallic));
            current.emissiveStrength  = static_cast<float>(numberField(material, "emissive", current.emissiveStrength));
            current.uvScale           = static_cast<float>(numberField(material, "uv_scale", current.uvScale));
            const std::string texture = stringField(material, "texture");
            if (!texture.empty())
            {
                current.textureSet = texture;
            }
            return capability->setObjectMaterial(handle, current);
        });
        engine.set_function("object_count", [this]() { return capability->objectCount(); });
    }

private:
    SceneCapability *capability;
};

} // namespace

SceneModule::SceneModule()
{
    componentManifest = ComponentManifest{
        .name    = "scene",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        // The implementation-bound member name: a future scene provider (a
        // streamed-world service, an editor bridge) declares its own member of
        // the "scene" family and the [providers] configuration picks between us.
        .providedCapabilities
        = {"scene.registry",
            // The Lua bridge extension member: declares this module as a
            // provider of the "scripting.lua_bridge" family. The
            // lua_scripting host acquires every active member through
            // ComponentContext::acquireEachCapability and asks each one
            // to install its own bindings into the host's engine table.
            "scripting.lua_bridge.scene"},
        .requiredCapabilities = {"logging"},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging"},
        .knownConfigKeys      = {"scene.max_objects"},
    };
}

SceneModule::~SceneModule()
{
    stop();
}

void SceneModule::construct(
    ComponentContext &context
)
{
    boundContext = &context;
    logger       = &context.acquireCapability<LoggerCapability>();

    // The configurable object capacity. Clamped once, here: a run whose
    // configuration asks beyond the GPU allocation (gpuMaxObjects - the
    // renderer's primitive/material buffers) would otherwise corrupt the
    // registry's contract halfway through the run.
    const std::int64_t configured = context.configurationIntegerInSection("scene", "max_objects")
                                        .value_or(static_cast<std::int64_t>(defaultMaxObjects));
    if (configured < 1 || configured > static_cast<std::int64_t>(gpuMaxObjects))
    {
        throw std::runtime_error(
            "scene module: [scene] max_objects must be between 1 and " + std::to_string(gpuMaxObjects)
            + " (the GPU primitive buffer's capacity), got " + std::to_string(configured)
        );
    }
    configuredObjectCapacity = static_cast<std::uint32_t>(configured);

    // Channel references cached once: the registry publishes through these
    // for the rest of the run with zero lookups (event_bus.hpp hot path
    // note). Consumers - the renderer, physics, animation - subscribe to the
    // same channels and coalesce the events into per-frame GPU or simulation
    // work.
    EventBus &bus    = context.eventBus();
    addedChannel     = &bus.channel<SceneObjectAddedEvent>();
    removedChannel   = &bus.channel<SceneObjectRemovedEvent>();
    changedChannel   = &bus.channel<SceneObjectChangedEvent>();
    meshAddedChannel = &bus.channel<SceneMeshAddedEvent>();

    // Construct the Lua bridge extension object. The module IS the
    // SceneCapability, so `this` is the capability pointer the bridge calls
    // back into.
    luaBridge = std::make_unique<SceneLuaBridge>(this);
}

void SceneModule::start()
{
    logger->logMessage(
        LogLevel::Information,
        "scene module started (" + std::to_string(registry.size()) + " object(s) registered, capacity "
            + std::to_string(configuredObjectCapacity) + ", " + std::to_string(meshRegistry.size())
            + " mesh(es) registered)"
    );
}

void SceneModule::stop()
{
    registry.clear();
    handleChunks.clear();
    handleChunkLiveCounts.clear();
    recycledHandleChunks.clear();
    handleChunkArena.clear();
    meshRegistry.clear();
    meshHandlesByName.clear();
    nextMeshHandle = invalidMesh + 1;
    // Reset the Lua bridge before nulling the capability pointers it captures.
    luaBridge.reset();
    boundContext     = nullptr;
    logger           = nullptr;
    addedChannel     = nullptr;
    removedChannel   = nullptr;
    changedChannel   = nullptr;
    meshAddedChannel = nullptr;
}

void *SceneModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    // The kernel validates and resolves through the DECLARED member name
    // (the construct phase asks for every name in providedCapabilities, and
    // the acquisition walk passes the resolved member, not the family name
    // the consumer requested).
    if (capabilityName == "scene.registry")
    {
        return static_cast<SceneCapability *>(this);
    }
    // The "scripting.lua_bridge.scene" member: returns the dedicated
    // SceneLuaBridge object this module owns (constructed in construct()).
    if (capabilityName == "scripting.lua_bridge.scene")
    {
        return luaBridge.get();
    }
    return nullptr;
}

SceneCapability::ObjectHandle SceneModule::addObject(
    const SceneObjectDescriptor &object
)
{
    if (registry.size() >= configuredObjectCapacity)
    {
        // The precise failure: the GPU primitive buffer is allocated at the
        // engine-wide maximum (see scene_capability.hpp), so the configured
        // capacity is what keeps the SDF's per-frame cost - linear in the
        // object count - honest. Callers see invalidObject and decide what a
        // full registry means for them.
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Error,
                "scene module: refused to register object " + std::to_string(registry.size() + 1)
                    + ": the registry is at capacity (" + std::to_string(configuredObjectCapacity)
                    + " objects; raise [scene] max_objects, at most " + std::to_string(gpuMaxObjects) + ")"
            );
        }
        return invalidObject;
    }
    // Mesh instances must name a registered mesh: a mesh object with no mesh
    // would rasterize nothing and trace a zero-size proxy - a silent visual
    // bug. The registry knows every mesh handle, so the check is exact.
    if (object.type == scene_object_type::mesh
        && (object.meshHandle == invalidMesh || meshRegistry.find(object.meshHandle) == meshRegistry.end()))
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Error,
                "scene module: refused to register mesh object: meshHandle " + std::to_string(object.meshHandle)
                    + " names no registered mesh (register it through addMesh first)"
            );
        }
        return invalidObject;
    }
    const ObjectHandle handle = nextHandle++;
    RegistryEntry entry;
    entry.handle          = handle;
    entry.object          = object;
    RegistryEntry &stored = registry.emplace(handle, std::move(entry)).first->second;
    // The handle's slot: one chunk acquisition when the handle enters a
    // chunk range with no storage yet (the recycle list makes that a
    // once-per-chunk cost, not a once-per-add cost), then one store.
    const std::size_t chunkIndex = static_cast<std::size_t>(handle) >> handleTableChunkBits;
    if (chunkIndex >= handleChunks.size() || handleChunks[chunkIndex] == nullptr)
    {
        acquireHandleChunk(chunkIndex);
    }
    (*handleChunks[chunkIndex])[handle & (handleTableChunkSize - 1)] = &stored;
    ++handleChunkLiveCounts[chunkIndex];
    if (logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Debug,
            "scene module: object " + std::to_string(handle) + " registered (type " + std::to_string(object.type) + ", "
                + std::to_string(registry.size()) + " live)"
        );
    }
    if (addedChannel != nullptr)
    {
        addedChannel->publish(SceneObjectAddedEvent{.objectHandle = handle});
    }
    return handle;
}

bool SceneModule::removeObject(
    ObjectHandle handle
)
{
    RegistryEntry *entry = registryEntryAt(handle);
    if (entry == nullptr)
    {
        return false;
    }
    // Drop the handle's slot first (the table is the one lookup surface),
    // then erase the entry itself. No shift, no binary search: one null
    // write, one live-count decrement, then the map's node erase. A chunk
    // whose last live slot just died is recycled here (see
    // releaseHandleChunk), which is what keeps the table's footprint
    // tracking the live set through a churn workload.
    const std::size_t chunkIndex = static_cast<std::size_t>(handle) >> handleTableChunkBits;
    (*handleChunks[chunkIndex])[handle & (handleTableChunkSize - 1)] = nullptr;
    if (--handleChunkLiveCounts[chunkIndex] == 0)
    {
        releaseHandleChunk(chunkIndex);
    }
    registry.erase(registry.find(handle));
    if (logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Debug,
            "scene module: object " + std::to_string(handle) + " removed (" + std::to_string(registry.size()) + " live)"
        );
    }
    if (removedChannel != nullptr)
    {
        removedChannel->publish(SceneObjectRemovedEvent{.objectHandle = handle});
    }
    return true;
}

bool SceneModule::setObjectTransform(
    ObjectHandle handle, const std::array<float, 3> &position, float rotationYRadians
)
{
    RegistryEntry *entry = registryEntryAt(handle);
    if (entry == nullptr)
    {
        return false;
    }
    entry->object.position         = position;
    entry->object.rotationYRadians = rotationYRadians;
    if (changedChannel != nullptr)
    {
        changedChannel->publish(SceneObjectChangedEvent{.objectHandle = handle});
    }
    return true;
}

bool SceneModule::setObjectMaterial(
    ObjectHandle handle, const SceneObjectDescriptor &material
)
{
    RegistryEntry *entry = registryEntryAt(handle);
    if (entry == nullptr)
    {
        return false;
    }
    // Only the material side of the descriptor: the object's identity (type)
    // and transform stay untouched - a material edit must never teleport an
    // object or turn a sphere into a box.
    entry->object.albedo           = material.albedo;
    entry->object.roughness        = material.roughness;
    entry->object.metallic         = material.metallic;
    entry->object.emissiveStrength = material.emissiveStrength;
    entry->object.textureSet       = material.textureSet;
    entry->object.uvScale          = material.uvScale;
    if (changedChannel != nullptr)
    {
        changedChannel->publish(SceneObjectChangedEvent{.objectHandle = handle});
    }
    return true;
}

bool SceneModule::setObjectEmissive(
    ObjectHandle handle, float emissiveStrength
)
{
    RegistryEntry *entry = registryEntryAt(handle);
    if (entry == nullptr)
    {
        return false;
    }
    // The single-field material write behind the emissive tween channel: the
    // read-modify-write through object() + setObjectMaterial() produced
    // exactly this end state while copying the whole descriptor (the texture
    // set string included) once per frame per tweened object. One float is
    // stored, one event is published - the same event, so every consumer
    // sees the identical mutation it always did.
    entry->object.emissiveStrength = emissiveStrength;
    if (changedChannel != nullptr)
    {
        changedChannel->publish(SceneObjectChangedEvent{.objectHandle = handle});
    }
    return true;
}

bool SceneModule::object(
    ObjectHandle handle, SceneObjectDescriptor &outObject
) const
{
    const RegistryEntry *entry = registryEntryAt(handle);
    if (entry == nullptr)
    {
        return false;
    }
    outObject = entry->object;
    return true;
}

bool SceneModule::objectTransform(
    ObjectHandle handle, SceneObjectTransform &outTransform
) const
{
    const RegistryEntry *entry = registryEntryAt(handle);
    if (entry == nullptr)
    {
        return false;
    }
    // The transform slice of the object readback above: same registry, same
    // fields, no material payload - a per-frame consumer (the physics step,
    // a tween, a script move) stops copying the descriptor's texture-set
    // string per object per frame.
    const SceneObjectDescriptor &object = entry->object;
    outTransform.type                   = object.type;
    outTransform.position               = object.position;
    outTransform.size                   = object.size;
    outTransform.rotationYRadians       = object.rotationYRadians;
    return true;
}

std::uint32_t SceneModule::objectCount() const
{
    return static_cast<std::uint32_t>(registry.size());
}

std::uint32_t SceneModule::objectCapacity() const
{
    return configuredObjectCapacity;
}

SceneCapability::MeshHandle SceneModule::addMesh(
    const std::string &name, const SceneMeshDescriptor &mesh
)
{
    // Idempotent registration: the same name is the same asset. An importer
    // that runs twice (or two objects sharing one mesh) must not duplicate
    // the GPU buffer.
    const auto known = meshHandlesByName.find(name);
    if (known != meshHandlesByName.end())
    {
        return known->second;
    }
    const std::size_t vertexFloats = mesh.vertices.size();
    const bool malformed = vertexFloats == 0 || vertexFloats % meshFloatsPerVertex != 0 || mesh.indices.size() < 3
                        || mesh.indices.size() % 3 != 0;
    if (malformed)
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Error,
                "scene module: refused to register mesh '" + name + "': the vertex array must be non-empty and a "
                    + "multiple of " + std::to_string(meshFloatsPerVertex) + " floats, the index array a multiple of 3"
            );
        }
        return invalidMesh;
    }
    // Per-vertex finite screen: one NaN/inf position or normal computes NaN
    // bounds right below, and NaN bounds poison every proxy box, BVH build
    // and contact test the mesh feeds. The malformed report names the field
    // so an importer bug is precise.
    for (std::size_t offset = 0; offset < vertexFloats; ++offset)
    {
        if (!std::isfinite(mesh.vertices[offset]))
        {
            if (logger != nullptr)
            {
                const std::size_t vertex = offset / meshFloatsPerVertex;
                const bool position      = offset % meshFloatsPerVertex < 3;
                logger->logMessage(
                    LogLevel::Error,
                    "scene module: refused to register mesh '" + name + "': vertex " + std::to_string(vertex)
                        + (position ? " position" : " normal") + " is not finite"
                );
            }
            return invalidMesh;
        }
    }
    if (meshRegistry.size() >= maxMeshes)
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Error,
                "scene module: refused to register mesh '" + name + "': the mesh registry is at capacity ("
                    + std::to_string(maxMeshes) + " meshes)"
            );
        }
        return invalidMesh;
    }
    const std::size_t vertexCount = vertexFloats / meshFloatsPerVertex;
    if (vertexCount > maxMeshVertices)
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Error,
                "scene module: refused to register mesh '" + name + "': " + std::to_string(vertexCount)
                    + " vertices exceed the per-mesh limit (" + std::to_string(maxMeshVertices) + ")"
            );
        }
        return invalidMesh;
    }
    for (const std::uint32_t index : mesh.indices)
    {
        if (index >= vertexCount)
        {
            if (logger != nullptr)
            {
                logger->logMessage(
                    LogLevel::Error,
                    "scene module: refused to register mesh '" + name + "': index " + std::to_string(index)
                        + " is out of range (" + std::to_string(vertexCount) + " vertices)"
                );
            }
            return invalidMesh;
        }
    }
    // Bounds from the vertices, once: every proxy box and every spawn
    // descriptor reads the same numbers, so they cannot drift apart.
    SceneMeshDescriptor stored;
    stored.vertices = mesh.vertices;
    stored.indices  = mesh.indices;
    glm::vec3 minimum{std::numeric_limits<float>::max()};
    glm::vec3 maximum{-std::numeric_limits<float>::max()};
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
    {
        const glm::vec3 position = glm::make_vec3(mesh.vertices.data() + vertex * meshFloatsPerVertex);
        minimum                  = glm::min(minimum, position);
        maximum                  = glm::max(maximum, position);
    }
    stored.boundsMin        = {minimum.x, minimum.y, minimum.z};
    stored.boundsMax        = {maximum.x, maximum.y, maximum.z};
    const MeshHandle handle = nextMeshHandle++;
    MeshEntry entry;
    entry.handle = handle;
    entry.name   = name;
    entry.mesh   = std::move(stored);
    meshRegistry.emplace(handle, std::move(entry));
    meshHandlesByName.emplace(name, handle);
    if (logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Debug,
            "scene module: mesh '" + name + "' registered as handle " + std::to_string(handle) + " ("
                + std::to_string(vertexCount) + " vertices, " + std::to_string(mesh.indices.size() / 3) + " triangles)"
        );
    }
    if (meshAddedChannel != nullptr)
    {
        meshAddedChannel->publish(SceneMeshAddedEvent{.meshHandle = handle});
    }
    return handle;
}

bool SceneModule::mesh(
    MeshHandle handle, SceneMeshDescriptor &outMesh
) const
{
    const auto found = meshRegistry.find(handle);
    if (found == meshRegistry.end())
    {
        return false;
    }
    outMesh = found->second.mesh;
    return true;
}

SceneCapability::MeshHandle SceneModule::meshHandle(
    const std::string &name
) const
{
    const auto found = meshHandlesByName.find(name);
    return found != meshHandlesByName.end() ? found->second : invalidMesh;
}

std::string SceneModule::meshName(
    MeshHandle handle
) const
{
    const auto found = meshRegistry.find(handle);
    return found != meshRegistry.end() ? found->second.name : std::string();
}

std::vector<SceneObjectDescriptor> SceneModule::objectSnapshot() const
{
    // Ascending handle order: the renderer's per-primitive material table and
    // the geometry pass both index by snapshot position, so the order must be
    // stable across frames regardless of unordered_map iteration order. The
    // chunk directory sits in handle order by construction, so the walk
    // visits chunk k's slots in order, skips the dead slots, and copies the
    // live entries linearly - the collect-and-sort the map walk used to pay
    // per call is gone, and no separate order cache needs maintaining.
    std::vector<SceneObjectDescriptor> snapshot;
    snapshot.reserve(registry.size());
    for (const HandleChunk *chunk : handleChunks)
    {
        if (chunk != nullptr)
        {
            for (const RegistryEntry *entry : *chunk)
            {
                if (entry != nullptr)
                {
                    snapshot.push_back(entry->object);
                }
            }
        }
    }
    return snapshot;
}

void SceneModule::objectSnapshotInto(
    std::vector<SceneObjectDescriptor> &outObjects
) const
{
    // The objectSnapshot walk with a caller-owned buffer: clear preserves the
    // caller's capacity, the fill is the same ascending-handle linear copy
    // pass over the chunk table, and a frame-rate consumer (the renderer's
    // dirty-frame re-sync) stops allocating its snapshot once its buffer has
    // warmed up.
    outObjects.clear();
    outObjects.reserve(registry.size());
    for (const HandleChunk *chunk : handleChunks)
    {
        if (chunk != nullptr)
        {
            for (const RegistryEntry *entry : *chunk)
            {
                if (entry != nullptr)
                {
                    outObjects.push_back(entry->object);
                }
            }
        }
    }
}

std::vector<SceneCapability::ObjectHandle> SceneModule::objectHandles() const
{
    std::vector<ObjectHandle> handles;
    handles.reserve(registry.size());
    for (std::size_t chunkIndex = 0; chunkIndex < handleChunks.size(); ++chunkIndex)
    {
        const HandleChunk *chunk = handleChunks[chunkIndex];
        if (chunk == nullptr)
        {
            continue;
        }
        const ObjectHandle chunkBase = static_cast<ObjectHandle>(chunkIndex << handleTableChunkBits);
        for (std::size_t slot = 0; slot < handleTableChunkSize; ++slot)
        {
            if ((*chunk)[slot] != nullptr)
            {
                handles.push_back(chunkBase + static_cast<ObjectHandle>(slot));
            }
        }
    }
    return handles;
}

SceneModule::RegistryEntry *SceneModule::registryEntryAt(
    ObjectHandle handle
)
{
    // One bounds check on the chunk index (a handle above every issued one
    // indexes past the directory) and one on the chunk pointer (an issued
    // but fully-dead chunk reads null). Handle 0's slot is never written,
    // so the invalid-handle case rides the same path.
    const std::size_t chunkIndex = static_cast<std::size_t>(handle) >> handleTableChunkBits;
    if (chunkIndex >= handleChunks.size())
    {
        return nullptr;
    }
    HandleChunk *chunk = handleChunks[chunkIndex];
    return chunk != nullptr ? (*chunk)[handle & (handleTableChunkSize - 1)] : nullptr;
}

const SceneModule::RegistryEntry *SceneModule::registryEntryAt(
    ObjectHandle handle
) const
{
    const std::size_t chunkIndex = static_cast<std::size_t>(handle) >> handleTableChunkBits;
    if (chunkIndex >= handleChunks.size())
    {
        return nullptr;
    }
    const HandleChunk *chunk = handleChunks[chunkIndex];
    return chunk != nullptr ? (*chunk)[handle & (handleTableChunkSize - 1)] : nullptr;
}

void SceneModule::acquireHandleChunk(
    std::size_t chunkIndex
)
{
    // Extend the directory to the requested index; the extension fills
    // nulls, which is exactly the "no storage" state the caller checked.
    if (chunkIndex >= handleChunks.size())
    {
        handleChunks.resize(chunkIndex + 1, nullptr);
        handleChunkLiveCounts.resize(chunkIndex + 1, 0);
    }
    // Recycle an emptied chunk when one is parked; grow the arena only when
    // the run has never emptied one (the steady state of a churn workload is
    // one chunk cycling through the free list, not one allocation per chunk
    // range). Chunks live in the arena for the module's lifetime - see the
    // arena note in the header - so neither path runs the allocator per
    // cycle.
    if (!recycledHandleChunks.empty())
    {
        HandleChunk *chunk = recycledHandleChunks.back();
        recycledHandleChunks.pop_back();
        std::fill(chunk->begin(), chunk->end(), nullptr);
        handleChunks[chunkIndex] = chunk;
        return;
    }
    handleChunkArena.push_back(std::make_unique<HandleChunk>());
    handleChunks[chunkIndex] = handleChunkArena.back().get();
}

void SceneModule::releaseHandleChunk(
    std::size_t chunkIndex
)
{
    HandleChunk *chunk       = handleChunks[chunkIndex];
    handleChunks[chunkIndex] = nullptr;
    if (chunkIndex == handleChunks.size() - 1)
    {
        // The tail chunk died: pop it and every other trailing empty the
        // directory may hold, so the directory's size tracks the highest
        // chunk range that ever held (or may yet hold) live storage.
        // "May yet hold" is nextHandle: a run that stopped issuing handles
        // keeps its directory trimmed to the last range a handle could
        // still name. The popped storage itself stays in the arena (see
        // the arena note in the header) - only the directory shrinks.
        do
        {
            handleChunks.pop_back();
            handleChunkLiveCounts.pop_back();
        } while (!handleChunks.empty() && handleChunks.back() == nullptr
                 && (handleChunks.size() - 1) * handleTableChunkSize >= nextHandle);
        recycledHandleChunks.push_back(chunk);
        return;
    }
    // A middle chunk with no live entries: park it for reuse instead of
    // freeing (the churn workload that emptied it will fill a chunk again,
    // and the same storage serves). The directory slot reads null until
    // then, which is exactly the dead-range answer every lookup wants.
    recycledHandleChunks.push_back(chunk);
}

} // namespace engine
