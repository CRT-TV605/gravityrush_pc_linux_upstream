// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_camera_motion.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gow3_toggles.h"

#include "video_core/host_shaders/camera_motion_comp.h"
#include "video_core/host_shaders/camera_motion_debug_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

struct PushConstants {
    std::array<float, 12> reproject;
    std::array<float, 4> proj;
    std::array<float, 4> prev_proj;
    std::array<float, 2> size;
    std::array<float, 2> jitter;
    std::array<float, 2> previous_jitter;
    u32 mode;
};

/// Inverse of a 4x4 matrix (rows), Gauss-Jordan with partial pivoting; false when singular.
bool Invert4(const float* m, std::array<double, 16>& out) {
    std::array<double, 32> a{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r * 8 + c] = m[r * 4 + c];
        }
        a[r * 8 + 4 + r] = 1.0;
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::abs(a[r * 8 + col]) > std::abs(a[pivot * 8 + col])) {
                pivot = r;
            }
        }
        if (std::abs(a[pivot * 8 + col]) < 1e-12) {
            return false;
        }
        if (pivot != col) {
            for (int c = 0; c < 8; ++c) {
                std::swap(a[col * 8 + c], a[pivot * 8 + c]);
            }
        }
        const double scale = 1.0 / a[col * 8 + col];
        for (int c = 0; c < 8; ++c) {
            a[col * 8 + c] *= scale;
        }
        for (int r = 0; r < 4; ++r) {
            const double f = a[r * 8 + col];
            if (r == col || f == 0.0) {
                continue;
            }
            for (int c = 0; c < 8; ++c) {
                a[r * 8 + c] -= f * a[col * 8 + c];
            }
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = a[r * 8 + 4 + c];
        }
    }
    return true;
}

/// a * b for 3x4 affine matrices (rows [R | t]).
std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = c == 3 ? a[r * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

} // namespace

CameraMotion::CameraMotion(const Instance& instance_, Scheduler& scheduler_,
                           VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_}, runtime{runtime_} {
    const char* env = std::getenv("GOW3_DEBUG_MOTION");
    debug_overlay = env && env[0] == '1';
    const char* upscaler = std::getenv("GOW3_UPSCALER");
    // The upscaler can be switched on from the menu at any time: the camera is always tracked
    // unless GOW3_UPSCALER=none.
    for_upscaler = !(upscaler && std::strcmp(upscaler, "none") == 0);
    const auto device = instance.GetDevice();
    if (for_upscaler) {
        const std::array<vk::DescriptorSetLayoutBinding, 3> motion_bindings = {{
            {.binding = 0,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 2,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        motion_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(motion_bindings.size()),
            .pBindings = motion_bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(PushConstants)};
        motion_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*motion_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto motion_module = CompileSPV(CAMERA_MOTION_COMP, device);
        motion_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = motion_module,
                              .pName = "main"},
                    .layout = *motion_pipeline_layout,
                }));
        device.destroyShaderModule(motion_module);
    }
    if (!debug_overlay) {
        return;
    }
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 3,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push_range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    }));
    const auto module = CompileSPV(CAMERA_MOTION_DEBUG_COMP, device);
    overlay_pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(module);
    for (auto& frame : frames) {
        frame = std::make_unique<VideoCore::Buffer>(instance, 0, 3840ull * 2160 * 4,
                                                    VideoCore::MemoryType::DeviceLocal);
    }
    std::printf("GPU: camera motion debug overlay on\n");
}

CameraMotion::~CameraMotion() = default;

float CameraMotion::VerticalFov() const noexcept {
    // (y scale is negative for a camera whose clip y points down the image.)
    return 2.0f * std::atan(1.0f / std::abs(current.proj[1]));
}

float CameraMotion::Near() const noexcept {
    // depth = zs + zo / z is 0 at the near plane.
    return -current.proj[3] / current.proj[2];
}

float CameraMotion::Far() const noexcept {
    // depth = zs + zo / z reaches 1 at z = zo / (1 - zs) when zs > 1 (zo < 0).
    const float zs = current.proj[2], zo = current.proj[3];
    const float near_plane = Near();
    if (zs <= 1.0f + 1e-7f || zo >= 0.0f) {
        return std::max(near_plane * 2.0f, 100000.0f);
    }
    return std::clamp(zo / (1.0f - zs), near_plane * 2.0f, 1000000.0f);
}

std::array<std::array<float, 4>, 3> CameraMotion::TaaDepthParameters() const noexcept {
    const auto transform = Multiply(previous.view, current.inv_view);
    return {current.proj, previous.proj,
            {transform[8], transform[9], transform[10], transform[11]}};
}

vk::ImageView CameraMotion::ObjectMotionView() const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->View() : vk::ImageView{};
}

vk::Image CameraMotion::ObjectMotionImage(u32 width, u32 height) const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->Image(width, height)
                                                      : vk::Image{};
}

void CameraMotion::RecordMotion(vk::CommandBuffer cmdbuf, vk::ImageView depth_view,
                                vk::ImageView motion_view, u32 width, u32 height) {
    bool object_valid = false;
    const vk::ImageView object_view = object_motion && object_motion->Enabled()
        ? object_motion->PrepareRead(cmdbuf, width, height, object_valid) : depth_view;
    const PushConstants push{
        .reproject = Multiply(previous.view, current.inv_view),
        .proj = current.proj,
        .prev_proj = previous.proj,
        .size = {float(width), float(height)},
        .jitter = jitter,
        .previous_jitter = previous_jitter,
        .mode = object_valid ? 1u : 0u,
    };
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo motion_info{.imageView = motion_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo object_info{.imageView = object_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 3> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &motion_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &object_info},
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *motion_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *motion_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*motion_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), &push);
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

void CameraMotion::OnConstants(const float* data) {
    // Scene constants: far plane 3000, 1/far, and the render size.
    if (data[0] != 3000.0f || data[4] < 64.0f || data[5] < 64.0f ||
        std::abs(data[1] * data[0] - 1.0f) > 1e-3f) {
        return;
    }
    if (frame_has_camera) {
        return; // the first one of a frame is the main camera
    }
    previous = current;
    std::memcpy(current.view.data(), data + 8, 12 * sizeof(float));
    std::memcpy(current.inv_view.data(), data + 180, 12 * sizeof(float));
    current.proj = {data[52], data[57], data[62], data[63]};
    current.valid = current.proj[0] != 0.0f && current.proj[1] != 0.0f;
    const std::array<u32, 2> size{u32(data[4]), u32(data[5])};
    if (size != render_size) {
        std::printf("Camera motion: scene render size %ux%u\n", size[0], size[1]);
        render_size = size;
    }
    frame_has_camera = true;
}

// The last camera for the runtime's memory diagnostics (GOW3_MEM_CMD scan): position, then the
// right, up and forward axes, in world space.
extern "C" {
volatile float gow3_camera_world[12];
volatile unsigned gow3_camera_seq;
}

bool CameraMotion::OnGravityRushCamera(const float* source, float yscale, float zscale,
                                       float zoffset) {
    // Called for every scene draw: the first one of a frame is the main camera. The constants
    // live in write-combined memory, slow to read: nothing is read once the camera is known
    // (reading them for each of ~750 draws a frame took most of the recording thread's time).
    if (frame_has_camera) {
        return true;
    }
    std::array<float, 32> data;
    std::memcpy(data.data(), source, sizeof(data));
    const float* v = data.data();       // view = (world, 1) * V
    const float* vp = data.data() + 16; // clip = (world, 1) * VP
    // An affine view, and a clip w of minus view z (a perspective camera looking down -z).
    if (v[3] != 0.0f || v[7] != 0.0f || v[11] != 0.0f || v[15] != 1.0f) {
        return false;
    }
    for (int r = 0; r < 4; ++r) {
        const float z = v[r * 4 + 2];
        if (std::abs(vp[r * 4 + 3] + z) > 1e-3f * (1.0f + std::abs(z))) {
            return false;
        }
    }
    std::array<double, 16> inv{};
    if (!Invert4(v, inv)) {
        return false;
    }
    // The projection alone: P = inverse(V) * VP.
    std::array<double, 16> p{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += inv[r * 4 + k] * vp[k * 4 + c];
            }
            p[r * 4 + c] = sum;
        }
    }
    // The shader's view space looks down +z (z' = -z, w = z'): flip z.
    const std::array<float, 3> flip{1.0f, 1.0f, -1.0f};
    const double w_scale = -p[2 * 4 + 3];
    if (!(w_scale > 0.0)) {
        return false;
    }
    previous = current;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            current.view[r * 4 + c] = flip[r] * v[c * 4 + r];
            current.inv_view[r * 4 + c] = float(inv[c * 4 + r]) * flip[c];
        }
        current.view[r * 4 + 3] = flip[r] * v[3 * 4 + r];
        current.inv_view[r * 4 + 3] = float(inv[3 * 4 + r]);
    }
    // NDC z = -p22 / w + p32 / w / z'; the viewport maps it to depth = zoffset + zscale * z.
    // The shader takes y up the image: a viewport not flipping y (yscale > 0) flips the camera.
    const double ndc_zs = -p[2 * 4 + 2] / w_scale, ndc_zo = p[3 * 4 + 2] / w_scale;
    current.proj = {float(p[0] / w_scale), float((yscale < 0.0f ? 1.0 : -1.0) * p[5] / w_scale),
                    float(zoffset + zscale * ndc_zs), float(zscale * ndc_zo)};
    current.valid = current.proj[0] != 0.0f && current.proj[1] != 0.0f && current.proj[3] < 0.0f;
    frame_has_camera = true;
    if (previous.valid && current.valid) {
        // A camera cut (a new shot in a cutscene, a teleport): the last frame does not apply.
        // Gravity shifts roll the camera fast, but well under 45 degrees a frame.
        float moved = 0.0f, facing = 0.0f;
        for (int r = 0; r < 3; ++r) {
            const float d = current.inv_view[r * 4 + 3] - previous.inv_view[r * 4 + 3];
            moved += d * d;
            facing += current.inv_view[r * 4 + 2] * previous.inv_view[r * 4 + 2];
        }
        if (moved > 20.0f * 20.0f || facing < 0.7f) {
            previous.valid = false;
        }
    }
    for (int r = 0; r < 3; ++r) {
        gow3_camera_world[r] = current.inv_view[r * 4 + 3];
        for (int c = 0; c < 3; ++c) {
            gow3_camera_world[3 + c * 3 + r] = current.inv_view[r * 4 + c];
        }
    }
    gow3_camera_seq = gow3_camera_seq + 1;
    // GOW3_CAMERA_LOG=1: the camera of every frame (position, forward, up) for timing tests.
    static const bool camera_log = [] {
        const char* e = std::getenv("GOW3_CAMERA_LOG");
        return e && *e && *e != '0';
    }();
    if (camera_log) {
        static u64 camera_frame = 0;
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
        std::printf("Camera log %llu %.2f pos %.4f %.4f %.4f fwd %.5f %.5f %.5f up %.5f %.5f %.5f\n",
                    (unsigned long long)camera_frame++, ms, current.inv_view[3],
                    current.inv_view[7], current.inv_view[11], current.inv_view[2],
                    current.inv_view[6], current.inv_view[10], current.inv_view[1],
                    current.inv_view[5], current.inv_view[9]);
    }
    static u32 log_counter = 0;
    if (log_counter++ % 1800 == 0) {
        // Self-check: a point 3 right, 2 up and 10 ahead of the camera, through the game's
        // view-projection and through ours (y up the image, depth after the viewport).
        const std::array<float, 3> local{3.0f, 2.0f, 10.0f};
        std::array<float, 3> world{};
        for (int r = 0; r < 3; ++r) {
            world[r] = current.inv_view[r * 4 + 3];
            for (int c = 0; c < 3; ++c) {
                world[r] += current.inv_view[r * 4 + c] * local[c];
            }
        }
        std::array<float, 4> clip{};
        for (int c = 0; c < 4; ++c) {
            clip[c] = vp[12 + c];
            for (int r = 0; r < 3; ++r) {
                clip[c] += world[r] * vp[r * 4 + c];
            }
        }
        const float game_y = (yscale < 0.0f ? 1.0f : -1.0f) * clip[1] / clip[3];
        std::printf("Camera motion: Gravity Rush camera proj %g %g %g %g (near %g, far %g, "
                    "off-centre %g %g, viewport y %g z %g+%g) at %g %g %g; check x %g/%g "
                    "y %g/%g depth %g/%g\n",
                    current.proj[0], current.proj[1], current.proj[2], current.proj[3], Near(),
                    Far(), p[2 * 4 + 0], p[2 * 4 + 1], yscale, zoffset, zscale,
                    current.inv_view[3], current.inv_view[7], current.inv_view[11],
                    clip[0] / clip[3], local[0] * current.proj[0] / local[2], game_y,
                    local[1] * current.proj[1] / local[2],
                    zoffset + zscale * clip[2] / clip[3], current.proj[2] + current.proj[3] / local[2]);
    }
    return true;
}

void CameraMotion::OnGBufferPass(VideoCore::ImageId depth) {
    depth_id = depth;
}

void CameraMotion::OnDisplayPass(VideoCore::ImageId frame) {
    if (debug_overlay && frame && depth_id && current.valid && previous.valid) {
        Overlay(frame);
    }
    if (!frame_has_camera) InvalidateHistory();
    frame_has_camera = false;
    depth_id = {};
}

void CameraMotion::Overlay(VideoCore::ImageId frame) {
    auto& depth = texture_cache.GetImage(depth_id);
    auto& color = texture_cache.GetImage(frame);
    const auto depth_format = depth.info.pixel_format;
    if ((depth_format != vk::Format::eD32Sfloat && depth_format != vk::Format::eD32SfloatS8Uint) ||
        color.info.pixel_format != vk::Format::eR8G8B8A8Unorm ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
        color.info.size.width != depth.info.size.width ||
        color.info.size.height != depth.info.size.height) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("Camera motion: overlay skipped (depth %s, frame %s %ux%u)\n",
                        vk::to_string(depth_format).c_str(),
                        vk::to_string(color.info.pixel_format).c_str(), color.info.size.width,
                        color.info.size.height);
        }
        return;
    }
    const auto device = instance.GetDevice();
    const auto depth_view = Check(device.createImageView({
        .image = vk::Image(depth.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = depth_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));
    const auto color_view = Check(device.createImageView({
        .image = vk::Image(color.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    runtime.FlushBarriers();

    const PushConstants push{
        .reproject = Multiply(previous.view, current.inv_view),
        .proj = current.proj,
        .prev_proj = previous.proj,
        .size = {float(color.info.size.width), float(color.info.size.height)},
        .jitter = jitter,
        .previous_jitter = previous_jitter,
        .mode = Gow3Toggle::Disabled(1u << 20)   ? 1u
                : Gow3Toggle::Disabled(1u << 21) ? 2u
                : Gow3Toggle::Disabled(1u << 22) ? 3u
                : Gow3Toggle::Disabled(1u << 23) ? 4u
                                               : 0u,
    };
    static u32 log_counter = 0;
    if (++log_counter % 200 == 0) {
        const auto& m = push.reproject;
        std::printf("Camera motion: proj %g %g %g %g prev %g %g %g %g\n"
                    "  view  %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f\n"
                    "  reproj %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f\n"
                    "  depth %s %ux%u, frame %ux%u\n",
                    push.proj[0], push.proj[1], push.proj[2], push.proj[3], push.prev_proj[0],
                    push.prev_proj[1], push.prev_proj[2], push.prev_proj[3], current.view[0],
                    current.view[1], current.view[2], current.view[3], current.view[4],
                    current.view[5], current.view[6], current.view[7], current.view[8],
                    current.view[9], current.view[10], current.view[11], m[0], m[1], m[2], m[3],
                    m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
                    vk::to_string(depth_format).c_str(), depth.info.size.width,
                    depth.info.size.height, color.info.size.width, color.info.size.height);
    }
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo color_info{.imageView = color_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const auto& prev_frame = *frames[frame_index ^ 1];
    const auto& next_frame = *frames[frame_index];
    frame_index ^= 1;
    const vk::DescriptorBufferInfo prev_info{prev_frame.Handle(), 0, prev_frame.SizeBytes()};
    const vk::DescriptorBufferInfo next_info{next_frame.Handle(), 0, next_frame.SizeBytes()};
    const std::array<vk::WriteDescriptorSet, 4> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &color_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &prev_info},
        {.dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &next_info},
    }};
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *overlay_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                         &push);
    // The previous frame buffer was written by the last dispatch.
    const vk::MemoryBarrier2 frame_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &frame_barrier});
    cmdbuf.dispatch((color.info.size.width + 7) / 8, (color.info.size.height + 7) / 8, 1);

    scheduler.DeferOperation([device, depth_view, color_view] {
        device.destroyImageView(depth_view);
        device.destroyImageView(color_view);
    });
}

} // namespace Vulkan
