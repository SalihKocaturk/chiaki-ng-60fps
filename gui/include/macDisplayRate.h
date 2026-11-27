#ifndef MACDISPLAYRATE_H
#define MACDISPLAYRATE_H

#include <QString>

// Frame generation shows a fixed frame rate (60 fps from a 30 fps stream). On a
// display whose refresh is not a multiple of it (144 Hz, 165 Hz, 75 Hz ...) each
// frame stays on screen for an uneven number of refreshes, which judders.
//
// macDisplayRateMatch switches the display under the given point (global
// coordinates, points) to a refresh that is a multiple of fps, keeping the
// resolution, for as long as the app runs. macDisplayRateRestore switches back.
// The change is app-only: macOS also undoes it when the app quits or crashes.
// Both return a line for the log (empty when nothing happened).
QString macDisplayRateMatch(double x, double y, double fps);
QString macDisplayRateRestore();

#endif // MACDISPLAYRATE_H
