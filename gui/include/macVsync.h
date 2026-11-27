#ifndef MACVSYNC_H
#define MACVSYNC_H

#include <cstdint>

// Display refresh timing for frame generation pacing on macOS.
//
// Frame gen used to present each image when its timer fired, at a phase set by
// when stream frames arrived. Whether a present made the next vblank depended
// on that random phase plus GPU time, so some minutes lost 1% of display slots
// and others 8%. With the vblank times known, every image can be given its own
// refresh and submitted a fixed lead before it.
//
// macVsyncStart runs a CVDisplayLink on the display under (x, y) (global
// coordinates, points); macVsyncGet returns a recent vblank time and the
// refresh period in chiaki_time_now_monotonic_us() microseconds.
bool macVsyncStart(double x, double y);
void macVsyncStop();
bool macVsyncGet(int64_t *vblank_us, int64_t *period_us);

#endif // MACVSYNC_H
