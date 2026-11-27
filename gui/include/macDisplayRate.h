#ifndef MACDISPLAYRATE_H
#define MACDISPLAYRATE_H

#include <QString>

// Frame generation shows a fixed frame rate (60 fps from a 30 fps stream) and is
// only judder-free when the display refreshes at that rate: VSync then shows
// exactly one image per refresh. At 144 Hz frames are held an uneven 2-3
// refreshes, and even at 120/240 Hz timer jitter moves frames by a refresh.
//
// macDisplayRateMatch switches the display under the given point (global
// coordinates, points) to fps Hz (or the lowest multiple of it), keeping the
// resolution, for as long as the app runs. macDisplayRateRestore switches back.
// The change is app-only: macOS also undoes it when the app quits or crashes.
// Both return a line for the log (empty when nothing happened). out_hz gets the
// display refresh in effect afterwards (0 when unknown).
QString macDisplayRateMatch(double x, double y, double fps, double *out_hz);
QString macDisplayRateRestore();

#endif // MACDISPLAYRATE_H
