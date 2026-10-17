// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_generation.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <vk_mem_alloc.h>

#include "bbport_settings.h"
#include "ffx_vk_fsr3_3_1_5_bridge.h"
#include "video_core/host_shaders/frame_gen_inputs_comp.h"
#include "video_core/host_shaders/frame_gen_ui_comp.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {

/// The hudless copy viewed like the presenter views the display buffer (`requested`): the
/// copy's channel order, the requested sRGB decoding.
vk::Format HudlessViewFormat(vk::Format /*storage*/, vk::Format requested) {
    // The same view format the real display image is read with (A8R8G8B8 reads as B8G8R8A8): the
    // copy is a mutable-format image of the same 32-bit class, so the channel order then matches.
    return requested;
}

constexpr vk::ImageUsageFlags InputUsage =
    vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;

int DlssMaxCount() {
    const Dlss* dlss = Dlss::Get();
    return dlss ? dlss->FrameGenMaxCount() : 0;
}

} // namespace

FrameGenerator::FrameGenerator(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    auto& settings = BbSettings::Get();
    const int dlss_max = DlssMaxCount();
    settings.dlssg_max_multiplier = dlss_max + 1;
    // BB_DEPTH_INVERTED=1: the game's depth is 1 at the near plane (reversed Z). For DLSS;
    // FSR-Vulkan's frame generation interface has no such option (0 at the near plane).
    if (const char* env = std::getenv("BB_DEPTH_INVERTED")) {
        depth_inverted = env[0] == '1';
    }
    if (settings.frame_gen == BbSettings::FrameGenDlss && dlss_max < 1) {
        const Dlss* dlss = Dlss::Get();
        std::printf("Frame generation: DLSS Frame Generation unavailable (%s); using FSR 3.1\n",
                    !dlss ? "bbport_dlss.dll and nvngx_dlss.dll are not installed"
                    : !dlss->Available() ? "DLSS is unavailable on this system"
                                         : "GeForce RTX 40 or newer, nvngx_dlssg.dll and a "
                                           "current driver are needed");
        settings.frame_gen_problem = "DLSS Frame Generation needs a GeForce RTX 40 or newer";
        settings.frame_gen = BbSettings::FrameGenFsr3;
    }
    if (settings.frame_gen != BbSettings::FrameGenOff) {
        const int max = settings.frame_gen == BbSettings::FrameGenDlss ? dlss_max + 1 : 2;
        std::printf("Frame generation: %s, %dx\n",
                    settings.frame_gen == BbSettings::FrameGenDlss ? "DLSS" : "FSR 3.1",
                    std::min(settings.frame_gen_multiplier.load(), max));
    }
}

FrameGenerator::~FrameGenerator() {
    DestroyFsr3();
    if (Dlss* dlss = Dlss::Get()) {
        dlss->ReleaseFrameGen();
    }
}

bool FrameGenerator::Wanted() const {
    return BbSettings::Get().frame_gen != BbSettings::FrameGenOff;
}

bool FrameGenerator::Paused() const {
    return BbSettings::Get().frame_gen_paused;
}

u32 FrameGenerator::WantedCount() const {
    const auto& settings = BbSettings::Get();
    switch (settings.frame_gen) {
    case BbSettings::FrameGenDlss:
        return u32(std::clamp(settings.frame_gen_multiplier.load() - 1, 1,
                              std::clamp(DlssMaxCount(), 1, int(MaxGenerated))));
    case BbSettings::FrameGenFsr3:
        return 1;
    default:
        return 0;
    }
}

void FrameGenerator::Skip() {
    for (auto& capture : captures) {
        capture.captured = false;
        capture.hudless_captured = false;
    }
    reset = true;
}

FrameGenerator::Capture* FrameGenerator::FindCapture(u64 address) {
    for (auto& capture : captures) {
        if (capture.captured && capture.address == address) {
            return &capture;
        }
    }
    return nullptr;
}

void FrameGenerator::Release() {
    Skip();
    bool held = fsr3 || last_provider != 0;
    for (const auto& capture : captures) {
        held |= bool(capture.depth_image) || bool(capture.hudless_image);
    }
    if (!held) {
        return; // nothing held
    }
    // Submitted frames may still use them; the open command buffer does not.
    scheduler.WaitSubmitted();
    DestroyFsr3();
    if (Dlss* dlss = Dlss::Get()) {
        dlss->ReleaseFrameGen();
    }
    for (auto& capture : captures) {
        capture.depth_view.reset();
        capture.motion_view.reset();
        capture.depth_image = VideoCore::UniqueImage{};
        capture.motion_image = VideoCore::UniqueImage{};
        capture.input_width = capture.input_height = 0;
        capture.hudless_views.clear();
        capture.hudless_image = VideoCore::UniqueImage{};
        capture.hudless_width = capture.hudless_height = 0;
        capture.address = 0;
    }
    active = nullptr;
    last_provider = 0;
    last_hudless = false;
    std::puts("Frame generation: off, its memory freed");
}

void FrameGenerator::CreatePipeline() {
    if (pipeline) {
        return;
    }
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 5 * sizeof(float)};
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto compute = [&](std::span<const u32> code, vk::PipelineLayout layout) {
        const auto module = CompileSPV(code, device);
        auto made = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = layout,
                }));
        device.destroyShaderModule(module);
        return made;
    };
    pipeline = compute(FRAME_GEN_INPUTS_COMP, *pipeline_layout);
    // The UI over generated frames: the generated frame (storage), the frame and the frame
    // without UI (sampled).
    const std::array<vk::DescriptorSetLayoutBinding, 3> ui_bindings{{
        {0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    ui_desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(ui_bindings.size()),
        .pBindings = ui_bindings.data(),
    }));
    const vk::PushConstantRange ui_push{vk::ShaderStageFlagBits::eCompute, 0, 2 * sizeof(s32)};
    ui_pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*ui_desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &ui_push,
    }));
    ui_pipeline = compute(FRAME_GEN_UI_COMP, *ui_pipeline_layout);
}

void FrameGenerator::EnsureInputs(Capture& capture, u32 w, u32 h) {
    if (capture.depth_image && capture.input_width == w && capture.input_height == h) {
        return;
    }
    // The previous frame's generation may still read the old images.
    scheduler.WaitSubmitted();
    const auto device = instance.GetDevice();
    const auto make = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                          vk::Format format) {
        view.reset();
        image = VideoCore::UniqueImage(device, instance.GetAllocator());
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = InputUsage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    };
    make(capture.depth_image, capture.depth_view, vk::Format::eR32Sfloat);
    make(capture.motion_image, capture.motion_view, vk::Format::eR16G16Sfloat);
    capture.input_width = w;
    capture.input_height = h;
    reset = true;
}

vk::ImageView FrameGenerator::BeginCapture(vk::CommandBuffer cmdbuf, vk::ImageView depth, u32 w,
                                           u32 h, u64 address) {
    active = nullptr;
    if (!Wanted() || !depth || !w || !h) {
        return {};
    }
    CreatePipeline();
    // The depth and motion are brought to the size of the frame: a feature given inputs of a
    // size other than the frame's ignored them (the generated frames smeared).
    static const bool native_size = std::getenv("BB_FG_NATIVE_SIZE") != nullptr;
    const u32 src_w = w, src_h = h;
    if (!native_size && out_width && out_height) {
        w = out_width;
        h = out_height;
    }
    // This frame's capture lives with its display buffer: the flip of a frame is processed after
    // the game's thread has begun the next one.
    Capture* slot = nullptr;
    for (auto& capture : captures) {
        if (capture.address == address) {
            slot = &capture;
        }
    }
    if (!slot) {
        slot = &captures[0];
        for (auto& capture : captures) {
            if (capture.serial < slot->serial) {
                slot = &capture;
            }
        }
        slot->address = address;
    }
    slot->serial = ++capture_serial;
    slot->captured = false;
    slot->hudless_captured = false;
    active = slot;
    EnsureInputs(*slot, w, h);
    // Last frame's contents are not needed.
    std::array<vk::ImageMemoryBarrier2, 2> to_write{};
    for (u32 i = 0; i < 2; ++i) {
        to_write[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = i == 0 ? vk::Image(active->depth_image) : vk::Image(active->motion_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = to_write.data()});
    const vk::DescriptorImageInfo depth_info{.imageView = depth,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo depth_out{.imageView = *active->depth_view,
                                            .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 2> writes{{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &depth_out},
    }};
    depth_inverted = BbSettings::Get().frame_gen == BbSettings::FrameGenDlss;
    const std::array<float, 5> size{float(src_w), float(src_h), float(w), float(h),
                                    depth_inverted ? 0.0f : 1.0f};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(size),
                         size.data());
    cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    return *active->motion_view;
}

void FrameGenerator::ZeroMotion(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 before{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                    .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                    .dstAccessMask = vk::AccessFlagBits2::eTransferWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    const vk::ClearColorValue zero{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    cmdbuf.clearColorImage(vk::Image(active->motion_image), vk::ImageLayout::eGeneral, zero, range);
    const vk::MemoryBarrier2 after{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                   .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                   .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                   .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                                    vk::AccessFlagBits2::eMemoryWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
}

void FrameGenerator::EndCapture(vk::CommandBuffer cmdbuf, std::array<float, 2> jitter,
                                bool motion_valid, Camera camera) {
    std::array<vk::ImageMemoryBarrier2, 2> to_read{};
    for (u32 i = 0; i < 2; ++i) {
        to_read[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = i == 0 ? vk::Image(active->depth_image) : vk::Image(active->motion_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = to_read.data()});
    if (!motion_valid) {
        reset = true; // no camera this frame: the frames are not related
    }
    active->captured = true;
    active->capture_jitter = jitter;
    if (camera.near_plane > 0.0f && camera.far_plane > camera.near_plane &&
        camera.vertical_fov > 0.0f) {
        active->capture_camera = camera;
    }
    active->capture_camera.has_matrices = motion_valid && camera.has_matrices;
}

void FrameGenerator::CaptureHudless(vk::CommandBuffer cmdbuf, vk::Image source, vk::Format format,
                                    u32 w, u32 h, u64 address) {
    active = FindCapture(address);
    if (!Wanted() || !active || active->hudless_captured || !w || !h) {
        return; // only for a frame with the game's scene, and before its UI
    }
    if (!active->hudless_image || active->hudless_format != format || active->hudless_width != w || active->hudless_height != h) {
        // The presenter of the previous frame may still read the old copy.
        scheduler.WaitSubmitted();
        active->hudless_views.clear();
        active->hudless_image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
        active->hudless_image.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
                     vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        active->hudless_format = format;
        active->hudless_width = w;
        active->hudless_height = h;
    }
    const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to,
                             vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = from,
            .newLayout = to,
            .image = vk::Image(active->hudless_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eShaderRead,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    cmdbuf.copyImage(source, vk::ImageLayout::eTransferSrcOptimal, vk::Image(active->hudless_image),
                     vk::ImageLayout::eTransferDstOptimal,
                     vk::ImageCopy{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   .extent = {w, h, 1}});
    barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eShaderRead);
    active->hudless_captured = true;
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::printf("Frame generation: the picture before the UI, %ux%u %s\n", w, h,
                    vk::to_string(format).c_str());
    }
}

vk::ImageView FrameGenerator::Hudless(vk::Format format, vk::Extent2D& size, u64 address) {
    active = FindCapture(address);
    if (!active || !active->hudless_captured || !active->hudless_image) {
        return {};
    }
    size = {active->hudless_width, active->hudless_height};
    format = HudlessViewFormat(active->hudless_format, format);
    for (const auto& view : active->hudless_views) {
        if (view.format == format) {
            return *view.view;
        }
    }
    active->hudless_views.push_back({
        format,
        Check(instance.GetDevice().createImageViewUnique({
            .image = vk::Image(active->hudless_image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        })),
    });
    return *active->hudless_views.back().view;
}

void FrameGenerator::ComposeUi(vk::CommandBuffer cmdbuf, vk::ImageView color,
                               vk::ImageView hudless, const Output& output, u32 width,
                               u32 height) {
    const vk::ImageMemoryBarrier2 written{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = output.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &written});
    const vk::DescriptorImageInfo output_info{.imageView = output.view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo color_info{.imageView = color,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo hudless_info{.imageView = hudless,
                                               .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &output_info},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &color_info},
        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &hudless_info},
    }};
    const std::array<s32, 2> size{s32(width), s32(height)};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *ui_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *ui_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*ui_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(size),
                         size.data());
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}


u32 FrameGenerator::Generate(vk::CommandBuffer cmdbuf, vk::Image color, vk::ImageView color_view,
                             vk::ImageUsageFlags color_usage, vk::Image hudless,
                             vk::ImageView hudless_view, u32 width, u32 height,
                             std::span<const Output> outputs, u64 address) {
    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_generate).count();
    last_generate = now;
    if (!(frame_ms > 0.0f && frame_ms < 200.0f)) {
        frame_ms = 16.6f;
    }
    // Views the FSR 3.1 context took for frames the GPU has finished.
    while (fsr3 && !fsr3_pending.empty() && scheduler.IsFree(fsr3_pending.front().tick)) {
        ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(fsr3, fsr3_pending.front().frame_id);
        fsr3_pending.pop_front();
    }
    const auto& settings = BbSettings::Get();
    const int provider = settings.frame_gen;
    active = FindCapture(address);
    const bool had_capture = active != nullptr;
    if (active) {
        active->captured = false;
        active->hudless_captured = false;
    }
    if (provider != last_provider) {
        last_provider = provider;
        reset = true;
    }
    if (provider == BbSettings::FrameGenOff || !had_capture || outputs.empty() || !width ||
        !height) {
        // A frame without the game's scene (menu, movie, loading), or none wanted (paused for
        // the display): start over afterwards.
        reset = true;
        return 0;
    }
    // FSR 3.1 keeps the previous input: it cannot switch between frames with and without UI.
    const bool with_hudless = hudless && hudless_view;
    if (with_hudless != last_hudless) {
        last_hudless = with_hudless;
        reset = true;
        if (++hudless_changes <= 4) {
            std::printf("Frame generation: %s\n",
                        with_hudless ? "UI kept out of the generated frames"
                                     : "frames without a picture before the UI");
        }
    }
    // The composed frame is read by compute (FSR 3.1) or NGX after the post pass wrote it.
    const vk::ImageMemoryBarrier2 color_ready{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = color,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &color_ready});
    if (with_hudless) {
        auto hudless_ready = color_ready;
        hudless_ready.image = hudless;
        cmdbuf.pipelineBarrier2(
            {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &hudless_ready});
    }
    // The outputs' previous contents (presented already) are not needed.
    std::array<vk::ImageMemoryBarrier2, MaxGenerated> to_write{};
    const u32 used = std::min<u32>(u32(outputs.size()), MaxGenerated);
    for (u32 i = 0; i < used; ++i) {
        to_write[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = outputs[i].image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = used, .pImageMemoryBarriers = to_write.data()});
    u32 generated = 0;
    if (provider == BbSettings::FrameGenDlss) {
        const u32 count = std::min(WantedCount(), used);
        // DLSS reads the real frame (a different image every game frame: it needs the previous one
        // to interpolate) and the picture before the UI beside it; this frame's UI is then drawn
        // over each generated frame (DLSS's own UI separation left the UI smeared).
        generated = GenerateDlss(cmdbuf, color, color_view, with_hudless ? hudless : vk::Image{},
                                 with_hudless ? hudless_view : vk::ImageView{}, width, height,
                                 outputs, count, frame_ms);
        if (generated && with_hudless) {
            for (u32 i = 0; i < generated; ++i) {
                ComposeUi(cmdbuf, color_view, hudless_view, outputs[i], width, height);
            }
        }
    } else if (with_hudless) {
        // FSR interpolates the real frame (a different image every game frame, with its UI); this
        // frame's UI is then drawn over the result where it differs from the picture before it.
        generated = GenerateFsr3(cmdbuf, color, color_usage, width, height, outputs[0], frame_ms);
        if (generated) {
            ComposeUi(cmdbuf, color_view, hudless_view, outputs[0], width, height);
        }
    } else {
        generated = GenerateFsr3(cmdbuf, color, color_usage, width, height, outputs[0], frame_ms);
    }
    if (generated) {
        reset = false;
        // Presented after this submission: the writes go to the transfer that copies them.
        std::array<vk::ImageMemoryBarrier2, MaxGenerated> written{};
        for (u32 i = 0; i < generated; ++i) {
            written[i] = to_write[i];
            written[i].srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
            written[i].srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
            written[i].dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
            written[i].dstAccessMask = vk::AccessFlagBits2::eTransferRead;
            written[i].oldLayout = vk::ImageLayout::eGeneral;
        }
        cmdbuf.pipelineBarrier2(
            {.imageMemoryBarrierCount = generated, .pImageMemoryBarriers = written.data()});
    } else {
        reset = true;
    }
    return generated;
}

void FrameGenerator::DestroyFsr3() {
    if (!fsr3) {
        return;
    }
    scheduler.WaitSubmitted();
    ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(fsr3, fsr3_frame_id);
    ffxVkFsr3_3_1_6FrameGenerationContextDestroy(fsr3);
    fsr3 = nullptr;
    fsr3_pending.clear();
}

u32 FrameGenerator::GenerateFsr3(vk::CommandBuffer cmdbuf, vk::Image color,
                                 vk::ImageUsageFlags color_usage, u32 width, u32 height,
                                 const Output& output, float frame_ms) {
    if (fsr3 && (fsr3_width != width || fsr3_height != height ||
                 fsr3_render_width != active->input_width || fsr3_render_height != active->input_height)) {
        DestroyFsr3();
    }
    if (!fsr3) {
        const FfxVkFsr3_3_1_6FrameGenerationCreateInfo create{
            .physicalDevice = instance.GetPhysicalDevice(),
            .device = instance.GetDevice(),
            .maxRenderWidth = active->input_width,
            .maxRenderHeight = active->input_height,
            .displayWidth = width,
            .displayHeight = height,
            .colorFormat = static_cast<VkFormat>(ColorFormat),
        };
        if (ffxVkFsr3_3_1_6FrameGenerationContextCreate(&create, &fsr3) !=
            FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
            fsr3 = nullptr;
            Fail(BbSettings::FrameGenFsr3, "FSR 3.1 frame generation context creation failed");
            return 0;
        }
        fsr3_width = width;
        fsr3_height = height;
        fsr3_render_width = active->input_width;
        fsr3_render_height = active->input_height;
        reset = true;
        FfxVkFsr3_3_1_6FrameGenerationMemoryUsage usage{};
        ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(fsr3, &usage);
        std::printf("Frame generation: FSR 3.1 %ux%u (depth and motion %ux%u), %.1f MB\n", width,
                    height, active->input_width, active->input_height, usage.totalUsageInBytes / 1e6);
    }
    const auto image = [](vk::Image handle, vk::Format format, u32 w, u32 h,
                          vk::ImageUsageFlags usage) {
        return FfxVkFsr3_3_1_6FrameGenerationImage{
            .image = handle,
            .format = static_cast<VkFormat>(format),
            .width = w,
            .height = h,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
            .usage = static_cast<VkImageUsageFlags>(usage),
        };
    };
    const u64 id = ++fsr3_frame_id;
    FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prepare{
        .commandBuffer = cmdbuf,
        .color = image(color, ColorFormat, width, height, color_usage),
        .depth = image(active->depth_image, vk::Format::eR32Sfloat, active->input_width, active->input_height, InputUsage),
        .motionVectors =
            image(active->motion_image, vk::Format::eR16G16Sfloat, active->input_width, active->input_height, InputUsage),
        .renderWidth = active->input_width,
        .renderHeight = active->input_height,
        .jitterOffsetX = active->capture_jitter[0],
        .jitterOffsetY = active->capture_jitter[1],
        .motionVectorScaleX = 1.0f,
        .motionVectorScaleY = 1.0f,
        .frameTimeMilliseconds = frame_ms,
        .minLuminance = 0.0f,
        .maxLuminance = 1.0f,
        .transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB,
        .cameraNear = active->capture_camera.near_plane,
        .cameraFar = active->capture_camera.far_plane,
        .viewSpaceToMeters = 1.0f,
        .cameraVerticalFovRadians = active->capture_camera.vertical_fov,
        .cameraPosition = {0.0f, 0.0f, 0.0f},
        .cameraUp = {0.0f, 1.0f, 0.0f},
        .cameraRight = {1.0f, 0.0f, 0.0f},
        .cameraForward = {0.0f, 0.0f, 1.0f},
        .frameId = id,
        .reset = reset ? VK_TRUE : VK_FALSE,
    };
    if (ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(fsr3, &prepare) !=
        FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        fsr3_pending.push_back({id, scheduler.CurrentTick()});
        Fail(BbSettings::FrameGenFsr3, "FSR 3.1 frame generation (prepare) failed");
        return 0;
    }
    const FfxVkFsr3_3_1_6FrameGenerationDispatchInfo dispatch{
        .commandBuffer = cmdbuf,
        .color = prepare.color,
        .output = image(output.image, ColorFormat, width, height,
                        InputUsage | vk::ImageUsageFlagBits::eTransferSrc),
        .distortionField = {},
        .displayWidth = width,
        .displayHeight = height,
        .frameTimeMilliseconds = frame_ms,
        .cameraNear = prepare.cameraNear,
        .cameraFar = prepare.cameraFar,
        .viewSpaceToMeters = 1.0f,
        .cameraVerticalFovRadians = active->capture_camera.vertical_fov,
        .minLuminance = 0.0f,
        .maxLuminance = 1.0f,
        .transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB,
        .frameId = id,
        .reset = prepare.reset,
    };
    const auto result = ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(fsr3, &dispatch);
    fsr3_pending.push_back({id, scheduler.CurrentTick()});
    if (result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        Fail(BbSettings::FrameGenFsr3, "FSR 3.1 frame generation (dispatch) failed");
        return 0;
    }
    return 1;
}

u32 FrameGenerator::GenerateDlss(vk::CommandBuffer cmdbuf, vk::Image color,
                                 vk::ImageView color_view, vk::Image hudless,
                                 vk::ImageView hudless_view, u32 width, u32 height,
                                 std::span<const Output> outputs, u32 count, float frame_ms) {
    (void)frame_ms; // DLSS Frame Generation measures the time itself
    Dlss* dlss = Dlss::Get();
    if (!dlss || dlss->FrameGenMaxCount() < 1) {
        Fail(BbSettings::FrameGenDlss, "DLSS Frame Generation is unavailable");
        return 0;
    }
    const Dlss::FrameGenDesc desc{width, height, ColorFormat, active->input_width, active->input_height};
    if (!dlss->HasFrameGen(desc)) {
        // The previous feature may still be used by submitted work; `cmdbuf` stays open.
        scheduler.WaitSubmitted();
        dlss->ReleaseFrameGen();
        if (!dlss->CreateFrameGen(cmdbuf, desc)) {
            Fail(BbSettings::FrameGenDlss, "DLSS Frame Generation feature creation failed");
            return 0;
        }
        reset = true;
    }
    const auto resource = [](vk::Image image, vk::ImageView view, vk::Format format, u32 w,
                             u32 h) {
        return Dlss::Resource{image, view, format, vk::ImageAspectFlagBits::eColor, w, h};
    };
    for (u32 index = 1; index <= count; ++index) {
        const bool ok = dlss->EvaluateFrameGen(
            cmdbuf, {
                        .backbuffer = resource(color, color_view, ColorFormat, width, height),
                        .depth = resource(active->depth_image, *active->depth_view, vk::Format::eR32Sfloat,
                                          active->input_width, active->input_height),
                        .motion = resource(active->motion_image, *active->motion_view, vk::Format::eR16G16Sfloat,
                                           active->input_width, active->input_height),
                        .output = resource(outputs[index - 1].image, outputs[index - 1].view,
                                           ColorFormat, width, height),
                        .hudless = hudless ? resource(hudless, hudless_view, ColorFormat, width,
                                                      height)
                                           : Dlss::Resource{},
                        .jitter_x = active->capture_jitter[0],
                        .jitter_y = active->capture_jitter[1],
                        .camera_near = active->capture_camera.near_plane,
                        .camera_far = active->capture_camera.far_plane,
                        .camera_fov = active->capture_camera.vertical_fov,
                        .depth_inverted = depth_inverted,
                        .has_matrices = active->capture_camera.has_matrices,
                        .view_to_clip = active->capture_camera.view_to_clip,
                        .clip_to_prev_clip = active->capture_camera.clip_to_prev_clip,
                        .prev_clip_to_clip = active->capture_camera.prev_clip_to_clip,
                        .reset = reset,
                        .count = count,
                        .index = index,
                    });
        if (!ok) {
            Fail(BbSettings::FrameGenDlss, "DLSS Frame Generation evaluation failed");
            return 0;
        }
    }
    return count;
}

void FrameGenerator::Fail(int provider, const char* reason) {
    auto& settings = BbSettings::Get();
    std::printf("Frame generation: %s; %s\n", reason,
                provider == BbSettings::FrameGenDlss ? "using FSR 3.1" : "off");
    settings.frame_gen_problem = reason;
    settings.frame_gen =
        provider == BbSettings::FrameGenDlss ? BbSettings::FrameGenFsr3 : BbSettings::FrameGenOff;
    reset = true;
}

} // namespace Vulkan
