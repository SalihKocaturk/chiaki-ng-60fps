// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Frame generation: builds a motion-compensated in-between frame from two
// decoded Vulkan video frames. Motion comes from the GPU's optical flow engine
// (VK_NV_optical_flow); warping/blending runs as libplacebo compute shaders.
// Everything stays on the GPU; nothing is copied to the CPU.

#ifndef CHIAKI_FRAMEINTERPOLATOR_H
#define CHIAKI_FRAMEINTERPOLATOR_H

#include <libplacebo/vulkan.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/renderer.h>

#include <cstdint>

class FrameInterpolator
{
public:
    // Frame generation was requested by the user (CHIAKI_FRAMEGEN=1 for now).
    static bool requested();

    // Device extension / queue / feature needed at VkDevice creation time.
    static const char *requiredExtension();
    static VkQueueFlags requiredQueueFlags();
    static const VkPhysicalDeviceFeatures2 *requiredFeatures();

    // Returns nullptr if the device cannot do optical flow.
    static FrameInterpolator *create(pl_log log, pl_vulkan vk);
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

private:
    FrameInterpolator() = default;
    bool init(pl_log log, pl_vulkan vk);
    bool ensureResources(const struct pl_frame *frame);
    void destroyResources();
    bool runLuma(const struct pl_frame *frame, int slot);
    bool runOpticalFlow(int cur_slot);
    bool runInterpolation(const struct pl_frame *a, const struct pl_frame *b);

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
