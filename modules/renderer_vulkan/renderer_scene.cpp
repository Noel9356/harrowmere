#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "renderer_state.hpp"

#include "mesh_generation.hpp"
#include "vulkan_utilities.hpp"
#include "engine/kernel/filesystem_utilities.hpp"
#include "engine/kernel/parallel_for.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine
{

void RendererState::uploadToDeviceLocal(
    GeometryBuffer &target, const void *data, vk::DeviceSize byteSize, vk::BufferUsageFlags usages
)
{
    vk::BufferCreateInfo stagingInfo;
    stagingInfo.setSize(byteSize);
    stagingInfo.setUsage(vk::BufferUsageFlagBits::eTransferSrc);
    stagingInfo.setSharingMode(vk::SharingMode::eExclusive);
    vk::raii::Buffer stagingBuffer(*device, stagingInfo);
    const vk::MemoryRequirements stagingRequirements = stagingBuffer.getMemoryRequirements();
    vk::MemoryAllocateInfo stagingAllocate;
    stagingAllocate.setAllocationSize(stagingRequirements.size);
    stagingAllocate.setMemoryTypeIndex(findMemoryType(
        **physicalDevice,
        stagingRequirements.memoryTypeBits,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
    ));
    vk::raii::DeviceMemory stagingMemory(*device, stagingAllocate);
    stagingBuffer.bindMemory(*stagingMemory, 0);
    void *mapped = stagingMemory.mapMemory(0, stagingRequirements.size);
    std::memcpy(mapped, data, static_cast<std::size_t>(byteSize));
    stagingMemory.unmapMemory();

    vk::BufferCreateInfo bufferInfo;
    bufferInfo.setSize(byteSize);
    bufferInfo.setUsage(usages | vk::BufferUsageFlagBits::eTransferDst);
    bufferInfo.setSharingMode(vk::SharingMode::eExclusive);
    target.buffer                             = std::make_unique<vk::raii::Buffer>(*device, bufferInfo);
    const vk::MemoryRequirements requirements = target.buffer->getMemoryRequirements();
    vk::MemoryAllocateInfo allocateInfo;
    allocateInfo.setAllocationSize(requirements.size);
    allocateInfo.setMemoryTypeIndex(
        findMemoryType(**physicalDevice, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal)
    );
    target.memory = std::make_unique<vk::raii::DeviceMemory>(*device, allocateInfo);
    target.buffer->bindMemory(*target.memory, 0);

    vk::CommandBufferAllocateInfo commandAllocateInfo;
    commandAllocateInfo.setCommandPool(**commandPool);
    commandAllocateInfo.setLevel(vk::CommandBufferLevel::ePrimary);
    commandAllocateInfo.setCommandBufferCount(1);
    vk::raii::CommandBuffer setupBuffer = std::move(vk::raii::CommandBuffers(*device, commandAllocateInfo).front());
    setupBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    vk::BufferCopy copyRegion(0, 0, byteSize);
    setupBuffer.copyBuffer(*stagingBuffer, *target.buffer, copyRegion);
    setupBuffer.end();
    vk::SubmitInfo submitInfo;
    submitInfo.setCommandBuffers(*setupBuffer);
    graphicsQueue.submit(submitInfo);
    graphicsQueue.waitIdle();
}

void RendererState::uploadBatchToDeviceLocal(
    std::vector<BatchedUpload> &uploads
)
{
    if (uploads.empty())
    {
        return;
    }
    // N meshes on one dirty frame used to pay N staging allocations, N
    // submissions and N waitIdles (one uploadToDeviceLocal per mesh); the
    // radiance-cascades module already documented the better shape for this
    // exact pattern - one staging buffer, one setup submission, one round
    // trip. Same per-target allocations (one device-local buffer per upload;
    // a suballocator is a separate, larger change), but the queue costs
    // collapse to one of each regardless of mesh count.
    vk::DeviceSize stagingCursor = 0;
    for (const BatchedUpload &upload : uploads)
    {
        stagingCursor += upload.data.size();
    }

    vk::BufferCreateInfo stagingInfo;
    stagingInfo.setSize(stagingCursor);
    stagingInfo.setUsage(vk::BufferUsageFlagBits::eTransferSrc);
    stagingInfo.setSharingMode(vk::SharingMode::eExclusive);
    vk::raii::Buffer stagingBuffer(*device, stagingInfo);
    const vk::MemoryRequirements stagingRequirements = stagingBuffer.getMemoryRequirements();
    vk::MemoryAllocateInfo stagingAllocate;
    stagingAllocate.setAllocationSize(stagingRequirements.size);
    stagingAllocate.setMemoryTypeIndex(findMemoryType(
        **physicalDevice,
        stagingRequirements.memoryTypeBits,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
    ));
    vk::raii::DeviceMemory stagingMemory(*device, stagingAllocate);
    stagingBuffer.bindMemory(*stagingMemory, 0);
    {
        void *mapped = stagingMemory.mapMemory(0, stagingRequirements.size);
        auto *cursor = static_cast<std::uint8_t *>(mapped);
        for (const BatchedUpload &upload : uploads)
        {
            std::memcpy(cursor, upload.data.data(), upload.data.size());
            cursor += upload.data.size();
        }
        stagingMemory.unmapMemory();
    }

    for (BatchedUpload &upload : uploads)
    {
        vk::BufferCreateInfo bufferInfo;
        bufferInfo.setSize(upload.data.size());
        bufferInfo.setUsage(upload.usages | vk::BufferUsageFlagBits::eTransferDst);
        bufferInfo.setSharingMode(vk::SharingMode::eExclusive);
        upload.target->buffer                     = std::make_unique<vk::raii::Buffer>(*device, bufferInfo);
        const vk::MemoryRequirements requirements = upload.target->buffer->getMemoryRequirements();
        vk::MemoryAllocateInfo allocateInfo;
        allocateInfo.setAllocationSize(requirements.size);
        allocateInfo.setMemoryTypeIndex(
            findMemoryType(**physicalDevice, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal)
        );
        upload.target->memory = std::make_unique<vk::raii::DeviceMemory>(*device, allocateInfo);
        upload.target->buffer->bindMemory(*upload.target->memory, 0);
    }

    vk::CommandBufferAllocateInfo commandAllocateInfo;
    commandAllocateInfo.setCommandPool(**commandPool);
    commandAllocateInfo.setLevel(vk::CommandBufferLevel::ePrimary);
    commandAllocateInfo.setCommandBufferCount(1);
    vk::raii::CommandBuffer setupBuffer = std::move(vk::raii::CommandBuffers(*device, commandAllocateInfo).front());
    setupBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    vk::DeviceSize offset = 0;
    for (const BatchedUpload &upload : uploads)
    {
        setupBuffer.copyBuffer(*stagingBuffer, *upload.target->buffer, vk::BufferCopy(offset, 0, upload.data.size()));
        offset += upload.data.size();
    }
    setupBuffer.end();
    vk::SubmitInfo submitInfo;
    submitInfo.setCommandBuffers(*setupBuffer);
    graphicsQueue.submit(submitInfo);
    graphicsQueue.waitIdle();
}

void RendererState::allocateSceneDataBuffers()
{
    // One allocation per buffer, at the engine-wide GPU capacity, for the
    // process lifetime: the incremental scene rebuild swaps content, never
    // buffers. The registry's configured cap ([scene] max_objects) may sit
    // anywhere at or below this - raising it is a configuration edit, never a
    // reallocation (scene_capability.hpp documents both numbers).
    //
    // Host visible and persistently mapped, like the camera/environment
    // uniforms: content updates become plain coherent writes on the serialized
    // frame path (device idle - see refreshSceneRegistry), with no staging
    // buffer, no submission and no waitIdle per upload. The shader consumers
    // read the used range every frame and the whole buffer is a few tens of
    // KB, so it lives in cache regardless of heap placement.
    const vk::DeviceSize primitivesCapacity
        = static_cast<vk::DeviceSize>(SceneCapability::gpuMaxObjects) * sizeof(GpuScenePrimitive);
    const vk::DeviceSize materialsCapacity
        = static_cast<vk::DeviceSize>(SceneCapability::gpuMaxObjects) * sizeof(GpuSceneMaterial);

    // Allocation without content: a degenerate stand-in fills the buffer in
    // uploadSceneData right after, so only this function may leave them
    // uncopyable-empty.
    {
        vk::BufferCreateInfo bufferInfo;
        bufferInfo.setSize(primitivesCapacity);
        bufferInfo.setUsage(vk::BufferUsageFlagBits::eStorageBuffer);
        bufferInfo.setSharingMode(vk::SharingMode::eExclusive);
        primitivesBuffer.buffer                   = std::make_unique<vk::raii::Buffer>(*device, bufferInfo);
        const vk::MemoryRequirements requirements = primitivesBuffer.buffer->getMemoryRequirements();
        vk::MemoryAllocateInfo allocateInfo;
        allocateInfo.setAllocationSize(requirements.size);
        allocateInfo.setMemoryTypeIndex(findMemoryType(
            **physicalDevice,
            requirements.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
        ));
        primitivesBuffer.memory = std::make_unique<vk::raii::DeviceMemory>(*device, allocateInfo);
        primitivesBuffer.buffer->bindMemory(*primitivesBuffer.memory, 0);
        primitivesMapped = primitivesBuffer.memory->mapMemory(0, primitivesCapacity);
    }
    {
        vk::BufferCreateInfo bufferInfo;
        bufferInfo.setSize(materialsCapacity);
        bufferInfo.setUsage(vk::BufferUsageFlagBits::eStorageBuffer);
        bufferInfo.setSharingMode(vk::SharingMode::eExclusive);
        materialsBuffer.buffer                    = std::make_unique<vk::raii::Buffer>(*device, bufferInfo);
        const vk::MemoryRequirements requirements = materialsBuffer.buffer->getMemoryRequirements();
        vk::MemoryAllocateInfo allocateInfo;
        allocateInfo.setAllocationSize(requirements.size);
        allocateInfo.setMemoryTypeIndex(findMemoryType(
            **physicalDevice,
            requirements.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
        ));
        materialsBuffer.memory = std::make_unique<vk::raii::DeviceMemory>(*device, allocateInfo);
        materialsBuffer.buffer->bindMemory(*materialsBuffer.memory, 0);
        materialsMapped = materialsBuffer.memory->mapMemory(0, materialsCapacity);
    }
}

void RendererState::uploadSceneData()
{
    // Analytic primitives for the lighting modules' SDF: the exact same
    // objects the geometry pass renders (scene_description.hpp documents the
    // std430 layout the shaders mirror). Always at least one entry: the
    // buffer is handed to the lighting passes even for an empty scene.
    // Member scratch (primitiveUploadScratch): the function runs once per
    // DIRTY frame, and while physics or a script writes transforms that is
    // every frame - the two fresh vectors this used to build per call were
    // heap churn whose size is bounded by the registry capacity anyway.
    // Cleared per call; capacity survives at the high-water mark.
    std::vector<GpuScenePrimitive> &primitives = primitiveUploadScratch;
    primitives.clear();
    // Rev 25: mesh instances may each add ONE occluder proxy (see below), so
    // reserve for the worst case up front.
    primitives.reserve(std::max<std::size_t>(activeObjects.size() * 2, 1));
    for (const SceneObject &object : activeObjects)
    {
        GpuScenePrimitive primitive;
        primitive.positionType
            = {object.position[0],
                object.position[1],
                object.position[2],
                static_cast<float>(static_cast<std::uint32_t>(object.type))};
        // Mesh objects carry their TRUE type into the analytic field: the
        // tracing consumers (sceneSDFOwned) skip mesh-typed primitives - the
        // field stays box/sphere-only, meshes shade through the raster path
        // (the graphics-uplift contract). The previous body tagged meshes as
        // BOX with their bbox half extents, so the 16 km terrain entered the
        // SDF as one world-spanning solid: every shadow ray started INSIDE
        // it and the whole world shaded black. The bbox half extents still
        // ride object.size for the collider/proxy readers; the field just
        // never sees them as occluders any more.
        primitive.sizeRotation    = {object.size[0], object.size[1], object.size[2], object.rotationYRadians};
        primitive.albedoRoughness = {object.albedo[0], object.albedo[1], object.albedo[2], object.roughness};
        // Rotation trig baked for the SDF shaders (scene_description.hpp): the
        // tracing loops must not re-evaluate sin/cos per primitive per step.
        primitive.metallicEmissiveStrength
            = {object.metallic,
                object.emissiveStrength,
                std::sin(object.rotationYRadians),
                std::cos(object.rotationYRadians)};
        primitives.push_back(primitive);

        // ---- rev 25: the mesh OCCLUDER PROXY --------------------------------
        //
        // Meshes cast no SDF shadow - which made every tree, stall and
        // cottage sit in a shadowless puddle of ambient ("lighting is
        // boring"). The fix: for every SMALL mesh instance, append an extra
        // analytic primitive (box or sphere by shape) that spans the mesh's
        // bbox, carrying the instance's transform. The tracer sees it; the
        // raster path is untouched. The original mesh-typed entry stays so
        // the "skip meshes" contract and every index-mapped consumer are
        // unchanged - the proxies only ADD occlusion.
        //
        // The size gate keeps the two world-scale meshes out (the 8 km
        // terrain patch and the 8 km water plane): a world-spanning proxy is
        // exactly the "whole world shades black" bug this field's history
        // records. Everything smaller - tree trunks and canopies (the only
        // mesh kinds the mod spawns besides terrain/water) - becomes a real
        // occluder, and houses' yard props (analytic boxes/spheres already)
        // keep their exact shapes.
        if (object.type == SceneObjectType::Mesh)
        {
            const auto found = meshPool.find(object.meshHandle);
            // rev 34: the instance scale multiplies the mesh proxy's extents
            // and the bounds centre - the traced box must wrap the SCALED
            // mesh the raster pass draws.
            const std::array<float, 3> meshScale = object.scale;
            const float scaledMaxHalf = std::max({object.size[0] * meshScale[0], object.size[1] * meshScale[1],
                object.size[2] * meshScale[2]});
            if (found != meshPool.end() && scaledMaxHalf > 0.05f && scaledMaxHalf < 48.0f
                && found->second.proxyMode == MeshGpu::ProxyMode::Bbox)
            {
                const std::array<float, 3> &center = found->second.localBoundsCenter;
                const std::array<float, 3> &half   = found->second.halfExtents;
                const float maxAxis   = std::max({half[0], half[1], half[2]});
                const float minAxis   = std::min({half[0], half[1], half[2]});
                // Blobby shapes (the noise-deformed canopies) shadow best as
                // a sphere; tall-thin shapes (trunks) as the bbox box.
                const bool blobby = minAxis > 0.55f * maxAxis;
                // The 0.94 inset keeps the proxy just INSIDE the visual
                // silhouette: a shadow ray grazing the drawn canopy leaves
                // the proxy slightly early rather than shadowing air beyond
                // the mesh.
                constexpr float kInset = 0.94f;
                GpuScenePrimitive proxy;
                // The centre offset must rotate by the instance yaw to match
                // the rasterized mesh (model = translate * rotateY). Trig
                // comes from the object, not a re-evaluation.
                const float sinY = std::sin(object.rotationYRadians);
                const float cosY = std::cos(object.rotationYRadians);
                proxy.positionType = {
                    object.position[0] + (cosY * center[0] * meshScale[0] + sinY * center[2] * meshScale[2]),
                    object.position[1] + center[1] * meshScale[1],
                    object.position[2] + (-sinY * center[0] * meshScale[0] + cosY * center[2] * meshScale[2]),
                    blobby ? 1.0f : 0.0f};
                if (blobby)
                {
                    proxy.sizeRotation = {maxAxis * kInset, maxAxis * kInset, maxAxis * kInset, 0.0f};
                } else
                {
                    proxy.sizeRotation = {half[0] * kInset * meshScale[0], half[1] * kInset * meshScale[1],
                        half[2] * kInset * meshScale[2], object.rotationYRadians};
                }
                proxy.albedoRoughness = primitive.albedoRoughness;
                proxy.metallicEmissiveStrength
                    = {object.metallic, object.emissiveStrength, sinY, cosY};
                primitives.push_back(proxy);
            }
            else if (found != meshPool.end() && scaledMaxHalf > 0.05f && scaledMaxHalf < 48.0f
                     && found->second.proxyMode == MeshGpu::ProxyMode::Figure)
            {
                // Rev 28 - the organic character's ONE figure proxy: a slab
                // spanning the whole villager (x/z half 0.30 m, y 0.02..1.85
                // m), replacing the four overlapping part-bbox proxies. One
                // occluder per villager still shades the ground contact and
                // casts the crowd shadow, at a quarter of the primitives.
                const float sinY = std::sin(object.rotationYRadians);
                const float cosY = std::cos(object.rotationYRadians);
                GpuScenePrimitive proxy;
                proxy.positionType = {object.position[0], object.position[1] + 0.935f * meshScale[1],
                                      object.position[2], 0.0f};
                proxy.sizeRotation = {0.30f * meshScale[0], 0.915f * meshScale[1], 0.30f * meshScale[2],
                                      object.rotationYRadians};
                proxy.albedoRoughness = primitive.albedoRoughness;
                proxy.metallicEmissiveStrength
                    = {object.metallic, object.emissiveStrength, sinY, cosY};
                primitives.push_back(proxy);
            }
        }
    }
    if (primitives.empty())
    {
        // Degenerate stand-in with rotation 0: sin 0, cos 1.
        primitives.push_back(
            GpuScenePrimitive{
                .positionType             = {0.0f, 0.0f, 0.0f, 0.0f},
                .sizeRotation             = {0.001f, 0.001f, 0.001f, 0.0f},
                .albedoRoughness          = {0.0f, 0.0f, 0.0f, 1.0f},
                .metallicEmissiveStrength = {0.0f, 0.0f, 0.0f, 1.0f}
            }
        );
    }
    if (primitives.size() > SceneCapability::gpuMaxObjects)
    {
        throw std::runtime_error(
            logPrefix + "scene data exceeds the GPU primitive capacity ("
            + std::to_string(SceneCapability::gpuMaxObjects) + "): the scene registry must enforce the cap"
        );
    }
    // Coherent host writes replace the old staging-copy path: no allocation,
    // no submission, no waitIdle. Bytes beyond the live count are never read
    // (the shaders bound their loops to the primitive count pushed next
    // frame), so only the used range is written.
    std::memcpy(
        primitivesMapped, primitives.data(), static_cast<std::size_t>(primitives.size() * sizeof(GpuScenePrimitive))
    );
    // Rev 25: publish the true primitive count for the frame context.
    primitiveUploadCount = static_cast<std::uint32_t>(primitives.size());

    // Material table: one entry per primitive, resolving each object's
    // texture set name into array layer indices (materials_common.glsl
    // consumes this on the tracing side; the geometry pass receives the same
    // numbers through push constants). Texture sets are the static [scene]
    // world's list: runtime-spawned objects reference the registered sets by
    // name or render flat - new GPU texture layers require a real asset
    // upload path, which the incremental rebuild deliberately does not have.
    //
    // Rev 25: the table must stay INDEX-ALIGNED with the primitives buffer,
    // so every appended occluder proxy gets a duplicate of its source
    // object's entry (the relight/reflection paths read materials[owner]).
    std::vector<GpuSceneMaterial> &materialTable = materialUploadScratch;
    materialTable.clear();
    materialTable.reserve(std::max<std::size_t>(primitives.size(), 1));
    for (const SceneObject &object : activeObjects)
    {
        GpuSceneMaterial material;
        material.layers        = {-1, -1, 0, 0};
        material.scaleAndFlags = {object.uvScale, 0.0f, 0.0f, 0.0f};
        if (!object.textureSet.empty())
        {
            // O(1) name -> layer lookup via the SceneDescription's parallel
            // map (populated at parse time). The previous std::find over the
            // textureSets vector was O(N) per object at rebuild time.
            const std::int32_t layer = scene.findTextureSetLayer(object.textureSet);
            if (layer >= 0)
            {
                material.layers = {layer, layer, 0, 0};
            }
        }
        materialTable.push_back(material);
        // The occluder proxy duplicates the source object's material row so
        // the two vectors stay the same length (see the proxy block above).
        // Rev 28: the Figure/None proxy modes emit at most one proxy per
        // character (the body part only), so the duplicate row rides the
        // SAME predicate the proxy pass uses.
        if (object.type == SceneObjectType::Mesh)
        {
            const auto found = meshPool.find(object.meshHandle);
            const float maxHalf = std::max({object.size[0], object.size[1], object.size[2]});
            if (found != meshPool.end() && maxHalf > 0.05f && maxHalf < 48.0f
                && found->second.proxyMode != MeshGpu::ProxyMode::None)
            {
                materialTable.push_back(material);
            }
        }
    }
    if (materialTable.empty())
    {
        materialTable.push_back(GpuSceneMaterial{.layers = {-1, -1, 0, 0}, .scaleAndFlags = {1.0f, 0.0f, 0.0f, 0.0f}});
    }
    std::memcpy(
        materialsMapped, materialTable.data(), static_cast<std::size_t>(materialTable.size() * sizeof(GpuSceneMaterial))
    );

    // Material diagnostics: which texture set resolved to which array layer,
    // and the loaded data statistics of each set (roughness/metallic means).
    // Without this, a silently failed set registration renders the object
    // with flat defaults and the only symptom is a wrong-looking material.
    // Logged when the material TABLE (or the texture-set data behind it)
    // actually changes: physics write-backs dirty the scene every step, and
    // rebuilding the diagnostic strings per upload drowned the audit trail in
    // transform-only noise on the frame path. A value compare keeps the audit
    // exact - every true material change is logged, in order, exactly once.
    if (runtimeLogger != nullptr && materialDiagnosticsChanged(materialTable))
    {
        for (std::size_t index = 0; index < scene.textureSets.size(); ++index)
        {
            const float roughMean = index < textureSetDataMeans.size() ? textureSetDataMeans[index][0] : -1.0f;
            const float metalMean = index < textureSetDataMeans.size() ? textureSetDataMeans[index][1] : -1.0f;
            runtimeLogger->logMessage(
                LogLevel::Debug,
                logPrefix + "texture set '" + scene.textureSets[index] + "' -> layer " + std::to_string(index)
                    + " (mean roughness " + std::to_string(roughMean) + ", mean metallic " + std::to_string(metalMean)
                    + ")"
            );
        }
        for (std::size_t index = 0; index < activeObjects.size(); ++index)
        {
            const SceneObject &object = activeObjects[index];
            if (!object.textureSet.empty())
            {
                // O(1) lookup through the parallel map; the diagnostic
                // reports the same layer the material table computed.
                const std::int32_t layer = scene.findTextureSetLayer(object.textureSet);
                runtimeLogger->logMessage(
                    LogLevel::Debug,
                    logPrefix + "object " + std::to_string(index) + " texture '" + object.textureSet + "' -> layer "
                        + (layer >= 0 ? std::to_string(layer) : std::string("UNRESOLVED (flat fallback)"))
                );
            }
        }
    }
}

bool RendererState::materialDiagnosticsChanged(
    const std::vector<GpuSceneMaterial> &materialTable
)
{
    // The diagnostic inputs are the uploaded material table plus the texture
    // set count and per-set data means (a late texture load changes the means
    // without touching the table). All three are cheap value compares.
    const bool changed = materialDiagnosticsTable != materialTable || materialDiagnosticsMeans != textureSetDataMeans
                      || materialDiagnosticsSetCount != scene.textureSets.size();
    if (changed)
    {
        materialDiagnosticsTable    = materialTable;
        materialDiagnosticsMeans    = textureSetDataMeans;
        materialDiagnosticsSetCount = scene.textureSets.size();
    }
    return changed;
}

void RendererState::pullEnvironmentOverride()
{
    // The mod-driven atmosphere slice: one capability call plus one integer
    // compare per frame. A new sequence re-resolves the effective exposure
    // and rebuilds the environment buffer (the fog/moon fields live in it);
    // the buffer upload itself happens inside buildEnvironmentBuffer.
    if (sceneCapability == nullptr)
    {
        return;
    }
    const std::uint32_t sequence = sceneCapability->environmentSequence();
    if (environmentOverrideLive && sequence == environmentSequenceSeen)
    {
        // Re-resolve the exposure every frame anyway: a live-configuration
        // reload rewrites `exposure` from config, and the override must win
        // back the very next frame.
        if (environmentOverride.exposure > 0.0f)
        {
            exposure = environmentOverride.exposure;
        }
        return;
    }
    environmentOverride      = sceneCapability->environment();
    environmentSequenceSeen  = sequence;
    environmentOverrideLive  = true;
    exposure = environmentOverride.exposure > 0.0f ? environmentOverride.exposure : configuredExposure;
    buildEnvironmentBuffer();
}

void RendererState::refreshSceneRegistry()
{
    // One frame-at-most re-sync: scene events only set the dirty flag; the
    // work happens here, once per dirty frame, on the serialized frame path
    // (device idle - the coherent host writes race nothing).
    if (sceneCapability != nullptr)
    {
        // The snapshot lands in the retained member buffer through the
        // capability's into-variant (same ascending-handle order, no fresh
        // vector per dirty frame - the by-value objectSnapshot() this used
        // to call allocated and freed a descriptor vector per refresh), and
        // the buffer is owned here, so the conversion loop may move out of
        // its elements.
        sceneCapability->objectSnapshotInto(sceneSnapshotScratch);
        registryScratch.clear();
        registryScratch.reserve(sceneSnapshotScratch.size());
        for (SceneObjectDescriptor &descriptor : sceneSnapshotScratch)
        {
            registryScratch.push_back(sceneObjectFromDescriptor(std::move(descriptor)));
        }
        // Rev 29: the daylight emissive scale applies HERE - after the
        // snapshot conversion, before the primitives upload and the draw
        // cache rebuild both read activeObjects - so the raster G-buffer
        // and the traced relight see ONE scaled value and the
        // relight-vs-raster gates keep their band. The window_set quads
        // also dim their ALBEDO with the same factor: the lit-glass texture
        // kept painting bright yellow panes at noon even with the emissive
        // gone (the 55% A-189 palette mix carries the set's own bright
        // albedo); scaled to a 0.15 floor the panes read as dark day glass
        // and return to full warmth as the glow fades in.
        if (const float glowScale = emissiveDaylightScale(); glowScale < 0.999f)
        {
            const float albedoKeep = 0.15f + 0.85f * glowScale;
            for (SceneObject &converted : registryScratch)
            {
                converted.emissiveStrength *= glowScale;
                if (converted.textureSet == "window_lit")
                {
                    converted.albedo[0] *= albedoKeep;
                    converted.albedo[1] *= albedoKeep;
                    converted.albedo[2] *= albedoKeep;
                }
            }
        }
        // Runtime texture-set growth (the graphics-uplift round): the static
        // [scene] parse registers the FILE's sets only, so every runtime-
        // spawned object referencing a mod texture set resolved to layer -1
        // and rendered flat fallback albedo forever (the flat-terrain
        // symptom). First-use order merge here - the same idiom
        // parseSceneDescription uses - registers the snapshot's new set
        // names; a growth triggers the array rebuild below so the new sets
        // become real GPU layers before the material table resolves.
        bool textureSetsGrew = false;
        for (const SceneObject &converted : registryScratch)
        {
            const std::string &set = converted.textureSet;
            if (!set.empty() && scene.textureSetLayerIndex.find(set) == scene.textureSetLayerIndex.end())
            {
                scene.textureSetLayerIndex.emplace(set, static_cast<std::int32_t>(scene.textureSets.size()));
                scene.textureSets.push_back(set);
                textureSetsGrew = true;
            }
        }
        // No-op short-circuit: physics writes transforms back through the
        // registry every step (and the write-back fires a change event even
        // when the resolved state matches what is already live - a body
        // resting on the ground plane), so this refresh used to pay the full
        // N-object pipeline - the whole snapshot conversion, the
        // primitive/material rebuild, the host writes and the draw-cache
        // rebuild with its per-object matrices - to reproduce bit-identical
        // GPU bytes. An exact value compare (the same idiom the camera
        // uniform signature and the material diagnostics use) runs the
        // pipeline only when content actually moved. Exact equality, no
        // epsilon: the upload would have been bit-identical, so skipping it
        // is unobservable - the dirty flag is consumed either way. The first
        // refresh after the capability connects always runs the full pass
        // (see sceneRegistrySyncedOnce): it is the adopt that uploads the
        // list's meshes, not a re-upload of identical content. A texture-set
        // growth bypasses the short-circuit: the arrays rebuilt below must
        // be followed by a material-table pass that resolves the new layers.
        if (sceneRegistrySyncedOnce && registryScratch == activeObjects && !textureSetsGrew)
        {
            return;
        }
        if (textureSetsGrew)
        {
            rebuildSceneTextureArrays();
        }
        sceneRegistrySyncedOnce = true;
        // Swap, not move: the swap keeps BOTH buffers alive - activeObjects
        // adopts the fresh content, and registryScratch keeps the previous
        // buffer as its build capacity for the next refresh, so a dirty-frame
        // run stops touching the heap after its high-water mark (the moved-from
        // vector would have dropped its buffer and re-allocated every
        // changed frame).
        activeObjects.swap(registryScratch);
        // New mesh handles first: uploadSceneData's proxy boxes reference
        // half extents the objects already carry, but the geometry pass can
        // only draw meshes the pool holds.
        ensureMeshesUploaded();
    } else
    {
        activeObjects = scene.objects;
    }
    uploadSceneData();
    // Draw state (matrices, texture layers, mesh bindings) resolves against
    // exactly the list this refresh produced - after the mesh uploads, so a
    // just-registered mesh binds the frame after it appears.
    rebuildObjectDrawCache();
}

void RendererState::rebuildObjectDrawCache()
{
    // One pass over the active objects: everything the per-frame geometry
    // pass used to recompute per frame (the model matrix's sin/cos, the
    // texture-set hash, the mesh-pool hash) resolves here, once per dirty
    // scene instead of once per frame. Same inputs, same results - the pass
    // below only stopped paying for them every frame.
    objectDrawCache.clear();
    objectDrawCache.reserve(activeObjects.size());
    for (const SceneObject &object : activeObjects)
    {
        ObjectDrawCache cached;
        const bool isMeshObject = object.type == SceneObjectType::Mesh;
        // Analytic primitives are unit meshes scaled by half extents; a
        // registered mesh is authored in meters and scales by its OWN
        // per-axis instance scale (rev 34: the character-assembly parts;
        // the default 1 preserves the old translate * rotateY contract).
        const float *scale = isMeshObject ? object.scale.data() : object.size.data();
        cached.model             = modelMatrix(object.position.data(), object.rotationYRadians, scale);
        cached.albedoLayer       = object.textureSet.empty() ? -1 : scene.findTextureSetLayer(object.textureSet);
        // Rev 25: the water plane flags itself through its texture set; the
        // geometry pass rides the flag into meshBox.w for the shader's
        // ripple/glitter branch.
        cached.waterFlag         = (object.textureSet == "water") ? 1.0f : 0.0f;
        // Rev 31 (the layered depth buffer): which pass draws this object.
        // The water plane is the first semi-transparent citizen: it leaves the
        // opaque G-buffer (layer 0) and fills the transparency layer instead,
        // so the composite can blend it over the seabed/sky behind it with a
        // per-pixel depth-delta opacity. The class comes from the descriptor;
        // the "water" texture set is the data-driven backstop (a static
        // scene.toml water row classifies itself without a mod).
        cached.transparency      = object.transparency;
        if (object.textureSet == "water"
            && cached.transparency == SceneTransparency::Opaque)
        {
            cached.transparency = SceneTransparency::SemiTransparent;
        }
        if (isMeshObject)
        {
            const auto found = meshPool.find(object.meshHandle);
            if (found != meshPool.end())
            {
                cached.mesh = &found->second.geometry;
            }
        }
        objectDrawCache.push_back(cached);
    }
}

void RendererState::ensureMeshesUploaded()
{
    if (sceneCapability == nullptr)
    {
        return;
    }
    // Pending meshes are COLLECTED first, then uploaded in one batch: a dirty
    // frame that introduces several registry meshes (a glTF import, a script
    // spawning instances) pays one staging allocation, one submission and one
    // waitIdle instead of one round trip per mesh. Validation and failure
    // bookkeeping stay per mesh and unchanged: a mesh that fails validation
    // never enters the batch, and a batch that fails on the device side
    // erases exactly the handles it carried (meshUploadFailed keeps the
    // skip-quietly contract; a later scene change retries nothing).
    std::vector<std::pair<std::uint32_t, std::size_t>> batchedHandles; // handle -> index into batch
    std::vector<BatchedUpload> batch;
    for (const SceneObject &object : activeObjects)
    {
        if (object.type != SceneObjectType::Mesh || object.meshHandle == 0)
        {
            continue;
        }
        if (meshPool.find(object.meshHandle) != meshPool.end()
            || meshUploadFailed.find(object.meshHandle) != meshUploadFailed.end())
        {
            continue;
        }
        SceneMeshDescriptor mesh;
        if (!sceneCapability->mesh(object.meshHandle, mesh))
        {
            // The registry validated the handle at addObject; failing here
            // means a foreign or stale handle. Skip precisely, once.
            if (runtimeLogger != nullptr)
            {
                runtimeLogger->logMessage(
                    LogLevel::Error,
                    logPrefix + "mesh handle " + std::to_string(object.meshHandle)
                        + " is not registered in the scene registry; the object renders nothing"
                );
            }
            meshUploadFailed.emplace(object.meshHandle, true);
            continue;
        }
        // A registry mesh with no vertices or no indices would enqueue a
        // 0-byte BatchedUpload; creating its staging buffer then throws
        // (VUID-VkBufferCreateInfo-size-00912) and the batch-wide catch would
        // fail every mesh in the batch for one empty one. Skip it precisely.
        if (mesh.vertices.empty() || mesh.indices.empty())
        {
            if (runtimeLogger != nullptr)
            {
                runtimeLogger->logMessage(
                    LogLevel::Error,
                    logPrefix + "mesh handle " + std::to_string(object.meshHandle) + " carries "
                        + std::to_string(mesh.vertices.size() / 6) + " vertices and "
                        + std::to_string(mesh.indices.size() / 3)
                        + " triangles; an empty mesh cannot be uploaded and renders nothing"
                );
            }
            meshUploadFailed.emplace(object.meshHandle, true);
            continue;
        }
        // One buffer per mesh: vertices (6 floats each) then indices.
        const vk::DeviceSize vertexBytes = static_cast<vk::DeviceSize>(mesh.vertices.size()) * sizeof(float);
        const vk::DeviceSize indexBytes  = static_cast<vk::DeviceSize>(mesh.indices.size()) * sizeof(std::uint32_t);
        BatchedUpload upload;
        upload.data.resize(static_cast<std::size_t>(vertexBytes + indexBytes));
        if (vertexBytes > 0)
        {
            std::memcpy(upload.data.data(), mesh.vertices.data(), static_cast<std::size_t>(vertexBytes));
        }
        if (indexBytes > 0)
        {
            std::memcpy(upload.data.data() + vertexBytes, mesh.indices.data(), static_cast<std::size_t>(indexBytes));
        }
        MeshGpu &gpu                  = meshPool[object.meshHandle];
        gpu.geometry.indexCount       = static_cast<std::uint32_t>(mesh.indices.size());
        gpu.geometry.indexOffsetBytes = vertexBytes;
        gpu.indexCount                = gpu.geometry.indexCount;
        // Rev 28: the per-vocabulary proxy mode from the registered name
        // (see MeshGpu::ProxyMode - the organic character parts share one
        // figure proxy instead of four overlapping bbox proxies).
        if (mesh.name.rfind("npc_body", 0) == 0)
        {
            gpu.proxyMode = MeshGpu::ProxyMode::Figure;
        }
        else if (mesh.name.rfind("npc_", 0) == 0)
        {
            gpu.proxyMode = MeshGpu::ProxyMode::None;
        }
        // Rev 31 (the inhabited-land round + the GI cost round): the ground
        // cover vocabulary gets NO occluder proxy either. Roads, crop rows,
        // pier planks and boat hulls are 8-35 cm tall - their "shadows" are
        // sub-pixel noise, and every proxy is one more primitive the SDF
        // march scans per step (the march cost is linear in the primitive
        // count, and the capture rig's frame budget proved primitive-bound).
        else if (mesh.name.rfind("prop_road", 0) == 0
                 || mesh.name.rfind("prop_crops", 0) == 0
                 || mesh.name.rfind("prop_dock_plank", 0) == 0
                 || mesh.name.rfind("prop_boat", 0) == 0)
        {
            gpu.proxyMode = MeshGpu::ProxyMode::None;
        }
        // Rev 25 (the shadow round): capture the mesh's local bbox for the
        // SDF proxy pass. The registry already measured boundsMin/boundsMax
        // from the same vertices, so no second scan runs here.
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const float lo                 = mesh.boundsMin[axis];
            const float hi                 = mesh.boundsMax[axis];
            gpu.localBoundsCenter[axis]    = (lo + hi) * 0.5f;
            gpu.halfExtents[axis]          = std::max((hi - lo) * 0.5f, 0.0f);
        }
        // The heightfield shadow grid: exactly one mesh exceeds 1 km on a
        // side (the terrain patch - the water plane is also 8 km on the XZ
        // axes, but it is FLAT, so its heights are constant and the grid is
        // degenerate; the >1 km test on the Y extent as well keeps the fill
        // terrain-only). Filled once - the terrain never moves.
        const float maxHalf = std::max({gpu.halfExtents[0], gpu.halfExtents[1], gpu.halfExtents[2]});
        if (!heightfieldShadow.filled && maxHalf > 1000.0f && gpu.halfExtents[1] > 4.0f)
        {
            try
            {
                fillHeightfieldImage(mesh, mesh.boundsMin, mesh.boundsMax);
            } catch (const std::exception &error)
            {
                // A failed shadow grid degrades to the pre-rev-25 look (no
                // terrain self-shadowing) - never a failed frame.
                heightfieldShadow.filled = false;
                if (runtimeLogger != nullptr)
                {
                    runtimeLogger->logMessage(
                        LogLevel::Error, logPrefix + "heightfield shadow grid failed: " + error.what()
                    );
                }
            }
        }
        upload.target                 = &gpu.geometry;
        upload.usages                 = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer;
        batchedHandles.emplace_back(object.meshHandle, batch.size());
        batch.push_back(std::move(upload));
        if (runtimeLogger != nullptr)
        {
            runtimeLogger->logMessage(
                LogLevel::Information,
                logPrefix + "mesh " + std::to_string(object.meshHandle) + " queued for upload ("
                    + std::to_string(mesh.vertices.size() / 6) + " vertices, " + std::to_string(mesh.indices.size() / 3)
                    + " triangles)"
            );
        }
    }
    if (batch.empty())
    {
        return;
    }
    try
    {
        uploadBatchToDeviceLocal(batch);
        if (runtimeLogger != nullptr && batch.size() > 1)
        {
            runtimeLogger->logMessage(
                LogLevel::Information,
                logPrefix + "mesh batch uploaded in one submission (" + std::to_string(batch.size()) + " meshes)"
            );
        }
    } catch (const std::exception &error)
    {
        // The batch failed as a whole: release every geometry it carried and
        // mark each handle failed, exactly like the per-mesh path did.
        for (const auto &[meshHandle, batchIndex] : batchedHandles)
        {
            meshPool.erase(meshHandle);
            meshUploadFailed.emplace(meshHandle, true);
        }
        if (runtimeLogger != nullptr)
        {
            runtimeLogger->logMessage(LogLevel::Error, logPrefix + "mesh upload failed: " + error.what());
        }
    }
}

void RendererState::fillHeightfieldImage(
    const SceneMeshDescriptor &mesh, const std::array<float, 3> &boundsMin,
    const std::array<float, 3> &boundsMax
)
{
    // The terrain heightfield shadow grid (rev 25): one R32_SFLOAT texel per
    // grid cell over the mesh's XZ footprint, holding the normalized height
    // of the grid vertex nearest that cell. The vertex lattice is a regular
    // grid (TerrainField::meshGrid -> the bridge's subsample), so each
    // vertex maps to exactly one texel by rounding its XZ onto the lattice.
    // Heights normalize to [0, 1] across [heightMin, heightMax]; the
    // consumers recover metres through the heightfieldParams pushed with
    // the frame context.
    constexpr std::uint32_t kGrid = 256;
    const float sizeX  = std::max(boundsMax[0] - boundsMin[0], 1.0f);
    const float sizeZ  = std::max(boundsMax[2] - boundsMin[2], 1.0f);
    const float invX   = static_cast<float>(kGrid - 1) / sizeX;
    const float invZ   = static_cast<float>(kGrid - 1) / sizeZ;
    const float yLo    = boundsMin[1];
    const float ySpan  = std::max(boundsMax[1] - boundsMin[1], 0.01f);

    std::vector<float> grid(static_cast<std::size_t>(kGrid) * kGrid, 0.0f);
    std::vector<std::uint32_t> seen(static_cast<std::size_t>(kGrid) * kGrid, 0u);
    const std::size_t vertexCount = mesh.vertices.size() / 6;
    for (std::size_t v = 0; v < vertexCount; ++v)
    {
        const float x = mesh.vertices[v * 6 + 0];
        const float y = mesh.vertices[v * 6 + 1];
        const float z = mesh.vertices[v * 6 + 2];
        const std::int32_t gx = std::clamp(
            static_cast<std::int32_t>(std::lround((x - boundsMin[0]) * invX)), 0, static_cast<std::int32_t>(kGrid - 1));
        const std::int32_t gz = std::clamp(
            static_cast<std::int32_t>(std::lround((z - boundsMin[2]) * invZ)), 0, static_cast<std::int32_t>(kGrid - 1));
        const std::size_t cell = static_cast<std::size_t>(gz) * kGrid + static_cast<std::size_t>(gx);
        // A sub-sampled lattice can drop several source rows into one cell;
        // keep the MAXIMUM height - for sun shadows from above, the tallest
        // terrain in a cell is the conservative occluder.
        const float normalized = std::clamp((y - yLo) / ySpan, 0.0f, 1.0f);
        if (seen[cell] == 0u || normalized > grid[cell])
        {
            grid[cell]     = normalized;
            seen[cell]     = 1u;
        }
    }
    // Unvisited cells (a lattice coarser than the grid leaves none, but a
    // defensive zero = heightMin = the bbox floor keeps their rays clear).

    vk::ImageCreateInfo imageInfo;
    imageInfo.setImageType(vk::ImageType::e2D);
    imageInfo.setFormat(vk::Format::eR32Sfloat);
    imageInfo.setExtent(vk::Extent3D(kGrid, kGrid, 1));
    imageInfo.setMipLevels(1);
    imageInfo.setArrayLayers(1);
    imageInfo.setSamples(vk::SampleCountFlagBits::e1);
    imageInfo.setTiling(vk::ImageTiling::eOptimal);
    imageInfo.setUsage(vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled);
    imageInfo.setSharingMode(vk::SharingMode::eExclusive);
    imageInfo.setInitialLayout(vk::ImageLayout::eUndefined);
    heightfieldShadow.image = std::make_unique<vk::raii::Image>(*device, imageInfo);
    const vk::MemoryRequirements requirements = heightfieldShadow.image->getMemoryRequirements();
    vk::MemoryAllocateInfo allocateInfo;
    allocateInfo.setAllocationSize(requirements.size);
    allocateInfo.setMemoryTypeIndex(
        findMemoryType(**physicalDevice, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal)
    );
    heightfieldShadow.memory = std::make_unique<vk::raii::DeviceMemory>(*device, allocateInfo);
    heightfieldShadow.image->bindMemory(*heightfieldShadow.memory, 0);

    const vk::DeviceSize texelBytes = static_cast<vk::DeviceSize>(grid.size()) * sizeof(float);
    vk::BufferCreateInfo stagingInfo;
    stagingInfo.setSize(texelBytes);
    stagingInfo.setUsage(vk::BufferUsageFlagBits::eTransferSrc);
    stagingInfo.setSharingMode(vk::SharingMode::eExclusive);
    vk::raii::Buffer stagingBuffer(*device, stagingInfo);
    const vk::MemoryRequirements stagingRequirements = stagingBuffer.getMemoryRequirements();
    vk::MemoryAllocateInfo stagingAllocate;
    stagingAllocate.setAllocationSize(stagingRequirements.size);
    stagingAllocate.setMemoryTypeIndex(
        findMemoryType(
            **physicalDevice, stagingRequirements.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
        )
    );
    vk::raii::DeviceMemory stagingMemory(*device, stagingAllocate);
    stagingBuffer.bindMemory(*stagingMemory, 0);
    void *mapped = stagingMemory.mapMemory(0, stagingRequirements.size);
    std::memcpy(mapped, grid.data(), static_cast<std::size_t>(texelBytes));
    stagingMemory.unmapMemory();

    vk::CommandBufferAllocateInfo commandAllocateInfo;
    commandAllocateInfo.setCommandPool(**commandPool);
    commandAllocateInfo.setLevel(vk::CommandBufferLevel::ePrimary);
    commandAllocateInfo.setCommandBufferCount(1);
    vk::raii::CommandBuffer setupBuffer = std::move(vk::raii::CommandBuffers(*device, commandAllocateInfo).front());
    setupBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    vk::ImageMemoryBarrier toTransfer;
    toTransfer.setImage(*heightfieldShadow.image);
    toTransfer.setSrcAccessMask(vk::AccessFlagBits::eNone);
    toTransfer.setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
    toTransfer.setOldLayout(vk::ImageLayout::eUndefined);
    toTransfer.setNewLayout(vk::ImageLayout::eTransferDstOptimal);
    toTransfer.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    setupBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags{},
        {}, {}, toTransfer
    );
    vk::BufferImageCopy region;
    region.setImageSubresource(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1));
    region.setImageExtent(vk::Extent3D(kGrid, kGrid, 1));
    setupBuffer.copyBufferToImage(*stagingBuffer, *heightfieldShadow.image, vk::ImageLayout::eTransferDstOptimal, region);
    vk::ImageMemoryBarrier toShaderRead = toTransfer;
    toShaderRead.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite);
    toShaderRead.setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    toShaderRead.setOldLayout(vk::ImageLayout::eTransferDstOptimal);
    toShaderRead.setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
    setupBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader, vk::DependencyFlags{},
        {}, {}, toShaderRead
    );
    setupBuffer.end();
    vk::SubmitInfo submitInfo;
    submitInfo.setCommandBuffers(*setupBuffer);
    graphicsQueue.submit(submitInfo);
    graphicsQueue.waitIdle();

    vk::ImageViewCreateInfo viewInfo;
    viewInfo.setImage(*heightfieldShadow.image);
    viewInfo.setViewType(vk::ImageViewType::e2D);
    viewInfo.setFormat(vk::Format::eR32Sfloat);
    viewInfo.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    heightfieldShadow.view = std::make_unique<vk::raii::ImageView>(*device, viewInfo);

    heightfieldShadow.worldSize = sizeX; // the patch is square
    heightfieldShadow.heightMin = yLo;
    heightfieldShadow.heightMax = boundsMax[1];
    heightfieldShadow.meshMin   = boundsMin;
    heightfieldShadow.meshMax   = boundsMax;
    heightfieldShadow.filled    = true;
    if (runtimeLogger != nullptr)
    {
        runtimeLogger->logMessage(
            LogLevel::Information,
            logPrefix + "heightfield shadow grid filled (" + std::to_string(kGrid) + "x" + std::to_string(kGrid)
                + ", world " + std::to_string(sizeX) + " m, heights " + std::to_string(yLo) + ".."
                + std::to_string(boundsMax[1]) + " m)"
        );
    }
}

float RendererState::emissiveDaylightScale() const
{
    // Rev 29 (the lit-windows-at-night round): the cottages' window quads
    // carried ONE authored emissive strength, so every midday capture
    // printed flat glowing-yellow rectangles over the facades - lamps at
    // noon. The scale rides the LIVE environment override: full glow at
    // night (the moon branch) and under a low sun, fading to 0 as the sun
    // climbs past ~22 degrees elevation. No override (the static-scene
    // unit tests) keeps the authored value: the scale is 1.
    if (!environmentOverrideLive)
    {
        return 1.0f;
    }
    if (environmentOverride.moonIntensity > 0.0f)
    {
        return 1.0f;
    }
    const std::array<float, 3> &dir = environmentOverride.sunDirection;
    const float lengthSq = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    if (lengthSq < 1e-8f)
    {
        // No live sun direction (the override drives fog/exposure only):
        // keep the authored glow.
        return 1.0f;
    }
    // The override's vector points TOWARDS the scene (the travel
    // direction), so the elevation sine is the negated normalized y - the
    // same convention the bridge's golden-hour curve uses.
    const float elevation = -dir[1] / std::sqrt(lengthSq);
    const float t = std::clamp((elevation - 0.08f) / 0.30f, 0.0f, 1.0f);
    const float smooth = t * t * (3.0f - 2.0f * t);
    return 1.0f - smooth;
}

SceneObject RendererState::sceneObjectFromDescriptor(
    SceneObjectDescriptor object
)
{
    // By value + move: the caller's descriptor is an owned element of the
    // retained snapshot buffer (objectSnapshotInto fills it; the next
    // refresh overwrites it), so the textureSet string and the arrays move
    // into the converted object instead of copying the string per object per
    // refresh.
    SceneObject converted;
    converted.type     = object.type == scene_object_type::sphere
                           ? SceneObjectType::Sphere
                           : (object.type == scene_object_type::mesh ? SceneObjectType::Mesh : SceneObjectType::Box);
    converted.position = object.position;
    converted.size     = object.size;
    converted.rotationYRadians = object.rotationYRadians;
    converted.albedo           = object.albedo;
    converted.roughness        = object.roughness;
    converted.metallic         = object.metallic;
    converted.emissiveStrength = object.emissiveStrength;
    converted.textureSet       = std::move(object.textureSet);
    converted.uvScale          = object.uvScale;
    converted.meshHandle       = object.meshHandle;
    converted.scale            = object.scale;
    converted.transparency     = object.transparency >= scene_object_transparency::fully_transparent
                                     ? SceneTransparency::FullyTransparent
                                     : (object.transparency >= scene_object_transparency::semi_transparent
                                            ? SceneTransparency::SemiTransparent
                                            : SceneTransparency::Opaque);
    return converted;
}

SceneObjectDescriptor RendererState::sceneObjectToDescriptor(
    const SceneObject &object
)
{
    SceneObjectDescriptor converted;
    converted.type             = static_cast<std::uint32_t>(object.type);
    converted.position         = object.position;
    converted.size             = object.size;
    converted.rotationYRadians = object.rotationYRadians;
    converted.albedo           = object.albedo;
    converted.roughness        = object.roughness;
    converted.metallic         = object.metallic;
    converted.emissiveStrength = object.emissiveStrength;
    converted.textureSet       = object.textureSet;
    converted.uvScale          = object.uvScale;
    converted.meshHandle       = object.meshHandle;
    converted.transparency     = static_cast<std::uint32_t>(object.transparency);
    return converted;
}

void RendererState::createSceneGeometry()
{
    const MeshData box    = buildBoxMesh();
    const MeshData sphere = buildSphereMesh(sphereMeshSlices, sphereMeshStacks);

    auto uploadMesh = [this](const MeshData &mesh, GeometryBuffer &target) {
        const vk::DeviceSize vertexBytes = mesh.vertices.size() * sizeof(float);
        const vk::DeviceSize indexBytes  = mesh.indices.size() * sizeof(std::uint32_t);
        std::vector<std::uint8_t> combined(static_cast<std::size_t>(vertexBytes + indexBytes));
        std::memcpy(combined.data(), mesh.vertices.data(), static_cast<std::size_t>(vertexBytes));
        std::memcpy(combined.data() + vertexBytes, mesh.indices.data(), static_cast<std::size_t>(indexBytes));
        uploadToDeviceLocal(
            target,
            combined.data(),
            static_cast<vk::DeviceSize>(combined.size()),
            vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer
        );
        target.indexCount       = static_cast<std::uint32_t>(mesh.indices.size());
        target.indexOffsetBytes = static_cast<std::size_t>(vertexBytes);
    };
    uploadMesh(box, boxMesh);
    uploadMesh(sphere, sphereMesh);

    // The static [scene] world is the initial active object list; when the
    // scene capability connects (start phase), the registry takes over and
    // this vector is re-synced from it (refreshSceneRegistry).
    if (activeObjects.empty())
    {
        activeObjects = scene.objects;
    }
    rebuildObjectDrawCache();

    // Primitive and material buffers: allocated ONCE at full registry
    // capacity, content re-uploaded on every scene change (the incremental
    // rebuild contract in scene_capability.hpp).
    allocateSceneDataBuffers();

    // Analytic primitives + material table: uploaded from activeObjects (the
    // static world now, the registry snapshot once the scene capability
    // connected). uploadSceneData is called again on every scene change; only
    // the buffer CONTENT changes, never the buffer handles.
    createTextureArrays();
    uploadSceneData();

    buildEnvironmentBuffer();

    // Camera uniform: host visible and persistently mapped; rebuilt on resize
    // (the aspect ratio feeds the projection).
    vk::BufferCreateInfo cameraInfo;
    cameraInfo.setSize(sizeof(CameraData));
    cameraInfo.setUsage(vk::BufferUsageFlagBits::eUniformBuffer);
    cameraInfo.setSharingMode(vk::SharingMode::eExclusive);
    cameraBufferStorage.buffer                      = std::make_unique<vk::raii::Buffer>(*device, cameraInfo);
    const vk::MemoryRequirements cameraRequirements = cameraBufferStorage.buffer->getMemoryRequirements();
    vk::MemoryAllocateInfo cameraAllocate;
    cameraAllocate.setAllocationSize(cameraRequirements.size);
    cameraAllocate.setMemoryTypeIndex(findMemoryType(
        **physicalDevice,
        cameraRequirements.memoryTypeBits,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
    ));
    cameraBufferStorage.memory = std::make_unique<vk::raii::DeviceMemory>(*device, cameraAllocate);
    cameraBufferStorage.buffer->bindMemory(*cameraBufferStorage.memory, 0);
    cameraMapped = cameraBufferStorage.memory->mapMemory(0, sizeof(CameraData));
    updateCameraBuffer();
}

void RendererState::createTextureArrays()
{
    // One albedo+data layer pair per registered texture set. Data packing
    // (materials_common.glsl documents the channels): R roughness, G
    // metallic, B/A tangent-space normal XY. Missing maps fall back to
    // neutral values: roughness 0.5, metallic 0, identity normal (0.5, 0.5).
    loadTextureSets();
    createArrayImages();
    uploadArrayLayersAndGenerateMips();

    vk::SamplerCreateInfo samplerInfo;
    samplerInfo.setMagFilter(vk::Filter::eLinear);
    samplerInfo.setMinFilter(vk::Filter::eLinear);
    samplerInfo.setMipmapMode(vk::SamplerMipmapMode::eLinear);
    samplerInfo.setAddressModeU(vk::SamplerAddressMode::eRepeat);
    samplerInfo.setAddressModeV(vk::SamplerAddressMode::eRepeat);
    samplerInfo.setAddressModeW(vk::SamplerAddressMode::eRepeat);
    samplerInfo.setMinLod(0.0f);
    samplerInfo.setMaxLod(static_cast<float>(textureArrayMipLevels));
    textureSampler = std::make_unique<vk::raii::Sampler>(*device, samplerInfo);
}

void RendererState::loadTextureSets()
{
    const std::size_t setCount = scene.textureSets.size();
    if (setCount > 0 && textureDirectory.empty())
    {
        // The engine ships no content: texture sets come from a mod package,
        // which provides [renderer].texture_directory through its shipped
        // defaults. A scene naming texture sets without a directory is a
        // precise configuration problem, not a silent flat-material scene.
        std::string setList;
        for (const std::string &textureSet : scene.textureSets)
        {
            setList += (setList.empty() ? "" : ", ") + textureSet;
        }
        throw std::runtime_error(
            logPrefix + "the scene references texture set(s) [" + setList
            + "] but [renderer].texture_directory is not configured; the content mod "
              "that ships the textures normally provides it"
        );
    }
    // One mean pair per set, indexed by the set's array layer: sized up
    // front (resize value-initializes the slots to {0, 0}) so a set that
    // fails to load cannot shift every later set's mean into the wrong layer
    // - the push_back-then-resize this replaced misattributed means whenever
    // a middle set had no Color.png.
    textureSetDataMeans.clear();
    textureSetDataMeans.resize(setCount);
    loadedTextureSets.clear();
    loadedTextureSets.resize(setCount);
    loadedArrayExtent   = vk::Extent2D{1, 1};
    anyTextureSetLoaded = false;

    // The decode is CPU-bound, per-set independent work - each set reads
    // only its own files and writes only its own scratch slot - and stb_image
    // decodes are reentrant per call. That makes it exactly the coarse-
    // grained batch the kernel's parallelFor exists for, and it runs by
    // default: the sets decode across the machine's cores instead of one
    // inflate pass after another on the startup path. Everything that
    // TOUCHES shared state - the cross-set resolution contract, the member
    // vectors, the warning log - stays in the serial join below. A throwing
    // decode thread (a set whose maps disagree with its own color map) is
    // rethrown by parallelFor after the join, so the failure stays a precise
    // startup error naming the offending set.
    struct DecodedTextureSet
    {
        std::vector<std::uint8_t> albedo; // RGBA8 pixels; empty until Color.png loads
        std::vector<std::uint8_t> data;   // packed data texel; empty until any map loads
        std::uint32_t width  = 0;
        std::uint32_t height = 0;
        bool colorLoaded     = false;
        std::array<float, 2> dataMean{0.0f, 0.0f};
        std::string setDirectory; // for the flat-fallback warning below
    };
    std::vector<DecodedTextureSet> decoded(setCount);

    parallelFor(setCount, [this, &decoded](std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index)
        {
            const std::filesystem::path setDirectory = resolvedTextureDirectory / scene.textureSets[index];
            DecodedTextureSet &loaded                = decoded[index];

            int albedoWidth                       = 0;
            int albedoHeight                      = 0;
            int albedoChannels                    = 0;
            const std::filesystem::path colorPath = setDirectory / colorMapFileName;
            if (stbi_uc *color = stbi_load(colorPath.string().c_str(), &albedoWidth, &albedoHeight, &albedoChannels, 4))
            {
                const std::size_t pixelCount
                    = static_cast<std::size_t>(albedoWidth) * static_cast<std::size_t>(albedoHeight);
                loaded.albedo.resize(pixelCount * 4);
                std::memcpy(loaded.albedo.data(), color, pixelCount * 4);
                stbi_image_free(color);
                loaded.width       = static_cast<std::uint32_t>(albedoWidth);
                loaded.height      = static_cast<std::uint32_t>(albedoHeight);
                loaded.colorLoaded = true;

                // Loads one single source channel of `fileName` into the packed
                // data texel's `channel` slot. `requestedComponents` selects how
                // many components stb_image decodes (1 = red only, 2 = RG) and
                // `sourceComponent` which of those lands in the slot: the normal
                // map's X and Y live in the RED and GREEN channels of NormalGL.png
                // respectively - decoding with 1 component twice would return the
                // RED channel both times, collapsing every normal to the +X+Y
                // diagonal and skewing the shading of every textured surface.
                // The optional second-slot pair lets one decode feed two slots
                // (the normal map's X and Y): the previous two-call shape for
                // that map stbi_load'ed the same file twice - two full inflate +
                // conversion passes per texture set - to move one byte per pixel
                // into each slot. The size check runs against the set's OWN
                // color extent (the cross-set contract is the serial join's
                // business), so the error message stays set-local.
                const auto loadDataChannel = [&](const char *fileName,
                                                 int channel,
                                                 int requestedComponents,
                                                 int sourceComponent, // NOLINT(readability-identifier-length)
                                                 int secondChannel         = -1,
                                                 int secondSourceComponent = -1) {
                    const std::filesystem::path path = setDirectory / fileName;
                    int width                        = 0;
                    int height                       = 0;
                    int channels                     = 0;
                    if (stbi_uc *map
                        = stbi_load(path.string().c_str(), &width, &height, &channels, requestedComponents))
                    {
                        if (static_cast<std::uint32_t>(width) != loaded.width
                            || static_cast<std::uint32_t>(height) != loaded.height)
                        {
                            throw std::runtime_error(
                                logPrefix + "texture set '" + scene.textureSets[index] + "' map '" + fileName
                                + "' does not match the set's color map size"
                            );
                        }
                        const std::size_t mapPixels
                            = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
                        if (loaded.data.size() != mapPixels * 4)
                        {
                            loaded.data.resize(mapPixels * 4);
                            // Neutral data texel: roughness 0.5, metallic 0,
                            // identity normal XY (0.5, 0.5). Per-channel strided
                            // fill - a flat std::fill_n would misalign the
                            // interleaved channels.
                            for (std::size_t pixel = 0; pixel < mapPixels; ++pixel)
                            {
                                std::uint8_t *texel = loaded.data.data() + pixel * 4;
                                std::copy(neutralDataTexel.begin(), neutralDataTexel.end(), texel);
                            }
                        }
                        for (std::size_t pixel = 0; pixel < mapPixels; ++pixel)
                        {
                            loaded.data[pixel * 4 + channel] = map[pixel * requestedComponents + sourceComponent];
                        }
                        if (secondChannel >= 0)
                        {
                            for (std::size_t pixel = 0; pixel < mapPixels; ++pixel)
                            {
                                loaded.data[pixel * 4 + secondChannel]
                                    = map[pixel * requestedComponents + secondSourceComponent];
                            }
                        }
                        stbi_image_free(map);
                    }
                };
                loadDataChannel(roughnessMapFileName.c_str(), dataChannelRoughness, 1, 0);
                loadDataChannel(metalnessMapFileName.c_str(), dataChannelMetallic, 1, 0);
                // One decode serves both normal slots: X = red channel, Y = green.
                loadDataChannel(normalMapFileName.c_str(), dataChannelNormalX, 2, 0, dataChannelNormalY, 1);

                // Startup diagnostics: mean roughness/metallic of the packed
                // data layer (logged with the material table below).
                const std::size_t dataPixels = loaded.data.size() / 4;
                double roughSum              = 0.0;
                double metalSum              = 0.0;
                for (std::size_t pixel = 0; pixel < dataPixels; ++pixel)
                {
                    roughSum += loaded.data[pixel * 4 + 0];
                    metalSum += loaded.data[pixel * 4 + 1];
                }
                loaded.dataMean
                    = {static_cast<float>(dataPixels > 0 ? roughSum / (dataPixels * 255.0) : 0.0),
                        static_cast<float>(dataPixels > 0 ? metalSum / (dataPixels * 255.0) : 0.0)};
            }
            loaded.setDirectory = setDirectory.string();
        }
    });

    // Serial join, in set order: the cross-set resolution contract (every
    // set shares the array's extent - the first LOADED set defines it) and
    // the move into the member vectors happen on one thread. Sets without a
    // Color.png keep their empty scratch slots: the staging upload pads an
    // empty layer with exactly the stand-in content this set advertises
    // (opaque white albedo, the neutral data texel), so the flat-material
    // fallback is byte-identical to the previous in-loop assignment.
    for (std::size_t index = 0; index < setCount; ++index)
    {
        DecodedTextureSet &set = decoded[index];
        if (!set.colorLoaded)
        {
            if (runtimeLogger != nullptr)
            {
                runtimeLogger->logMessage(
                    LogLevel::Warning,
                    logPrefix + "texture set '" + scene.textureSets[index] + "' has no Color.png under '"
                        + set.setDirectory + "'; objects using it fall back to flat materials"
                );
            }
            continue;
        }
        if (anyTextureSetLoaded && (loadedArrayExtent.width != set.width || loadedArrayExtent.height != set.height))
        {
            throw std::runtime_error(
                logPrefix + "texture set '" + scene.textureSets[index] + "' is " + std::to_string(set.width) + "x"
                + std::to_string(set.height) + " but earlier sets are " + std::to_string(loadedArrayExtent.width) + "x"
                + std::to_string(loadedArrayExtent.height) + "; every texture set in a scene must share one resolution"
            );
        }
        loadedArrayExtent               = vk::Extent2D(set.width, set.height);
        anyTextureSetLoaded             = true;
        loadedTextureSets[index].albedo = std::move(set.albedo);
        loadedTextureSets[index].data   = std::move(set.data);
        textureSetDataMeans[index]      = set.dataMean;
    }
}

void RendererState::createArrayImages()
{
    const std::size_t setCount = scene.textureSets.size();
    const std::uint32_t layers = static_cast<std::uint32_t>(std::max<std::size_t>(setCount, 1));
    textureArrayMipLevels = 1; // TEMP PROBE E (uplift round): mip chain off
    (void)anyTextureSetLoaded;
    arrayLayerCount       = layers;

    // Layout plan: UNDEFINED -> transfer dst (upload) -> transfer src/dst
    // (mip chain) -> SHADER_READ_ONLY_OPTIMAL. The arrays are only ever
    // sampled afterwards, so they rest in the read-only layout for life.
    const auto createArray = [&](FrameImage &target, vk::Format format) {
        vk::ImageCreateInfo imageInfo;
        imageInfo.setImageType(vk::ImageType::e2D);
        imageInfo.setFormat(format);
        imageInfo.setExtent(vk::Extent3D(loadedArrayExtent.width, loadedArrayExtent.height, 1));
        imageInfo.setMipLevels(textureArrayMipLevels);
        imageInfo.setArrayLayers(layers);
        imageInfo.setSamples(vk::SampleCountFlagBits::e1);
        imageInfo.setTiling(vk::ImageTiling::eOptimal);
        imageInfo.setUsage(
            vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc
            | vk::ImageUsageFlagBits::eSampled
        );
        imageInfo.setSharingMode(vk::SharingMode::eExclusive);
        imageInfo.setInitialLayout(vk::ImageLayout::eUndefined);
        target.image                              = std::make_unique<vk::raii::Image>(*device, imageInfo);
        const vk::MemoryRequirements requirements = target.image->getMemoryRequirements();
        vk::MemoryAllocateInfo allocate;
        allocate.setAllocationSize(requirements.size);
        allocate.setMemoryTypeIndex(
            findMemoryType(**physicalDevice, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal)
        );
        target.memory = std::make_unique<vk::raii::DeviceMemory>(*device, allocate);
        target.image->bindMemory(*target.memory, 0);
        vk::ImageViewCreateInfo viewInfo;
        viewInfo.setImage(**target.image);
        viewInfo.setViewType(vk::ImageViewType::e2DArray);
        viewInfo.setFormat(format);
        viewInfo.setSubresourceRange(
            vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, textureArrayMipLevels, 0, layers)
        );
        target.view = std::make_unique<vk::raii::ImageView>(*device, viewInfo);
    };

    createArray(albedoArrayImage, textureAlbedoFormat);
    createArray(dataArrayImage, textureDataFormat);

    if (!anyTextureSetLoaded)
    {
        // No texture sets: the 1x1x1 stand-ins still need a defined layout -
        // every material-consuming shader interface references the arrays,
        // and the validation layer checks the declared layout even when a
        // branch never reaches the sampling instruction.
        vk::CommandBufferAllocateInfo allocateInfo;
        allocateInfo.setCommandPool(**commandPool);
        allocateInfo.setLevel(vk::CommandBufferLevel::ePrimary);
        allocateInfo.setCommandBufferCount(1);
        vk::raii::CommandBuffer setupBuffer = std::move(vk::raii::CommandBuffers(*device, allocateInfo).front());
        setupBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
        std::array<vk::ImageMemoryBarrier2, 2> barriers;
        const vk::Image arrayImages[2] = {*albedoArrayImage.image, *dataArrayImage.image};
        for (std::size_t index = 0; index < 2; ++index)
        {
            barriers[index].setSrcStageMask(vk::PipelineStageFlagBits2::eTopOfPipe);
            barriers[index].setSrcAccessMask(vk::AccessFlags2());
            barriers[index].setDstStageMask(vk::PipelineStageFlagBits2::eAllCommands);
            barriers[index].setDstAccessMask(vk::AccessFlagBits2::eShaderRead);
            barriers[index].setOldLayout(vk::ImageLayout::eUndefined);
            barriers[index].setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
            barriers[index].setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            barriers[index].setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            barriers[index].setImage(arrayImages[index]);
            barriers[index].setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
        }
        vk::DependencyInfo dependency;
        dependency.setImageMemoryBarriers(barriers);
        setupBuffer.pipelineBarrier2(dependency);
        setupBuffer.end();
        vk::SubmitInfo submitInfo;
        submitInfo.setCommandBuffers(*setupBuffer);
        graphicsQueue.submit(submitInfo);
        graphicsQueue.waitIdle();
    }
}

void RendererState::uploadArrayLayersAndGenerateMips()
{
    const std::uint32_t layers = arrayLayerCount;
    if (anyTextureSetLoaded)
    {
        // One staging buffer for both arrays; layer i's bytes start at
        // (2*layerIndex + array) * layerSize (4 bytes per pixel, no padding
        // because the extent is a multiple of 4 bytes per row).
        const std::size_t layerBytes = static_cast<std::size_t>(loadedArrayExtent.width) * loadedArrayExtent.height * 4;
        // Normalize every set to one full layer before the staging copy: a
        // set with missing maps carries only 4-byte stand-ins (loadTextureSets
        // assigns them and resizes only when a map actually loads), while the
        // single staging upload below memcpys layerBytes from EVERY set - the
        // unnormalized reads would run layerBytes-4 bytes past the stand-in.
        // Padded regions get exactly the fallback content those sets
        // advertise: opaque white albedo, the neutral data texel (strided
        // per-channel fill - a flat std::fill_n would misalign the
        // interleaved channels).
        for (LoadedTextureSet &loaded : loadedTextureSets)
        {
            loaded.albedo.resize(layerBytes, 255);
            if (loaded.data.size() < layerBytes)
            {
                const std::size_t firstPaddingTexel = loaded.data.size() / 4;
                loaded.data.resize(layerBytes);
                for (std::size_t pixel = firstPaddingTexel; pixel < layerBytes / 4; ++pixel)
                {
                    std::uint8_t *texel = loaded.data.data() + pixel * 4;
                    std::copy(neutralDataTexel.begin(), neutralDataTexel.end(), texel);
                }
            }
        }
        std::vector<std::uint8_t> staging(loadedTextureSets.size() * 2 * layerBytes);
        for (std::size_t index = 0; index < loadedTextureSets.size(); ++index)
        {
            std::memcpy(staging.data() + (index * 2) * layerBytes, loadedTextureSets[index].albedo.data(), layerBytes);
            std::memcpy(
                staging.data() + (index * 2 + 1) * layerBytes, loadedTextureSets[index].data.data(), layerBytes
            );
        }

        vk::BufferCreateInfo bufferInfo;
        bufferInfo.setSize(static_cast<vk::DeviceSize>(staging.size()));
        bufferInfo.setUsage(vk::BufferUsageFlagBits::eTransferSrc);
        bufferInfo.setSharingMode(vk::SharingMode::eExclusive);
        vk::raii::Buffer stagingBuffer(*device, bufferInfo);
        const vk::MemoryRequirements stagingRequirements = stagingBuffer.getMemoryRequirements();
        vk::MemoryAllocateInfo stagingAllocate;
        stagingAllocate.setAllocationSize(stagingRequirements.size);
        stagingAllocate.setMemoryTypeIndex(findMemoryType(
            **physicalDevice,
            stagingRequirements.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
        ));
        vk::raii::DeviceMemory stagingMemory(*device, stagingAllocate);
        stagingBuffer.bindMemory(*stagingMemory, 0);
        void *mapped = stagingMemory.mapMemory(0, stagingRequirements.size);
        std::memcpy(mapped, staging.data(), staging.size());
        stagingMemory.unmapMemory();

        vk::CommandBufferAllocateInfo allocateInfo;
        allocateInfo.setCommandPool(**commandPool);
        allocateInfo.setLevel(vk::CommandBufferLevel::ePrimary);
        allocateInfo.setCommandBufferCount(1);
        vk::raii::CommandBuffer setupBuffer = std::move(vk::raii::CommandBuffers(*device, allocateInfo).front());
        setupBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

        const auto uploadArray = [&](const FrameImage &target, bool dataArray) {
            std::vector<vk::BufferImageCopy> copies(loadedTextureSets.size());
            for (std::size_t index = 0; index < loadedTextureSets.size(); ++index)
            {
                // Staging layout: albedo layer i at (2i)*layerBytes, data
                // layer i at (2i+1)*layerBytes. The copy lands in ARRAY
                // LAYER index - baseArrayLayer is the third subresource
                // member and must be set explicitly (a zero here silently
                // writes every set into layer 0, last one wins).
                copies[index].setBufferOffset(
                    static_cast<vk::DeviceSize>((index * 2 + (dataArray ? 1 : 0)) * layerBytes)
                );
                copies[index].setBufferRowLength(0);
                copies[index].setBufferImageHeight(0);
                copies[index].setImageSubresource(
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, static_cast<std::uint32_t>(index), 1)
                );
                copies[index].setImageOffset(vk::Offset3D(0, 0, 0));
                copies[index].setImageExtent(vk::Extent3D(loadedArrayExtent.width, loadedArrayExtent.height, 1));
            }
            setupBuffer.copyBufferToImage(stagingBuffer, *target.image, vk::ImageLayout::eTransferDstOptimal, copies);
        };

        // UNDEFINED -> TRANSFER_DST for the copy, then TRANSFER_SRC for the
        // blit chain.
        std::array<vk::ImageMemoryBarrier2, 2> enterBarriers;
        const vk::Image arrayImages[2] = {*albedoArrayImage.image, *dataArrayImage.image};
        for (std::size_t index = 0; index < 2; ++index)
        {
            enterBarriers[index].setSrcStageMask(vk::PipelineStageFlagBits2::eTopOfPipe);
            enterBarriers[index].setSrcAccessMask(vk::AccessFlags2());
            enterBarriers[index].setDstStageMask(vk::PipelineStageFlagBits2::eTransfer);
            enterBarriers[index].setDstAccessMask(vk::AccessFlagBits2::eTransferWrite);
            enterBarriers[index].setOldLayout(vk::ImageLayout::eUndefined);
            enterBarriers[index].setNewLayout(vk::ImageLayout::eTransferDstOptimal);
            enterBarriers[index].setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            enterBarriers[index].setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            enterBarriers[index].setImage(arrayImages[index]);
            enterBarriers[index].setSubresourceRange(
                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers)
            );
        }
        {
            vk::DependencyInfo dependency;
            dependency.setImageMemoryBarriers(enterBarriers);
            setupBuffer.pipelineBarrier2(dependency);
        }
        uploadArray(albedoArrayImage, false);
        uploadArray(dataArrayImage, true);

        // Mipmap chain per layer: blit level l-1 into level l for every
        // layer, halving the extent (the last level clamps to 1x1). Each
        // source level moves TRANSFER_DST -> TRANSFER_SRC right before it is
        // read; each destination level enters UNDEFINED -> TRANSFER_DST
        // before the first write (explicit per-level transitions: mip levels
        // are independent subresources with independent layouts).
        for (std::uint32_t level = 1; level < textureArrayMipLevels; ++level)
        {
            std::array<vk::ImageMemoryBarrier2, 4> levelBarriers;
            const vk::Image arrayImages2[2] = {*albedoArrayImage.image, *dataArrayImage.image};
            for (std::size_t index = 0; index < 2; ++index)
            {
                // Source level: written last iteration (or by the upload for
                // level 0), now read by the blit.
                levelBarriers[index * 2].setSrcStageMask(vk::PipelineStageFlagBits2::eTransfer);
                levelBarriers[index * 2].setSrcAccessMask(vk::AccessFlagBits2::eTransferWrite);
                levelBarriers[index * 2].setDstStageMask(vk::PipelineStageFlagBits2::eTransfer);
                levelBarriers[index * 2].setDstAccessMask(vk::AccessFlagBits2::eTransferRead);
                levelBarriers[index * 2].setOldLayout(vk::ImageLayout::eTransferDstOptimal);
                levelBarriers[index * 2].setNewLayout(vk::ImageLayout::eTransferSrcOptimal);
                levelBarriers[index * 2].setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
                levelBarriers[index * 2].setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
                levelBarriers[index * 2].setImage(arrayImages2[index]);
                levelBarriers[index * 2].setSubresourceRange(
                    vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, level - 1, 1, 0, layers)
                );
                // Destination level: first use.
                levelBarriers[index * 2 + 1].setSrcStageMask(vk::PipelineStageFlagBits2::eTopOfPipe);
                levelBarriers[index * 2 + 1].setSrcAccessMask(vk::AccessFlags2());
                levelBarriers[index * 2 + 1].setDstStageMask(vk::PipelineStageFlagBits2::eTransfer);
                levelBarriers[index * 2 + 1].setDstAccessMask(vk::AccessFlagBits2::eTransferWrite);
                levelBarriers[index * 2 + 1].setOldLayout(vk::ImageLayout::eUndefined);
                levelBarriers[index * 2 + 1].setNewLayout(vk::ImageLayout::eTransferDstOptimal);
                levelBarriers[index * 2 + 1].setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
                levelBarriers[index * 2 + 1].setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
                levelBarriers[index * 2 + 1].setImage(arrayImages2[index]);
                levelBarriers[index * 2 + 1].setSubresourceRange(
                    vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, level, 1, 0, layers)
                );
            }
            {
                vk::DependencyInfo dependency;
                dependency.setImageMemoryBarriers(levelBarriers);
                setupBuffer.pipelineBarrier2(dependency);
            }

            const std::uint32_t sourceWidth  = std::max(1u, loadedArrayExtent.width >> (level - 1));
            const std::uint32_t sourceHeight = std::max(1u, loadedArrayExtent.height >> (level - 1));
            const std::uint32_t targetWidth  = std::max(1u, loadedArrayExtent.width >> level);
            const std::uint32_t targetHeight = std::max(1u, loadedArrayExtent.height >> level);
            std::vector<vk::ImageBlit2> blits(layers);
            for (std::uint32_t layer = 0; layer < layers; ++layer)
            {
                blits[layer].setSrcSubresource(
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, level - 1, layer, 1)
                );
                blits[layer].setSrcOffsets(
                    {vk::Offset3D(0, 0, 0),
                        vk::Offset3D(
                            static_cast<std::int32_t>(sourceWidth), static_cast<std::int32_t>(sourceHeight), 1
                        )}
                );
                blits[layer].setDstSubresource(
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, level, layer, 1)
                );
                blits[layer].setDstOffsets(
                    {vk::Offset3D(0, 0, 0),
                        vk::Offset3D(
                            static_cast<std::int32_t>(targetWidth), static_cast<std::int32_t>(targetHeight), 1
                        )}
                );
            }
            vk::BlitImageInfo2 blitInfo;
            blitInfo.setSrcImage(*albedoArrayImage.image);
            blitInfo.setSrcImageLayout(vk::ImageLayout::eTransferSrcOptimal);
            blitInfo.setDstImage(*albedoArrayImage.image);
            blitInfo.setDstImageLayout(vk::ImageLayout::eTransferDstOptimal);
            blitInfo.setRegionCount(static_cast<std::uint32_t>(blits.size()));
            blitInfo.setPRegions(blits.data());
            blitInfo.setFilter(vk::Filter::eLinear);
            setupBuffer.blitImage2(blitInfo);
            blitInfo.setSrcImage(*dataArrayImage.image);
            blitInfo.setDstImage(*dataArrayImage.image);
            setupBuffer.blitImage2(blitInfo);
        }

        // Everything -> SHADER_READ_ONLY_OPTIMAL for the engine's lifetime.
        // Sources: level 0..N-2 rest in TRANSFER_SRC, level N-1 in
        // TRANSFER_DST (both hold after the last blit); a single-level chain
        // (no mips) only has the TRANSFER_DST copy.
        {
            std::vector<vk::ImageMemoryBarrier2> finalBarriers;
            for (std::size_t index = 0; index < 2; ++index)
            {
                const vk::Image image = arrayImages[index];
                if (textureArrayMipLevels > 1)
                {
                    vk::ImageMemoryBarrier2 sourceLevels;
                    sourceLevels.setSrcStageMask(vk::PipelineStageFlagBits2::eTransfer);
                    sourceLevels.setSrcAccessMask(
                        vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eTransferRead
                    );
                    sourceLevels.setDstStageMask(vk::PipelineStageFlagBits2::eAllCommands);
                    sourceLevels.setDstAccessMask(vk::AccessFlagBits2::eShaderRead);
                    sourceLevels.setOldLayout(vk::ImageLayout::eTransferSrcOptimal);
                    sourceLevels.setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
                    sourceLevels.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
                    sourceLevels.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
                    sourceLevels.setImage(image);
                    sourceLevels.setSubresourceRange(
                        vk::ImageSubresourceRange(
                            vk::ImageAspectFlagBits::eColor, 0, textureArrayMipLevels - 1, 0, layers
                        )
                    );
                    finalBarriers.push_back(sourceLevels);
                }
                vk::ImageMemoryBarrier2 topLevel;
                topLevel.setSrcStageMask(vk::PipelineStageFlagBits2::eTransfer);
                topLevel.setSrcAccessMask(vk::AccessFlagBits2::eTransferWrite);
                topLevel.setDstStageMask(vk::PipelineStageFlagBits2::eAllCommands);
                topLevel.setDstAccessMask(vk::AccessFlagBits2::eShaderRead);
                topLevel.setOldLayout(vk::ImageLayout::eTransferDstOptimal);
                topLevel.setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
                topLevel.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
                topLevel.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
                topLevel.setImage(image);
                topLevel.setSubresourceRange(
                    vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, textureArrayMipLevels - 1, 1, 0, layers)
                );
                finalBarriers.push_back(topLevel);
            }
            vk::DependencyInfo dependency;
            dependency.setImageMemoryBarriers(finalBarriers);
            setupBuffer.pipelineBarrier2(dependency);
        }
        setupBuffer.end();
        vk::SubmitInfo submitInfo;
        submitInfo.setCommandBuffers(*setupBuffer);
        graphicsQueue.submit(submitInfo);
        graphicsQueue.waitIdle();
    }
}

void RendererState::rebuildSceneTextureArrays()
{
    // The runtime texture-set growth path (the graphics-uplift round). The
    // refresh runs serialized on the frame path with the device idle (its
    // own contract), so destroying and recreating the arrays here races
    // nothing. Order matters: the descriptor set dies first (it references
    // the old views), then the arrays rebuild through the same three phases
    // createTextureArrays runs at init, then the set re-allocates and binds
    // the new views. The material table's layer resolution happens later in
    // the same refresh (uploadSceneData), against the map the merge filled.
    sceneDescriptorSet.reset();
    loadTextureSets();
    createArrayImages();
    uploadArrayLayersAndGenerateMips();
    writeSceneDescriptors();
    if (runtimeLogger != nullptr)
    {
        runtimeLogger->logMessage(
            LogLevel::Information, logPrefix + "texture arrays rebuilt for " + std::to_string(scene.textureSets.size())
                + " texture set(s) (runtime set growth)"
        );
    }
}

void RendererState::buildEnvironmentBuffer()
{
    // Sky gradient from [scene.sky] (defaults to the flat sky_radiance) plus
    // the sun disk. The disk radiance is the sun's irradiance divided by its
    // solid angle, so the disk integrates back to exactly the configured sun
    // intensity: bright enough for ACES to clamp, without adding energy.
    EnvironmentData environment{};
    environment.skyZenith  = {scene.sky.zenith[0], scene.sky.zenith[1], scene.sky.zenith[2], skyZenithBlendExponent};
    environment.skyHorizon = {scene.sky.horizon[0], scene.sky.horizon[1], scene.sky.horizon[2], skyGroundBlendExponent};
    environment.skyGround  = {scene.sky.ground[0], scene.sky.ground[1], scene.sky.ground[2], 0.0f};

    float sunIntensity            = 0.0f;
    std::array<float, 3> sunColor = scene.sun.color;
    // The sun description rides on the scene (parsed from [scene.sun]); the
    // environment buffer is rebuilt whenever the scene changes.
    sunIntensity                      = scene.sun.intensity;
    std::array<float, 3> sunDirection = scene.sun.direction;
    float directionLength             = std::sqrt(
        sunDirection[0] * sunDirection[0] + sunDirection[1] * sunDirection[1] + sunDirection[2] * sunDirection[2]
    );
    if (directionLength < 1e-6f)
    {
        directionLength = 1.0f;
    }
    for (float &component : sunDirection)
    {
        component /= directionLength;
    }

    const bool diskEnabled   = scene.sky.sunDisk && sunIntensity > 0.0f;
    const float innerDegrees = std::max(scene.sky.sunDiskSizeDegrees * 0.5f, sunDiskMinSizeDegrees);
    const float outerDegrees = innerDegrees + sunDiskSoftLimbDegrees;
    const float cosInner     = std::cos(degreesToRadians(innerDegrees));
    const float cosOuter     = std::cos(degreesToRadians(outerDegrees));
    const float solidAngle   = twoPi * (1.0f - cosInner);
    const float diskRadiance = solidAngle > 1e-8f ? sunIntensity / solidAngle : 0.0f;
    environment.skyGround[3] = diskEnabled ? 1.0f : 0.0f;
    // [scene.sun].direction points TOWARDS the scene (the light's travel
    // direction); the disk sits in the sky at the OPPOSITE direction - the
    // one an observer looks towards to see the sun.
    environment.sunDiskDirection = {-sunDirection[0], -sunDirection[1], -sunDirection[2], cosInner};
    environment.sunDiskRadiance
        = {sunColor[0] * diskRadiance, sunColor[1] * diskRadiance, sunColor[2] * diskRadiance, cosOuter};

    // The runtime atmosphere override (the mod's environment state) merges
    // OVER the static [scene] base: sky gradient, fog, moon and the sun's
    // disk color/intensity when the override drives them (the sun's DIRECT
    // light comes from the lighting provider; the override's sun intensity
    // re-scales the DISK radiance so the visible sun and the direct light
    // agree). Neutral defaults when nothing was pushed: clear air, no moon.
    environment.fogDensityTint         = {0.0f, 0.7f, 0.7f, 0.7f};
    environment.moonDirectionIntensity = {0.0f, 0.0f, 0.0f, 0.0f};
    if (environmentOverrideLive)
    {
        const SceneEnvironmentOverride &overrideState = environmentOverride;
        environment.skyZenith                         = {overrideState.skyZenith[0], overrideState.skyZenith[1],
                                                         overrideState.skyZenith[2], skyZenithBlendExponent};
        environment.skyHorizon                        = {overrideState.skyHorizon[0], overrideState.skyHorizon[1],
                                                         overrideState.skyHorizon[2], skyGroundBlendExponent};
        environment.skyGround                         = {overrideState.skyGround[0], overrideState.skyGround[1],
                                                         overrideState.skyGround[2], diskEnabled ? 1.0f : 0.0f};
        environment.fogDensityTint = {overrideState.fogDensity, overrideState.fogTint[0], overrideState.fogTint[1],
            overrideState.fogTint[2]};
        environment.moonDirectionIntensity = {overrideState.moonDirection[0], overrideState.moonDirection[1],
            overrideState.moonDirection[2], overrideState.moonIntensity};
        if (overrideState.sunIntensity >= 0.0f)
        {
            const float overrideRadiance = solidAngle > 1e-8f ? overrideState.sunIntensity / solidAngle : 0.0f;
            const std::array<float, 3> &overrideColor    = overrideState.sunColor;
            environment.sunDiskRadiance = {overrideColor[0] * overrideRadiance, overrideColor[1] * overrideRadiance,
                overrideColor[2] * overrideRadiance, cosOuter};
        }
        // rev 26: the live sun direction - the visible disk tracks the light
        // the pbr module applies (a zero vector keeps the static direction).
        // The override's vector points TOWARDS the scene (the travel
        // direction), so the disk sits at its opposite, like the static
        // path above.
        const float overrideDirLength = std::sqrt(
            overrideState.sunDirection[0] * overrideState.sunDirection[0]
            + overrideState.sunDirection[1] * overrideState.sunDirection[1]
            + overrideState.sunDirection[2] * overrideState.sunDirection[2]);
        if (overrideDirLength > 1e-5f)
        {
            environment.sunDiskDirection = {
                -overrideState.sunDirection[0] / overrideDirLength,
                -overrideState.sunDirection[1] / overrideDirLength,
                -overrideState.sunDirection[2] / overrideDirLength, cosInner};
        }
    }

    if (environmentBufferStorage.buffer == nullptr)
    {
        vk::BufferCreateInfo environmentInfo;
        environmentInfo.setSize(sizeof(EnvironmentData));
        environmentInfo.setUsage(vk::BufferUsageFlagBits::eUniformBuffer);
        environmentInfo.setSharingMode(vk::SharingMode::eExclusive);
        environmentBufferStorage.buffer = std::make_unique<vk::raii::Buffer>(*device, environmentInfo);
        const vk::MemoryRequirements environmentRequirements = environmentBufferStorage.buffer->getMemoryRequirements();
        vk::MemoryAllocateInfo environmentAllocate;
        environmentAllocate.setAllocationSize(environmentRequirements.size);
        environmentAllocate.setMemoryTypeIndex(findMemoryType(
            **physicalDevice,
            environmentRequirements.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
        ));
        environmentBufferStorage.memory = std::make_unique<vk::raii::DeviceMemory>(*device, environmentAllocate);
        environmentBufferStorage.buffer->bindMemory(*environmentBufferStorage.memory, 0);
        environmentMapped = environmentBufferStorage.memory->mapMemory(0, sizeof(EnvironmentData));
    }
    if (environmentMapped != nullptr)
    {
        std::memcpy(environmentMapped, &environment, sizeof(EnvironmentData));
    }
}

std::int32_t RendererState::resolvedTonemapMode()
{
    // The mode string is startup/reload data, the passes run every frame:
    // resolve on change (tracked by the string it was computed from) and
    // hand the cached integer to both composite paths. Same mapping the
    // inline ternary chain used to produce, unknown names stay tonemapNone.
    if (tonemapMode != tonemapModeResolvedFrom)
    {
        tonemapModeResolvedFrom = tonemapMode;
        tonemapModeValue        = tonemapMode == "aces"     ? tonemapAces
                                : tonemapMode == "reinhard" ? tonemapReinhard
                                                            : tonemapNone;
    }
    return tonemapModeValue;
}

void RendererState::updateCameraBuffer()
{
    // Skip the whole rebuild - four matrix builds, one of them a full
    // Gauss-Jordan inversion - when no camera input moved since the last
    // upload. The serialized frame path calls this every frame; a static
    // camera pays one struct compare instead.
    const CameraUniformSignature signature{
        .position                   = scene.camera.position,
        .lookAt                     = scene.camera.lookAt,
        .verticalFieldOfViewDegrees = scene.camera.verticalFieldOfViewDegrees,
        .nearPlane                  = scene.camera.nearPlane,
        .farPlane                   = scene.camera.farPlane,
        .extentWidth                = swapchainExtent.width,
        .extentHeight               = swapchainExtent.height
    };
    if (cameraUniformValid && signature == cameraUniformSignature)
    {
        return;
    }
    cameraUniformSignature = signature;
    cameraUniformValid     = true;

    const float aspect = swapchainExtent.height > 0
                           ? static_cast<float>(swapchainExtent.width) / static_cast<float>(swapchainExtent.height)
                           : 1.0f;
    const Mat4 view    = lookAtMatrix(
        scene.camera.position.data(), scene.camera.lookAt.data(), std::array<float, 3>{0.0f, 1.0f, 0.0f}.data()
    );
    const Mat4 projection = perspectiveMatrix(
        degreesToRadians(scene.camera.verticalFieldOfViewDegrees), aspect, scene.camera.nearPlane, scene.camera.farPlane
    );
    const Mat4 viewProjection = multiplyMatrix(projection, view);
    // CameraData is the GPU-side layout (render_interface.hpp mirrors the
    // shader's std430 block): plain float arrays, GLM value_ptr copies the
    // column-major matrix into exactly that storage.
    std::memcpy(cameraData.viewProjection.data(), glm::value_ptr(viewProjection), sizeof(cameraData.viewProjection));
    std::memcpy(
        cameraData.inverseViewProjection.data(),
        glm::value_ptr(inverseMatrix(viewProjection)),
        sizeof(cameraData.inverseViewProjection)
    );
    cameraData.position = {scene.camera.position[0], scene.camera.position[1], scene.camera.position[2], 0.0f};
    if (cameraMapped != nullptr)
    {
        std::memcpy(cameraMapped, &cameraData, sizeof(CameraData));
    }
}

} // namespace engine
