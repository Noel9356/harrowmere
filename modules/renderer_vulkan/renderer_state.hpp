#ifndef ENGINE_MODULES_RENDERER_RENDERER_STATE_HPP
#define ENGINE_MODULES_RENDERER_RENDERER_STATE_HPP

// Module-private shared state for the renderer's translation units. The
// renderer is one state struct (RendererState) implemented across several
// .cpp files split by concern - device/swapchain, scene resources, pipelines,
// frame recording - instead of one 3000+ line file. This header is the only
// place those units meet; it is not part of any mod-facing contract.

#include "camera_controller.hpp"
#include "frame_sync_policy.hpp"
#include "render_interface.hpp"
#include "renderer_overlay_capability.hpp"
#include "scene_capability.hpp"
#include "scene_description.hpp"
#include "scene_math.hpp"
#include "logging_capability.hpp"
#include "renderer_capability.hpp"
#include "ui_capability.hpp"
#include "window_capability.hpp"
#include "engine/kernel/event_bus.hpp"

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine
{

inline constexpr const char *validationLayerName     = "VK_LAYER_KHRONOS_validation";
inline constexpr const char *swapchainExtensionName  = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
inline constexpr const char *debugUtilsExtensionName = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
// One backend key shared by the instance-extension query and the presentation
// surface request; the window contract switches on it.
inline constexpr const char *vulkanBackendName         = "vulkan";
inline constexpr std::uint64_t frameTimeoutNanoseconds = 1000000000ULL;
// Camera integration clamps the per-frame delta against pathological stalls
// (debugger pauses, window drags): a five-second gap must not teleport
// the camera.
inline constexpr double maxCameraDeltaTimeSeconds = 0.1;
// The capture readback needs the frame fence, but a capture is a rare,
// correctness-critical event: give slow frames a generous window instead of
// the 1 s presentation budget, whose timeout silently discarded captures.
inline constexpr std::uint64_t captureTimeoutNanoseconds = 60000000000ULL;
inline constexpr std::size_t bytesPerPixel               = 4;
inline constexpr std::size_t maxPushConstantBytes        = 128;
// G-buffer attachment formats. Named once: the frame images (createFrame-
// Resources), the scene pipeline's rendering info, and the format comments in
// render_interface.hpp must agree, and a raw literal at each site would let
// them drift silently.
inline constexpr vk::Format gbufferAlbedoFormat = vk::Format::eR8G8B8A8Unorm;      // rgb albedo, a roughness
inline constexpr vk::Format hdrColorFormat      = vk::Format::eR16G16B16A16Sfloat; // normal, emissive, radiance, dummy
inline constexpr vk::Format gbufferDepthFormat  = vk::Format::eD32Sfloat;
// Texture-array formats (independent of the G-buffer formats above, even
// where the enum values coincide).
inline constexpr vk::Format textureAlbedoFormat = vk::Format::eR8G8B8A8Srgb;
inline constexpr vk::Format textureDataFormat   = vk::Format::eR8G8B8A8Unorm;
// Texture set file names (ambientCG layout) and their slots in the packed
// data texel (R roughness, G metallic, BA normal XY - materials_common.glsl).
inline constexpr const char *colorMapName     = "Color.png";
inline constexpr const char *roughnessMapName = "Roughness.png";
inline constexpr const char *metalnessMapName = "Metalness.png";
inline constexpr const char *normalMapName    = "NormalGL.png";
inline constexpr int dataChannelRoughness     = 0;
inline constexpr int dataChannelMetallic      = 1;
inline constexpr int dataChannelNormalX       = 2;
inline constexpr int dataChannelNormalY       = 3;
// Neutral packed-data texel for pixels whose source map is missing:
// roughness 0.5, metallic 0, identity tangent normal (0.5, 0.5).
inline constexpr std::array<std::uint8_t, 4> neutralDataTexel{128, 0, 128, 128};
// Sky model shape ([scene.sky] feeds the values; these are the fixed parts):
// gradient falloff exponents and the sun disk's soft limb in degrees.
inline constexpr float skyZenithBlendExponent = 0.6f;
inline constexpr float skyGroundBlendExponent = 0.7f;
inline constexpr float sunDiskMinSizeDegrees  = 0.05f;
inline constexpr float sunDiskSoftLimbDegrees = 0.15f;
// Physical device scoring: a deterministic preference order for multi-device
// machines. Bonus applies to whichever device class is preferred
// ([renderer].prefer_discrete_device); devices without both queue families
// are already filtered before scoring, so the penalty only ranks CPU/other
// types below GPUs; the limits bonus is a stable tie-breaker.
inline constexpr std::int32_t preferredDeviceScoreBonus      = 1000;
inline constexpr std::int32_t nonQueueDeviceScorePenalty     = -100;
inline constexpr std::uint32_t maxImageDimensionScoreCap     = 10000u;
inline constexpr std::uint32_t maxImageDimensionScoreDivisor = 1000u;
// Instance identity reported to the Vulkan loader (application and engine
// name/version intentionally match: one product).
inline constexpr const char *vulkanApplicationName = "engine";
// Kept in step with engineVersionString (kernel/version.hpp) and the root
// CMakeLists project() VERSION - one product version.
inline constexpr std::uint32_t vulkanInstanceVersion = VK_MAKE_VERSION(0, 1, 0);
// Composite descriptor layout: bindings 0..6 are the image inputs, 7/8 the
// environment and camera uniforms. The composite shader declares exactly this
// layout (scene_composite.frag).
inline constexpr std::uint32_t compositeImageBindingCount  = 7;
inline constexpr std::uint32_t compositeEnvironmentBinding = 7;
inline constexpr std::uint32_t compositeCameraBinding      = 8;
// Scene descriptor layout (scene.vert / scene.frag): bindings 2/3 are
// reserved for the shared lighting set the techniques bind; the scene pass
// only uses camera, primitives and the two texture arrays.
inline constexpr std::uint32_t sceneCameraBinding      = 0;
inline constexpr std::uint32_t scenePrimitivesBinding  = 1;
inline constexpr std::uint32_t sceneAlbedoArrayBinding = 4;
inline constexpr std::uint32_t sceneDataArrayBinding   = 5;
// Tonemapping operator codes handed to the composite shader through push
// constants; scene_composite.frag switches on the same numbers.
inline constexpr std::int32_t tonemapNone     = 0;
inline constexpr std::int32_t tonemapAces     = 1;
inline constexpr std::int32_t tonemapReinhard = 2;
// Log prefix of every message this module emits (named once so a rename or a
// typo cannot split the module's messages across log filters).
inline const std::string logPrefix = "renderer module: ";

// Push-constant blocks as the shaders see them. Declared at file scope so the
// pipeline layouts can size their ranges with sizeof instead of restating the
// byte counts as magic literals, and so the static_asserts below fail at
// compile time when a block outgrows the push-constant budget (128/32 bytes
// are the shader-side expectations; scene.vert/scene.frag and
// scene_composite.frag mirror these layouts).
struct ObjectPush
{
    Mat4 model;
    std::array<float, 4> albedoRoughness;
    std::array<float, 4> metallicEmissiveStrength;
    std::array<float, 4> materialLayers;
    // Mesh objects only (SceneObjectType::Mesh): the registered mesh's
    // bounding-box half extents, the SAME numbers the tracing proxy box
    // carries (sizeRotation). The fragment shader derives mesh UVs from them
    // with the box parameterization, so raster and relight sample identical
    // texels. Unused (zeros) for analytic primitives.
    std::array<float, 4> meshBox;
};

struct CompositePush
{
    std::array<float, 4> flagsExposure;    // x unused, w exposure
    std::array<std::int32_t, 4> modeFlags; // x tonemap mode, y/z/w target flags
};

static_assert(sizeof(ObjectPush) == 128, "scene.vert/scene.frag expect a 128-byte object push block");
static_assert(sizeof(CompositePush) == 32, "scene_composite.frag expects a 32-byte composite push block");
static_assert(
    sizeof(ObjectPush) <= maxPushConstantBytes && sizeof(CompositePush) <= maxPushConstantBytes,
    "push-constant blocks must stay within the guaranteed minimum"
);

struct QueueFamilyIndices
{
    std::optional<std::uint32_t> graphicsFamily;
    std::optional<std::uint32_t> presentFamily;

    bool complete() const
    {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct FrameImage
{
    std::unique_ptr<vk::raii::Image> image;
    std::unique_ptr<vk::raii::DeviceMemory> memory;
    std::unique_ptr<vk::raii::ImageView> view;
};

struct LoadedTextureSet
{
    std::vector<std::uint8_t> albedo;
    std::vector<std::uint8_t> data;
};

// std140 mirror of environment_common.glsl's EnvironmentUniform (and of the

struct EnvironmentData
{
    std::array<float, 4> skyZenith;        // rgb zenith radiance, w horizon exponent
    std::array<float, 4> skyHorizon;       // rgb horizon radiance, w ground exponent
    std::array<float, 4> skyGround;        // rgb ground radiance, w sun disk enabled
    std::array<float, 4> sunDiskDirection; // xyz direction towards the scene, w cos(inner angle)
    std::array<float, 4> sunDiskRadiance;  // rgb disk radiance, w cos(outer angle)
};

// std430 mirror of materials_common.glsl's GpuMaterial. One entry per scene

struct GpuSceneMaterial
{
    std::array<std::int32_t, 4> layers; // x albedo array layer, y data array layer (-1 = none)
    std::array<float, 4> scaleAndFlags; // x uv tiling scale

    bool operator==(const GpuSceneMaterial &) const = default;
};

struct RendererState
{
    // The owning module's component context: the frame path re-resolves the
    // tri-state optional capabilities (the ui overlay) that could not exist
    // at construct time (providers construct before consumers).
    ComponentContext *boundContext = nullptr;

    // Configuration, read once during construct.
    bool enableValidation           = true;
    bool vsync                      = true;
    bool preferDiscreteDevice       = true;
    bool failOnValidationError      = false;
    std::uint32_t maxFramesInFlight = defaultFramesInFlight;
    // Shaders are module build output; the runtime layout is runtime/shaders.
    std::filesystem::path shaderDirectory = "shaders";
    std::filesystem::path resolvedShaderDirectory;
    // Content assets live in mod packages, not in the engine: the directory
    // is provided by whichever package ships the world (its config.toml
    // defaults), or by the assembly. Empty means "no content mod in this
    // build" - a scene that still names texture sets then fails precisely.
    std::filesystem::path textureDirectory;
    std::filesystem::path resolvedTextureDirectory;
    float exposure          = 1.0f;
    std::string tonemapMode = "aces";
    // The tonemap string resolved once per change instead of two std::string
    // comparisons per frame in the composite and bloom passes. The cache
    // tracks the string it was computed from, so a live-configuration reload
    // (handleConfigurationReloaded rewrites tonemapMode) re-resolves on the
    // next frame with no invalidation hook.
    std::string tonemapModeResolvedFrom;
    std::int32_t tonemapModeValue = tonemapAces;
    std::int32_t resolvedTonemapMode();
    // Bloom (post-processing) configuration. Off by default: it is a look
    // choice, and the honest default is the scene as authored.
    bool bloomEnabled             = false;
    float bloomThreshold          = 1.0f;
    float bloomStrength           = 0.15f;
    WindowIdentifier targetWindow = invalidWindowIdentifier;
    SceneDescription scene;

    // Dynamic scene support (modules/scene_registry): the object list the geometry
    // pass and the SDF passes actually consume. It starts as the static
    // [scene] world; when the scene capability connects, the registry becomes
    // the source of truth and this vector is re-synced whenever a scene event
    // marked the data dirty. Kept separate from scene.objects so the
    // configured world (camera, sun, sky, texture sets) always survives:
    // only the OBJECTS are runtime mutable.
    std::vector<SceneObject> activeObjects;
    // Set by the scene event handlers (renderer module); consumed once per
    // frame by refreshSceneRegistry before recording.
    bool sceneRegistryDirty = false;
    // Tri-state resolved during start: null = scene module absent from this
    // build (the world stays the static [scene] array forever).
    SceneCapability *sceneCapability = nullptr;
    // Handles the static world's objects got in the registry, in
    // scene.objects order; lets scripts address the demo room's objects.
    std::vector<SceneCapability::ObjectHandle> registryHandles;

    // Vulkan objects in destruction-reverse-safe declaration order: members
    // destroy in reverse, so the instance always outlives the device and the
    // device always outlives its children.
    vk::raii::Context context;
    std::unique_ptr<vk::raii::Instance> instance;
    std::unique_ptr<vk::raii::DebugUtilsMessengerEXT> debugMessenger;
    std::unique_ptr<vk::raii::SurfaceKHR> surface;
    std::unique_ptr<vk::raii::PhysicalDevice> physicalDevice;
    std::unique_ptr<vk::raii::Device> device;
    // One pipeline cache for the whole device: every Pipeline constructor
    // (renderer's own pipelines AND every attached lighting technique pass)
    // receives it. NVIDIA's Vulkan Dos and Don'ts: "Use pipeline cache." On
    // the first creation the cache is empty; on every subsequent rebuild
    // (swapchain recreate, shader hot reload) the driver can hit the cache
    // for pipelines whose SPIR-V did not change, skipping the PSO compile
    // cost. The cache lives with the device and is never persisted to disk
    // by this engine (a future engine run could merge its data into a disk
    // cache file for cross-run reuse; that is a separate feature and not
    // required for the in-session dev-loop benefit).
    std::unique_ptr<vk::raii::PipelineCache> pipelineCache;
    std::unique_ptr<vk::raii::SwapchainKHR> swapchain;
    std::vector<vk::Image> swapchainImages;
    vk::Format swapchainFormat = vk::Format::eB8G8R8A8Srgb;
    vk::Extent2D swapchainExtent;
    std::uint32_t swapchainMinImageCount = 0; // overlay backend bundle
    std::vector<vk::raii::ImageView> swapchainImageViews;

    // Frame resources (rebuilt together with the swapchain). All color
    // attachments live in GENERAL layout permanently: they are written as
    // attachments and read as sampled or storage images without per-frame
    // layout churn; the swapchain image keeps its own attachment/present
    // transitions.
    FrameImage albedoImage;           // RGBA8_UNORM: rgb albedo, a roughness
    FrameImage normalImage;           // RGBA16F: rgb world normal, a metallic
    FrameImage emissiveImage;         // RGBA16F: rgb emissive radiance
    FrameImage directRadianceImage;   // RGBA16F: direct radiance (only with a technique writing it)
    FrameImage indirectRadianceImage; // RGBA16F: diffuse GI irradiance (only with a technique writing it)
    FrameImage indirectSpecularImage; // RGBA16F: reflection radiance (only with a technique writing it)
    FrameImage depthImage;            // D32_SFLOAT
    FrameImage dummyImage;            // 1x1 RGBA16F bound where a lighting target is absent
    // Bloom chain (post-processing; [renderer] bloom): the HDR intermediate
    // the composite writes pre-tonemap, and the half-resolution bright-pass
    // ping/pong pair. Recreated with the swapchain like every frame image.
    FrameImage hdrSceneImage; // RGBA16F: linear HDR scene (bloom path only)
    FrameImage bloomImageA;   // half extent: bright pass / vertical blur output
    FrameImage bloomImageB;   // half extent: horizontal blur output
    std::unique_ptr<vk::raii::Sampler> linearSampler;
    // The depth binding of the composite descriptor set points at this one:
    // depth formats are not required to support linear filtering, and every
    // depth consumer wants texel-exact reads, never interpolation.
    std::unique_ptr<vk::raii::Sampler> nearestSampler;

    // Material texture arrays (one albedo layer pair per texture set, built
    // once in createSceneGeometry; independent of the swapchain). Empty
    // scenes get 1x1x1 stand-ins so every descriptor binding stays valid.
    FrameImage albedoArrayImage; // RGBA8_SRGB: color maps
    FrameImage dataArrayImage;   // RGBA8_UNORM: R rough, G metal, BA normal xy
    std::uint32_t textureArrayMipLevels = 1;
    std::unique_ptr<vk::raii::Sampler> textureSampler;
    // Per-set packed-data means (roughness, metallic) for the startup
    // material diagnostics; filled by createTextureArrays.
    std::vector<std::array<float, 2>> textureSetDataMeans;
    // Texture-array build state handed from loadTextureSets through
    // createArrayImages to uploadArrayLayersAndGenerateMips.
    std::vector<LoadedTextureSet> loadedTextureSets;
    vk::Extent2D loadedArrayExtent{1, 1};
    std::uint32_t arrayLayerCount = 1;
    bool anyTextureSetLoaded      = false;

    // Module logger handed to initialize(); used for one-time asset
    // diagnostics (missing texture sets) outside the validation path.
    LoggerCapability *runtimeLogger = nullptr;

    std::unique_ptr<vk::raii::PipelineLayout> scenePipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> scenePipeline;
    std::unique_ptr<vk::raii::DescriptorSetLayout> sceneSetLayout;
    std::unique_ptr<vk::raii::DescriptorPool> sceneDescriptorPool;
    std::unique_ptr<vk::raii::DescriptorSet> sceneDescriptorSet;
    std::unique_ptr<vk::raii::PipelineLayout> compositePipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> compositePipeline;
    std::unique_ptr<vk::raii::DescriptorSetLayout> compositeSetLayout;
    std::unique_ptr<vk::raii::DescriptorPool> compositeDescriptorPool;
    std::unique_ptr<vk::raii::DescriptorSet> compositeDescriptorSet;

    // Bloom passes: two compute stages (bright extract, separable blur) and
    // the final tonemap-and-add graphics pass. Declared after the composite
    // objects so reverse destruction tears them down first.
    std::unique_ptr<vk::raii::PipelineLayout> bloomExtractPipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> bloomExtractPipeline;
    std::unique_ptr<vk::raii::DescriptorSetLayout> bloomExtractSetLayout;
    std::unique_ptr<vk::raii::DescriptorPool> bloomExtractDescriptorPool;
    std::unique_ptr<vk::raii::DescriptorSet> bloomExtractDescriptorSet;
    std::unique_ptr<vk::raii::PipelineLayout> bloomBlurPipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> bloomBlurPipeline;
    std::unique_ptr<vk::raii::DescriptorSetLayout> bloomBlurSetLayout;
    std::unique_ptr<vk::raii::DescriptorPool> bloomBlurDescriptorPool;
    std::unique_ptr<vk::raii::DescriptorSet> bloomBlurSetA; // read A, write B (horizontal)
    std::unique_ptr<vk::raii::DescriptorSet> bloomBlurSetB; // read B, write A (vertical)
    std::unique_ptr<vk::raii::PipelineLayout> bloomFinalPipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> bloomFinalPipeline;
    std::unique_ptr<vk::raii::DescriptorSetLayout> bloomFinalSetLayout;
    std::unique_ptr<vk::raii::DescriptorPool> bloomFinalDescriptorPool;
    std::unique_ptr<vk::raii::DescriptorSet> bloomFinalDescriptorSet;

    // Scene geometry: device-local buffers uploaded once through staging.
    struct GeometryBuffer
    {
        std::unique_ptr<vk::raii::Buffer> buffer;
        std::unique_ptr<vk::raii::DeviceMemory> memory;
        std::uint32_t indexCount     = 0;
        std::size_t indexOffsetBytes = 0;
    };
    // Sky environment (std140 EnvironmentData; shaders mirror it in
    // environment_common.glsl and the composite pass). Host visible and
    // persistently mapped - the disk radiance depends on the configured sun.
    GeometryBuffer environmentBufferStorage;
    void *environmentMapped = nullptr;
    GeometryBuffer boxMesh;
    GeometryBuffer sphereMesh;
    // Analytic primitives + material table (std430 GpuScenePrimitive /
    // GpuSceneMaterial; shaders mirror them in scene_description.hpp).
    // Host visible and persistently mapped, like the camera/environment
    // uniforms: the incremental scene rebuild used to push these through a
    // staging buffer - two fresh staging allocations, two submissions and two
    // waitIdles per dirty frame (physics write-backs dirty the scene every
    // step) - for 24 KB of data. With the serialized frame path the device is
    // idle when the upload runs, so a coherent host write is exactly as safe
    // and strictly cheaper; the tracing consumers keep full-speed reads (the
    // working set fits L2 regardless of where it lives).
    GeometryBuffer primitivesBuffer;
    void *primitivesMapped = nullptr;
    GeometryBuffer materialsBuffer;
    void *materialsMapped = nullptr;
    GeometryBuffer cameraBufferStorage; // host visible, persistently mapped
    void *cameraMapped = nullptr;
    CameraData cameraData;
    // Dirty-check signature of the camera uniform inputs: view, projection
    // and the Gauss-Jordan inverse are only rebuilt when one of them moved.
    // A free-fly camera dirties most frames, but an authored scene's static
    // camera - and every frame of a paused or menued run - skips four matrix
    // builds including the matrix inverse entirely. The swapchain extent
    // participates because the aspect ratio feeds the projection (a resize
    // must not render with a stale projection).
    struct CameraUniformSignature
    {
        std::array<float, 3> position    = {};
        std::array<float, 3> lookAt      = {};
        float verticalFieldOfViewDegrees = 0.0f;
        float nearPlane                  = 0.0f;
        float farPlane                   = 0.0f;
        std::uint32_t extentWidth        = 0;
        std::uint32_t extentHeight       = 0;

        bool operator==(const CameraUniformSignature &) const = default;
    };
    CameraUniformSignature cameraUniformSignature{};
    bool cameraUniformValid = false;

    // Registered scene-registry meshes (SceneObjectType::Mesh), uploaded once
    // per handle through the staging path: one buffer per mesh, vertices at
    // offset 0, indices after (uint32, naturally 4-byte aligned because the
    // vertex stride is 6 floats). The geometry pass binds these like the unit
    // meshes; the tracing consumers see the bbox proxy instead.
    struct MeshGpu
    {
        GeometryBuffer geometry;
        std::uint32_t indexCount = 0;
    };
    std::unordered_map<std::uint32_t, MeshGpu> meshPool;
    // Mesh handles whose upload failed: skipped with one precise log instead
    // of a per-frame retry storm (the registry keeps the data; a hot reload
    // or pipeline rebuild does not touch this pool).
    std::unordered_map<std::uint32_t, bool> meshUploadFailed;

    // Per-object draw state resolved ONCE per scene refresh, parallel to
    // activeObjects. The geometry pass previously rebuilt every object's
    // model matrix (a sin/cos pair per object), re-hashed the texture-set
    // name into a layer and re-hashed the mesh pool every frame - all for
    // data that only changes when the scene registry dirties (the same event
    // that rebuilds this cache). Resolved here instead: recordGeometryPass
    // becomes a pure read+push+draw loop over ready data.
    struct ObjectDrawCache
    {
        Mat4 model{};                         // translate * rotateY * scale, prebuilt
        std::int32_t albedoLayer   = -1;      // resolved texture-set layer (-1 = flat)
        const GeometryBuffer *mesh = nullptr; // resolved geometry (null -> unit box/sphere)
    };
    std::vector<ObjectDrawCache> objectDrawCache;

    std::unique_ptr<vk::raii::CommandPool> commandPool;
    std::vector<vk::raii::CommandBuffer> commandBuffers;
    // Semaphore ownership is described by FrameSyncPolicy (frame_sync_policy.hpp):
    // acquire semaphores are owned by the FRAME SLOT, render-finished (present)
    // semaphores by the SWAPCHAIN IMAGE. The image-scoped pool must be rebuilt
    // whenever the swapchain is rebuilt; slot-scoped objects survive recreation.
    std::vector<vk::raii::Semaphore> imageAvailableSemaphores;
    std::vector<vk::raii::Semaphore> renderingFinishedSemaphores;
    std::vector<vk::raii::Fence> inFlightFences;
    FrameSyncPolicy frameSyncPolicy;

    std::uint32_t graphicsQueueFamily = 0;
    std::uint32_t presentQueueFamily  = 0;
    vk::Queue graphicsQueue{nullptr};
    vk::Queue presentQueue{nullptr};

    std::uint64_t frameCounter = 0;
    std::uint32_t currentFrame = 0;
    // The frame slot whose fence was signaled by the most recent submission
    // (or the slot whose fence is still init-signaled before the first
    // submit). Serialization waits exactly this fence: a single queue retires
    // submissions in order, so one wait covers every earlier submission as
    // well - including this slot's previous occupant. Skipping the second
    // (redundant in the normal path) fence wait halves the per-frame fence
    // traffic without weakening any of the guarantees below.
    std::uint32_t lastSubmittedSlot = 0;
    bool framebufferResized         = false;
    bool captureRequested           = false;
    bool validationEnabled          = false;

    // The overlay seam (renderer_overlay_capability.hpp): the registered UI
    // recorder, called after the composite wrote the swapchain image, and the
    // backend handle bundle the UI backend initialized against. The bundle is
    // refilled from the live device state on every overlayBackendHandles()
    // query; the recorder pointer is null unless a UI module registered.
    OverlayRecorder *overlayRecorder = nullptr;
    OverlayBackendHandles overlayHandles;

    // Attached lighting techniques (in-tree modules; see render_interface.hpp).
    // Ordered direct-radiance-first: techniques that evaluate emitters record
    // before techniques that transport light. Declared LAST so reverse
    // destruction order tears the techniques (and their Vulkan objects) down
    // BEFORE the device they were created from.
    std::vector<std::shared_ptr<LightingTechniquePass>> lightingTechniques;

    bool hasDirectRadianceTarget   = false;
    bool hasIndirectDiffuseTarget  = false;
    bool hasIndirectSpecularTarget = false;

    // Free-fly camera control. The input capability is optional (minimal
    // builds may exclude the input module): without it the camera stays at
    // its configured pose and nothing else changes.
    InputCapability *inputCapability = nullptr;
    // The fly controller's semantic action bindings ([input.actions] entries
    // read at construct; empty = the controller's historical fixed keys).
    FlyCameraController::CameraBindings cameraBindings;
    // Texture set file names ([renderer.texture_maps]; the defaults are the
    // ambientCG layout constants above). Read once at construct.
    std::string colorMapFileName     = colorMapName;
    std::string roughnessMapFileName = roughnessMapName;
    std::string metalnessMapFileName = metalnessMapName;
    std::string normalMapFileName    = normalMapName;
    // The ui module's overlay, resolved tri-state (the screenshot-capability
    // pattern): the free-fly camera pauses its pointer handling while the
    // overlay claims the cursor (UiCapability::wantsMouse). Absent module =
    // the camera never pauses.
    UiCapability *uiOverlayCapability = nullptr;
    int uiResolutionState             = 0;
    bool uiWarnedUnavailable          = false;
    FlyCameraController flyCamera;
    std::chrono::steady_clock::time_point lastFrameTime{};
    bool flyCameraSeeded = false;

    // Written by the debug messenger callback; observed through the contract.
    std::atomic<bool> validationErrorSeen{false};
    LoggerCapability *validationLogger = nullptr;

    // Capture events are published through this channel reference; cached at
    // construct together with the other frame channels.
    EventChannel<FrameCapturedEvent> *frameCapturedChannel = nullptr;

    // Capture staging: one host visible buffer sized to the swapchain, plus a
    // tightly packed RGBA8 copy published through the event bus.
    std::unique_ptr<vk::raii::Buffer> captureBuffer;
    std::unique_ptr<vk::raii::DeviceMemory> captureMemory;
    void *captureMapped              = nullptr;
    vk::DeviceSize captureBufferSize = 0;
    std::vector<std::uint8_t> capturePixels;

    // Barrier scratch reused across frames by the record functions: recording
    // runs every frame and a fresh vector per barrier group was pure heap
    // churn on the record path. Cleared per use, never carried between groups.
    std::vector<vk::ImageMemoryBarrier2> frameStartBarriers;
    std::vector<vk::ImageMemoryBarrier2> radianceBarriers;

    // uploadSceneData's build buffers, on the same scratch pattern: the
    // function runs once per DIRTY frame, and while physics (or any script)
    // is writing transforms that is every frame - two heap allocations plus
    // two frees per frame for buffers whose size is bounded by the registry
    // capacity. Cleared per call, capacity retained at the high-water mark.
    std::vector<GpuScenePrimitive> primitiveUploadScratch;
    std::vector<GpuSceneMaterial> materialUploadScratch;

    // refreshSceneRegistry's conversion scratch: the fresh snapshot converts
    // here BEFORE overwriting activeObjects, so a no-op change event (the
    // registry equal to the uploaded state) can skip the entire rebuild
    // without an extra copy of the previous list. Cleared per refresh,
    // capacity retained at the high-water mark.
    std::vector<SceneObject> registryScratch;
    // refreshSceneRegistry's descriptor buffer, filled through
    // SceneCapability::objectSnapshotInto (the capability's into-variant for
    // exactly this frame-rate refresh): capacity retained across dirty
    // frames, so a physics-driven run - which dirties the registry every
    // step - stops allocating the snapshot vector after the object count's
    // high-water mark. The by-value objectSnapshot() call this used to make
    // allocated a fresh vector of descriptors per dirty frame.
    std::vector<SceneObjectDescriptor> sceneSnapshotScratch;
    // False until refreshSceneRegistry's FIRST full pass (the initial adopt
    // of the registry at renderer start). The no-op short-circuit must not
    // fire on that first call even when the registry content already equals
    // the static-world list: the equality would be true (the registry was
    // just seeded from the same scene), but the mesh uploads the full pass
    // performs for the list's mesh handles have not happened yet. After
    // that first pass, an equal snapshot can only reproduce already-uploaded
    // state (a new mesh requires a new object, which changes the list), so
    // skipping is unobservable.
    bool sceneRegistrySyncedOnce = false;

    QueueFamilyIndices
    findQueueFamilies(const vk::raii::PhysicalDevice &candidate, const vk::raii::SurfaceKHR &targetSurface) const;
    bool supportsSwapchainExtension(const vk::raii::PhysicalDevice &candidate) const;
    std::int32_t deviceScore(const vk::raii::PhysicalDevice &candidate) const;
    void initialize(WindowCapability &window, LoggerCapability &logger);
    void createInstance(WindowCapability &window, LoggerCapability &logger);
    void createDeviceAndSurface(WindowCapability &window, LoggerCapability &logger);
    void createSwapchain(WindowCapability &window, LoggerCapability &logger);
    void createSwapchainImageViews();
    void createFrameImage(
        FrameImage &target,
        vk::Extent2D extent,
        vk::Format format,
        vk::ImageUsageFlags usages,
        vk::ImageAspectFlags aspect,
        vk::ImageLayout finalLayout
    );
    void transitionImageLayout(
        const vk::Image &image, vk::ImageLayout from, vk::ImageLayout to, vk::ImageAspectFlags aspect
    );
    void clearFrameImage(const FrameImage &target, const std::array<float, 4> &color);
    void createFrameResources();
    void releaseFrameResources();
    void createSceneGeometry();
    // Dynamic scene data: allocate the primitive and material buffers ONCE at
    // full registry capacity, then re-upload only their content whenever the
    // registry changed. Content-only updates keep every descriptor set, and
    // with it every technique pipeline, valid across scene edits
    // (scene_capability.hpp documents the contract).
    void allocateSceneDataBuffers();
    void uploadSceneData();
    // True when the material diagnostics (uploaded table, texture-set count,
    // per-set data means) changed since the last emitted audit; also updates
    // the cached copies. See uploadSceneData for why this gates the logging.
    bool materialDiagnosticsChanged(const std::vector<GpuSceneMaterial> &materialTable);
    std::vector<GpuSceneMaterial> materialDiagnosticsTable;
    std::vector<std::array<float, 2>> materialDiagnosticsMeans;
    std::size_t materialDiagnosticsSetCount = 0;
    // Syncs activeObjects from the connected scene registry (or the static
    // scene when no registry exists) and re-uploads the GPU content. Called
    // from the frame path when sceneRegistryDirty was set.
    void refreshSceneRegistry();
    // Rebuilds the per-object draw cache (model matrices, texture layers,
    // mesh bindings) from activeObjects. Runs at the end of every scene
    // refresh - after ensureMeshesUploaded, so mesh pointers resolve against
    // the pool the frame will actually draw - and once when the static world
    // becomes the initial active list.
    void rebuildObjectDrawCache();
    // Uploads every registry mesh the active objects reference but the pool
    // does not hold yet (device idle - this runs on the serialized frame path
    // before any recording).
    void ensureMeshesUploaded();
    static SceneObject sceneObjectFromDescriptor(SceneObjectDescriptor object);
    static SceneObjectDescriptor sceneObjectToDescriptor(const SceneObject &object);
    void createTextureArrays();
    // createTextureArrays phases: load/pack the sets from disk, then create
    // the arrays, then upload layers and build the mip chain.
    void loadTextureSets();
    void createArrayImages();
    void uploadArrayLayersAndGenerateMips();
    void buildEnvironmentBuffer();
    void
    uploadToDeviceLocal(GeometryBuffer &target, const void *data, vk::DeviceSize byteSize, vk::BufferUsageFlags usages);
    // Batched form of uploadToDeviceLocal: one staging buffer, one setup
    // submission and one waitIdle for N uploads (ensureMeshesUploaded uses
    // this when a dirty frame introduces several registry meshes at once).
    struct BatchedUpload
    {
        GeometryBuffer *target = nullptr;
        std::vector<std::uint8_t> data;
        vk::BufferUsageFlags usages{};
    };
    void uploadBatchToDeviceLocal(std::vector<BatchedUpload> &uploads);
    void updateCameraBuffer();
    void createPipelines();
    void createScenePipeline();
    void createCompositePipeline();
    // Bloom (post-processing): frame images follow createFrameResources,
    // pipelines/descriptors follow createPipelines.
    void createBloomResources();
    void createBloomPipelines();
    void writeBloomDescriptors();
    void recordBloomPass(vk::raii::CommandBuffer &commandBuffer, std::uint32_t imageIndex);
    // The composite pass into an arbitrary target: the swapchain image in the
    // direct path, the HDR intermediate (pre-tonemap) in the bloom path.
    // targetLayout is the attachment's CURRENT layout - the swapchain image
    // enters as COLOR_ATTACHMENT_OPTIMAL while every frame image persists in
    // GENERAL (createFrameResources), and dynamic-rendering validation checks
    // the declared layout against the real one.
    void recordCompositeIntoTarget(
        vk::raii::CommandBuffer &commandBuffer,
        const vk::ImageView &targetView,
        vk::Extent2D extent,
        std::int32_t tonemapModeValue,
        vk::ImageLayout targetLayout = vk::ImageLayout::eColorAttachmentOptimal
    );
    void writeCompositeDescriptors();
    void createCommandAndSyncObjects();
    void createRenderFinishedSemaphores();
    void recreateSwapchain(WindowCapability &window, LoggerCapability &logger);
    void releaseCaptureBuffer();
    void ensureCaptureBuffer();
    void recordGeometryPass(vk::raii::CommandBuffer &commandBuffer, const FrameRenderContext &frameContext);
    void recordCompositePass(vk::raii::CommandBuffer &commandBuffer, std::uint32_t imageIndex);
    void recordFrameCommands(vk::raii::CommandBuffer &commandBuffer, std::uint32_t imageIndex);
    // recordFrameCommands phases: the frame-start write-after-read hazard,
    // the G-buffer hand-off to compute/fragment reads, the compute-to-
    // composite hand-off, and the swapchain image's per-frame transitions
    // (enter composite, optional capture copy, present).
    void recordFrameStartBarrier(vk::raii::CommandBuffer &commandBuffer);
    void recordGBufferHandoffBarrier(vk::raii::CommandBuffer &commandBuffer);
    void recordRadianceToCompositeBarrier(vk::raii::CommandBuffer &commandBuffer);
    void recordSwapchainEnterTransition(vk::raii::CommandBuffer &commandBuffer, std::uint32_t imageIndex);
    void recordSwapchainCaptureAndPresentTransitions(vk::raii::CommandBuffer &commandBuffer, std::uint32_t imageIndex);
    // renderFrame phases: free-fly camera integration, the two-fence frame
    // serialization, and the capture readback + publish.
    void updateCameraIntegration(LoggerCapability &logger);
    bool waitForFrameSerializationGuarded(LoggerCapability &logger);
    bool readbackCapturePixels(LoggerCapability &logger);
    void renderFrame(LoggerCapability &logger);
};

} // namespace engine

#endif // ENGINE_MODULES_RENDERER_RENDERER_STATE_HPP
