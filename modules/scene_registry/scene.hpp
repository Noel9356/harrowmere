#ifndef ENGINE_MODULES_SCENE_HPP
#define ENGINE_MODULES_SCENE_HPP

#include "logging_capability.hpp"
#include "lua_bridge_extension.hpp"
#include "scene_capability.hpp"
#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/event_bus.hpp"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine
{

// Module "scene_registry": provides the SceneCapability - the runtime object registry.
//
// The registry starts empty. During the renderer's start phase the static
// world (the parsed [scene] configuration) is pushed into it, so every
// consumer sees one world regardless of where its objects came from; script
// mods and modules then mutate through the capability, and every mutation
// publishes an event on the bus. The renderer subscribes and rebuilds its
// GPU scene data incrementally (content-only re-upload of the fixed-capacity
// primitive and material buffers); the physics module writes stepped body
// transforms back through the same interface.
//
// The module is pure logic: no window, no Vulkan, no scripting runtime. That
// keeps it usable in minimal builds (a headless simulation with physics and
// animation but no renderer never loads a frame loop).
//
// Besides objects the registry owns triangle MESHES (addMesh): assets shared
// by mesh-type object instances. The renderer uploads a registered mesh's
// vertex/index data once and rasterizes its triangles; the tracing consumers
// see the registry-computed bounding box as the proxy primitive. Meshes live
// for the run (they are assets, not instances); objects come and go.
//
// The module additionally provides the "scripting.lua_bridge.scene" Lua bridge
// extension: it installs engine.spawn_object, engine.remove_object,
// engine.set_object_position, engine.set_object_rotation,
// engine.set_object_material and engine.object_count into every script state
// through a dedicated SceneLuaBridge object owned by this module (see
// scene.cpp). The host discovers this extension through the multi-provider
// acquisition of the "scripting.lua_bridge" family, so adding this capability
// to the module never touches the lua_scripting source.
class SceneModule final : public ModuleInterface, public SceneCapability
{
public:
    SceneModule();
    ~SceneModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    // SceneCapability (the module IS the implementation behind the family
    // member "scene.registry").
    ObjectHandle addObject(const SceneObjectDescriptor &object) override;
    bool removeObject(ObjectHandle handle) override;
    bool setObjectTransform(ObjectHandle handle, const std::array<float, 3> &position, float rotationYRadians) override;
    bool setObjectMaterial(ObjectHandle handle, const SceneObjectDescriptor &material) override;
    bool setObjectEmissive(ObjectHandle handle, float emissiveStrength) override;
    bool object(ObjectHandle handle, SceneObjectDescriptor &outObject) const override;
    bool objectTransform(ObjectHandle handle, SceneObjectTransform &outTransform) const override;
    std::uint32_t objectCount() const override;
    std::uint32_t objectCapacity() const override;
    std::vector<SceneObjectDescriptor> objectSnapshot() const override;
    std::vector<ObjectHandle> objectHandles() const override;
    void objectSnapshotInto(std::vector<SceneObjectDescriptor> &outObjects) const override;

    MeshHandle addMesh(const std::string &name, const SceneMeshDescriptor &mesh) override;
    bool mesh(MeshHandle handle, SceneMeshDescriptor &outMesh) const override;
    MeshHandle meshHandle(const std::string &name) const override;
    std::string meshName(MeshHandle handle) const override;

private:
    struct RegistryEntry
    {
        ObjectHandle handle = invalidObject;
        SceneObjectDescriptor object;
    };

    struct MeshEntry
    {
        MeshHandle handle = invalidMesh;
        std::string name;
        SceneMeshDescriptor mesh;
    };

    // Publishes the change event for one object through the cached channel.
    template <typename EventType> void publishEvent(const EventType &event);

    ComponentManifest componentManifest;
    ComponentContext *boundContext = nullptr;
    LoggerCapability *logger       = nullptr;

    // Channel references cached at construct (the bus anchors channels on the
    // heap; the registry's hot path publishes with zero lookups).
    EventChannel<SceneObjectAddedEvent> *addedChannel     = nullptr;
    EventChannel<SceneObjectRemovedEvent> *removedChannel = nullptr;
    EventChannel<SceneObjectChangedEvent> *changedChannel = nullptr;
    EventChannel<SceneMeshAddedEvent> *meshAddedChannel   = nullptr;

    // The registry: live objects keyed by handle. An unordered_map keeps
    // add/remove/update constant-cost and anchors the entries (node-based
    // storage: a live entry's address is stable for its whole lifetime,
    // which both the snapshot walks and the handle table below rely on);
    // the map's iteration order is never observable - every readout goes
    // through the handle table below, whose order is ascending by
    // construction.
    std::unordered_map<ObjectHandle, RegistryEntry> registry;
    ObjectHandle nextHandle = invalidObject + 1;

    // Direct-index handle table: the structure the hash-map lookups of the
    // capability calls were replaced with. Handles are handed out
    // monotonically (nextHandle++ and never reused), so a handle IS a slot
    // address - no hash, no probe: one bounds check and one or two loads.
    // This is the engine's hottest registry surface: the physics step reads
    // and writes every body's transform per substep and the tweens read and
    // write their targets per frame, all through handles.
    //
    // The storage is CHUNKED - handleTableChunkSize slots per chunk, a chunk
    // directory indexed by handle >> handleTableChunkBits - rather than one
    // flat vector, for a memory-shape reason a flat vector cannot survive:
    // handles are never reused, so a run that spawn/destroys in a loop for
    // hours issues an unbounded handle count, and a flat table either keeps
    // one slot (8 bytes) per handle ever issued or has to be trimmed - and
    // a trimmed flat table thrashes, because the next add must resize the
    // vector back up past every handle issued since the trim, re-nulling a
    // gap that only ever grows. Chunks cut the tie: a chunk whose handles
    // are all dead is recycled (reused for the next chunk index that needs
    // storage through a free list, or popped outright at the directory's
    // tail), so the steady-state footprint tracks the LIVE object count
    // while a lookup stays two dependent loads (the directory is a few
    // pointers, L1-resident).
    //
    // The chunks double as the ascending-handle order the snapshots need:
    // chunk k holds exactly handles [k * chunkSize, (k+1) * chunkSize), so
    // walking the directory and each chunk's slots in order visits every
    // live entry in ascending handle order - the same linear pass the
    // eagerly maintained pointer cache made, with one less piece of state
    // to keep current on every mutation (a remove used to binary-search its
    // pointer away in that cache; here it writes one null and one counter).
    static constexpr std::size_t handleTableChunkBits = 10;
    static constexpr std::size_t handleTableChunkSize = std::size_t{1} << handleTableChunkBits;
    using HandleChunk                                 = std::array<RegistryEntry *, handleTableChunkSize>;

    // The chunk directory: entry k (possibly null) holds the storage for
    // handles [k * chunkSize, ...). A null entry means "no handle in this
    // chunk's range is live" - the directory grows only as handles pass
    // into new chunk ranges (one pointer per chunkSize handles ever
    // ISSUED - 8 kilobytes per million - and it never needs to shrink:
    // reuse and emptiness are expressed through the chunk pointers).
    std::vector<HandleChunk *> handleChunks;
    // Live-slot count per chunk (parallel to the directory): the emptiness
    // signal the recycling runs on, maintained on every add and remove.
    std::vector<std::uint32_t> handleChunkLiveCounts;
    // The chunk ARENA: every chunk ever allocated lives here for the
    // module's lifetime, so a churn workload never re-enters the allocator
    // after its high-water chunk count - emptied chunks cycle through the
    // free list below as raw storage, and the arena grows only to the peak
    // of concurrently-non-empty chunks (the live set's footprint, never the
    // ever-issued handle count). The chunk objects are heap-anchored
    // (unique_ptr, not an element vector): the directory and the free list
    // hold raw pointers into this pool, and those addresses must never
    // move.
    std::vector<std::unique_ptr<HandleChunk>> handleChunkArena;
    // Fully-empty chunks held for reuse, so a churn workload allocates a
    // chunk once and then cycles the same storage for the rest of the run.
    std::vector<HandleChunk *> recycledHandleChunks;

    // The shared lookup tail of the handle-addressed capability calls: the
    // live entry for a handle, or null when the handle is invalid (0), was
    // never issued, or names a removed object. Handle 0's slot is never
    // written (handles start at invalidObject + 1), so the invalid-handle
    // case needs no special guard - the table answers it the same way it
    // answers every other dead handle.
    RegistryEntry *registryEntryAt(ObjectHandle handle);
    const RegistryEntry *registryEntryAt(ObjectHandle handle) const;

    // Allocates (or recycles) the chunk storage for one handle's chunk
    // index, extending the directory to cover it. The shared tail of
    // addObject's slot write.
    void acquireHandleChunk(std::size_t chunkIndex);

    // Releases a chunk whose last live slot just died: popped when it is
    // the directory's tail, parked in the recycle list otherwise. The
    // shared tail of removeObject's slot clear.
    void releaseHandleChunk(std::size_t chunkIndex);

    // The configured live-object capacity ([scene] max_objects, clamped to
    // [1, gpuMaxObjects]); read once at construct. Default: defaultMaxObjects.
    std::uint32_t configuredObjectCapacity = defaultMaxObjects;

    // Registered meshes keyed by handle, plus the name index (name -> handle)
    // that makes imports idempotent: registering the same name again returns
    // the same handle instead of a second copy.
    std::unordered_map<MeshHandle, MeshEntry> meshRegistry;
    std::unordered_map<std::string, MeshHandle> meshHandlesByName;
    MeshHandle nextMeshHandle = invalidMesh + 1;
    // The Lua bridge extension object owned by this module. Constructed in
    // construct() and returned from capabilityPointerFor for the
    // "scripting.lua_bridge.scene" member. Held as the base LuaBridgeExtension
    // pointer: the concrete SceneLuaBridge type (defined in scene.cpp) stays
    // private to this module's translation unit.
    std::unique_ptr<LuaBridgeExtension> luaBridge;
};

} // namespace engine

#endif // ENGINE_MODULES_SCENE_HPP
