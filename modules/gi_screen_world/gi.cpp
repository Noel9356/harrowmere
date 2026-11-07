#include "gi.hpp"

#include "light_uniform.hpp"
#include "render_interface.hpp"
#include "renderer.hpp"
#include "vulkan_utilities.hpp"

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace engine
{

constexpr std::string_view moduleContext = "gi_screen_world module";

// Push-constant block mirrored by gi_screen_world.comp (rev 33: every
// stage's knobs ride in; 96 bytes, well under the 128-byte minimum push
// guarantee the renderer's device profile pins).
struct GiScreenWorldPush
{
    std::array<std::uint32_t, 4> counts; // primitives, frame index, world march steps, second bounce
    // x max distance, y temporal alpha, z rays per pixel, w the indirect
    // (per-ray) radiance clamp - the rev-31 firefly limit.
    std::array<float, 4> params;
    // The screen stage (rev 33 config): x stride steps, y thickness
    // tolerance base (m), z screen march bound (m), w bisect steps.
    std::array<float, 4> params2;
    // Switches (rev 33): x screen traces 0/1, y animated jitter 0/1,
    // z temporal normal rejection 0/1, w short-range AO radius (px, 0 = off).
    std::array<std::uint32_t, 4> flags;
    // Rev 33 scalars: x temporal ramp cap (frames), y short-range AO
    // strength, z output intensity, w sky-miss scale.
    std::array<float, 4> params3;
    // The spatial filter (rev 33): x kernel radius in px (0..4, 1 = the
    // rev-30 3x3 shape; the denoise dispatch shares this layout), y-z-w
    // reserved.
    std::array<float, 4> params4;
};

static_assert(sizeof(GiScreenWorldPush) == 96, "gi_screen_world.comp expects a 96-byte push block");

// std140 mirror of the shader's PreviousCameraUniform (mat4, mat4, vec4).
struct PreviousCameraUniform
{
    std::array<float, 16> viewProjection{};
    std::array<float, 16> inverseViewProjection{};
    std::array<float, 4> position{};
};

// Occlusion policy the relight shares with the direct pass: the GI's
// sunDiffuse/pointDiffuse read lightCountsAndFlags.y/z, so both sides of the
// light transport must agree on whether shadow rays march and how far.
struct GiScreenWorldShadowPolicy
{
    bool shadowsEnabled = true;
    std::int32_t shadowSteps = 24;
};

// The pass object handed to the renderer. Owns its Vulkan objects; creates
// them lazily on the first record and rebuilds them whenever the frame
// resources were recreated (the pbr pass's lifecycle contract).
//
// Lighting truth: this module owns NO light state. The GpuLightUniform is
// refilled from the acquired LightingCapability every frame (light_uniform.hpp
// is the single definition) - the relight stays consistent with whatever sun
// and point lights exist at runtime, including lights a script mod places
// mid-session. Shadow policy mirrors the direct pass's configuration keys so
// the two passes can never disagree about occlusion.
//
// Frame data flow (one dispatch + one copy per frame):
//   1. upload the PREVIOUS frame's camera + the current lights;
//   2. history barriers: initial layout (fresh image) and last frame's copy
//      write -> this frame's shader read;
//   3. dispatch: reads G-buffer + history, writes the indirectRadiance target;
//   4. barrier target compute-write -> transfer-read, copy target -> history,
//      barrier history transfer-write -> shader-read (next frame's input).
// The renderer's own barriers hand the target to the composite afterwards;
// the copy is an extra read that never mutates the target.
class GiScreenWorldPass final : public LightingTechniquePass
{
public:
    GiScreenWorldPass(
        const GiScreenWorldSettings &passSettings, LightingCapability *sharedLights, const GiScreenWorldShadowPolicy &shadowParams
    )
        : settings(passSettings), lights(sharedLights), shadowPolicy(shadowParams)
    {}

    const char *techniqueName() const override
    {
        return "gi_screen_world";
    }

    LightingTarget targets() const override
    {
        return LightingTarget::IndirectDiffuse;
    }

    // One-time layout transitions (fresh images) + the cross-frame ordering
    // barriers: the previous frame's copy writes must be visible to this
    // frame's gather (history read) and denoise (filter write) - frames N-1
    // and N do not share a command buffer, so they are recorded every frame.
    void recordPreDispatchBarriers(
        vk::raii::CommandBuffer &commandBuffer, const FrameRenderContext &context
    )
    {
        std::array<vk::ImageMemoryBarrier2, 4> barriers;
        std::uint32_t count = 0;
        if (historyNeedsInitialLayout)
        {
            vk::ImageMemoryBarrier2 &transition = barriers[count++];
            transition.setSrcStageMask(vk::PipelineStageFlagBits2::eTopOfPipe);
            transition.setSrcAccessMask(vk::AccessFlagBits2::eNone);
            transition.setDstStageMask(vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer);
            transition.setDstAccessMask(vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferWrite);
            transition.setOldLayout(vk::ImageLayout::eUndefined);
            transition.setNewLayout(vk::ImageLayout::eGeneral);
            transition.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            transition.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            transition.setImage(**historyImage.image);
            transition.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
            historyNeedsInitialLayout = false;
        }
        if (gatherNeedsInitialLayout)
        {
            vk::ImageMemoryBarrier2 &transition = barriers[count++];
            transition.setSrcStageMask(vk::PipelineStageFlagBits2::eTopOfPipe);
            transition.setSrcAccessMask(vk::AccessFlagBits2::eNone);
            transition.setDstStageMask(vk::PipelineStageFlagBits2::eComputeShader);
            transition.setDstAccessMask(vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead);
            transition.setOldLayout(vk::ImageLayout::eUndefined);
            transition.setNewLayout(vk::ImageLayout::eGeneral);
            transition.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            transition.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            transition.setImage(**halfGatherImage.image);
            transition.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
            gatherNeedsInitialLayout = false;
        }
        if (filterNeedsInitialLayout)
        {
            vk::ImageMemoryBarrier2 &transition = barriers[count++];
            transition.setSrcStageMask(vk::PipelineStageFlagBits2::eTopOfPipe);
            transition.setSrcAccessMask(vk::AccessFlagBits2::eNone);
            transition.setDstStageMask(vk::PipelineStageFlagBits2::eComputeShader);
            transition.setDstAccessMask(vk::AccessFlagBits2::eShaderWrite);
            transition.setOldLayout(vk::ImageLayout::eUndefined);
            transition.setNewLayout(vk::ImageLayout::eGeneral);
            transition.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            transition.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            transition.setImage(**filterImage.image);
            transition.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
            filterNeedsInitialLayout = false;
        }
        {
            vk::ImageMemoryBarrier2 &historyVisibility = barriers[count++];
            historyVisibility.setSrcStageMask(vk::PipelineStageFlagBits2::eTransfer);
            historyVisibility.setSrcAccessMask(vk::AccessFlagBits2::eTransferWrite);
            historyVisibility.setDstStageMask(vk::PipelineStageFlagBits2::eComputeShader);
            historyVisibility.setDstAccessMask(vk::AccessFlagBits2::eShaderRead);
            historyVisibility.setOldLayout(vk::ImageLayout::eGeneral);
            historyVisibility.setNewLayout(vk::ImageLayout::eGeneral);
            historyVisibility.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            historyVisibility.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            historyVisibility.setImage(**historyImage.image);
            historyVisibility.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
        }
        std::vector<vk::ImageMemoryBarrier2> active(barriers.begin(), barriers.begin() + static_cast<std::ptrdiff_t>(count));
        vk::DependencyInfo dependency;
        dependency.setImageMemoryBarriers(active);
        commandBuffer.pipelineBarrier2(dependency);
    }

    void record(
        vk::raii::CommandBuffer &commandBuffer, const FrameRenderContext &context
    ) override
    {
        const bool trace = std::getenv("HARROWMERE_GI_DEBUG") != nullptr;
        if (trace) std::fprintf(stderr, "[gi-debug] record enter frame=%u extent=%ux%u\n", frameIndex, context.extent.width, context.extent.height);
        if (pipeline == nullptr || context.extent != cachedExtent || needsRecreate)
        {
            rebuild(context);
        }
        if (trace) std::fprintf(stderr, "[gi-debug] rebuilt\n");

        // Lights: refilled from the capability every frame, change-gated
        // (the same signature-compare upload the pbr pass uses). Shadow
        // policy mirrors the direct pass's keys (see construct()).
        GpuLightUniform uniform;
        fillSunAndPointLights(
            uniform, lights != nullptr && lights->hasSunLight(),
            lights != nullptr ? lights->sunLight() : DirectionalLightDescription{},
            nullptr, 0u
        );
        if (lights != nullptr)
        {
            const std::uint32_t count = lights->pointLightCount();
            for (std::uint32_t index = 0; index < count; ++index)
            {
                if (index >= maxPointLights)
                {
                    break;
                }
                const PointLightDescription light = lights->pointLight(index);
                uniform.pointPositionRadius[index] = {light.position[0], light.position[1], light.position[2], light.radius};
                uniform.pointColorIntensity[index] = {light.color[0], light.color[1], light.color[2], light.intensity};
            }
            uniform.lightCountsAndFlags[0] = static_cast<std::int32_t>(std::min<std::uint32_t>(count, maxPointLights));
        }
        else
        {
            uniform.lightCountsAndFlags[0] = 0;
        }
        uniform.lightCountsAndFlags[1] = shadowPolicy.shadowsEnabled ? 1 : 0;
        uniform.lightCountsAndFlags[2] = shadowPolicy.shadowSteps;
        if (!uploadedLightsValid || !(uniform == uploadedLights))
        {
            std::memcpy(lightStorage.mapped, &uniform, sizeof(uniform));
            uploadedLights      = uniform;
            uploadedLightsValid = true;
        }

        if (trace) std::fprintf(stderr, "[gi-debug] lights uploaded (sun=%d points=%d shadows=%d steps=%d intensity=%.1f)\n",
                                (int)uniform.sunColorEnabled[3], uniform.lightCountsAndFlags[0],
                                uniform.lightCountsAndFlags[1], uniform.lightCountsAndFlags[2],
                                uniform.pointColorIntensity[0][3]);
        // Previous camera: the reprojection source this dispatch reads.
        std::memcpy(prevCameraStorage.mapped, &prevCamera, sizeof(prevCamera));

        // (2) History/filter barriers (see recordPreDispatchBarriers).
        // rev 31: the call was recorded TWICE in rev 30 (a merge slip) - the
        // second round re-recorded the identical history-visibility barrier,
        // pure duplicate command bytes. One call.
        recordPreDispatchBarriers(commandBuffer, context);
        if (trace) std::fprintf(stderr, "[gi-debug] pre barriers recorded\n");
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, **pipeline);
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, **pipelineLayout, 0, **sharedSet, {});
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, **pipelineLayout, 1, **frameSet, {});

        const GiScreenWorldPush pushValue{
            .counts  = {context.primitiveCount, frameIndex, settings.marchSteps, settings.secondBounce ? 1u : 0u},
            .params  = {settings.maxDistance, settings.temporalAlpha,
                        static_cast<float>(std::clamp(settings.raysPerPixel, 1u, 8u)), settings.indirectClamp},
            .params2 = {static_cast<float>(std::clamp(settings.screenSteps, 1u, 64u)), settings.screenThickness,
                        settings.screenMaxDistance, static_cast<float>(std::clamp(settings.screenRefine, 1u, 8u))},
            .flags   = {settings.screenTrace ? 1u : 0u, settings.animatedJitter ? 1u : 0u,
                        settings.temporalNormalRejection ? 1u : 0u,
                        std::clamp(settings.shortRangeAoRadius, 0u, 16u)},
            .params3 = {static_cast<float>(std::clamp(settings.temporalMaxFrames, 1u, 4096u)),
                        settings.shortRangeAoStrength, settings.intensity, settings.skyMissScale},
            .params4 = {static_cast<float>(std::clamp(settings.filterRadius, 0u, 4u)), 0.0f, 0.0f, 0.0f}
        };
        commandBuffer.pushConstants(
            **pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pushValue), &pushValue
        );

        if (trace) std::fprintf(stderr, "[gi-debug] dispatch about to record\n");
        // Rev 31: the gather + denoise run at HALF resolution (the SDF march
        // is the pass's cost - the full-res gather measured 50-100 s per
        // frame under the capture rig's software rasterizer, four frames per
        // 180 s window, and the temporal accumulator never converged). The
        // upsample lifts the filtered half-res result to the full-res target.
        const vk::Extent2D halfExtent{std::max(context.extent.width / 2, 1u), std::max(context.extent.height / 2, 1u)};
        const std::uint32_t halfGroupsX = (halfExtent.width + 7) / 8;
        const std::uint32_t halfGroupsY = (halfExtent.height + 7) / 8;
        const std::uint32_t fullGroupsX = (context.extent.width + 7) / 8;
        const std::uint32_t fullGroupsY = (context.extent.height + 7) / 8;
        commandBuffer.dispatch(halfGroupsX, halfGroupsY, 1);

        // The denoise dispatch: a 3x3 Gaussian depth/normal bilateral with a
        // neighborhood-outlier clamp over the gather output, at half
        // resolution.
        {
            vk::ImageMemoryBarrier2 toFilter;
            toFilter.setSrcStageMask(vk::PipelineStageFlagBits2::eComputeShader);
            toFilter.setSrcAccessMask(vk::AccessFlagBits2::eShaderWrite);
            toFilter.setDstStageMask(vk::PipelineStageFlagBits2::eComputeShader);
            toFilter.setDstAccessMask(vk::AccessFlagBits2::eShaderRead);
            toFilter.setOldLayout(vk::ImageLayout::eGeneral);
            toFilter.setNewLayout(vk::ImageLayout::eGeneral);
            toFilter.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            toFilter.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            toFilter.setImage(**halfGatherImage.image);
            toFilter.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
            vk::DependencyInfo dependency;
            dependency.setImageMemoryBarriers(toFilter);
            commandBuffer.pipelineBarrier2(dependency);
        }
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, **denoisePipeline);
        commandBuffer.dispatch(halfGroupsX, halfGroupsY, 1);
        if (trace) std::fprintf(stderr, "[gi-debug] gather + denoise recorded\n");

        // Copy the FILTERED half-res result into the history (the temporal
        // state stays filtered at the gather resolution), then the upsample
        // dispatch lifts it onto the full-res target the composite reads.
        {
            vk::ImageMemoryBarrier2 filterToCopy;
            filterToCopy.setSrcStageMask(vk::PipelineStageFlagBits2::eComputeShader);
            filterToCopy.setSrcAccessMask(vk::AccessFlagBits2::eShaderWrite);
            filterToCopy.setDstStageMask(vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eComputeShader);
            filterToCopy.setDstAccessMask(vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eShaderRead);
            filterToCopy.setOldLayout(vk::ImageLayout::eGeneral);
            filterToCopy.setNewLayout(vk::ImageLayout::eGeneral);
            filterToCopy.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            filterToCopy.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            filterToCopy.setImage(**filterImage.image);
            filterToCopy.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
            vk::ImageMemoryBarrier2 historyToWrite;
            historyToWrite.setSrcStageMask(vk::PipelineStageFlagBits2::eComputeShader);
            historyToWrite.setSrcAccessMask(vk::AccessFlagBits2::eShaderRead);
            historyToWrite.setDstStageMask(vk::PipelineStageFlagBits2::eTransfer);
            historyToWrite.setDstAccessMask(vk::AccessFlagBits2::eTransferWrite);
            historyToWrite.setOldLayout(vk::ImageLayout::eGeneral);
            historyToWrite.setNewLayout(vk::ImageLayout::eGeneral);
            historyToWrite.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            historyToWrite.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            historyToWrite.setImage(**historyImage.image);
            historyToWrite.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
            vk::ImageMemoryBarrier2 targetForUpsample;
            targetForUpsample.setSrcStageMask(vk::PipelineStageFlagBits2::eAllCommands);
            targetForUpsample.setSrcAccessMask(vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
            targetForUpsample.setDstStageMask(vk::PipelineStageFlagBits2::eComputeShader);
            targetForUpsample.setDstAccessMask(vk::AccessFlagBits2::eShaderWrite);
            targetForUpsample.setOldLayout(vk::ImageLayout::eGeneral);
            targetForUpsample.setNewLayout(vk::ImageLayout::eGeneral);
            targetForUpsample.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            targetForUpsample.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            targetForUpsample.setImage(context.indirectRadianceImage);
            targetForUpsample.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
            std::array<vk::ImageMemoryBarrier2, 3> copyBarriers{filterToCopy, historyToWrite, targetForUpsample};
            vk::DependencyInfo dependency;
            dependency.setImageMemoryBarriers(copyBarriers);
            commandBuffer.pipelineBarrier2(dependency);
        }
        vk::ImageCopy copyRegion;
        copyRegion.setSrcSubresource(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1));
        copyRegion.setDstSubresource(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1));
        copyRegion.setExtent(vk::Extent3D(halfExtent.width, halfExtent.height, 1));
        commandBuffer.copyImage(
            **filterImage.image, vk::ImageLayout::eGeneral,
            **historyImage.image, vk::ImageLayout::eGeneral, copyRegion
        );
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, **upsamplePipeline);
        commandBuffer.dispatch(fullGroupsX, fullGroupsY, 1);
        if (trace) std::fprintf(stderr, "[gi-debug] copy + upsample recorded\n");
        {
            // The history feeds next frame's gather + denoise reads. The
            // full-res target's compute write -> composite fragment read is
            // the renderer's radiance hand-off barrier's job.
            vk::ImageMemoryBarrier2 historyForNextFrame;
            historyForNextFrame.setSrcStageMask(vk::PipelineStageFlagBits2::eTransfer);
            historyForNextFrame.setSrcAccessMask(vk::AccessFlagBits2::eTransferWrite);
            historyForNextFrame.setDstStageMask(vk::PipelineStageFlagBits2::eComputeShader);
            historyForNextFrame.setDstAccessMask(vk::AccessFlagBits2::eShaderRead);
            historyForNextFrame.setOldLayout(vk::ImageLayout::eGeneral);
            historyForNextFrame.setNewLayout(vk::ImageLayout::eGeneral);
            historyForNextFrame.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            historyForNextFrame.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            historyForNextFrame.setImage(**historyImage.image);
            historyForNextFrame.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
            std::array<vk::ImageMemoryBarrier2, 1> endBarriers{historyForNextFrame};
            vk::DependencyInfo dependency;
            dependency.setImageMemoryBarriers(endBarriers);
            commandBuffer.pipelineBarrier2(dependency);
        }
        if (trace) std::fprintf(stderr, "[gi-debug] copies + end barriers recorded\n");
        // (4) The camera the NEXT frame reprojects with.
        prevCamera.viewProjection        = context.cameraViewProjection;
        prevCamera.inverseViewProjection = context.cameraInverseViewProjection;
        prevCamera.position              = {context.cameraPosition[0], context.cameraPosition[1],
                                             context.cameraPosition[2], 0.0f};
        ++frameIndex;
    }

    void notifyFrameResourcesRecreated() override
    {
        needsRecreate = true;
    }

private:
    struct HistoryImage
    {
        std::unique_ptr<vk::raii::Image> image;
        std::unique_ptr<vk::raii::DeviceMemory> memory;
        std::unique_ptr<vk::raii::ImageView> view;
    };

    void createHistory(
        const FrameRenderContext &context
    )
    {
        // RGBA16F like the target it mirrors (rgb gathered irradiance/pi,
        // a view depth). TransferDst feeds the frame copy, Sampled feeds the
        // two temporal consumers; the view persists in eGeneral like every
        // frame image in this renderer.
        // Rev 31: HALF resolution - the temporal state lives at the gather
        // resolution (the reprojection confidence works in view depth, which
        // is resolution-independent).
        const vk::Extent2D halfExtent{std::max(context.extent.width / 2, 1u), std::max(context.extent.height / 2, 1u)};
        const vk::Format format = vk::Format::eR16G16B16A16Sfloat;
        vk::ImageCreateInfo imageInfo;
        imageInfo.setImageType(vk::ImageType::e2D);
        imageInfo.setFormat(format);
        imageInfo.setExtent(vk::Extent3D(halfExtent.width, halfExtent.height, 1));
        imageInfo.setMipLevels(1);
        imageInfo.setArrayLayers(1);
        imageInfo.setSamples(vk::SampleCountFlagBits::e1);
        imageInfo.setTiling(vk::ImageTiling::eOptimal);
        imageInfo.setUsage(vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled);
        imageInfo.setSharingMode(vk::SharingMode::eExclusive);
        imageInfo.setInitialLayout(vk::ImageLayout::eUndefined);
        historyImage.image = std::make_unique<vk::raii::Image>(context.device, imageInfo);

        const vk::MemoryRequirements requirements = historyImage.image->getMemoryRequirements();
        vk::MemoryAllocateInfo allocateInfo;
        allocateInfo.setAllocationSize(requirements.size);
        allocateInfo.setMemoryTypeIndex(
            findMemoryType(context.physicalDevice, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal)
        );
        historyImage.memory = std::make_unique<vk::raii::DeviceMemory>(context.device, allocateInfo);
        historyImage.image->bindMemory(*historyImage.memory, 0);

        vk::ImageViewCreateInfo viewInfo;
        viewInfo.setImage(**historyImage.image);
        viewInfo.setViewType(vk::ImageViewType::e2D);
        viewInfo.setFormat(format);
        viewInfo.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
        historyImage.view = std::make_unique<vk::raii::ImageView>(context.device, viewInfo);

        // The frame command buffer transitions the fresh image into eGeneral
        // on its first use (setup submissions are the renderer's business;
        // techniques only ever see the frame command buffer).
        historyNeedsInitialLayout = true;
    }

    // The HALF-res gather target (rev 31): the dispatch's storage image.
    // The march is the pass's cost, and it scales with PIXELS - at half
    // resolution the gather runs ~4x cheaper, which is what turns the
    // capture window's four frames into enough frames for the temporal
    // accumulator to converge. Storage write (binding 3) + sampled re-read
    // by the denoise (binding 5).
    void createHalfGather(
        const FrameRenderContext &context
    )
    {
        const vk::Extent2D halfExtent{std::max(context.extent.width / 2, 1u), std::max(context.extent.height / 2, 1u)};
        const vk::Format format = vk::Format::eR16G16B16A16Sfloat;
        vk::ImageCreateInfo imageInfo;
        imageInfo.setImageType(vk::ImageType::e2D);
        imageInfo.setFormat(format);
        imageInfo.setExtent(vk::Extent3D(halfExtent.width, halfExtent.height, 1));
        imageInfo.setMipLevels(1);
        imageInfo.setArrayLayers(1);
        imageInfo.setSamples(vk::SampleCountFlagBits::e1);
        imageInfo.setTiling(vk::ImageTiling::eOptimal);
        imageInfo.setUsage(vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);
        imageInfo.setSharingMode(vk::SharingMode::eExclusive);
        imageInfo.setInitialLayout(vk::ImageLayout::eUndefined);
        halfGatherImage.image = std::make_unique<vk::raii::Image>(context.device, imageInfo);

        const vk::MemoryRequirements requirements = halfGatherImage.image->getMemoryRequirements();
        vk::MemoryAllocateInfo allocateInfo;
        allocateInfo.setAllocationSize(requirements.size);
        allocateInfo.setMemoryTypeIndex(
            findMemoryType(context.physicalDevice, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal)
        );
        halfGatherImage.memory = std::make_unique<vk::raii::DeviceMemory>(context.device, allocateInfo);
        halfGatherImage.image->bindMemory(*halfGatherImage.memory, 0);

        vk::ImageViewCreateInfo viewInfo;
        viewInfo.setImage(**halfGatherImage.image);
        viewInfo.setViewType(vk::ImageViewType::e2D);
        viewInfo.setFormat(format);
        viewInfo.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
        halfGatherImage.view = std::make_unique<vk::raii::ImageView>(context.device, viewInfo);
        gatherNeedsInitialLayout = true;
    }

    // The denoise dispatch's output image: HALF resolution like the gather
    // (rev 31). The pass copies it into the history (the temporal state
    // stays filtered at the gather resolution) and the UPSAMPLE dispatch
    // lifts it to the full-res target the composite reads.
    void createFilter(
        const FrameRenderContext &context
    )
    {
        const vk::Extent2D halfExtent{std::max(context.extent.width / 2, 1u), std::max(context.extent.height / 2, 1u)};
        const vk::Format format = vk::Format::eR16G16B16A16Sfloat;
        vk::ImageCreateInfo imageInfo;
        imageInfo.setImageType(vk::ImageType::e2D);
        imageInfo.setFormat(format);
        imageInfo.setExtent(vk::Extent3D(halfExtent.width, halfExtent.height, 1));
        imageInfo.setMipLevels(1);
        imageInfo.setArrayLayers(1);
        imageInfo.setSamples(vk::SampleCountFlagBits::e1);
        imageInfo.setTiling(vk::ImageTiling::eOptimal);
        imageInfo.setUsage(vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc);
        imageInfo.setSharingMode(vk::SharingMode::eExclusive);
        imageInfo.setInitialLayout(vk::ImageLayout::eUndefined);
        filterImage.image = std::make_unique<vk::raii::Image>(context.device, imageInfo);

        const vk::MemoryRequirements requirements = filterImage.image->getMemoryRequirements();
        vk::MemoryAllocateInfo allocateInfo;
        allocateInfo.setAllocationSize(requirements.size);
        allocateInfo.setMemoryTypeIndex(
            findMemoryType(context.physicalDevice, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal)
        );
        filterImage.memory = std::make_unique<vk::raii::DeviceMemory>(context.device, allocateInfo);
        filterImage.image->bindMemory(*filterImage.memory, 0);

        vk::ImageViewCreateInfo viewInfo;
        viewInfo.setImage(**filterImage.image);
        viewInfo.setViewType(vk::ImageViewType::e2D);
        viewInfo.setFormat(format);
        viewInfo.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
        filterImage.view = std::make_unique<vk::raii::ImageView>(context.device, viewInfo);
        filterNeedsInitialLayout = true;
    }

    void rebuild(
        const FrameRenderContext &context
    )
    {
        // Teardown in strict reverse dependency order (the pbr pass's rule).
        frameSet.reset();
        framePool.reset();
        sharedSet.reset();
        sharedPool.reset();
        pipeline.reset();
        pipelineLayout.reset();
        frameSetLayout.reset();
        sharedSetLayout.reset();
        lightStorage.reset();
        prevCameraStorage.reset();
        historyImage = {};
        halfGatherImage = {};
        filterImage = {};
        historyNeedsInitialLayout = false;
        gatherNeedsInitialLayout  = false;
        filterNeedsInitialLayout  = false;

        lightStorage = createMappedHostVisibleBuffer(
            context.device, context.physicalDevice, sizeof(GpuLightUniform), vk::BufferUsageFlagBits::eUniformBuffer
        );
        prevCameraStorage = createMappedHostVisibleBuffer(
            context.device, context.physicalDevice, sizeof(PreviousCameraUniform), vk::BufferUsageFlagBits::eUniformBuffer
        );

        sampler       = makeNearestClampSampler(context.device);
        linearSampler = std::make_unique<vk::raii::Sampler>(context.device, [&] {
            // The history is reprojected bilinearly: the reprojection lands
            // between texels under camera motion, and nearest sampling prints
            // that lattice into the blend (the heightfield shadow grid made
            // the same switch in rev 25).
            vk::SamplerCreateInfo samplerInfo;
            samplerInfo.setMagFilter(vk::Filter::eLinear);
            samplerInfo.setMinFilter(vk::Filter::eLinear);
            samplerInfo.setMipmapMode(vk::SamplerMipmapMode::eNearest);
            samplerInfo.setAddressModeU(vk::SamplerAddressMode::eClampToEdge);
            samplerInfo.setAddressModeV(vk::SamplerAddressMode::eClampToEdge);
            samplerInfo.setAddressModeW(vk::SamplerAddressMode::eClampToEdge);
            return samplerInfo;
        }());

        createHistory(context);
        createHalfGather(context);
        createFilter(context);

        // Set 0 (shared, the technique layout contract): camera, primitives,
        // lights, materials, albedo/data arrays, environment, prev camera.
        {
            std::array<vk::DescriptorSetLayoutBinding, 8> bindings;
            for (std::uint32_t binding = 0; binding < 8; ++binding)
            {
                bindings[binding].setBinding(binding).setDescriptorCount(1);
                bindings[binding].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            }
            bindings[0].setDescriptorType(vk::DescriptorType::eUniformBuffer);
            bindings[1].setDescriptorType(vk::DescriptorType::eStorageBuffer);
            bindings[2].setDescriptorType(vk::DescriptorType::eUniformBuffer);
            bindings[3].setDescriptorType(vk::DescriptorType::eStorageBuffer);
            bindings[4].setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            bindings[5].setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            bindings[6].setDescriptorType(vk::DescriptorType::eUniformBuffer);
            bindings[7].setDescriptorType(vk::DescriptorType::eUniformBuffer);
            vk::DescriptorSetLayoutCreateInfo setInfo;
            setInfo.setBindings(bindings);
            sharedSetLayout = std::make_unique<vk::raii::DescriptorSetLayout>(context.device, setInfo);
        }

        // Set 1 (frame): G-buffer inputs, the HALF-res gather target (storage
        // write) re-read by the denoise (5), the history (sampled read), the
        // denoise's half-res filter output (6) re-read by the upsample (7),
        // and the FULL-res indirect target the upsample writes (8). Rev 31.
        {
            std::array<vk::DescriptorSetLayoutBinding, 9> bindings;
            for (std::uint32_t binding = 0; binding < 9; ++binding)
            {
                bindings[binding].setBinding(binding).setDescriptorCount(1);
                bindings[binding].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            }
            bindings[0].setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            bindings[1].setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            bindings[2].setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            bindings[3].setDescriptorType(vk::DescriptorType::eStorageImage);
            bindings[4].setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            bindings[5].setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            bindings[6].setDescriptorType(vk::DescriptorType::eStorageImage);
            bindings[7].setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            bindings[8].setDescriptorType(vk::DescriptorType::eStorageImage);
            vk::DescriptorSetLayoutCreateInfo setInfo;
            setInfo.setBindings(bindings);
            frameSetLayout = std::make_unique<vk::raii::DescriptorSetLayout>(context.device, setInfo);
        }

        vk::PushConstantRange pushRange;
        pushRange.setStageFlags(vk::ShaderStageFlagBits::eCompute);
        pushRange.setOffset(0);
        pushRange.setSize(sizeof(GiScreenWorldPush));
        vk::PipelineLayoutCreateInfo layoutInfo;
        const std::array<vk::DescriptorSetLayout, 2> setLayouts{**sharedSetLayout, **frameSetLayout};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushRange);
        pipelineLayout = std::make_unique<vk::raii::PipelineLayout>(context.device, layoutInfo);

        const std::vector<char> code
            = readShaderBytes(context.shaderDirectory / "gi_screen_world.comp.spv", moduleContext);
        vk::ShaderModuleCreateInfo moduleInfo;
        moduleInfo.setCodeSize(code.size()).setPCode(reinterpret_cast<const std::uint32_t *>(code.data()));
        vk::raii::ShaderModule module(context.device, moduleInfo);
        vk::PipelineShaderStageCreateInfo stage;
        stage.setStage(vk::ShaderStageFlagBits::eCompute).setModule(*module).setPName("main");
        vk::ComputePipelineCreateInfo pipelineInfo;
        pipelineInfo.setStage(stage);
        pipelineInfo.setLayout(**pipelineLayout);
        pipeline = std::make_unique<vk::raii::Pipeline>(context.device, *context.pipelineCache, pipelineInfo);

        const std::vector<char> denoiseCode
            = readShaderBytes(context.shaderDirectory / "gi_screen_world_denoise.comp.spv", moduleContext);
        vk::ShaderModuleCreateInfo denoiseModuleInfo;
        denoiseModuleInfo.setCodeSize(denoiseCode.size())
            .setPCode(reinterpret_cast<const std::uint32_t *>(denoiseCode.data()));
        vk::raii::ShaderModule denoiseModule(context.device, denoiseModuleInfo);
        vk::PipelineShaderStageCreateInfo denoiseStage;
        denoiseStage.setStage(vk::ShaderStageFlagBits::eCompute).setModule(*denoiseModule).setPName("main");
        vk::ComputePipelineCreateInfo denoisePipelineInfo;
        denoisePipelineInfo.setStage(denoiseStage);
        denoisePipelineInfo.setLayout(**pipelineLayout);
        denoisePipeline = std::make_unique<vk::raii::Pipeline>(context.device, *context.pipelineCache, denoisePipelineInfo);

        // Rev 31: the bilateral upsample (half-res filter -> full-res target).
        const std::vector<char> upsampleCode
            = readShaderBytes(context.shaderDirectory / "gi_screen_world_upsample.comp.spv", moduleContext);
        vk::ShaderModuleCreateInfo upsampleModuleInfo;
        upsampleModuleInfo.setCodeSize(upsampleCode.size())
            .setPCode(reinterpret_cast<const std::uint32_t *>(upsampleCode.data()));
        vk::raii::ShaderModule upsampleModule(context.device, upsampleModuleInfo);
        vk::PipelineShaderStageCreateInfo upsampleStage;
        upsampleStage.setStage(vk::ShaderStageFlagBits::eCompute).setModule(*upsampleModule).setPName("main");
        vk::ComputePipelineCreateInfo upsamplePipelineInfo;
        upsamplePipelineInfo.setStage(upsampleStage);
        upsamplePipelineInfo.setLayout(**pipelineLayout);
        upsamplePipeline = std::make_unique<vk::raii::Pipeline>(context.device, *context.pipelineCache, upsamplePipelineInfo);

        // Shared set 0.
        {
            std::array<vk::DescriptorPoolSize, 3> poolSizes{
                vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, 3),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 2),
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 2)
            };
            vk::DescriptorPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
            poolInfo.setMaxSets(1);
            poolInfo.setPoolSizes(poolSizes);
            sharedPool = std::make_unique<vk::raii::DescriptorPool>(context.device, poolInfo);
            vk::DescriptorSetAllocateInfo allocate;
            allocate.setDescriptorPool(**sharedPool);
            allocate.setSetLayouts(**sharedSetLayout);
            sharedSet = std::make_unique<vk::raii::DescriptorSet>(
                std::move(vk::raii::DescriptorSets(context.device, allocate).front())
            );

            vk::DescriptorBufferInfo cameraInfo(context.cameraBuffer, 0, sizeof(CameraData));
            vk::DescriptorBufferInfo primitivesInfo(context.primitivesBuffer, 0, vk::WholeSize);
            vk::DescriptorBufferInfo lightingInfo(**lightStorage.buffer, 0, sizeof(GpuLightUniform));
            vk::DescriptorBufferInfo materialsInfo(context.materialsBuffer, 0, vk::WholeSize);
            vk::DescriptorImageInfo albedoArrayInfo(**sampler, context.albedoArrayView, vk::ImageLayout::eShaderReadOnlyOptimal);
            vk::DescriptorImageInfo dataArrayInfo(**sampler, context.dataArrayView, vk::ImageLayout::eShaderReadOnlyOptimal);
            vk::DescriptorBufferInfo environmentInfo(context.environmentBuffer, 0, vk::WholeSize);
            vk::DescriptorBufferInfo prevCameraInfo(**prevCameraStorage.buffer, 0, sizeof(PreviousCameraUniform));
            std::array<vk::WriteDescriptorSet, 8> writes;
            writes[0].setDstSet(**sharedSet).setDstBinding(0).setDescriptorType(vk::DescriptorType::eUniformBuffer);
            writes[0].setBufferInfo(cameraInfo);
            writes[1].setDstSet(**sharedSet).setDstBinding(1).setDescriptorType(vk::DescriptorType::eStorageBuffer);
            writes[1].setBufferInfo(primitivesInfo);
            writes[2].setDstSet(**sharedSet).setDstBinding(2).setDescriptorType(vk::DescriptorType::eUniformBuffer);
            writes[2].setBufferInfo(lightingInfo);
            writes[3].setDstSet(**sharedSet).setDstBinding(3).setDescriptorType(vk::DescriptorType::eStorageBuffer);
            writes[3].setBufferInfo(materialsInfo);
            writes[4].setDstSet(**sharedSet).setDstBinding(4).setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[4].setImageInfo(albedoArrayInfo);
            writes[5].setDstSet(**sharedSet).setDstBinding(5).setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[5].setImageInfo(dataArrayInfo);
            writes[6].setDstSet(**sharedSet).setDstBinding(6).setDescriptorType(vk::DescriptorType::eUniformBuffer);
            writes[6].setBufferInfo(environmentInfo);
            writes[7].setDstSet(**sharedSet).setDstBinding(7).setDescriptorType(vk::DescriptorType::eUniformBuffer);
            writes[7].setBufferInfo(prevCameraInfo);
            context.device.updateDescriptorSets(writes, {});
        }

        // Frame set 1.
        {
            std::array<vk::DescriptorPoolSize, 2> poolSizes{
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 6),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, 3)
            };
            vk::DescriptorPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
            poolInfo.setMaxSets(1);
            poolInfo.setPoolSizes(poolSizes);
            framePool = std::make_unique<vk::raii::DescriptorPool>(context.device, poolInfo);
            vk::DescriptorSetAllocateInfo allocate;
            allocate.setDescriptorPool(**framePool);
            allocate.setSetLayouts(**frameSetLayout);
            frameSet = std::make_unique<vk::raii::DescriptorSet>(
                std::move(vk::raii::DescriptorSets(context.device, allocate).front())
            );

            std::array<vk::DescriptorImageInfo, 3> inputs;
            inputs[0] = vk::DescriptorImageInfo(**sampler, context.albedoView, vk::ImageLayout::eGeneral);
            inputs[1] = vk::DescriptorImageInfo(**sampler, context.normalView, vk::ImageLayout::eGeneral);
            inputs[2] = vk::DescriptorImageInfo(**sampler, context.depthView, vk::ImageLayout::eGeneral);
            // Rev 31: the gather writes the HALF-res target; the upsample
            // writes the FULL-res target the composite reads.
            vk::DescriptorImageInfo halfGatherTarget(**sampler, *halfGatherImage.view, vk::ImageLayout::eGeneral);
            vk::DescriptorImageInfo history(**linearSampler, *historyImage.view, vk::ImageLayout::eGeneral);
            vk::DescriptorImageInfo giInput(**sampler, *halfGatherImage.view, vk::ImageLayout::eGeneral);
            vk::DescriptorImageInfo filterTarget(**sampler, *filterImage.view, vk::ImageLayout::eGeneral);
            vk::DescriptorImageInfo fullTarget(**sampler, context.indirectRadianceView, vk::ImageLayout::eGeneral);
            std::array<vk::WriteDescriptorSet, 9> writes;
            for (std::uint32_t binding = 0; binding < 3; ++binding)
            {
                writes[binding]
                    .setDstSet(**frameSet)
                    .setDstBinding(binding)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
                writes[binding].setImageInfo(inputs[binding]);
            }
            writes[3].setDstSet(**frameSet).setDstBinding(3).setDescriptorType(vk::DescriptorType::eStorageImage);
            writes[3].setImageInfo(halfGatherTarget);
            writes[4].setDstSet(**frameSet).setDstBinding(4).setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[4].setImageInfo(history);
            writes[5].setDstSet(**frameSet).setDstBinding(5).setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[5].setImageInfo(giInput);
            writes[6].setDstSet(**frameSet).setDstBinding(6).setDescriptorType(vk::DescriptorType::eStorageImage);
            writes[6].setImageInfo(filterTarget);
            writes[7].setDstSet(**frameSet).setDstBinding(7).setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[7].setImageInfo(filterTarget);
            writes[8].setDstSet(**frameSet).setDstBinding(8).setDescriptorType(vk::DescriptorType::eStorageImage);
            writes[8].setImageInfo(fullTarget);
            context.device.updateDescriptorSets(writes, {});
        }

        cachedExtent  = context.extent;
        needsRecreate = false;
    }

    GiScreenWorldSettings settings;
    LightingCapability *lights = nullptr;
    const GiScreenWorldShadowPolicy shadowPolicy;
    vk::Extent2D cachedExtent{};
    bool needsRecreate = false;
    std::uint32_t frameIndex = 0;
    GpuLightUniform uploadedLights{};
    bool uploadedLightsValid = false;
    PreviousCameraUniform prevCamera{};
    MappedHostVisibleBuffer lightStorage;
    MappedHostVisibleBuffer prevCameraStorage;
    HistoryImage historyImage;
    HistoryImage halfGatherImage; // rev 31: the HALF-res gather target (storage write, denoise re-read)
    HistoryImage filterImage;     // the denoise dispatch's HALF-res output (copied into history; upsample input)
    bool historyNeedsInitialLayout = false;
    bool gatherNeedsInitialLayout  = false;
    bool filterNeedsInitialLayout  = false;
    std::unique_ptr<vk::raii::Sampler> sampler;
    std::unique_ptr<vk::raii::Sampler> linearSampler;
    // Layouts declared BEFORE the pools/sets/pipelines that embed them (the
    // pbr pass's destruction-order rule).
    std::unique_ptr<vk::raii::DescriptorSetLayout> sharedSetLayout;
    std::unique_ptr<vk::raii::DescriptorSetLayout> frameSetLayout;
    std::unique_ptr<vk::raii::DescriptorPool> sharedPool;
    std::unique_ptr<vk::raii::DescriptorSet> sharedSet;
    std::unique_ptr<vk::raii::DescriptorPool> framePool;
    std::unique_ptr<vk::raii::DescriptorSet> frameSet;
    std::unique_ptr<vk::raii::PipelineLayout> pipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> pipeline;
    std::unique_ptr<vk::raii::Pipeline> denoisePipeline;
    std::unique_ptr<vk::raii::Pipeline> upsamplePipeline; // rev 31: half-res filter -> full-res target
};

// ---- Module lifecycle ----------------------------------------------------------------

GiScreenWorldModule::GiScreenWorldModule()
{
    componentManifest = ComponentManifest{
        .name    = "gi_screen_world",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        // "gi_screen_world_diffuse" is the technique's demand marker: the run demands
        // it when the ray-traced diffuse GI pass should attach to the renderer
        // (the same wiring contract pbr_direct_diffuse follows). Deliberately
        // NOT a "lighting.*" family member: that family resolves to ONE
        // provider and the scene light state belongs to the pbr module - this
        // module only reads it through the LightingCapability contract.
        .providedCapabilities = {"gi_screen_world_diffuse"},
        // "lighting" is a REQUIREMENT, not a provided family: the relight
        // fills its light uniform from the scene's LightingCapability (the
        // pbr module implements it). Declaring it here (instead of acquiring
        // defensively) is what makes the permission grant explicit - the
        // kernel blocks capability acquisitions the manifest did not
        // declare, which is exactly the failure a silent degradation would
        // have hidden.
        .requiredCapabilities = {"logging", "renderer", "lighting"},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging", "capability.renderer", "capability.lighting"},
        .knownConfigKeys      = {
            "lighting.gi_screen_world.enabled",
            "lighting.gi_screen_world.max_distance",
            "lighting.gi_screen_world.march_steps",
            "lighting.gi_screen_world.screen_trace",
            "lighting.gi_screen_world.screen_steps",
            "lighting.gi_screen_world.screen_refine",
            "lighting.gi_screen_world.screen_max_distance",
            "lighting.gi_screen_world.screen_thickness",
            "lighting.gi_screen_world.rays_per_pixel",
            "lighting.gi_screen_world.indirect_clamp",
            "lighting.gi_screen_world.second_bounce",
            "lighting.gi_screen_world.temporal_alpha",
            "lighting.gi_screen_world.temporal_max_frames",
            "lighting.gi_screen_world.temporal_normal_rejection",
            "lighting.gi_screen_world.short_range_ao_radius",
            "lighting.gi_screen_world.short_range_ao_strength",
            "lighting.gi_screen_world.filter_radius",
            "lighting.gi_screen_world.intensity",
            "lighting.gi_screen_world.sky_miss_scale",
            "lighting.gi_screen_world.animated_jitter",
            // Shadow policy hand-off: the relight mirrors the direct pass's
            // keys so both sides of the light transport agree on occlusion.
            "lighting.pbr.shadows",
            "lighting.pbr.shadow_steps",
        },
    };
}

GiScreenWorldModule::~GiScreenWorldModule()
{
    stop();
}

void GiScreenWorldModule::construct(
    ComponentContext &context
)
{
    logger = &context.acquireCapability<LoggerCapability>();

    settings.enabled
        = context.configurationBooleanInSection("lighting", "gi_screen_world.enabled").value_or(true);
    settings.maxDistance
        = static_cast<float>(context.configurationFloatingInSection("lighting", "gi_screen_world.max_distance").value_or(60.0));
    settings.marchSteps = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(context.configurationIntegerInSection("lighting", "gi_screen_world.march_steps").value_or(96), 8, 512));
    // The screen stage (Lumen "Screen Traces" - the rasterized-truth detail
    // source every gather ray consults before the world SDF).
    settings.screenTrace
        = context.configurationBooleanInSection("lighting", "gi_screen_world.screen_trace").value_or(true);
    settings.screenSteps = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(context.configurationIntegerInSection("lighting", "gi_screen_world.screen_steps").value_or(24), 1, 64));
    settings.screenRefine = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(context.configurationIntegerInSection("lighting", "gi_screen_world.screen_refine").value_or(6), 1, 8));
    settings.screenMaxDistance
        = static_cast<float>(context.configurationFloatingInSection("lighting", "gi_screen_world.screen_max_distance").value_or(24.0));
    settings.screenThickness
        = static_cast<float>(context.configurationFloatingInSection("lighting", "gi_screen_world.screen_thickness").value_or(0.12));
    // The final gather's estimator.
    settings.raysPerPixel = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(context.configurationIntegerInSection("lighting", "gi_screen_world.rays_per_pixel").value_or(2), 1, 8));
    settings.indirectClamp
        = static_cast<float>(context.configurationFloatingInSection("lighting", "gi_screen_world.indirect_clamp").value_or(8.0));
    // The second bounce (the screen-space radiance memory).
    settings.secondBounce
        = context.configurationBooleanInSection("lighting", "gi_screen_world.second_bounce").value_or(true);
    // Temporal accumulation.
    settings.temporalAlpha
        = static_cast<float>(context.configurationFloatingInSection("lighting", "gi_screen_world.temporal_alpha").value_or(0.125));
    settings.temporalMaxFrames = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(context.configurationIntegerInSection("lighting", "gi_screen_world.temporal_max_frames").value_or(64), 1, 4096));
    settings.temporalNormalRejection
        = context.configurationBooleanInSection("lighting", "gi_screen_world.temporal_normal_rejection").value_or(true);
    // Short-range occlusion (Lumen ShortRangeAO; 0 px = off).
    settings.shortRangeAoRadius = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(context.configurationIntegerInSection("lighting", "gi_screen_world.short_range_ao_radius").value_or(0), 0, 16));
    settings.shortRangeAoStrength
        = static_cast<float>(context.configurationFloatingInSection("lighting", "gi_screen_world.short_range_ao_strength").value_or(1.0));
    // The spatial filter.
    settings.filterRadius = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(context.configurationIntegerInSection("lighting", "gi_screen_world.filter_radius").value_or(1), 0, 4));
    // Output shaping.
    settings.intensity
        = static_cast<float>(context.configurationFloatingInSection("lighting", "gi_screen_world.intensity").value_or(1.0));
    settings.skyMissScale
        = static_cast<float>(context.configurationFloatingInSection("lighting", "gi_screen_world.sky_miss_scale").value_or(1.0));
    settings.animatedJitter
        = context.configurationBooleanInSection("lighting", "gi_screen_world.animated_jitter").value_or(true);

    // Attachment follows demand (the pbr module's wiring contract): the
    // kernel activated this module because the run demanded one of its
    // capabilities; the pass attaches only for the technique's marker.
    passAttached = settings.enabled && context.isCapabilityDemanded("gi_screen_world_diffuse");

    // The relight reads the scene's light state through the LightingCapability
    // (the pbr module implements it; the shared light_uniform.hpp fill keeps
    // the transport consistent with the direct pass). Only an ATTACHED pass
    // needs it: an idle module never touches the lighting family. The shadow
    // policy mirrors the direct pass's keys so transport and direct shading
    // can never disagree about occlusion.
    LightingCapability *sharedLights = nullptr;
    GiScreenWorldShadowPolicy shadowPolicy;
    if (passAttached)
    {
        sharedLights = &context.acquireCapability<LightingCapability>();
        shadowPolicy.shadowsEnabled
            = context.configurationBooleanInSection("lighting", "pbr.shadows").value_or(true);
        shadowPolicy.shadowSteps = static_cast<std::int32_t>(
            context.configurationIntegerInSection("lighting", "pbr.shadow_steps").value_or(24));
    }

    pass = std::make_shared<GiScreenWorldPass>(settings, sharedLights, shadowPolicy);
    if (passAttached)
    {
        RendererCapability &rendererCapability = context.acquireCapability<RendererCapability>();
        // In-tree wiring: this build's renderer capability is implemented by
        // RendererModule (see app/main.cpp, the composition root).
        auto *rendererModule = static_cast<RendererModule *>(&rendererCapability);
        rendererModule->attachLightingTechnique(pass);
    }
}

void GiScreenWorldModule::start()
{
    logger->logMessage(
        LogLevel::Information,
        std::string("gi_screen_world module started; technique ")
            + (passAttached
                   ? "attached (indirect diffuse radiance)"
                   : "gi_screen_world_diffuse not demanded or disabled; idle")
            + ", screen_trace " + (settings.screenTrace ? "on" : "off") + " ("
            + std::to_string(settings.screenSteps) + " strides, "
            + std::to_string(settings.screenRefine) + " bisect, "
            + std::to_string(settings.screenMaxDistance) + " m, thickness "
            + std::to_string(settings.screenThickness) + " m)"
            + ", max_distance " + std::to_string(settings.maxDistance) + " m, march_steps "
            + std::to_string(settings.marchSteps) + ", second_bounce " + (settings.secondBounce ? "on" : "off")
            + ", rays_per_pixel " + std::to_string(settings.raysPerPixel) + ", indirect_clamp "
            + std::to_string(settings.indirectClamp) + ", temporal_alpha "
            + std::to_string(settings.temporalAlpha) + ", temporal_max_frames "
            + std::to_string(settings.temporalMaxFrames) + ", normal_rejection "
            + (settings.temporalNormalRejection ? "on" : "off") + ", short_range_ao "
            + std::to_string(settings.shortRangeAoRadius) + " px, filter_radius "
            + std::to_string(settings.filterRadius) + ", intensity "
            + std::to_string(settings.intensity) + ", sky_miss_scale "
            + std::to_string(settings.skyMissScale) + ", animated_jitter "
            + (settings.animatedJitter ? "on" : "off")
    );
}

void GiScreenWorldModule::stop()
{
    if (passAttached)
    {
        // Detach before the pass dies: the renderer may outlive this module.
        // The renderer holds a shared_ptr; reverse construction order (the
        // kernel tears modules down opposite to construct) releases it before
        // the lighting capability this pass reads disappears.
        passAttached = false;
    }
    pass.reset();
    logger = nullptr;
}

void *GiScreenWorldModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    // "gi_screen_world_diffuse" is a demand marker without a call interface - the
    // module object itself is the implementation behind it.
    if (capabilityName == "gi_screen_world_diffuse")
    {
        return this;
    }
    return nullptr;
}

} // namespace engine
