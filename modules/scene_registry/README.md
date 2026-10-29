# scene_registry module - the runtime object registry

## What this module provides

Capability `scene` (implementation-bound member `scene.registry`, published in `scene_capability.hpp`): the live object registry of the world. The static `[scene]` configuration is parsed once at startup; this module makes the world addressable at runtime on top of it:

- **spawn**: `addObject(SceneObjectDescriptor)` registers an object and returns a stable handle (0 = registry full; the cap mirrors the GPU primitive buffer's allocation size).
- **transform**: `setObjectTransform(handle, position, rotationY)`. A no-op write (a body whose resolved transform equals what the registry already holds) is the CALLER's decision to skip - the physics step skips it, so a scene at rest publishes no change events at all.
- **material**: `setObjectMaterial(handle, descriptor)` - albedo, roughness, metallic, emissive strength, texture set, uv scale. The object's kind and transform are deliberately untouched by a material edit. `setObjectEmissive(handle, strength)` is the per-frame shape of the emissive edit: it writes the one field (same change event, same end state the read-modify-write through `object` + `setObjectMaterial` produced) without copying the whole descriptor, which is what an emissive tween used to do once per frame per tweened object - texture-set string included.
- **remove**: `removeObject(handle)`; handles are never reused.
- **readback**: `object(handle)`, `objectTransform(handle, out)` (the transform slice - type, position, size, Y rotation - as a plain struct with no material payload, so a per-frame consumer stops copying the texture-set string per object per frame), `objectCount()`, `objectSnapshot()` (every live object in ascending handle order) and `objectSnapshotInto(buffer)` (the same walk into a caller-owned buffer, for frame-rate consumers that keep the buffer's capacity across refreshes - the renderer's dirty-frame re-sync, which stops allocating its snapshot once the buffer has warmed up).
- **meshes**: `addMesh(name, SceneMeshDescriptor)` registers a triangle mesh under a name unique within the run (the same name returns the SAME handle - imports are idempotent by name, which is what makes persistence restore mesh references exactly); `mesh(handle)` reads one back; `meshHandle(name)` / `meshName(handle)` translate between the two addresses. Mesh objects are ordinary objects with `type = mesh` and a `meshHandle` - several objects may instance one mesh, each through its own transform and material.

Every mutation publishes an event on the bus: `SceneObjectAddedEvent`, `SceneObjectRemovedEvent`, `SceneObjectChangedEvent`, `SceneMeshAddedEvent` - each carrying the handle. Consumers coalesce events per frame; many edits in one frame cost one GPU refresh, not one per edit.

## How the world gets in

The registry starts EMPTY. During the renderer's start phase the static world - the parsed `[scene]` configuration - is pushed into the registry, so every consumer sees one world regardless of where its objects came from. A headless build without a renderer simply starts with an empty registry and populates it through scripts or modules. Meshes arrive through `addMesh` callers: the asset capability's glTF import (full-mesh fidelity registers under deterministic names like `Dragon#n0p0`), native mods, or the scripting bridge.

## Who consumes it

The scene registry is consumed by whatever active components need to read, mutate, or persist the world. The consumers describe themselves by capability, not by module name:

- the **renderer** subscribes to the events; rebuilds the GPU scene data incrementally; uploads registered meshes once through its staging path and rasterizes mesh objects as real triangles.
- the **script host** exposes spawn/transform/remove to script mods (`engine.spawn_object` and friends) through a Lua bridge extension member of the `scripting.lua_bridge` family.
- the **physics** provider steps bodies and writes transforms back through `setObjectTransform`; mesh objects collide as their bounding-box proxy. Its per-step snapshot reads the transform slice through `objectTransform`, and a body whose resolved transform matches the registry's skips the write-back and its change event entirely - a scene at rest stops being "dirty" every frame.
- the **animation** provider tweens transforms and materials the same way (transform channels read through `objectTransform`; the emissive channel writes through `setObjectEmissive`).
- the **asset** provider registers imported meshes through `addMesh` (idempotent by name).
- the **persistence** provider snapshots the object list to TOML and spawns rows back (mesh objects by NAME).

Which consumers are active in any given run is decided by capability demand, not enumerated here.

## The incremental rebuild contract

The renderer allocates its primitive and material buffers ONCE at the engine-wide GPU maximum (`SceneCapability::gpuMaxObjects` = 256, mirrored by `scene_common.glsl` on the shader side) and only re-uploads their CONTENT when a scene event arrives. No buffer handle, descriptor set or pipeline is ever invalidated by a scene edit, so the lighting techniques need no rebuild path at all: they re-read `primitiveCount` per frame and keep tracing whatever the buffer holds. The full-capacity allocation is also the honest cost statement: the analytic SDF scales linearly with the object count, and the cap is what keeps a runaway script from silently degrading the frame rate.

Registered meshes upload once per handle (staging copy, device idle on the serialized frame path) and draw in the geometry pass with the SAME interleaved vertex layout (position xyz + normal xyz, `meshFloatsPerVertex` = 6) the unit primitives use - one vertex input state serves both. The analytic consumers never see mesh triangles: each mesh object contributes its ORIENTED BOUNDING BOX as an exact box primitive carrying the object's material (the proxy contract, documented on `scene_object_type::mesh`), so raster and relight paths agree on geometry by construction.

## How handles resolve

Handles are handed out monotonically and never reused, so a handle IS a slot address: the registry keeps a chunked direct-index table (`scene.hpp`, `registryEntryAt`) where the handle's top bits pick a chunk and the low bits pick the slot, and the slot holds the entry's address (or null once the handle was removed). A lookup is one bounds check plus one or two loads - no hash, no probe - which is what the per-frame traffic needs: the physics step reads and writes every body's transform per substep and a tween reads and writes its target per frame, all through handles. Chunks whose handles all died are recycled through a free list for the rest of the run, so the table's footprint tracks the live objects (plus a bounded directory), not the handles ever issued - a script that spawn/destroys in a loop for hours neither grows the table nor re-enters the allocator per cycle. The same chunks give the snapshots their ascending-handle order by construction: walking the directory and each chunk's slots in order visits every live object deterministically, which is what keeps the renderer's primitive indices (and with them its per-primitive material table) stable across frames.

## Configuration

```toml
[scene]
max_objects = 32   # live-object capacity, clamped to 1..256 (the GPU buffer's size)
```

The static initial world comes from the renderer's `[scene]` parse (or an empty registry in builds without a renderer); `max_objects` is the only key the module itself reads. Raising the cap between runs is a configuration edit, never a code change: the GPU buffers are allocated at the 256 maximum regardless, and `objectCapacity()` reports what a run actually enforces.

## Files

- `scene_capability.hpp` - the published capability surface
- `scene.hpp` / `scene.cpp` - the module and the registry implementation
- `module_plugin.cpp` - the static plugin descriptor
- `CMakeLists.txt` - target wiring (`ENGINE_BUILD_MODULE_SCENE_REGISTRY`)
