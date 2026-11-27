#include "macVsync.h"

#include "chiaki/time.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#include <atomic>
#include <mutex>

// CVDisplayLink is deprecated since macOS 15 but still works and, unlike
// CADisplayLink, needs no NSView and runs on its own thread.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static std::mutex link_mutex;
static CVDisplayLinkRef s_link = nullptr;
static std::atomic<int64_t> vblank_us{0};
static std::atomic<int64_t> period_us{0};
static double ticks_to_us = 0.0;

static CVReturn onVblank(CVDisplayLinkRef, const CVTimeStamp *, const CVTimeStamp *out, CVOptionFlags, CVOptionFlags *, void *)
{
    // out->hostTime is the vblank this callback prepares for, in mach ticks.
    // chiaki's clock (CLOCK_MONOTONIC) runs at the same rate with another
    // origin; take the offset from two back-to-back reads.
    const int64_t now_ticks = static_cast<int64_t>(mach_absolute_time());
    const int64_t now_us = static_cast<int64_t>(chiaki_time_now_monotonic_us());
    const int64_t v = now_us + static_cast<int64_t>((static_cast<int64_t>(out->hostTime) - now_ticks) * ticks_to_us);
    vblank_us.store(v, std::memory_order_release);
    if ((out->flags & kCVTimeStampVideoRefreshPeriodValid) && out->videoTimeScale > 0 && out->videoRefreshPeriod > 0) {
        const int64_t p = static_cast<int64_t>(1e6 * out->videoRefreshPeriod / out->videoTimeScale);
        if (p > 2000 && p < 50000)
            period_us.store(p, std::memory_order_release);
    }
    return kCVReturnSuccess;
}

bool macVsyncStart(double x, double y)
{
    std::lock_guard<std::mutex> lock(link_mutex);
    if (ticks_to_us == 0.0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        ticks_to_us = static_cast<double>(tb.numer) / tb.denom / 1000.0;
    }
    if (s_link) {
        CVDisplayLinkStop(s_link);
        CVDisplayLinkRelease(s_link);
        s_link = nullptr;
    }
    vblank_us.store(0);
    period_us.store(0);
    CGDirectDisplayID display = 0;
    uint32_t count = 0;
    if (CGGetDisplaysWithPoint(CGPointMake(x, y), 1, &display, &count) != kCGErrorSuccess || count == 0)
        display = CGMainDisplayID();
    if (CVDisplayLinkCreateWithCGDisplay(display, &s_link) != kCVReturnSuccess || !s_link) {
        s_link = nullptr;
        return false;
    }
    CVDisplayLinkSetOutputCallback(s_link, onVblank, nullptr);
    if (CVDisplayLinkStart(s_link) != kCVReturnSuccess) {
        CVDisplayLinkRelease(s_link);
        s_link = nullptr;
        return false;
    }
    return true;
}

void macVsyncStop()
{
    std::lock_guard<std::mutex> lock(link_mutex);
    if (s_link) {
        CVDisplayLinkStop(s_link);
        CVDisplayLinkRelease(s_link);
        s_link = nullptr;
    }
    vblank_us.store(0);
    period_us.store(0);
}

bool macVsyncGet(int64_t *v, int64_t *p)
{
    *v = vblank_us.load(std::memory_order_acquire);
    *p = period_us.load(std::memory_order_acquire);
    if (*v <= 0 || *p <= 0)
        return false;
    // Stale (link stopped or display asleep): don't pace on it.
    const int64_t now = static_cast<int64_t>(chiaki_time_now_monotonic_us());
    return now - *v < 200000;
}

#pragma clang diagnostic pop
