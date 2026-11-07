#ifndef ENGINE_MODULES_SCENE_SCENE_CAPABILITY_HPP
#define ENGINE_MODULES_SCENE_SCENE_CAPABILITY_HPP

// The published capability surface of the scene module. Every consumer -
// other modules, native mods, the app - includes THIS header, from the
// module that provides the capability: the provider owns and publishes its
// own capability vocabulary, so there is no central contract directory and
// no mirrored copy that could drift. Like every published capability header
// this stays implementation-, platform- and vendor-agnostic.
//
// The capability is the RUNTIME object registry: it starts from the static
// world (the renderer pushes the parsed [scene] objects into it during its
// start phase) and then accepts mutations from any granted consumer - script
// mods through the scripting bridge, native mods and sibling modules through
// this interface. The renderer and the lighting techniques rebuild their GPU
// data from the registry incrementally: the primitive and material buffers
// are allocated once at full capacity and only their CONTENT is re-uploaded,
// so no descriptor set, pipeline or buffer handle is ever invalidated by a
// scene edit - the consumers' reverse-dependency teardown never replaces a
// live handle.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace engine
{

// Object kinds, mirroring the renderer's SceneObjectType values (the GPU
// primitive type lane carries these exact numbers; scene_common.glsl
// documents the shader side). Declared as constants instead of an enum class
// so descriptors survive any ABI boundary as plain data.
namespace scene_object_type
{
inline constexpr std::uint32_t box    = 0;
inline constexpr std::uint32_t sphere = 1;
// A triangle mesh registered through addMesh. The renderer rasterizes the
// mesh's actual triangles; the analytic tracing consumers (GI, reflections,
// shadows) see the mesh's oriented bounding-box proxy - a real box primitive
// carrying the same material: the two paths agree on the proxy exactly.
inline constexpr std::uint32_t mesh = 2;
} // namespace scene_object_type

// Transparency classes of the LAYERED DEPTH BUFFER (rev 31, the owner's
// proposal). The geometry pass no longer forces every object through one
// depth: OPAQUE geometry fills layer 0 (the classic G-buffer every lighting
// consumer reads), SEMI-TRANSPARENT geometry (the water plane, future glass)
// fills a SECOND depth+material layer the composite blends over layer 0 with
// a per-pixel depth-delta opacity, and FULLY-TRANSPARENT geometry (future
// alpha-cut foliage, particles) is classified for the consumers but writes no
// surface contribution yet. The class is data, not a render-side guess: a
// mod (or a [scene.objects] row) names it explicitly.
namespace scene_object_transparency
{
inline constexpr std::uint32_t opaque            = 0;
inline constexpr std::uint32_t semi_transparent  = 1;
inline constexpr std::uint32_t fully_transparent = 2;
} // namespace scene_object_transparency

// Floats per vertex in SceneMeshDescriptor::vertices: position xyz, normal
// xyz, interleaved. This is the same layout the renderer's unit meshes use
// (the geometry pass consumes registered meshes through the same vertex input
// state), so a registered mesh uploads exactly the bytes the descriptor
// carries.
inline constexpr std::size_t meshFloatsPerVertex = 6;

// One scene object as the registry stores and hands it out. Plain values
// only: position and size in world units, Y rotation in radians, the PBR
// material quad (albedo / roughness / metallic / emissive strength) and the
// optional texture set name (resolved against the renderer's texture array
// layers; an unknown name renders flat - the startup diagnostics call this
// out per object, exactly like the static scene path).
struct SceneObjectDescriptor
{
    std::uint32_t type            = scene_object_type::box;
    std::array<float, 3> position = {0.0f, 0.0f, 0.0f};
    // Box: half extents; sphere: radius in x (y/z ignored - spheres keep
    // exact normals under uniform radius); mesh: the registry fills this with
    // the registered mesh's bounding-box HALF EXTENTS (the proxy box the
    // tracing consumers see and the collider shape physics uses).
    std::array<float, 3> size   = {1.0f, 1.0f, 1.0f};
    float rotationYRadians      = 0.0f;
    std::array<float, 3> albedo = {0.8f, 0.8f, 0.8f};
    float roughness             = 0.8f;
    float metallic              = 0.0f;
    float emissiveStrength      = 0.0f;
    std::string textureSet;
    float uvScale = 1.0f;
    // scene_object_type::mesh only: the registry mesh this object instances
    // (a handle addMesh returned). Zero for analytic primitives. Meshes are
    // shared: several objects may instance one mesh, each through its own
    // transform and material.
    std::uint32_t meshHandle = 0;
    // rev 34: per-axis instance scale for MESH objects (meters per authored
    // meter; default 1 = every pre-34 object keeps its exact size). The
    // renderer applies it to the drawn vertices AND the tracing proxy box,
    // so rasterized and traced geometry stay the same shape. Analytic
    // primitives ignore it (their size IS the scale).
    std::array<float, 3> scale = {1.0f, 1.0f, 1.0f};
    // Rev 31 (the layered depth buffer): which depth layer the object fills.
    // Defaults to opaque - every pre-rev-31 object keeps its exact behavior.
    std::uint32_t transparency = scene_object_transparency::opaque;
};

// One registered triangle mesh, in the object's local space (meters). The
// registry computes the bounds from the vertices at registration; the bounds
// are what spawn descriptors receive as their size and what the renderer
// uploads as the tracing proxy box. Vertices are interleaved
// meshFloatsPerVertex floats: position xyz, normal xyz - the same layout the
// renderer's unit meshes use, so the geometry pass draws a registered mesh
// with its existing vertex input state. Normals are required (the asset
// importer computes smooth normals when the source file has none).
struct SceneMeshDescriptor
{
    std::vector<float> vertices;                         // meshFloatsPerVertex floats per vertex
    std::vector<std::uint32_t> indices;                  // triangle list
    std::array<float, 3> boundsMin = {0.0f, 0.0f, 0.0f}; // registry-computed
    std::array<float, 3> boundsMax = {0.0f, 0.0f, 0.0f}; // registry-computed
    // The registered name (rev 28: the renderer's proxy pass reads the
    // vocabulary prefix to give the organic character parts ONE shared
    // figure proxy instead of four overlapping bbox proxies - a crowd of
    // four-part characters otherwise spends eight primitives per villager
    // and overflows the 1024-primitive analytic field).
    std::string name;
};

// The RUNTIME environment override: the mod-driven slice of the sky/light
// state the renderer's [scene] description would otherwise own statically.
// A game mod pushes this per frame (atmosphere: time-of-day sky gradient,
// fog, exposure, moon); the renderer merges it OVER the static scene base
// whenever the registry reports a new sequence. Fields the game does not
// drive stay at their neutral sentinels (negative exposure = "keep the
// configured value"; a zero-length moon direction = "no moon term").
struct SceneEnvironmentOverride
{
    std::array<float, 3> skyZenith   = {0.0f, 0.0f, 0.0f}; // linear RGB
    std::array<float, 3> skyHorizon  = {0.0f, 0.0f, 0.0f}; // linear RGB
    std::array<float, 3> skyGround   = {0.0f, 0.0f, 0.0f}; // linear RGB
    float fogDensity                 = 0.0f;               // [0, 1]; 0 = clear
    std::array<float, 3> fogTint     = {0.7f, 0.7f, 0.7f}; // linear RGB
    float exposure                   = -1.0f;              // <= 0: keep the configured exposure
    std::array<float, 3> moonDirection = {0.0f, 0.0f, 0.0f}; // unit vector towards the scene; zero = no moon
    float moonIntensity              = 0.0f;
    std::array<float, 3> sunColor    = {1.0f, 1.0f, 1.0f};  // the sun disc + direct light color
    float sunIntensity               = -1.0f;               // <= 0: keep the lighting provider's value
    // rev 26: the LIVE sun direction (towards the scene). The sun DISK used
    // to sit at the static [scene].sun direction forever - the animated sun
    // moved, the visible disk did not, and every camera aimed at the real
    // light (the coast staging) framed an empty sky with the glitter path
    // pointing at nothing. A zero vector keeps the static direction (the
    // pre-rev-26 behavior).
    std::array<float, 3> sunDirection = {0.0f, 0.0f, 0.0f};

    // The exact memberwise compare the registry's bump-on-change gate uses
    // (the same idiom the renderer's material diagnostics table uses).
    bool operator==(const SceneEnvironmentOverride &) const = default;
};

// One scene object's transform slice: the fields a transform consumer
// (physics integration, a tween moving the object, a script moving or
// rotating it) reads and writes, without the material payload the full
// descriptor carries - most importantly without the texture set string, so
// a per-frame consumer stops copying a std::string per object per frame
// (a heap round trip whenever the name outgrows its small-string buffer).
// Plain values only, exactly like SceneObjectDescriptor.
struct SceneObjectTransform
{
    std::uint32_t type            = scene_object_type::box;
    std::array<float, 3> position = {0.0f, 0.0f, 0.0f};
    // Same convention as SceneObjectDescriptor::size: box half extents,
    // sphere radius in x, mesh proxy half extents.
    std::array<float, 3> size = {1.0f, 1.0f, 1.0f};
    float rotationYRadians    = 0.0f;
};

// Published after the registry accepted one mesh. The handle is the
// registry's stable address of the mesh for every later instance.
struct SceneMeshAddedEvent
{
    std::uint32_t meshHandle = 0;
};

// Published after the registry accepted one object. The handle is the
// registry's stable address of the object for every later call.
struct SceneObjectAddedEvent
{
    std::uint32_t objectHandle = 0;
};

// Published after an object left the registry. The handle is NOT reused:
// consumers that still hold it are expected to drop it.
struct SceneObjectRemovedEvent
{
    std::uint32_t objectHandle = 0;
};

// Published after an object's transform or material changed. The renderer
// coalesces these into one GPU data refresh per frame: many edits in one
// frame cost one re-upload, not one per edit.
struct SceneObjectChangedEvent
{
    std::uint32_t objectHandle = 0;
};

// Capability "scene": the runtime object registry. Handles are stable and
// never reused; the object cap exists because the GPU primitive buffer is
// allocated once at the engine-wide maximum (the analytic SDF's cost model
// scales linearly with the object count - see the renderer README) and the
// cap is what keeps a runaway script from silently degrading the frame rate.
class SceneCapability
{
public:
    static constexpr const char *capabilityName = "scene";

    using ObjectHandle = std::uint32_t;
    using MeshHandle   = std::uint32_t;

    static constexpr ObjectHandle invalidObject = 0;
    static constexpr MeshHandle invalidMesh     = 0;

    // The default live-object capacity ([scene] max_objects; configurable up
    // to gpuMaxObjects through objectCapacity's configuration). The GPU
    // primitive and material buffers are allocated once at gpuMaxObjects, so
    // raising the configured cap never reallocates anything.
    static constexpr std::uint32_t defaultMaxObjects = 32;
    // The engine-wide GPU capacity (renderer's primitive/material buffer
    // allocation size; also the hard ceiling for the configured cap).
    static constexpr std::uint32_t gpuMaxObjects = 1024;

    // Registry mesh limits: meshes are assets shared by instances, so a
    // modest count is enough; the vertex cap bounds one mesh's GPU buffer.
    // (rev 24: 8 -> 16 - the proper-tree-models round registers a trunk +
    // canopy per species with a small variant each, which - on top of the
    // terrain and water planes - needs ten slots; rev 26 replaces the
    // procedural trees with the converted Kenney CC0 vocabulary (prop_mesh_
    // data_cc0.h: 13 tree slots), adds three market props and nine blocky-
    // character part meshes (three body variants x {body, legs, head} x two
    // baked scales) - 34 slots with headroom for a future glTF import;
    // rev 27 bakes the walk gait: each variant+scale gains two stride
    // poses ({body, legs} x 2 poses x 6 sets = 24 more rows, 57 total) -
    // 64 kept headroom for the future glTF import. rev 28: the organic
    // round adds six hair-cap meshes ({3 styles} x {2 scales}, 63 table
    // rows) and the terrain + water planes take two more slots, so 64
    // overflowed and silently dropped the market cart and the dead tree
    // (the 2026-11-06 capture log refused both). 96 restores the headroom.
    // rev 34: 96 -> 128 (A-209) - the character-assembly part vocabulary
    // (11 meshes: the downloaded CC0 donor parts + the authored organic-style
    // parts) hit the A-208 cap's silent-refusal disease again: the registry
    // refused every chimera_* row and the lab spawned nothing. The GPU cost
    // of 32 more slot descriptors is trivial; the vocabulary is registered
    // once at startup.
    static constexpr std::uint32_t maxMeshes       = 128;
    static constexpr std::uint32_t maxMeshVertices = 1000000;

    virtual ~SceneCapability() = default;

    // Registers one object and returns its handle, or invalidObject when the
    // registry is full. The failure is precise (the provider logs it); the
    // caller decides whether that is an error or a tolerable cap.
    virtual ObjectHandle addObject(const SceneObjectDescriptor &object) = 0;

    // Removes one object. Returns false for unknown handles (including one
    // that was already removed - handles are never reused).
    virtual bool removeObject(ObjectHandle handle) = 0;

    // Replaces one object's transform. Returns false for unknown handles.
    virtual bool setObjectTransform(ObjectHandle handle, const std::array<float, 3> &position, float rotationYRadians)
        = 0;

    // Replaces one object's material (albedo, roughness, metallic, emissive,
    // texture set, uv scale). Returns false for unknown handles.
    virtual bool setObjectMaterial(ObjectHandle handle, const SceneObjectDescriptor &material) = 0;

    // Replaces one object's emissive strength, leaving every other material
    // field exactly as stored. Returns false for unknown handles. This is
    // the per-frame shape of the emissive edit (a tween pulsing the glow):
    // the read-modify-write through object() + setObjectMaterial() answers
    // the same question but copies the whole descriptor - the texture set
    // string included - once per frame per tweened object, where this
    // writes one float. Appended after the established surface, so the
    // existing pure-virtual contract above is untouched. Main thread only,
    // like every capability call.
    virtual bool setObjectEmissive(ObjectHandle handle, float emissiveStrength) = 0;

    // Readback of one object. Returns false for unknown handles.
    virtual bool object(ObjectHandle handle, SceneObjectDescriptor &outObject) const = 0;

    // Transform-only readback of one object (see SceneObjectTransform):
    // the fields a per-frame transform consumer (the physics step's
    // snapshot, a tween's read-modify-write, a script's move/rotate)
    // actually reads, without the material payload and its texture-set
    // string. Returns false for unknown handles. Appended after the
    // established surface, so the existing pure-virtual contract above is
    // untouched. Main thread only, like every capability call.
    virtual bool objectTransform(ObjectHandle handle, SceneObjectTransform &outTransform) const = 0;

    // Current live object count (registry size, never counting removed
    // handles).
    virtual std::uint32_t objectCount() const = 0;

    // The configured live-object capacity ([scene] max_objects, clamped to
    // [1, gpuMaxObjects]). addObject refuses beyond this; the GPU buffers are
    // allocated at gpuMaxObjects regardless, so raising the cap between runs
    // is a configuration edit, never a code change.
    virtual std::uint32_t objectCapacity() const = 0;

    // Registers one triangle mesh under a name unique within this run (the
    // same name returns the SAME handle: imports are idempotent) and returns
    // its handle. invalidMesh when the mesh cap is reached, the vertex/index
    // counts exceed the limits, or the arrays are malformed - each failure
    // logged precisely. The registry copies the data; the caller's vectors
    // are free afterwards. Bounds are computed here from the vertices.
    virtual MeshHandle addMesh(const std::string &name, const SceneMeshDescriptor &mesh) = 0;

    // Readback of one registered mesh (the renderer uploads new handles from
    // this). Returns false for unknown handles.
    virtual bool mesh(MeshHandle handle, SceneMeshDescriptor &outMesh) const = 0;

    // Lookup by the registration name. invalidMesh for unknown names
    // (persistence and scripts address meshes by name; handles are runtime
    // values that a saved file cannot legally carry).
    virtual MeshHandle meshHandle(const std::string &name) const = 0;

    // The registration name of one registered mesh, or an empty string for
    // unknown handles (diagnostics and the persistence module).
    virtual std::string meshName(MeshHandle handle) const = 0;

    // Snapshot of every live object in ascending handle order. The renderer
    // consumes this once per dirty frame to rebuild its GPU data; the stable
    // order is what keeps primitive indices (and with them the per-primitive
    // material table) deterministic across frames.
    virtual std::vector<SceneObjectDescriptor> objectSnapshot() const = 0;

    // Every live handle in ascending order (the same order objectSnapshot
    // uses). Consumers that must remove everything or address each object
    // individually read this instead of probing handles.
    virtual std::vector<ObjectHandle> objectHandles() const = 0;

    // The objectSnapshot walk into a CALLER-OWNED buffer, for consumers that
    // refresh at frame rate (the renderer's dirty-frame re-sync): the buffer
    // is cleared and refilled in the same ascending-handle order, and its
    // capacity is the caller's to keep, so a run stops allocating the
    // snapshot vector once the buffer has warmed to its object high-water
    // mark (the retained-scratch discipline the event bus's drain buffer and
    // the physics step's snapshot follow). Appended after the established
    // surface, so the existing pure-virtual contract above is untouched.
    // Main thread only, like every capability call.
    virtual void objectSnapshotInto(std::vector<SceneObjectDescriptor> &outObjects) const = 0;

    // The runtime environment override (see SceneEnvironmentOverride).
    // Default no-ops keep every existing SceneCapability implementation
    // compiling; the scene registry overrides both. A mod pushes per frame;
    // the registry stores the latest state and bumps an integer sequence
    // ONLY when a pushed state differs from the stored one, so a renderer
    // that skips identical frames by sequence compare stays correct.
    virtual void setEnvironment(const SceneEnvironmentOverride & /*environment*/) {}
    virtual SceneEnvironmentOverride environment() const { return {}; }
    virtual std::uint32_t environmentSequence() const { return 0; }
};

} // namespace engine

#endif // ENGINE_MODULES_SCENE_SCENE_CAPABILITY_HPP
