// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// See frameinterpolator.h for the overview. Pipeline per new frame B (previous A):
//   1. luma:   copy B's Y plane into an R8 image the optical flow engine can read
//   2. OF:     VK_NV_optical_flow on its own queue, A<->B, 4x4 grid, both directions
//   3. motion: per 4x4 block pick the best A->B motion among flow candidates (SAD test)
//   4. mid:    per pixel pick among neighbouring block motions, sample A at x-m/2 and
//              B at x+m/2, average -> Y and UV planes of the mid frame
// Sync between libplacebo and the raw Vulkan optical flow submit uses
// pl_vulkan_hold_ex / pl_vulkan_release_ex with timeline semaphores.

#include "frameinterpolator.h"

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>
#include <libplacebo/shaders/custom.h>

#include <QLoggingCategory>
#include <QtGlobal>
#include <QVarLengthArray>

#include <cstring>

Q_DECLARE_LOGGING_CATEGORY(chiakiGui);

// libplacebo's `pl_xxx_params(...)` helper macros take the address of a C
// compound literal, which C++ rejects; this is the C++ equivalent (the
// temporary lives until the end of the full expression).
template <typename T> static T *pl_tmp_ptr(T &&v) { return &v; }
#define PL_TMP(type, ...) pl_tmp_ptr(type{__VA_ARGS__})


#define FG_LOG qCInfo(chiakiGui).nospace() << "[framegen] "
#define FG_WARN qCWarning(chiakiGui).nospace() << "[framegen] "

struct FrameInterpolator::Funcs {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkWaitSemaphores WaitSemaphores;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
    PFN_vkCreateOpticalFlowSessionNV CreateOpticalFlowSessionNV;
    PFN_vkDestroyOpticalFlowSessionNV DestroyOpticalFlowSessionNV;
    PFN_vkBindOpticalFlowSessionImageNV BindOpticalFlowSessionImageNV;
    PFN_vkCmdOpticalFlowExecuteNV CmdOpticalFlowExecuteNV;
};

namespace {

constexpr int kGrid = 4; // optical flow output grid (pixels per vector)

VkPhysicalDeviceOpticalFlowFeaturesNV g_of_features = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV, nullptr, VK_TRUE,
};
VkPhysicalDeviceFeatures2 g_features = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &g_of_features, {},
};

// ---- shaders (GLSL bodies for pl_shader_custom) ----

const char *kLumaBody = R"(
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (all(lessThan(p, imageSize(dst_luma))))
        imageStore(dst_luma, p, vec4(texelFetch(src_y, p, 0).r));
)";

// Per 4x4 block: choose the A->B motion with the lowest bilateral SAD.
// Optical flow conventions (input = B, reference = A):
//   flow_f at B pixel p: B(p) ~ A(p + v)  -> motion A->B = -v
//   flow_b at A pixel q: A(q) ~ B(q + v)  -> motion A->B = +v
const char *kMotionHeader = R"(
float block_sad(vec2 corner, vec2 m, vec2 inv)
{
    float s = 0.0;
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            vec2 q = corner + vec2(float(i) + 0.5, float(j) + 0.5);
            float a = textureLod(luma_a, (q - 0.5 * m) * inv, 0.0).r;
            float b = textureLod(luma_b, (q + 0.5 * m) * inv, 0.0).r;
            s += abs(a - b);
        }
    }
    return s * (1.0 / 16.0);
}
)";

const char *kMotionBody = R"(
    ivec2 g = ivec2(gl_GlobalInvocationID.xy);
    ivec2 gs = imageSize(dst_motion);
    if (any(greaterThanEqual(g, gs)))
        return;
    vec2 inv = 1.0 / vec2(textureSize(luma_a, 0));
    vec2 corner = vec2(g) * 4.0;
    vec2 best = vec2(0.0);
    float best_cost = block_sad(corner, best, inv);
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            ivec2 gg = clamp(g + ivec2(dx, dy), ivec2(0), gs - 1);
            float penalty = (dx == 0 && dy == 0) ? 0.0 : 0.002;
            vec2 m1 = -vec2(texelFetch(flow_f, gg, 0).xy) * (1.0 / 32.0);
            vec2 m2 =  vec2(texelFetch(flow_b, gg, 0).xy) * (1.0 / 32.0);
            float c1 = block_sad(corner, m1, inv) + penalty + 0.0002 * length(m1);
            float c2 = block_sad(corner, m2, inv) + penalty + 0.0002 * length(m2);
            if (c1 < best_cost) { best_cost = c1; best = m1; }
            if (c2 < best_cost) { best_cost = c2; best = m2; }
        }
    }
    // Forward/backward consistency: both flow fields, sampled where the
    // block's content sits in A and in B, should agree with the chosen
    // motion. Disagreement = occlusion or motion beyond the flow's range.
    vec2 cb = corner + 2.0;
    ivec2 gb_b = clamp(ivec2((cb + 0.5 * best) / 4.0), ivec2(0), gs - 1);
    ivec2 gb_a = clamp(ivec2((cb - 0.5 * best) / 4.0), ivec2(0), gs - 1);
    vec2 mf = -vec2(texelFetch(flow_f, gb_b, 0).xy) * (1.0 / 32.0);
    vec2 mb = vec2(texelFetch(flow_b, gb_a, 0).xy) * (1.0 / 32.0);
    float inc = max(length(mf - best), length(mb - best));
    // > 1.0 means inconsistent (tolerance grows with motion length)
    float bad = inc / (3.0 + 0.2 * length(best));
    imageStore(dst_motion, g, vec4(best, best_cost, bad));
)";

// One work group: fraction of inconsistent blocks -> frame_stats.x
const char *kStatsHeader = R"(
shared float partial[256];
)";
const char *kStatsBody = R"(
    uint t = gl_LocalInvocationIndex;
    ivec2 gs = textureSize(motion_raw, 0);
    int total = gs.x * gs.y;
    float cnt = 0.0;
    for (int i = int(t); i < total; i += 256) {
        vec4 v = texelFetch(motion_raw, ivec2(i % gs.x, i / gs.x), 0);
        cnt += v.w > 1.0 ? 1.0 : 0.0;
    }
    partial[t] = cnt;
    barrier();
    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (t < s)
            partial[t] += partial[t + s];
        barrier();
    }
    if (t == 0u)
        imageStore(dst_stats, ivec2(0), vec4(partial[0] / float(total), 0.0, 0.0, 1.0));
)";

// 3x3 vector median of the block motion: removes isolated wrong vectors
// (typical in flat areas like sky) that show up as torn, blocky edges.
const char *kSmoothBody = R"(
    ivec2 g = ivec2(gl_GlobalInvocationID.xy);
    ivec2 gs = imageSize(dst_motion);
    if (any(greaterThanEqual(g, gs)))
        return;
    vec4 v[9];
    int k = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            v[k++] = texelFetch(src_motion, clamp(g + ivec2(dx, dy), ivec2(0), gs - 1), 0);
    int best = 4;
    float best_d = 1e30;
    for (int i = 0; i < 9; i++) {
        float d = 0.0;
        for (int j = 0; j < 9; j++)
            d += length(v[i].xy - v[j].xy);
        if (d < best_d) { best_d = d; best = i; }
    }
    // keep the block's own cost so the per-pixel pass can still judge it
    imageStore(dst_motion, g, vec4(v[best].xy, v[4].z, max(v[4].w, v[best].w)));
)";

// Shared by the Y and UV passes: pick the motion for luma position x among
// the 4 surrounding block motions and their bilinear blend.
const char *kPickHeader = R"(
vec4 fetch_motion(ivec2 g)
{
    ivec2 gs = textureSize(motion, 0);
    return texelFetch(motion, clamp(g, ivec2(0), gs - 1), 0);
}

float pixel_err(vec2 x, vec2 m, vec2 inv)
{
    float e = 0.0;
    e += abs(textureLod(src_a_y, (x - 0.5 * m) * inv, 0.0).r -
             textureLod(src_b_y, (x + 0.5 * m) * inv, 0.0).r);
    e += abs(textureLod(src_a_y, (x + vec2(1.0, 0.0) - 0.5 * m) * inv, 0.0).r -
             textureLod(src_b_y, (x + vec2(1.0, 0.0) + 0.5 * m) * inv, 0.0).r);
    e += abs(textureLod(src_a_y, (x + vec2(0.0, 1.0) - 0.5 * m) * inv, 0.0).r -
             textureLod(src_b_y, (x + vec2(0.0, 1.0) + 0.5 * m) * inv, 0.0).r);
    return e * (1.0 / 3.0);
}

// Where A and B disagree after motion compensation (wrong / unknown motion,
// occlusion), a 50/50 blend shows two half-transparent copies. Fade towards
// B alone there: one sharp copy instead of a ghost.
float ghost_guard(float err)
{
    return smoothstep(0.03, 0.10, err);
}

// Returns xy = chosen A->B motion, z = its pixel matching error,
// w = flow inconsistency around x (> 1 = unreliable).
vec4 pick_motion(vec2 x, vec2 inv)
{
    vec2 gp = x / 4.0 - 0.5;
    ivec2 g0 = ivec2(floor(gp));
    vec2 fr = gp - vec2(g0);
    vec4 m00 = fetch_motion(g0);
    vec4 m10 = fetch_motion(g0 + ivec2(1, 0));
    vec4 m01 = fetch_motion(g0 + ivec2(0, 1));
    vec4 m11 = fetch_motion(g0 + ivec2(1, 1));
    vec4 mb = mix(mix(m00, m10, fr.x), mix(m01, m11, fr.x), fr.y);

    vec2 best = mb.xy;
    float best_pe = pixel_err(x, mb.xy, inv);
    float best_e = best_pe + 0.5 * mb.z;
    float pe, e;
    // The smooth (bilinear) motion wins unless a single block's motion is
    // clearly better; hard switches between blocks cause staircase edges.
    best_e -= 0.015;
    pe = pixel_err(x, m00.xy, inv); e = pe + 0.5 * m00.z; if (e < best_e) { best_e = e; best_pe = pe; best = m00.xy; }
    pe = pixel_err(x, m10.xy, inv); e = pe + 0.5 * m10.z; if (e < best_e) { best_e = e; best_pe = pe; best = m10.xy; }
    pe = pixel_err(x, m01.xy, inv); e = pe + 0.5 * m01.z; if (e < best_e) { best_e = e; best_pe = pe; best = m01.xy; }
    pe = pixel_err(x, m11.xy, inv); e = pe + 0.5 * m11.z; if (e < best_e) { best_e = e; best_pe = pe; best = m11.xy; }
    // Static content (HUD, minimap) must not be dragged along with the scene.
    pe = pixel_err(x, vec2(0.0), inv);
    if (pe < 0.004 && best_pe > 0.04)
        return vec4(0.0, 0.0, pe, 0.0);
    return vec4(best, best_pe, mb.w);
}

// How much of the unwarped B frame to show instead of the interpolation.
float fallback_weight(vec4 me)
{
    // Disabled: whole-frame fallback fired exactly during fast camera turns and
    // turned them into 30 fps judder (user test 2026-11-20). Kept for tuning.
    return 0.0 * texelFetch(frame_stats, ivec2(0), 0).x;
}

)";

const char *kMidYBody = R"(
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(p, imageSize(dst_y))))
        return;
    vec2 inv = 1.0 / vec2(textureSize(src_a_y, 0));
    vec2 x = vec2(p) + 0.5;
    vec4 me = pick_motion(x, inv);
    vec2 m = me.xy;
    float a = textureLod(src_a_y, (x - 0.5 * m) * inv, 0.0).r;
    float b = textureLod(src_b_y, (x + 0.5 * m) * inv, 0.0).r;
    float b0 = textureLod(src_b_y, x * inv, 0.0).r;
    float v = mix(0.5 * (a + b), b, ghost_guard(me.z));
    imageStore(dst_y, p, vec4(mix(v, b0, fallback_weight(me))));
)";

const char *kMidUVBody = R"(
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(p, imageSize(dst_uv))))
        return;
    vec2 inv = 1.0 / vec2(textureSize(src_a_y, 0));
    // luma-space position of this chroma sample's centre; normalized
    // coordinates are shared between the Y and UV planes
    vec2 x = (vec2(p) + 0.5) * (vec2(textureSize(src_a_y, 0)) / vec2(imageSize(dst_uv)));
    vec4 me = pick_motion(x, inv);
    vec2 m = me.xy;
    vec2 a = textureLod(src_a_uv, (x - 0.5 * m) * inv, 0.0).rg;
    vec2 b = textureLod(src_b_uv, (x + 0.5 * m) * inv, 0.0).rg;
    vec2 b0 = textureLod(src_b_uv, x * inv, 0.0).rg;
    vec2 v = mix(0.5 * (a + b), b, ghost_guard(me.z));
    imageStore(dst_uv, p, vec4(mix(v, b0, fallback_weight(me)), 0.0, 1.0));
)";

struct pl_shader_desc sampled(const char *name, pl_tex tex, bool linear)
{
    struct pl_shader_desc d = {};
    d.desc.name = name;
    d.desc.type = PL_DESC_SAMPLED_TEX;
    d.binding.object = tex;
    d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
    d.binding.sample_mode = linear ? PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;
    return d;
}

struct pl_shader_desc storage(const char *name, pl_tex tex)
{
    struct pl_shader_desc d = {};
    d.desc.name = name;
    d.desc.type = PL_DESC_STORAGE_IMG;
    d.desc.access = PL_DESC_ACCESS_WRITEONLY;
    d.binding.object = tex;
    return d;
}

} // namespace

bool FrameInterpolator::requested()
{
    return qEnvironmentVariableIntValue("CHIAKI_FRAMEGEN") == 1;
}

const char *FrameInterpolator::requiredExtension() { return VK_NV_OPTICAL_FLOW_EXTENSION_NAME; }
VkQueueFlags FrameInterpolator::requiredQueueFlags() { return VK_QUEUE_OPTICAL_FLOW_BIT_NV; }
const VkPhysicalDeviceFeatures2 *FrameInterpolator::requiredFeatures() { return &g_features; }

FrameInterpolator *FrameInterpolator::create(pl_log log, pl_vulkan vk)
{
    auto *fi = new FrameInterpolator();
    if (!fi->init(log, vk)) {
        delete fi;
        return nullptr;
    }
    return fi;
}

bool FrameInterpolator::init(pl_log log_, pl_vulkan vk_)
{
    log = log_;
    vk = vk_;
    gpu = vk->gpu;

    bool has_ext = false;
    for (int i = 0; i < vk->num_extensions; i++)
        has_ext |= std::strcmp(vk->extensions[i], VK_NV_OPTICAL_FLOW_EXTENSION_NAME) == 0;
    if (!has_ext) {
        FG_WARN << "device has no " VK_NV_OPTICAL_FLOW_EXTENSION_NAME ", frame generation disabled";
        return false;
    }

    fn = new Funcs();
#define LOAD_I(name) fn->name = reinterpret_cast<PFN_vk##name>(vk->get_proc_addr(vk->instance, "vk" #name))
#define LOAD_D(name) fn->name = reinterpret_cast<PFN_vk##name>(fn->GetDeviceProcAddr(vk->device, "vk" #name))
    LOAD_I(GetDeviceProcAddr);
    LOAD_I(GetPhysicalDeviceMemoryProperties);
    LOAD_I(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_I(GetPhysicalDeviceFeatures2);
    if (!fn->GetDeviceProcAddr)
        return false;
    LOAD_D(CreateImage); LOAD_D(DestroyImage); LOAD_D(GetImageMemoryRequirements);
    LOAD_D(AllocateMemory); LOAD_D(FreeMemory); LOAD_D(BindImageMemory);
    LOAD_D(CreateImageView); LOAD_D(DestroyImageView);
    LOAD_D(CreateCommandPool); LOAD_D(DestroyCommandPool); LOAD_D(AllocateCommandBuffers);
    LOAD_D(BeginCommandBuffer); LOAD_D(EndCommandBuffer); LOAD_D(ResetCommandBuffer);
    LOAD_D(GetDeviceQueue); LOAD_D(QueueSubmit); LOAD_D(WaitSemaphores); LOAD_D(DeviceWaitIdle);
    LOAD_D(CreateOpticalFlowSessionNV); LOAD_D(DestroyOpticalFlowSessionNV);
    LOAD_D(BindOpticalFlowSessionImageNV); LOAD_D(CmdOpticalFlowExecuteNV);
#undef LOAD_I
#undef LOAD_D
    if (!fn->CreateOpticalFlowSessionNV || !fn->CmdOpticalFlowExecuteNV || !fn->WaitSemaphores) {
        FG_WARN << "optical flow entry points missing";
        return false;
    }

    // Was the feature actually enabled on the device?
    bool feature_on = false;
    for (auto *s = reinterpret_cast<const VkBaseInStructure *>(vk->features); s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV)
            feature_on = reinterpret_cast<const VkPhysicalDeviceOpticalFlowFeaturesNV *>(s)->opticalFlow;
    }
    if (!feature_on) {
        FG_WARN << "opticalFlow feature not enabled on device";
        return false;
    }

    uint32_t qn = 0;
    fn->GetPhysicalDeviceQueueFamilyProperties(vk->phys_device, &qn, nullptr);
    QVarLengthArray<VkQueueFamilyProperties, 16> props(qn);
    fn->GetPhysicalDeviceQueueFamilyProperties(vk->phys_device, &qn, props.data());
    for (uint32_t i = 0; i < qn; i++) {
        if (props[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) {
            of_qf = i;
            break;
        }
    }
    if (of_qf == UINT32_MAX) {
        FG_WARN << "no optical flow queue family";
        return false;
    }
    fn->GetDeviceQueue(vk->device, of_qf, 0, &of_queue);

    // Queue families our images are shared between (concurrent sharing).
    auto add_qf = [this](uint32_t qf) {
        for (uint32_t i = 0; i < num_qfs; i++)
            if (qfs[i] == qf)
                return;
        qfs[num_qfs++] = qf;
    };
    add_qf(vk->queue_graphics.index);
    add_qf(vk->queue_compute.index);
    add_qf(vk->queue_transfer.index);
    add_qf(of_qf);

    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = of_qf;
    if (fn->CreateCommandPool(vk->device, &pool_info, nullptr, &cmd_pool) != VK_SUCCESS)
        return false;
    VkCommandBufferAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = cmd_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = kCmdRing;
    if (fn->AllocateCommandBuffers(vk->device, &alloc_info, cmd) != VK_SUCCESS)
        return false;

    of_sem = pl_vulkan_sem_create(gpu, PL_TMP(pl_vulkan_sem_params, .type = VK_SEMAPHORE_TYPE_TIMELINE));
    if (!of_sem)
        return false;

    dp = pl_dispatch_create(log, gpu);
    if (!dp)
        return false;

    FG_LOG << "initialized, optical flow queue family " << of_qf;
    return true;
}

FrameInterpolator::~FrameInterpolator()
{
    if (gpu)
        pl_gpu_finish(gpu);
    if (fn && fn->DeviceWaitIdle && vk)
        fn->DeviceWaitIdle(vk->device);
    destroyResources();
    if (dp)
        pl_dispatch_destroy(&dp);
    if (of_sem)
        pl_vulkan_sem_destroy(gpu, &of_sem);
    if (cmd_pool)
        fn->DestroyCommandPool(vk->device, cmd_pool, nullptr);
    delete fn;
}

bool FrameInterpolator::createImage(Image &img, int w, int h, VkFormat of_format, VkFormat pl_format,
                                    uint32_t of_usage, const char *name)
{
    VkOpticalFlowImageFormatInfoNV of_info = {VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV};
    of_info.usage = of_usage;
    const VkFormat view_formats[2] = {of_format, pl_format};
    VkImageFormatListCreateInfo fmt_list = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    fmt_list.viewFormatCount = 2;
    fmt_list.pViewFormats = view_formats;
    const bool mutable_fmt = of_format != pl_format;
    if (mutable_fmt)
        of_info.pNext = &fmt_list;

    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.pNext = &of_info;
    info.flags = mutable_fmt ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = of_format;
    info.extent = {uint32_t(w), uint32_t(h), 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    info.sharingMode = num_qfs > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    info.queueFamilyIndexCount = num_qfs;
    info.pQueueFamilyIndices = qfs;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (fn->CreateImage(vk->device, &info, nullptr, &img.image) != VK_SUCCESS) {
        FG_WARN << "vkCreateImage failed for " << name;
        return false;
    }

    VkMemoryRequirements req;
    fn->GetImageMemoryRequirements(vk->device, img.image, &req);
    VkPhysicalDeviceMemoryProperties mem;
    fn->GetPhysicalDeviceMemoryProperties(vk->phys_device, &mem);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (fn->AllocateMemory(vk->device, &ai, nullptr, &img.memory) != VK_SUCCESS)
        return false;
    if (fn->BindImageMemory(vk->device, img.image, img.memory, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = of_format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (fn->CreateImageView(vk->device, &vi, nullptr, &img.of_view) != VK_SUCCESS)
        return false;

    img.tex = pl_vulkan_wrap(gpu, PL_TMP(pl_vulkan_wrap_params,
        .image = img.image,
        .width = w,
        .height = h,
        .format = pl_format,
        .usage = info.usage,
    ));
    if (!img.tex) {
        FG_WARN << "pl_vulkan_wrap failed for " << name;
        return false;
    }
    // Wrapped images start out "held" by us; hand them to libplacebo.
    pl_vulkan_release_ex(gpu, PL_TMP(pl_vulkan_release_params,
        .tex = img.tex,
        .layout = VK_IMAGE_LAYOUT_UNDEFINED,
        .qf = VK_QUEUE_FAMILY_IGNORED,
    ));

    img.hold_sem = pl_vulkan_sem_create(gpu, PL_TMP(pl_vulkan_sem_params, .type = VK_SEMAPHORE_TYPE_TIMELINE));
    img.hold_value = 0;
    return img.hold_sem != VK_NULL_HANDLE;
}

void FrameInterpolator::destroyImage(Image &img)
{
    if (img.tex)
        pl_tex_destroy(gpu, &img.tex);
    if (img.hold_sem)
        pl_vulkan_sem_destroy(gpu, &img.hold_sem);
    if (img.of_view)
        fn->DestroyImageView(vk->device, img.of_view, nullptr);
    if (img.image)
        fn->DestroyImage(vk->device, img.image, nullptr);
    if (img.memory)
        fn->FreeMemory(vk->device, img.memory, nullptr);
    img = Image();
}

void FrameInterpolator::destroyResources()
{
    if (!gpu)
        return;
    pl_gpu_finish(gpu);
    if (fn && fn->DeviceWaitIdle)
        fn->DeviceWaitIdle(vk->device);
    for (auto &s : session) {
        if (s)
            fn->DestroyOpticalFlowSessionNV(vk->device, s, nullptr);
        s = VK_NULL_HANDLE;
    }
    for (auto &l : luma)
        destroyImage(l);
    destroyImage(flow_fwd);
    destroyImage(flow_bwd);
    destroyImage(cost_fwd);
    destroyImage(cost_bwd);
    pl_tex_destroy(gpu, &motion);
    pl_tex_destroy(gpu, &motion_smooth);
    pl_tex_destroy(gpu, &frame_stats);
    pl_tex_destroy(gpu, &out_planes[0]);
    pl_tex_destroy(gpu, &out_planes[1]);
    width = height = grid_w = grid_h = 0;
    plane_fmt[0] = plane_fmt[1] = nullptr;
    resources_ok = false;
    has_prev_luma = false;
}

bool FrameInterpolator::ensureResources(const struct pl_frame *frame)
{
    if (frame->num_planes != 2 || !frame->planes[0].texture || !frame->planes[1].texture)
        return false; // only NV12 / P010 style (Y + interleaved UV) is handled

    pl_tex y = frame->planes[0].texture;
    pl_tex uv = frame->planes[1].texture;
    if (resources_ok && y->params.w == width && y->params.h == height &&
        y->params.format == plane_fmt[0] && uv->params.format == plane_fmt[1])
        return true;

    destroyResources();
    width = y->params.w;
    height = y->params.h;
    grid_w = (width + kGrid - 1) / kGrid;
    grid_h = (height + kGrid - 1) / kGrid;
    plane_fmt[0] = y->params.format;
    plane_fmt[1] = uv->params.format;

    bool ok = true;
    ok = ok && createImage(luma[0], width, height, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM,
                           VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV, "luma0");
    ok = ok && createImage(luma[1], width, height, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM,
                           VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV, "luma1");
    ok = ok && createImage(flow_fwd, grid_w, grid_h, VK_FORMAT_R16G16_S10_5_NV, VK_FORMAT_R16G16_SINT,
                           VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV, "flow_fwd");
    ok = ok && createImage(flow_bwd, grid_w, grid_h, VK_FORMAT_R16G16_S10_5_NV, VK_FORMAT_R16G16_SINT,
                           VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV, "flow_bwd");
    ok = ok && createImage(cost_fwd, grid_w, grid_h, VK_FORMAT_R8_UINT, VK_FORMAT_R8_UINT,
                           VK_OPTICAL_FLOW_USAGE_COST_BIT_NV, "cost_fwd");
    ok = ok && createImage(cost_bwd, grid_w, grid_h, VK_FORMAT_R8_UINT, VK_FORMAT_R8_UINT,
                           VK_OPTICAL_FLOW_USAGE_COST_BIT_NV, "cost_bwd");
    if (!ok) {
        FG_WARN << "failed to create optical flow images " << width << "x" << height;
        destroyResources();
        return false;
    }

    const int perf_env = qEnvironmentVariableIntValue("CHIAKI_FRAMEGEN_PERF"); // 1 slow, 2 medium, 3 fast
    VkOpticalFlowPerformanceLevelNV perf = VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_SLOW_NV; // best quality
    if (perf_env >= 1 && perf_env <= 3)
        perf = static_cast<VkOpticalFlowPerformanceLevelNV>(perf_env);

    for (int i = 0; i < 2; i++) {
        VkOpticalFlowSessionCreateInfoNV si = {VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_INFO_NV};
        si.width = uint32_t(width);
        si.height = uint32_t(height);
        si.imageFormat = VK_FORMAT_R8_UNORM;
        si.flowVectorFormat = VK_FORMAT_R16G16_S10_5_NV;
        si.costFormat = VK_FORMAT_R8_UINT;
        si.outputGridSize = VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV;
        si.performanceLevel = perf;
        si.flags = VK_OPTICAL_FLOW_SESSION_CREATE_ENABLE_COST_BIT_NV |
                   VK_OPTICAL_FLOW_SESSION_CREATE_BOTH_DIRECTIONS_BIT_NV;
        VkResult res = fn->CreateOpticalFlowSessionNV(vk->device, &si, nullptr, &session[i]);
        if (res != VK_SUCCESS) {
            FG_WARN << "vkCreateOpticalFlowSessionNV failed: " << int(res);
            destroyResources();
            return false;
        }
        const VkImageLayout L = VK_IMAGE_LAYOUT_GENERAL;
        auto bind = [&](VkOpticalFlowSessionBindingPointNV bp, VkImageView v) {
            return fn->BindOpticalFlowSessionImageNV(vk->device, session[i], bp, v, L) == VK_SUCCESS;
        };
        bool bok = bind(VK_OPTICAL_FLOW_SESSION_BINDING_POINT_INPUT_NV, luma[i].of_view) &&
                   bind(VK_OPTICAL_FLOW_SESSION_BINDING_POINT_REFERENCE_NV, luma[1 - i].of_view) &&
                   bind(VK_OPTICAL_FLOW_SESSION_BINDING_POINT_FLOW_VECTOR_NV, flow_fwd.of_view) &&
                   bind(VK_OPTICAL_FLOW_SESSION_BINDING_POINT_BACKWARD_FLOW_VECTOR_NV, flow_bwd.of_view) &&
                   bind(VK_OPTICAL_FLOW_SESSION_BINDING_POINT_COST_NV, cost_fwd.of_view) &&
                   bind(VK_OPTICAL_FLOW_SESSION_BINDING_POINT_BACKWARD_COST_NV, cost_bwd.of_view);
        if (!bok) {
            FG_WARN << "vkBindOpticalFlowSessionImageNV failed";
            destroyResources();
            return false;
        }
    }

    pl_fmt motion_fmt = pl_find_named_fmt(gpu, "rgba16f");
    motion = motion_fmt ? pl_tex_create(gpu, PL_TMP(pl_tex_params,
        .w = grid_w, .h = grid_h, .format = motion_fmt,
        .sampleable = true, .storable = true)) : nullptr;
    motion_smooth = motion_fmt ? pl_tex_create(gpu, PL_TMP(pl_tex_params,
        .w = grid_w, .h = grid_h, .format = motion_fmt,
        .sampleable = true, .storable = true)) : nullptr;
    frame_stats = motion_fmt ? pl_tex_create(gpu, PL_TMP(pl_tex_params,
        .w = 1, .h = 1, .format = motion_fmt,
        .sampleable = true, .storable = true)) : nullptr;
    for (int i = 0; i < 2; i++) {
        pl_tex src = frame->planes[i].texture;
        out_planes[i] = pl_tex_create(gpu, PL_TMP(pl_tex_params,
            .w = src->params.w, .h = src->params.h, .format = src->params.format,
            .sampleable = true, .storable = true));
    }
    if (!motion || !motion_smooth || !frame_stats || !out_planes[0] || !out_planes[1]) {
        FG_WARN << "failed to create shader textures (format "
                << (plane_fmt[0] ? plane_fmt[0]->name : "?") << "/"
                << (plane_fmt[1] ? plane_fmt[1]->name : "?") << ")";
        destroyResources();
        return false;
    }

    FG_LOG << "resources " << width << "x" << height << " grid " << grid_w << "x" << grid_h
           << " planes " << plane_fmt[0]->name << "+" << plane_fmt[1]->name << " perf " << int(perf);
    resources_ok = true;
    return true;
}

bool FrameInterpolator::runLuma(const struct pl_frame *frame, int slot)
{
    pl_shader sh = pl_dispatch_begin(dp);
    const struct pl_shader_desc descs[] = {
        sampled("src_y", frame->planes[0].texture, false),
        storage("dst_luma", luma[slot].tex),
    };
    struct pl_custom_shader cs = {};
    cs.description = "framegen luma";
    cs.body = kLumaBody;
    cs.input = PL_SHADER_SIG_NONE;
    cs.output = PL_SHADER_SIG_NONE;
    cs.descriptors = descs;
    cs.num_descriptors = 2;
    cs.compute = true;
    cs.compute_group_size[0] = 16;
    cs.compute_group_size[1] = 16;
    if (!pl_shader_custom(sh, &cs)) {
        pl_dispatch_abort(dp, &sh);
        return false;
    }
    return pl_dispatch_compute(dp, PL_TMP(pl_dispatch_compute_params,
        .shader = &sh,
        .dispatch_size = {(width + 15) / 16, (height + 15) / 16, 1},
    ));
}

bool FrameInterpolator::runOpticalFlow(int cur_slot)
{
    Image *imgs[] = {&luma[0], &luma[1], &flow_fwd, &flow_bwd, &cost_fwd, &cost_bwd};
    constexpr int n = 6;

    // Reuse a command buffer only once its previous submission finished.
    const int ci = cmd_index;
    cmd_index = (cmd_index + 1) % kCmdRing;
    if (cmd_value[ci]) {
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &of_sem;
        wi.pValues = &cmd_value[ci];
        if (fn->WaitSemaphores(vk->device, &wi, 100ull * 1000 * 1000) != VK_SUCCESS) {
            FG_WARN << "optical flow command buffer still busy";
            return false;
        }
    }

    VkSemaphore wait_sems[n];
    uint64_t wait_vals[n];
    VkPipelineStageFlags wait_stages[n];
    for (int i = 0; i < n; i++) {
        Image &img = *imgs[i];
        const uint64_t v = img.hold_value + 1;
        if (!pl_vulkan_hold_ex(gpu, PL_TMP(pl_vulkan_hold_params,
                .tex = img.tex,
                .layout = VK_IMAGE_LAYOUT_GENERAL,
                .qf = VK_QUEUE_FAMILY_IGNORED,
                .semaphore = {img.hold_sem, v}))) {
            // Give back whatever we already hold.
            for (int j = 0; j < i; j++)
                pl_vulkan_release_ex(gpu, PL_TMP(pl_vulkan_release_params,
                    .tex = imgs[j]->tex, .layout = VK_IMAGE_LAYOUT_GENERAL,
                    .qf = VK_QUEUE_FAMILY_IGNORED));
            FG_WARN << "pl_vulkan_hold_ex failed";
            return false;
        }
        img.hold_value = v;
        wait_sems[i] = img.hold_sem;
        wait_vals[i] = v;
        wait_stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    VkCommandBuffer cb = cmd[ci];
    fn->ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fn->BeginCommandBuffer(cb, &bi);
    VkOpticalFlowExecuteInfoNV ei = {VK_STRUCTURE_TYPE_OPTICAL_FLOW_EXECUTE_INFO_NV};
    fn->CmdOpticalFlowExecuteNV(cb, session[cur_slot], &ei);
    fn->EndCommandBuffer(cb);

    const uint64_t signal_val = ++of_value;
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = n;
    ts.pWaitSemaphoreValues = wait_vals;
    ts.signalSemaphoreValueCount = 1;
    ts.pSignalSemaphoreValues = &signal_val;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = n;
    si.pWaitSemaphores = wait_sems;
    si.pWaitDstStageMask = wait_stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &of_sem;

    vk->lock_queue(vk, of_qf, 0);
    VkResult res = fn->QueueSubmit(of_queue, 1, &si, VK_NULL_HANDLE);
    vk->unlock_queue(vk, of_qf, 0);
    const bool ok = res == VK_SUCCESS;
    if (ok)
        cmd_value[ci] = signal_val;
    else
        FG_WARN << "optical flow submit failed: " << int(res);

    // Hand everything back; libplacebo waits on the flow result before use.
    for (int i = 0; i < n; i++) {
        pl_vulkan_release_ex(gpu, PL_TMP(pl_vulkan_release_params,
            .tex = imgs[i]->tex,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
            .qf = VK_QUEUE_FAMILY_IGNORED,
            .semaphore = ok ? pl_vulkan_sem{of_sem, signal_val} : pl_vulkan_sem{}));
    }
    return ok;
}

bool FrameInterpolator::runInterpolation(const struct pl_frame *a, const struct pl_frame *b)
{
    const int cur = 1 - prev_slot;
    const pl_tex la = luma[prev_slot].tex, lb = luma[cur].tex;

    // Pass 1: per-block motion
    {
        pl_shader sh = pl_dispatch_begin(dp);
        const struct pl_shader_desc descs[] = {
            sampled("luma_a", la, true),
            sampled("luma_b", lb, true),
            sampled("flow_f", flow_fwd.tex, false),
            sampled("flow_b", flow_bwd.tex, false),
            storage("dst_motion", motion),
        };
        struct pl_custom_shader cs = {};
        cs.description = "framegen motion";
        cs.header = kMotionHeader;
        cs.body = kMotionBody;
        cs.descriptors = descs;
        cs.num_descriptors = 5;
        cs.compute = true;
        cs.compute_group_size[0] = 16;
        cs.compute_group_size[1] = 16;
        if (!pl_shader_custom(sh, &cs)) {
            pl_dispatch_abort(dp, &sh);
            return false;
        }
        if (!pl_dispatch_compute(dp, PL_TMP(pl_dispatch_compute_params,
                .shader = &sh, .dispatch_size = {(grid_w + 15) / 16, (grid_h + 15) / 16, 1})))
            return false;
    }

    // Pass 1b: vector median (motion -> motion_smooth)
    {
        pl_shader sh = pl_dispatch_begin(dp);
        const struct pl_shader_desc descs[] = {
            sampled("src_motion", motion, false),
            storage("dst_motion", motion_smooth),
        };
        struct pl_custom_shader cs = {};
        cs.description = "framegen motion smooth";
        cs.body = kSmoothBody;
        cs.descriptors = descs;
        cs.num_descriptors = 2;
        cs.compute = true;
        cs.compute_group_size[0] = 16;
        cs.compute_group_size[1] = 16;
        if (!pl_shader_custom(sh, &cs)) {
            pl_dispatch_abort(dp, &sh);
            return false;
        }
        if (!pl_dispatch_compute(dp, PL_TMP(pl_dispatch_compute_params,
                .shader = &sh, .dispatch_size = {(grid_w + 15) / 16, (grid_h + 15) / 16, 1})))
            return false;
    }

    // Pass 1c: frame-level reliability (fraction of inconsistent blocks)
    {
        pl_shader sh = pl_dispatch_begin(dp);
        const struct pl_shader_desc descs[] = {
            sampled("motion_raw", motion, false),
            storage("dst_stats", frame_stats),
        };
        struct pl_custom_shader cs = {};
        cs.description = "framegen stats";
        cs.header = kStatsHeader;
        cs.body = kStatsBody;
        cs.descriptors = descs;
        cs.num_descriptors = 2;
        cs.compute = true;
        cs.compute_shmem = 256 * sizeof(float);
        cs.compute_group_size[0] = 16;
        cs.compute_group_size[1] = 16;
        if (!pl_shader_custom(sh, &cs)) {
            pl_dispatch_abort(dp, &sh);
            return false;
        }
        if (!pl_dispatch_compute(dp, PL_TMP(pl_dispatch_compute_params,
                .shader = &sh, .dispatch_size = {1, 1, 1})))
            return false;
    }

    // Pass 2/3: mid frame planes
    for (int plane = 0; plane < 2; plane++) {
        pl_shader sh = pl_dispatch_begin(dp);
        pl_tex dst = out_planes[plane];
        struct pl_shader_desc descs[7];
        int nd = 0;
        descs[nd++] = sampled("frame_stats", frame_stats, false);
        descs[nd++] = sampled("src_a_y", a->planes[0].texture, true);
        descs[nd++] = sampled("src_b_y", b->planes[0].texture, true);
        descs[nd++] = sampled("motion", motion_smooth, false);
        if (plane == 0) {
            descs[nd++] = storage("dst_y", dst);
        } else {
            descs[nd++] = sampled("src_a_uv", a->planes[1].texture, true);
            descs[nd++] = sampled("src_b_uv", b->planes[1].texture, true);
            descs[nd++] = storage("dst_uv", dst);
        }
        struct pl_custom_shader cs = {};
        cs.description = plane == 0 ? "framegen mid Y" : "framegen mid UV";
        cs.header = kPickHeader;
        cs.body = plane == 0 ? kMidYBody : kMidUVBody;
        cs.descriptors = descs;
        cs.num_descriptors = nd;
        cs.compute = true;
        cs.compute_group_size[0] = 16;
        cs.compute_group_size[1] = 16;
        if (!pl_shader_custom(sh, &cs)) {
            pl_dispatch_abort(dp, &sh);
            return false;
        }
        const int w = dst->params.w, h = dst->params.h;
        if (!pl_dispatch_compute(dp, PL_TMP(pl_dispatch_compute_params,
                .shader = &sh, .dispatch_size = {(w + 15) / 16, (h + 15) / 16, 1})))
            return false;
    }
    return true;
}

bool FrameInterpolator::interpolate(const struct pl_frame *a, const struct pl_frame *b)
{
    if (!ensureResources(b)) {
        has_prev_luma = false;
        failed++;
        return false;
    }
    if (a && (a->planes[0].texture->params.w != width || a->planes[0].texture->params.h != height ||
              a->planes[0].texture->params.format != plane_fmt[0]))
        a = nullptr;
    const bool use_a = a && has_prev_luma;
    const int slot = use_a ? 1 - prev_slot : (has_prev_luma ? 1 - prev_slot : 0);

    // The decoded images belong to FFmpeg; acquire/release does the AVVkFrame
    // locking and semaphore hand-off (same as pl_render_image does). The
    // callbacks take a non-const frame but do not modify it.
    auto *bm = const_cast<struct pl_frame *>(b);
    auto *am = const_cast<struct pl_frame *>(a);
    const bool b_acq = !bm->acquire || bm->acquire(gpu, bm);
    const bool a_acq = use_a && (!am->acquire || am->acquire(gpu, am));
    bool ok = b_acq && runLuma(b, slot);
    const bool luma_ok = ok;
    if (ok && use_a)
        ok = a_acq && runOpticalFlow(slot) && runInterpolation(a, b);
    if (use_a && am->release)
        am->release(gpu, am);
    if (bm->release)
        bm->release(gpu, bm);

    has_prev_luma = luma_ok;
    prev_slot = slot;

    if (!use_a)
        return false;
    if (!ok) {
        failed++;
        return false;
    }
    mid = *b;
    mid.planes[0].texture = out_planes[0];
    mid.planes[1].texture = out_planes[1];
    mid.acquire = nullptr;
    mid.release = nullptr;
    mid.user_data = nullptr;
    produced++;
    return true;
}
