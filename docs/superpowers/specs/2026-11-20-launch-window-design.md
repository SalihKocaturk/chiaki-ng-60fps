# Frame Generation launch window — design

## Intent
Every launch shows a small window *before* the main window and the Vulkan device
exist, so frame generation settings are picked up front and never need a restart.
One press (Continue / Enter) skips it. Look: a GTA 6–inspired beach sunset, built
from our own vector art (no game logo, font, name or assets).

## Goals
- Shown on every launch, before `QmlMainWindow` is created (`gui/src/main.cpp`).
- Controls: Frame Generation on/off, Quality (High / Balanced / Fast), Buffer (0–50 ms, default 8).
- GPU chip: card name + supported / not supported. Unsupported (no `VK_NV_optical_flow`):
  toggle locked, "This graphics card is not supported" shown, Continue still works.
- Settings saved to the existing keys (`framegen_enabled`, `framegen_quality`, `framegen_buffer_ms`).
- Continue (button or Enter) closes it and starts the app normally. No icon on the button.
- 10 languages, auto-picked from the system language, English fallback.

## Non-goals / anti-goals
- No restart in this flow. No change to the frame-gen-off code path.
- No Rockstar/GTA logo, font, name or screenshots anywhere in the app or repo art.
- Not a full app translation (see open question).

## Approaches considered
- **A. Separate pre-window before the Vulkan device (chosen by user):** no restart, rule kept.
- B. Live-switchable frame generation: device always built with optical flow; bends the "off = original" rule.
- C. Screen inside the main window + restart on change: least work, restarts on every change.

## Look (theme 1, "beach sunset", layout A)
- 380 px wide card, rounded 18 px. Background: night-purple → magenta → orange → gold sky,
  striped retro sun with sea reflection, palm silhouettes (QML Shape / SVG, scales, no bitmaps).
- Title "FRAME / GENERATION" in Anton (OFL, bundled). Controls on a frosted dark glass panel.
- Primary button: pink→orange→gold gradient, "CONTINUE", no icon.
- Motion: critically damped, no bounce; respects reduced motion (apple-design SKILL.md).

## Languages
English, Türkçe, Deutsch, Español, Français, Português (Brasil), Italiano, Русский, 日本語, 简体中文.
Only this window is translated. A string table in the QML file, picked from `QLocale::system()`
(English fallback) — no Qt Linguist tooling needed for ~15 strings.

## Repo (after the window is built)
- README for the fork: what it does, requirements (NVIDIA RTX 20+ for now), how to build,
  launch-window screenshot, 30 vs 60 comparison (slowed side-by-side GIF; GIF cannot hold true
  60 fps, max ~50), AGPL notice, "not affiliated with Sony or Rockstar".
- Comparison needs a 60 fps capture from the user (Game Bar, 60 fps).

## Left to the builder
Exact spacing/colors within the mockup, translation wording, how the GPU is probed before the
main window (temporary Vulkan instance, enumerate devices, check the extension).
