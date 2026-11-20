# Frame Generation Launch Window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A themed pre-launch window (before `QmlMainWindow` / the Vulkan device) to set frame generation on/off, quality and buffer, skippable with one press.

**Architecture:** `RunMain` in `gui/src/main.cpp` runs a `FrameGenLaunch` helper first: it probes Vulkan GPUs for `VK_NV_optical_flow`, shows `LaunchWindow.qml` in its own `QQmlApplicationEngine`, waits in a local `QEventLoop` until Continue, writes the three settings, destroys the engine, then the normal main window starts. The scene is one SVG in qrc; strings live in a QML table keyed by language.

**Tech Stack:** C++17, Qt 6.12 (Quick, Svg), libplacebo `pl_vk_inst` for the probe, Anton font (OFL).

**Spec:** `docs/superpowers/specs/2026-11-20-launch-window-design.md`

## Global Constraints

- Shown on every normal launch (`RunMain`). Not shown for the `stream` CLI command (`RunStream`) — builder's call: that path is for scripted/Steam launches.
- Settings keys unchanged: `framegen_enabled` (default true), `framegen_quality` 0..2 (default 0), `framegen_buffer_ms` 0..50 (default 8). Use the existing `Settings` getters/setters.
- Buffer choices exactly `[0, 4, 8, 12, 16, 25, 33]` ms (same as the Video settings combo).
- Button text "CONTINUE", no icon. Enter and Return also continue. Closing the window (✕) also continues — never exits the app.
- No GTA/Rockstar logo, font, name or assets. Scene is our own vector art.
- 10 languages: en, tr, de, es, fr, pt_BR, it, ru, ja, zh_CN; pick by `Qt.locale().name`, then language prefix, else English. Translations only for this window. (Spec said `qt_add_translations`; a QML string table is used instead — no build tooling for ~15 strings. Spec updated.)
- Motion: no bounce; `Qt.styleHints` / OS reduced-motion → no animation.
- Frame-gen-off code path and Vulkan device creation stay untouched.

## Review Focus

- No Vulkan / no driver at all: probe must return "not supported" without crashing, window still shows and continues.
- Non-RTX GPU: toggle locked off-looking, but the stored `framegen_enabled` value is **not** overwritten (user may move to an RTX machine).
- Laptop with iGPU + RTX: supported if *any* physical device has the extension; chip shows that device's name.
- Unknown locale (e.g. `ko_KR`): English, not empty strings.
- Small screen / 125–150 % scaling: window fully visible, fixed logical size 400×560, centered.

---

### Task 1: GPU probe + launch flow (C++)

**Files:**
- Create: `gui/include/framegenlaunch.h`, `gui/src/framegenlaunch.cpp`
- Modify: `gui/CMakeLists.txt` (add both files next to `frameinterpolator`), `gui/src/main.cpp` (`RunMain`)

**Interfaces:**
- Produces: `class FrameGenLaunch : public QObject` with Q_PROPERTYs read by QML (all CONSTANT except the three settings):
  `bool supported`, `QString gpuName`, `bool frameGenEnabled` (RW), `int quality` (RW), `int bufferMs` (RW),
  `Q_INVOKABLE void accept()`; static `bool run(Settings *settings)` (blocks, returns after Continue).
- `static bool probeOpticalFlow(QString *gpu_name)`: `pl_vk_inst_create` (no debug), enumerate physical devices via `get_proc_addr`, return true + name of the first device whose extensions include `VK_NV_OPTICAL_FLOW_EXTENSION_NAME`; else false + name of the first discrete (or first) device; empty name if no Vulkan. Destroy the instance before returning.
- QML context property name: `launch`.

- [ ] **Step 1:** Implement header/source as above. `run()` creates the engine, sets `launch`, loads `qrc:/LaunchWindow.qml`, `QEventLoop::exec()` until `accept()` (or window closed → `accept()`), then writes settings only for values the user could change (`supported == false` → leave `framegen_enabled` as stored).
- [ ] **Step 2:** In `RunMain`, call `FrameGenLaunch::run(settings)` before constructing `QmlMainWindow`.
- [ ] **Step 3:** Build: `powershell -ExecutionPolicy Bypass -File tools\build.ps1 build` → exit 0, no new errors in `tools\logs_build.txt`.
- [ ] **Step 4:** Commit `Frame generation: pre-launch settings window (probe + flow)`.

### Task 2: LaunchWindow.qml, scene, font, 10 languages

**Files:**
- Create: `gui/src/qml/LaunchWindow.qml`, `gui/res/launch/scene.svg`, `gui/res/fonts/Anton-Regular.ttf`, `gui/res/fonts/OFL.txt`
- Modify: `gui/src/qml/qml.qrc`, `gui/res/resources.qrc`

**Interfaces:**
- Consumes: `launch` from Task 1.
- Font from github.com/google/fonts `ofl/anton` (official), loaded with `FontLoader`.

- [ ] **Step 1:** `scene.svg` = the mockup's sunset (sky gradient `#1b0b3a → #5a1a73 → #ff3d8b → #ff8a3d → #ffc857`, striped sun, sea `#2b0f4f → #0d0520`, reflections, 3 palms) at viewBox 400×560.
- [ ] **Step 2:** `LaunchWindow.qml`: `Window` 400×560, frameless-free (normal title bar, title "chiaki-ng"), not resizable, centered. Layout and colors from `launch-theme-v2.html` option 1: title "FRAME / GENERATION" (Anton, 34 px, second line `#ffe27a`), GPU chip (cyan dot; red-orange dot + "not supported" text when `!launch.supported`), frosted panel with toggle / 3-segment quality / buffer slider snapping to the 7 values, gradient CONTINUE button. Window gets focus; `Keys.onReturnPressed/onEnterPressed: launch.accept()`.
- [ ] **Step 3:** String table `readonly property var strings` with keys `subtitle, quality, high, balanced, fast, buffer, bufferHint, supported, unsupported, continue, title1, title2` for the 10 languages; `tr(key)` helper falls back to `en`.
- [ ] **Step 4:** Build + deploy, run `tools\run_framegen.cmd`. Check: window shows before main window, Enter continues, settings persist across two launches, main window opens normally.
- [ ] **Step 5:** Language check: `FrameGenLaunch` exposes `QString language` (CONSTANT) = env `CHIAKI_LAUNCH_LOCALE` if set (test-only override), else `QLocale::system().name()`; QML uses it instead of `Qt.locale()`. Run with `tr_TR`, `ja_JP`, `ru_RU`, `ko_KR` (→ English) and confirm no empty labels and nothing clipped.
- [ ] **Step 6:** Commit `Frame generation: themed launch window with 10 languages`.

### Task 3: Screenshot + fork README

**Files:**
- Create: `docs/images/launch-window.png`
- Modify: `README.md` (fork section at top; upstream README kept below)

- [ ] **Step 1:** Capture the launch window (PowerShell `CopyFromScreen` of the window rect) → `docs/images/launch-window.png`.
- [ ] **Step 2:** README top section: what it does (30 → 60 fps optical-flow frame generation), requirements (NVIDIA RTX 20+ today; others planned), how to use (launch window / Settings > Video), build (VC + clang-cl, `tools\build.ps1`), screenshot, placeholder for 30 vs 60 comparison (added when a 60 fps capture exists), AGPL-3.0 note, "Not affiliated with Sony Interactive Entertainment or Rockstar Games."
- [ ] **Step 3:** Commit `Docs: fork README with launch window screenshot`. Push to `fork` only after the user approves.
