#include "macDisplayRate.h"

#include <CoreGraphics/CoreGraphics.h>
#include <cmath>

static bool switched = false;

// Within 0.5 % of a whole multiple of fps (accepts 59.94 / 119.88 Hz modes).
static bool isMultiple(double hz, double fps)
{
    if (hz <= 0.0 || fps <= 0.0)
        return false;
    const double k = std::round(hz / fps);
    return k >= 1.0 && std::fabs(hz - k * fps) <= hz * 0.005;
}

QString macDisplayRateMatch(double x, double y, double fps, double *out_hz)
{
    *out_hz = 0.0;
    if (qEnvironmentVariableIsSet("CHIAKI_NO_REFRESH_SWITCH"))
        return {};
    CGDirectDisplayID display = 0;
    uint32_t count = 0;
    if (CGGetDisplaysWithPoint(CGPointMake(x, y), 1, &display, &count) != kCGErrorSuccess || count == 0)
        display = CGMainDisplayID();

    CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
    if (!current)
        return {};
    const double cur_hz = CGDisplayModeGetRefreshRate(current);
    *out_hz = cur_hz;
    const size_t w = CGDisplayModeGetWidth(current), h = CGDisplayModeGetHeight(current);
    const size_t pw = CGDisplayModeGetPixelWidth(current), ph = CGDisplayModeGetPixelHeight(current);
    if (cur_hz <= 0.0 || isMultiple(cur_hz, fps)) {
        CGDisplayModeRelease(current);
        return cur_hz > 0.0 ? QStringLiteral("[display] %1 Hz fits %2 fps, unchanged").arg(cur_hz, 0, 'f', 2).arg(fps, 0, 'f', 0) : QString();
    }

    // Same size, refresh a multiple of fps; prefer the highest one not above the
    // current refresh (144 Hz -> 120 Hz, 75 Hz -> 60 Hz).
    const void *keys[] = { kCGDisplayShowDuplicateLowResolutionModes };
    const void *vals[] = { kCFBooleanTrue };
    CFDictionaryRef opts = CFDictionaryCreate(nullptr, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, opts);
    CFRelease(opts);
    CGDisplayModeRef best = nullptr;
    double best_hz = 0.0;
    for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); i++) {
        auto m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        const double hz = CGDisplayModeGetRefreshRate(m);
        if (CGDisplayModeGetWidth(m) != w || CGDisplayModeGetHeight(m) != h
            || CGDisplayModeGetPixelWidth(m) != pw || CGDisplayModeGetPixelHeight(m) != ph
            || !CGDisplayModeIsUsableForDesktopGUI(m) || !isMultiple(hz, fps) || hz > cur_hz + 0.5)
            continue;
        if (hz > best_hz) {
            best = m;
            best_hz = hz;
        }
    }
    QString msg;
    if (!best) {
        msg = QStringLiteral("[display] %1 Hz is not a multiple of %2 fps and no matching mode exists; expect uneven frame pacing. Set the display to 120 or 60 Hz.")
                  .arg(cur_hz, 0, 'f', 2).arg(fps, 0, 'f', 0);
    } else {
        CGDisplayConfigRef cfg = nullptr;
        CGError err = CGBeginDisplayConfiguration(&cfg);
        if (err == kCGErrorSuccess)
            err = CGConfigureDisplayWithDisplayMode(cfg, display, best, nullptr);
        if (err == kCGErrorSuccess)
            err = CGCompleteDisplayConfiguration(cfg, kCGConfigureForAppOnly);
        else if (cfg)
            CGCancelDisplayConfiguration(cfg);
        if (err == kCGErrorSuccess) {
            switched = true;
            *out_hz = best_hz;
            msg = QStringLiteral("[display] switched %1 Hz -> %2 Hz for %3 fps frame generation (restored when streaming ends)")
                      .arg(cur_hz, 0, 'f', 2).arg(best_hz, 0, 'f', 2).arg(fps, 0, 'f', 0);
        } else {
            msg = QStringLiteral("[display] could not switch %1 Hz -> %2 Hz (error %3)").arg(cur_hz, 0, 'f', 2).arg(best_hz, 0, 'f', 2).arg(int(err));
        }
    }
    if (modes)
        CFRelease(modes);
    CGDisplayModeRelease(current);
    return msg;
}

QString macDisplayRateRestore()
{
    if (!switched)
        return {};
    switched = false;
    CGRestorePermanentDisplayConfiguration();
    return QStringLiteral("[display] refresh rate restored");
}
