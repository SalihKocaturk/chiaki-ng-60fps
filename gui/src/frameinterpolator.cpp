// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// See frameinterpolator.h for the overview. Pipeline per new frame B (previous A):
//   1. luma:   copy B's Y plane into an R8 image the optical flow engine can read
//   2. OF:     VK_NV_optical_flow on its own queue, A<->B, 4x4 grid, both directions
//              (or, without the engine: compute block matching on a luma pyramid,
//              same output format, see kFlowBody)
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

#include <algorithm>
#include <initializer_list>
#include <string>
#include <cstring>
#include <cmath>

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
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkCreateQueryPool CreateQueryPool;
    PFN_vkDestroyQueryPool DestroyQueryPool;
    PFN_vkCmdResetQueryPool CmdResetQueryPool;
    PFN_vkCmdWriteTimestamp CmdWriteTimestamp;
    PFN_vkGetQueryPoolResults GetQueryPoolResults;
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

// ---- compute optical flow (GPUs without VK_NV_optical_flow) ----

// 2x downscale of the luma pyramid (one bilinear tap = 2x2 average).
const char *kPyrBody = R"(
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(p, imageSize(dst_pyr))))
        return;
    vec2 uv = (vec2(p) * 2.0 + 1.0) / vec2(textureSize(pyr_src, 0));
    imageStore(dst_pyr, p, vec4(textureLod(pyr_src, uv, 0.0).r));
)";

// Block matching for one pyramid level and one direction. One vector per
// 4x4 block of this level, matched with the 8x8 window centred on it.
// src_img holds the blocks, ref_img is searched: src(p) ~ ref(p + v), the
// same convention as the optical flow engine's output for (input = src,
// reference = ref). Coarsest level: full search within +-FG_RADIUS. Finer
// levels: the 3x3 parent vectors (x2) and zero as candidates, then a
// +-FG_RADIUS refinement around the best. Final level: sub-pixel parabola
// fit, written as pixels * 32 (the engine's S10.5 scale).
// Constants (#defines, prepended per level): FG_TOP, FG_FINAL, FG_RADIUS, FG_STEP.
const char *kFlowHeader = R"(
#define FG_WIN 8
#define FG_N ((FG_WIN / FG_STEP) * (FG_WIN / FG_STEP))
float fg_src[FG_N];
ivec2 fg_lim;

void fg_load_src(ivec2 base)
{
    int k = 0;
    for (int j = 0; j < FG_WIN; j += FG_STEP)
        for (int i = 0; i < FG_WIN; i += FG_STEP)
            fg_src[k++] = texelFetch(src_img, clamp(base + ivec2(i, j), ivec2(0), fg_lim), 0).r;
}

float fg_cost(ivec2 base, ivec2 v)
{
    float s = 0.0;
    int k = 0;
    for (int j = 0; j < FG_WIN; j += FG_STEP)
        for (int i = 0; i < FG_WIN; i += FG_STEP)
            s += abs(fg_src[k++] - texelFetch(ref_img, clamp(base + ivec2(i, j) + v, ivec2(0), fg_lim), 0).r);
    return s * (1.0 / float(FG_N));
}

// small bias towards short / coherent vectors so flat areas (sky) stay still
const float fg_lambda = 0.0015;
)";

const char *kFlowBody = R"(
    ivec2 g = ivec2(gl_GlobalInvocationID.xy);
    ivec2 gs = imageSize(dst_flow);
    if (any(greaterThanEqual(g, gs)))
        return;
    fg_lim = textureSize(src_img, 0) - 1;
    ivec2 base = g * 4 - 2;
    fg_load_src(base);
    ivec2 best = ivec2(0);
    float best_c = fg_cost(base, best);
#if FG_TOP
    // Wide search on a stride-2 lattice, then +-1 around the winner: about a
    // quarter of the full search's cost (this level has few blocks, so each
    // thread's serial loop sets the time). The coarse SAD surface is smooth.
    for (int dy = -FG_RADIUS; dy <= FG_RADIUS; dy += 2) {
        for (int dx = -FG_RADIUS; dx <= FG_RADIUS; dx += 2) {
            ivec2 v = ivec2(dx, dy);
            float c = fg_cost(base, v) + fg_lambda * length(vec2(v));
            if (c < best_c) { best_c = c; best = v; }
        }
    }
    ivec2 coarse = best;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            ivec2 v = coarse + ivec2(dx, dy);
            if ((dx == 0 && dy == 0) || any(greaterThan(abs(v), ivec2(FG_RADIUS))))
                continue;
            float c = fg_cost(base, v) + fg_lambda * length(vec2(v));
            if (c < best_c) { best_c = c; best = v; }
        }
    }
#else
    ivec2 pgs = textureSize(parent_flow, 0);
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            ivec2 pg = clamp(g / 2 + ivec2(dx, dy), ivec2(0), pgs - 1);
            ivec2 v = ivec2(round(texelFetch(parent_flow, pg, 0).xy * 2.0));
            float c = fg_cost(base, v) + ((dx == 0 && dy == 0) ? 0.0 : fg_lambda);
            if (c < best_c) { best_c = c; best = v; }
        }
    }
    ivec2 center = best;
    for (int dy = -FG_RADIUS; dy <= FG_RADIUS; dy++) {
        for (int dx = -FG_RADIUS; dx <= FG_RADIUS; dx++) {
            if (dx == 0 && dy == 0)
                continue;
            ivec2 v = center + ivec2(dx, dy);
            float c = fg_cost(base, v) + fg_lambda * length(vec2(dx, dy));
            if (c < best_c) { best_c = c; best = v; }
        }
    }
#endif
#if FG_FINAL
    vec2 sub = vec2(0.0);
    float c0 = fg_cost(base, best);
    float cxm = fg_cost(base, best - ivec2(1, 0)), cxp = fg_cost(base, best + ivec2(1, 0));
    float cym = fg_cost(base, best - ivec2(0, 1)), cyp = fg_cost(base, best + ivec2(0, 1));
    float dxx = cxm - 2.0 * c0 + cxp, dyy = cym - 2.0 * c0 + cyp;
    if (dxx > 1e-5) sub.x = clamp(0.5 * (cxm - cxp) / dxx, -0.5, 0.5);
    if (dyy > 1e-5) sub.y = clamp(0.5 * (cym - cyp) / dyy, -0.5, 0.5);
    imageStore(dst_flow, g, vec4((vec2(best) + sub) * 32.0, 0.0, 1.0));
#else
    imageStore(dst_flow, g, vec4(vec2(best), 0.0, 1.0));
#endif
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

// One work group, per frame: x = fraction of inconsistent blocks (flow
// beyond range / occlusion), y = fraction of blocks the ghost guard mostly
// replaces by B (no real interpolation there), z = mean motion length (px),
// w = max motion length. x drives the (disabled) fallback; all four are read
// back for the quality stats.
const char *kStatsHeader = R"(
shared vec4 partial[256];
)";
const char *kStatsBody = R"(
    uint t = gl_LocalInvocationIndex;
    ivec2 gs = textureSize(motion_raw, 0);
    int total = gs.x * gs.y;
    vec4 acc = vec4(0.0);
    for (int i = int(t); i < total; i += 256) {
        vec4 v = texelFetch(motion_raw, ivec2(i % gs.x, i / gs.x), 0);
        float len = length(v.xy);
        acc.x += v.w > 1.0 ? 1.0 : 0.0;
        acc.y += v.z >= 0.065 ? 1.0 : 0.0;
        acc.z += len;
        acc.w = max(acc.w, len);
    }
    partial[t] = acc;
    barrier();
    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (t < s) {
            vec4 o = partial[t + s];
            partial[t] = vec4(partial[t].xyz + o.xyz, max(partial[t].w, o.w));
        }
        barrier();
    }
    if (t == 0u) {
        vec4 p = partial[0];
        imageStore(dst_stats, ivec2(0), vec4(p.xyz / float(total), p.w));
    }
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

const char *FrameInterpolator::requiredExtension() { return VK_NV_OPTICAL_FLOW_EXTENSION_NAME; }
VkQueueFlags FrameInterpolator::requiredQueueFlags() { return VK_QUEUE_OPTICAL_FLOW_BIT_NV; }
const VkPhysicalDeviceFeatures2 *FrameInterpolator::requiredFeatures() { return &g_features; }

FrameInterpolator *FrameInterpolator::create(pl_log log, pl_vulkan vk, int quality)
{
    auto *fi = new FrameInterpolator();
    fi->quality = qBound(0, quality, 2);
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

    dp = pl_dispatch_create(log, gpu);
    if (!dp)
        return false;
    // Measurement only: missing timer support just means no GPU numbers.
    for (auto &t : timers)
        t = pl_timer_create(gpu);

    bool has_ext = false;
    for (int i = 0; i < vk->num_extensions; i++)
        has_ext |= std::strcmp(vk->extensions[i], VK_NV_OPTICAL_FLOW_EXTENSION_NAME) == 0;
    const bool force_compute = qEnvironmentVariableIntValue("CHIAKI_FRAMEGEN_COMPUTE") != 0;
    if (has_ext && !force_compute && initOpticalFlowEngine())
        return true;

    // No optical flow engine (AMD, Intel, older NVIDIA) or forced: compute path.
    compute_flow = true;
    FG_LOG << "initialized, compute optical flow"
           << (force_compute ? " (forced by CHIAKI_FRAMEGEN_COMPUTE)" : has_ext ? " (engine init failed)" : "");
    return true;
}

bool FrameInterpolator::initOpticalFlowEngine()
{
    fn = new Funcs();
#define LOAD_I(name) fn->name = reinterpret_cast<PFN_vk##name>(vk->get_proc_addr(vk->instance, "vk" #name))
#define LOAD_D(name) fn->name = reinterpret_cast<PFN_vk##name>(fn->GetDeviceProcAddr(vk->device, "vk" #name))
    LOAD_I(GetDeviceProcAddr);
    LOAD_I(GetPhysicalDeviceMemoryProperties);
    LOAD_I(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_I(GetPhysicalDeviceFeatures2);
    LOAD_I(GetPhysicalDeviceProperties);
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
    LOAD_D(CreateQueryPool); LOAD_D(DestroyQueryPool); LOAD_D(CmdResetQueryPool);
    LOAD_D(CmdWriteTimestamp); LOAD_D(GetQueryPoolResults);
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

    if (props[of_qf].timestampValidBits > 0 && fn->GetPhysicalDeviceProperties && fn->CreateQueryPool &&
        fn->CmdResetQueryPool && fn->CmdWriteTimestamp && fn->GetQueryPoolResults) {
        VkPhysicalDeviceProperties pp = {};
        fn->GetPhysicalDeviceProperties(vk->phys_device, &pp);
        ts_period_ns = pp.limits.timestampPeriod;
        const uint32_t bits = props[of_qf].timestampValidBits;
        ts_mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * kCmdRing;
        if (fn->CreateQueryPool(vk->device, &qi, nullptr, &of_query) != VK_SUCCESS)
            of_query = VK_NULL_HANDLE;
    }
    if (!of_query)
        FG_LOG << "optical flow queue has no timestamps, OF GPU time not measured";

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
    for (auto &t : timers)
        pl_timer_destroy(gpu, &t);
    if (of_query)
        fn->DestroyQueryPool(vk->device, of_query, nullptr);
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
    for (int i = 0; i < 2; i++) {
        for (int l = 0; l <= kMaxLevels; l++) {
            pl_tex_destroy(gpu, &pyr[i][l]);
            pl_tex_destroy(gpu, &lflow[i][l]);
        }
    }
    num_levels = 0;
    pl_tex_destroy(gpu, &motion);
    pl_tex_destroy(gpu, &motion_smooth);
    pl_tex_destroy(gpu, &frame_stats);
    for (int i = 0; i < kStatsRing; i++) {
        pl_buf_destroy(gpu, &stats_buf[i]);
        stats_pending[i] = false;
    }
    stats_readable = false;
    pl_tex_destroy(gpu, &out_planes[0]);
    pl_tex_destroy(gpu, &out_planes[1]);
    width = height = grid_w = grid_h = 0;
    plane_fmt[0] = plane_fmt[1] = nullptr;
    resources_ok = false;
    has_prev_luma = false;
}

// Optical flow engine images (luma inputs, flow + cost outputs) and the two
// sessions (one per luma slot as input).
bool FrameInterpolator::ensureEngineResources(int perf_level)
{
    const auto perf = static_cast<VkOpticalFlowPerformanceLevelNV>(perf_level);
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
        return false;
    }

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
            return false;
        }
    }

    return true;
}

// First format of `names` that supports everything in `caps`.
static pl_fmt findFmt(pl_gpu gpu, std::initializer_list<const char *> names, pl_fmt_caps caps)
{
    for (const char *n : names) {
        pl_fmt f = pl_find_named_fmt(gpu, n);
        if (f && (f->caps & caps) == caps)
            return f;
    }
    return nullptr;
}

// Compute optical flow: luma + pyramid per slot, coarse flow per direction
// and level, final flow (pixels * 32) in flow_fwd / flow_bwd.
bool FrameInterpolator::ensureComputeFlowResources()
{
    const pl_fmt_caps rw = pl_fmt_caps(PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
    pl_fmt luma_fmt = findFmt(gpu, {"r8", "r16f", "r32f"}, rw);
    pl_fmt flow_fmt = findFmt(gpu, {"rg16f", "rg32f", "rgba16f"},
                              pl_fmt_caps(PL_FMT_CAP_STORABLE | PL_FMT_CAP_SAMPLEABLE));
    if (!luma_fmt || !flow_fmt) {
        FG_WARN << "no storable formats for compute optical flow";
        return false;
    }

    // Levels until the coarsest is about 1/16 (or the picture gets tiny).
    num_levels = 0;
    while (num_levels < kMaxLevels && (width >> (num_levels + 1)) >= 64 && (height >> (num_levels + 1)) >= 36)
        num_levels++;

    auto tex = [&](int w, int h, pl_fmt f) {
        return pl_tex_create(gpu, PL_TMP(pl_tex_params,
            .w = w, .h = h, .format = f, .sampleable = true, .storable = true));
    };
    auto lw = [&](int l) { return (width + (1 << l) - 1) >> l; };
    auto lh = [&](int l) { return (height + (1 << l) - 1) >> l; };
    auto gw = [&](int l) { return (lw(l) + kGrid - 1) / kGrid; };
    auto gh = [&](int l) { return (lh(l) + kGrid - 1) / kGrid; };

    bool ok = true;
    for (int i = 0; i < 2; i++) {
        luma[i].tex = tex(width, height, luma_fmt);
        ok = ok && luma[i].tex;
        for (int l = 1; l <= num_levels; l++) {
            pyr[i][l] = tex(lw(l), lh(l), luma_fmt);
            lflow[i][l] = tex(gw(l), gh(l), flow_fmt);
            ok = ok && pyr[i][l] && lflow[i][l];
        }
    }
    flow_fwd.tex = tex(grid_w, grid_h, flow_fmt);
    flow_bwd.tex = tex(grid_w, grid_h, flow_fmt);
    ok = ok && flow_fwd.tex && flow_bwd.tex;
    if (!ok) {
        FG_WARN << "failed to create compute optical flow textures " << width << "x" << height;
        return false;
    }

    // Per level constants. quality 0 high / 1 balanced / 2 fast: the coarse
    // full search is the same (it decides how fast a motion can be caught);
    // the expensive full-size levels get sparser windows / smaller refinement.
    // The coarsest level is tiny (1/16 size), so a wide search there is cheap:
    // +-16 at 1/16 catches about +-280 px per real frame at full size (fast
    // camera turns at 30 fps); +-8 topped out near 150 px and broke on turns.
    for (int l = 0; l <= num_levels; l++) {
        const bool top = l == num_levels;
        int radius = top ? 16 : (l == 0 ? 1 : 2);
        int step = 1;
        if (quality >= 1 && l == 0)
            step = 2;
        if (quality >= 2 && l <= 1) {
            step = 2;
            if (!top)
                radius = 1;
        }
        flow_hdr[l] = "#define FG_TOP " + std::to_string(top ? 1 : 0) +
                      "\n#define FG_FINAL " + std::to_string(l == 0 ? 1 : 0) +
                      "\n#define FG_RADIUS " + std::to_string(radius) +
                      "\n#define FG_STEP " + std::to_string(step) + "\n" + kFlowHeader;
    }
    return true;
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

    // quality 0 / 1 / 2 -> SLOW (best) / MEDIUM / FAST
    const auto perf = static_cast<VkOpticalFlowPerformanceLevelNV>(VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_SLOW_NV + quality);

    const bool ok = compute_flow ? ensureComputeFlowResources() : ensureEngineResources(perf);
    if (!ok) {
        destroyResources();
        return false;
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
        .sampleable = true, .storable = true, .host_readable = true)) : nullptr;
    stats_readable = frame_stats != nullptr;
    if (!frame_stats && motion_fmt) // no host-readable storage image: no quality stats
        frame_stats = pl_tex_create(gpu, PL_TMP(pl_tex_params,
            .w = 1, .h = 1, .format = motion_fmt, .sampleable = true, .storable = true));
    for (int i = 0; i < kStatsRing && stats_readable; i++) {
        stats_buf[i] = pl_buf_create(gpu, PL_TMP(pl_buf_params, .size = 8, .host_readable = true));
        stats_pending[i] = false;
    }
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
           << " planes " << plane_fmt[0]->name << "+" << plane_fmt[1]->name;
    if (compute_flow)
        FG_LOG << "compute optical flow: " << num_levels << " pyramid levels, quality " << quality;
    else
        FG_LOG << "optical flow engine perf " << int(perf);
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
        .timer = timers[kTLuma],
    ));
}

// Compute optical flow. runPyramid builds the pyramid of every new luma (the
// previous frame's stays for the next pair); runComputeFlow then does
// coarse-to-fine block matching in both directions. Plain libplacebo
// dispatches, so libplacebo handles all synchronisation.
bool FrameInterpolator::runPyramid(int cur_slot)
{
    for (int l = 1; l <= num_levels; l++) {
        pl_tex src = l == 1 ? luma[cur_slot].tex : pyr[cur_slot][l - 1];
        pl_shader sh = pl_dispatch_begin(dp);
        const struct pl_shader_desc descs[] = {
            sampled("pyr_src", src, true),
            storage("dst_pyr", pyr[cur_slot][l]),
        };
        struct pl_custom_shader cs = {};
        cs.description = "framegen pyramid";
        cs.body = kPyrBody;
        cs.descriptors = descs;
        cs.num_descriptors = 2;
        cs.compute = true;
        cs.compute_group_size[0] = 16;
        cs.compute_group_size[1] = 16;
        if (!pl_shader_custom(sh, &cs)) {
            pl_dispatch_abort(dp, &sh);
            return false;
        }
        const int w = pyr[cur_slot][l]->params.w, h = pyr[cur_slot][l]->params.h;
        if (!pl_dispatch_compute(dp, PL_TMP(pl_dispatch_compute_params,
                .shader = &sh, .dispatch_size = {(w + 15) / 16, (h + 15) / 16, 1},
                .timer = timers[kTPyr0 + l - 1])))
            return false;
    }
    return true;
}

bool FrameInterpolator::runComputeFlow(int cur_slot)
{
    const int prev = 1 - cur_slot;
    auto level_tex = [&](int slot, int l) { return l == 0 ? luma[slot].tex : pyr[slot][l]; };
    // dir 0 -> flow_fwd: blocks of B (new) searched in A
    // dir 1 -> flow_bwd: blocks of A searched in B
    for (int l = num_levels; l >= 0; l--) {
        for (int dir = 0; dir < 2; dir++) {
            const int src_slot = dir == 0 ? cur_slot : prev;
            const int ref_slot = dir == 0 ? prev : cur_slot;
            pl_tex out = l == 0 ? (dir == 0 ? flow_fwd.tex : flow_bwd.tex) : lflow[dir][l];
            pl_shader sh = pl_dispatch_begin(dp);
            struct pl_shader_desc descs[4];
            int nd = 0;
            descs[nd++] = sampled("src_img", level_tex(src_slot, l), false);
            descs[nd++] = sampled("ref_img", level_tex(ref_slot, l), false);
            if (l < num_levels)
                descs[nd++] = sampled("parent_flow", lflow[dir][l + 1], false);
            descs[nd++] = storage("dst_flow", out);
            struct pl_custom_shader cs = {};
            cs.description = "framegen compute flow";
            cs.header = flow_hdr[l].c_str();
            cs.body = kFlowBody;
            cs.descriptors = descs;
            cs.num_descriptors = nd;
            cs.compute = true;
            cs.compute_group_size[0] = 8;
            cs.compute_group_size[1] = 8;
            if (!pl_shader_custom(sh, &cs)) {
                pl_dispatch_abort(dp, &sh);
                return false;
            }
            const int w = out->params.w, h = out->params.h;
            if (!pl_dispatch_compute(dp, PL_TMP(pl_dispatch_compute_params,
                    .shader = &sh, .dispatch_size = {(w + 7) / 8, (h + 7) / 8, 1},
                    .timer = timers[kTFlow0 + 2 * l + dir])))
                return false;
        }
    }
    return true;
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
    // The slot's previous submission has finished, so its timestamps are
    // ready (no wait flag: this never blocks).
    if (of_query && of_query_written[ci]) {
        uint64_t t[2] = {};
        if (fn->GetQueryPoolResults(vk->device, of_query, 2 * ci, 2, sizeof(t), t, sizeof(uint64_t),
                                    VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && of_ms.size() < 4096)
            of_ms.push_back(float(double((t[1] - t[0]) & ts_mask) * ts_period_ns / 1e6));
        of_query_written[ci] = false;
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
    if (of_query) {
        fn->CmdResetQueryPool(cb, of_query, 2 * ci, 2);
        fn->CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, of_query, 2 * ci);
    }
    fn->CmdOpticalFlowExecuteNV(cb, session[cur_slot], &ei);
    if (of_query)
        fn->CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, of_query, 2 * ci + 1);
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
    of_query_written[ci] = ok && of_query;
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
                .shader = &sh, .dispatch_size = {(grid_w + 15) / 16, (grid_h + 15) / 16, 1},
                .timer = timers[kTMotion])))
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
                .shader = &sh, .dispatch_size = {(grid_w + 15) / 16, (grid_h + 15) / 16, 1},
                .timer = timers[kTSmooth])))
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
        cs.compute_shmem = 256 * 4 * sizeof(float);
        cs.compute_group_size[0] = 16;
        cs.compute_group_size[1] = 16;
        if (!pl_shader_custom(sh, &cs)) {
            pl_dispatch_abort(dp, &sh);
            return false;
        }
        if (!pl_dispatch_compute(dp, PL_TMP(pl_dispatch_compute_params,
                .shader = &sh, .dispatch_size = {1, 1, 1}, .timer = timers[kTStats])))
            return false;
        // Async read-back for the quality stats (polled later, never waited on).
        if (stats_readable && stats_buf[stats_index] && !stats_pending[stats_index]) {
            if (pl_tex_download(gpu, PL_TMP(pl_tex_transfer_params,
                    .tex = frame_stats, .buf = stats_buf[stats_index])))
                stats_pending[stats_index] = true;
            stats_index = (stats_index + 1) % kStatsRing;
        }
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
                .shader = &sh, .dispatch_size = {(w + 15) / 16, (h + 15) / 16, 1},
                .timer = timers[plane == 0 ? kTMidY : kTMidUV])))
            return false;
    }
    return true;
}

void FrameInterpolator::pollTimers()
{
    for (int i = 0; i < kNumTimers; i++) {
        uint64_t ns;
        while ((ns = pl_timer_query(gpu, timers[i])) != 0) {
            if (timer_ms[i].size() < 4096)
                timer_ms[i].push_back(float(ns / 1e6));
        }
    }
}

namespace {
float halfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
    float v;
    if (exp == 0)
        v = std::ldexp(float(mant), -24);
    else if (exp == 31)
        v = mant ? 0.0f : 65504.0f;
    else
        v = std::ldexp(float(mant | 0x400), int(exp) - 25);
    return sign ? -v : v;
}
} // namespace

void FrameInterpolator::pollQuality()
{
    for (int i = 0; i < kStatsRing; i++) {
        if (!stats_pending[i] || !stats_buf[i] || pl_buf_poll(gpu, stats_buf[i], 0))
            continue;
        uint16_t hv[4] = {};
        stats_pending[i] = false;
        if (!pl_buf_read(gpu, stats_buf[i], 0, hv, sizeof(hv)) || q_bad.size() >= 4096)
            continue;
        q_bad.push_back(halfToFloat(hv[0]));
        q_hold.push_back(halfToFloat(hv[1]));
        q_motion.push_back(halfToFloat(hv[2]));
        q_motion_max.push_back(halfToFloat(hv[3]));
    }
}

FrameInterpolator::QualityStats FrameInterpolator::takeQualityStats()
{
    pollQuality();
    QualityStats q;
    const size_t n = q_bad.size();
    q.n = int(n);
    if (n) {
        std::vector<float> fast_bad, fast_hold;
        double sb = 0, sh = 0, sm = 0;
        for (size_t i = 0; i < n; i++) {
            sb += q_bad[i]; sh += q_hold[i]; sm += q_motion[i];
            q.motion_max = std::max(q.motion_max, double(q_motion_max[i]));
            // "fast": mean motion over 12 px per real frame (camera turn)
            if (q_motion[i] >= 12.0f) {
                fast_bad.push_back(q_bad[i]);
                fast_hold.push_back(q_hold[i]);
            }
            if (q_bad[i] >= 0.15f || q_hold[i] >= 0.15f)
                q.poor_frames++;
        }
        q.bad_avg = sb / n; q.hold_avg = sh / n; q.motion_avg = sm / n;
        q.fast_n = int(fast_bad.size());
        for (size_t i = 0; i < fast_bad.size(); i++) {
            q.fast_bad_avg += fast_bad[i] / fast_bad.size();
            q.fast_hold_avg += fast_hold[i] / fast_hold.size();
        }
        std::vector<float> mm = q_motion;
        std::sort(mm.begin(), mm.end());
        q.motion_p99 = mm[std::min(n - 1, size_t(0.99 * (n - 1) + 0.5))];
    }
    q_bad.clear(); q_hold.clear(); q_motion.clear(); q_motion_max.clear();
    return q;
}

namespace {
double percentile(std::vector<float> &v, double q)
{
    if (v.empty())
        return 0.0;
    const size_t k = std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// Merges the passes of one stage. Passes cannot be paired per frame, so
// avg = sum of means (exact); p50/p99/max = sums (upper bounds).
FrameInterpolator::StageStat stageStat(std::vector<float> *const *parts, int nparts)
{
    FrameInterpolator::StageStat st;
    for (int i = 0; i < nparts; i++) {
        std::vector<float> &v = *parts[i];
        if (v.empty())
            continue;
        double sum = 0.0;
        for (float x : v)
            sum += x;
        st.n = st.n == 0 ? int(v.size()) : std::min(st.n, int(v.size()));
        st.avg += sum / v.size();
        st.max += *std::max_element(v.begin(), v.end());
        st.p99 += percentile(v, 0.99);
        st.p50 += percentile(v, 0.50);
        v.clear();
    }
    return st;
}
} // namespace

FrameInterpolator::GpuStats FrameInterpolator::takeGpuStats()
{
    pollTimers();
    GpuStats g;
    // OF stage: engine timestamps, or the compute pyramid + flow passes
    std::vector<float> *of_parts[1 + kNumTimers - kTPyr0];
    int n_of = 0;
    of_parts[n_of++] = &of_ms;
    for (int i = kTPyr0; i < kNumTimers; i++)
        if (!timer_ms[i].empty())
            of_parts[n_of++] = &timer_ms[i];
    std::vector<float> *luma_parts[] = {&timer_ms[kTLuma]};
    std::vector<float> *motion_parts[] = {&timer_ms[kTMotion], &timer_ms[kTSmooth], &timer_ms[kTStats]};
    std::vector<float> *mid_parts[] = {&timer_ms[kTMidY], &timer_ms[kTMidUV]};
    g.of = stageStat(of_parts, n_of);
    g.luma = stageStat(luma_parts, 1);
    g.motion = stageStat(motion_parts, 3);
    g.mid = stageStat(mid_parts, 2);
    g.total_avg = g.of.avg + g.luma.avg + g.motion.avg + g.mid.avg;
    g.total_p99_ub = g.of.p99 + g.luma.p99 + g.motion.p99 + g.mid.p99;
    return g;
}

bool FrameInterpolator::interpolate(const struct pl_frame *a, const struct pl_frame *b)
{
    pollTimers();
    pollQuality();
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
    bool ok = b_acq && runLuma(b, slot) && (!compute_flow || runPyramid(slot));
    const bool luma_ok = ok;
    if (ok && use_a)
        ok = a_acq && (compute_flow ? runComputeFlow(slot) : runOpticalFlow(slot)) && runInterpolation(a, b);
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
