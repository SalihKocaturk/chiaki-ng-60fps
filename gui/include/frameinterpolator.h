// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Frame generation: builds a motion-compensated in-between frame from two
// decoded Vulkan video frames. Motion comes from the GPU's optical flow engine
// (VK_NV_optical_flow) when the device has one, otherwise from a compute
// shader block matcher (any Vulkan GPU); warping/blending runs as libplacebo
// compute shaders in both cases.
// Everything stays on the GPU; nothing is copied to the CPU.

#ifndef CHIAKI_FRAMEINTERPOLATOR_H
#define CHIAKI_FRAMEINTERPOLATOR_H

#include <libplacebo/vulkan.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/renderer.h>
#include <libplacebo/gpu.h>

#include <cstdint>
#include <string>
#include <vector>

class FrameInterpolator
{
public:
    // Device extension / queue / feature needed at VkDevice creation time.
    static const char *requiredExtension();
    static VkQueueFlags requiredQueueFlags();
    static const VkPhysicalDeviceFeatures2 *requiredFeatures();

    // Returns nullptr on failure. Uses the optical flow engine if available,
    // else compute optical flow (CHIAKI_FRAMEGEN_COMPUTE=1 forces compute).
    // quality: 0 = high (slow optical flow), 1 = balanced, 2 = fast.
    static FrameInterpolator *create(pl_log log, pl_vulkan vk, int quality);
    ~FrameInterpolator();

    // Feed frames in display order. `b` is the newest mapped frame, `a` the
    // one passed as `b` in the previous call (or nullptr). Frames stay owned
    // (mapped) by the caller and must stay mapped while midFrame() is in use
    // and until the next call. Returns true if a mid frame between `a` and `b`
    // was produced (see midFrame()).
    bool interpolate(const struct pl_frame *a, const struct pl_frame *b);

    // Forget history (stream reset / frame mapping restarted).
    void reset() { has_prev_luma = false; }

    const struct pl_frame *midFrame() const { return &mid; }

    // Counters for logging / stats.
    uint64_t producedFrames() const { return produced; }
    uint64_t failedFrames() const { return failed; }
    bool usesComputeFlow() const { return compute_flow; }

    // GPU time per stage (ms), collected since the last call. Timer results
    // arrive a few frames late and are never waited for (no CPU stall).
    // libplacebo may drop single timer results, so stages cannot be paired
    // per frame: total_avg is exact (sum of means), total_p99_ub is an upper
    // bound (sum of per-stage p99).
    struct StageStat { int n = 0; double avg = 0.0, p50 = 0.0, p99 = 0.0, max = 0.0; };
    struct GpuStats {
        StageStat of, luma, motion, mid; // motion = motion+smooth+stats passes, mid = Y+UV
        double total_avg = 0.0, total_p99_ub = 0.0;
    };
    GpuStats takeGpuStats();

    // Mid frame quality since the last call (GPU read-back, a few frames
    // late, never waited for). Fractions are of the 4x4 blocks per frame:
    // bad = flow inconsistent (motion beyond range / occlusion), hold = the
    // ghost guard mostly shows B there instead of interpolating. Motion in px
    // per real frame; "fast" frames have a mean motion >= 12 px (turns).
    struct QualityStats {
        int n = 0, fast_n = 0, poor_frames = 0; // poor: bad or hold >= 15%
        double bad_avg = 0.0, hold_avg = 0.0, motion_avg = 0.0, motion_p99 = 0.0, motion_max = 0.0;
        double fast_bad_avg = 0.0, fast_hold_avg = 0.0;
    };
    QualityStats takeQualityStats();

private:
    FrameInterpolator() = default;
    bool init(pl_log log, pl_vulkan vk);
    bool initOpticalFlowEngine();
    bool ensureEngineResources(int perf_level);
    bool ensureComputeFlowResources();
    bool runPyramid(int cur_slot);
    bool runComputeFlow(int cur_slot);
    int quality = 0;
    bool ensureResources(const struct pl_frame *frame);
    void destroyResources();
    bool runLuma(const struct pl_frame *frame, int slot);
    bool runOpticalFlow(int cur_slot);
    bool runInterpolation(const struct pl_frame *a, const struct pl_frame *b);
    void pollTimers();
    void pollQuality();
    static constexpr int kStatsRing = 4;
    pl_buf stats_buf[kStatsRing] = {};
    bool stats_pending[kStatsRing] = {};
    int stats_index = 0;
    bool stats_readable = false;
    std::vector<float> q_bad, q_hold, q_motion, q_motion_max;

    // GPU timers: libplacebo pl_timer per compute pass, Vulkan timestamp
    // queries around the optical flow command (own queue, own command buffers).
    // Compute optical flow: pyramid levels 1..kMaxLevels below full size.
    static constexpr int kMaxLevels = 4;
    enum { kTLuma, kTMotion, kTSmooth, kTStats, kTMidY, kTMidUV,
           kTPyr0, kTFlow0 = kTPyr0 + kMaxLevels, kNumTimers = kTFlow0 + 2 * (kMaxLevels + 1) };
    pl_timer timers[kNumTimers] = {};
    std::vector<float> timer_ms[kNumTimers];
    std::vector<float> of_ms;

    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView of_view = VK_NULL_HANDLE; // view used by the optical flow engine
        pl_tex tex = nullptr;                 // libplacebo wrapper (for shaders)
        VkSemaphore hold_sem = VK_NULL_HANDLE;
        uint64_t hold_value = 0;
    };
    bool createImage(Image &img, int w, int h, VkFormat of_format, VkFormat pl_format,
                     uint32_t of_usage, const char *name);
    void destroyImage(Image &img);

    pl_log log = nullptr;
    pl_vulkan vk = nullptr;
    pl_gpu gpu = nullptr;
    pl_dispatch dp = nullptr;

    struct Funcs;
    Funcs *fn = nullptr;

    uint32_t of_qf = UINT32_MAX;
    VkQueue of_queue = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    static constexpr int kCmdRing = 4;
    VkCommandBuffer cmd[kCmdRing] = {};
    uint64_t cmd_value[kCmdRing] = {};
    int cmd_index = 0;
    VkQueryPool of_query = VK_NULL_HANDLE; // 2 timestamps per command buffer slot
    bool of_query_written[kCmdRing] = {};
    double ts_period_ns = 0.0;
    uint64_t ts_mask = 0;
    VkSemaphore of_sem = VK_NULL_HANDLE;
    uint64_t of_value = 0;
    uint32_t qfs[4] = {};
    uint32_t num_qfs = 0;

    // Resources sized for the current stream.
    int width = 0, height = 0;   // luma plane size
    int grid_w = 0, grid_h = 0;  // optical flow output size (4x4 grid)
    const struct pl_fmt_t *plane_fmt[2] = {};
    Image luma[2];
    Image flow_fwd, flow_bwd, cost_fwd, cost_bwd;
    VkOpticalFlowSessionNV session[2] = {}; // session[i]: input = luma[i], reference = luma[1-i]
    // Compute optical flow (compute_flow == true): luma/flow_* above are plain
    // libplacebo textures (Image::tex only), flow in pixels*32 like the engine.
    bool compute_flow = false;
    int num_levels = 0;                        // pyramid levels below full size
    pl_tex pyr[2][kMaxLevels + 1] = {};        // [slot][level], level 0 unused (= luma)
    pl_tex lflow[2][kMaxLevels + 1] = {};      // [dir][level] coarse flow, level 0 unused
    std::string flow_hdr[kMaxLevels + 1];      // per level shader constants
    pl_tex motion = nullptr;      // per-block motion for the mid frame (rgba16f)
    pl_tex motion_smooth = nullptr; // vector-median filtered motion
    pl_tex frame_stats = nullptr;   // 1x1: fraction of unreliable blocks
    pl_tex out_planes[2] = {};    // mid frame planes (Y, UV)
    bool resources_ok = false;

    bool has_prev_luma = false;
    int prev_slot = 0;
    struct pl_frame mid = {};

    uint64_t produced = 0;
    uint64_t failed = 0;
};

#endif // CHIAKI_FRAMEINTERPOLATOR_H
