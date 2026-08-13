# Phase 6 — Windowing & Input

**Goal:** the standalone `Engine` application runs a map on Linux.

**Deliverable:** `./Build/Linux_Release/EsotericaEngine -map data://path_to_map.map` opens a
window and renders.

**Prerequisites:** Phase 5 through bring-up step 8 (bindless verified). Swapchain present —
step 9 — is completed jointly with this phase.

**Rough cost:** 3–4 weeks.

**Read first:** [00-Conventions.md](../00-Conventions.md),
[02-Architecture.md § Application and entry points](../02-Architecture.md#application-and-entry-points).

---

## Approach: SDL3, and lean on upstream imgui

Three of the largest platform files are windowing and input:

| File | LOC |
|---|---|
| `Code/Base/Imgui/Platform/ImguiPlatform_Win32.cpp` | 1,198 |
| `Code/Base/Application/Platform/Application_Win32.cpp` | 727 |
| `Code/Base/Input/InputDevices/Platform/InputDevice_KeyboardMouse_Win32.cpp` | 408 |

That is ~2,300 of the port's ~4,300 platform lines — but the real cost is much lower than it
looks, because **`ImguiPlatform_Win32.cpp` is a vendored copy of upstream Dear ImGui's
`imgui_impl_win32.cpp`**, lightly adapted. The survey confirmed this: it contains
`ImGui_ImplWin32_Data`, `ImGui_ImplWin32_ViewportData`,
`ImGui_ImplWin32_UpdateMonitors_EnumFunc`, and the standard DPI-awareness helpers, all with
upstream's naming.

So the Linux equivalent is **upstream's `imgui_impl_sdl3.cpp`**, adapted the same way — a port of
the adaptations, not a from-scratch backend. Diff the vendored file against the matching upstream
release of `imgui_impl_win32.cpp` first, to isolate exactly what Esoterica changed. That diff is
your actual worklist.

SDL3 also absorbs keyboard, mouse, gamepad, multi-monitor, DPI, and the X11-vs-Wayland question.
Do not hand-roll any of it.

---

## Tasks

### P6.1 — Acquire SDL3

Resolves [open question 4](../Progress.md#open-questions). Extend
`DownloadDependencies.sh` with an `sdl3` target; add `pkg-config --libs sdl3` to the generator's
dependency mapping. Pin the version — SDL3's API was still settling recently.

Note this is **SDL3**, not SDL2. Dear ImGui's `imgui_impl_sdl3.cpp` targets SDL3, and SDL3's
Vulkan surface creation (`SDL_Vulkan_CreateSurface`) is what Phase 5's swapchain needs.

### P6.2 — `LinuxApplication`

**New:** `Code/Base/Application/Platform/Application_Linux.{h,cpp}`

Mirror `Win32Application`. Its virtual interface is *almost* platform-neutral already — these
carry over unchanged:

`Initialize`, `Shutdown`, `ApplicationLoop`, `ResizeMainWindow`, `OnUserExitRequest`,
`FatalError`, `OnFirstShowMainWindow`, `ProcessWindowDestructionMessage`,
`ReadWindowSettings`, `WriteWindowSettings`, `RequestApplicationExit`,
`GetBorderlessTitleBarInfo`

These are Win32-typed and need SDL3 equivalents:

| Win32 | Linux |
|---|---|
| `Win32Application( HINSTANCE, char const* name, int iconResourceID, int splashResourceID, TBitFlags<InitOptions> )` | `LinuxApplication( char const* name, ... )` — no `HINSTANCE`; icons load from a file, not a resource ID |
| `virtual LRESULT WindowMessageProcessor( HWND, UINT, WPARAM, LPARAM )` | `virtual bool ProcessEvent( SDL_Event const& )` |
| `virtual void ProcessInputMessage( UINT, WPARAM, LPARAM )` | `virtual void ProcessInputEvent( SDL_Event const& )` |
| `HICON GetIcon() const` | `SDL_Surface*` or drop |
| `LRESULT BorderlessWindowHitTest( POINT )` | SDL3 hit-test callback (`SDL_SetWindowHitTest`) |
| `TryCreateSplashScreen` / `DestroySplashScreen` (from a `.bmp` resource) | An SDL borderless window with an image loaded from disk |

Also carry over the `InitOptions` flags (`StartMinimized`, `Borderless`).

**Windows resources have no Linux equivalent.** `Code/Applications/{Editor,Engine}/Win32/` contain
`.rc`, `.ico`, `.aps`, and `SplashScreen.bmp`. The icon and splash screen must be loaded from
files on Linux (or dropped initially). Do not try to parse `.rc` files.

**Do not refactor `Win32Application` into a shared base class.** It is a large edit to an upstream
file for cosmetic benefit, it conflicts on every future merge, and the duplication it saves is
small — the app subclasses are 124 and 127 lines.

### P6.3 — imgui platform backend

**New:** `Code/Base/Imgui/Platform/ImguiPlatform_Linux.{h,cpp}`,
`Code/Base/Imgui/Platform/ImguiX_Linux.cpp`
**Edit:** `Code/Base/Imgui/ImguiSystem.cpp:12` — change `#if _WIN32` to
`#if _WIN32 || defined( __linux__ )` so the Freetype rasteriser is used on Linux too.

Procedure:

1. Identify which Dear ImGui version is vendored at `Code/Base/ThirdParty/imgui/`.
2. Diff `ImguiPlatform_Win32.cpp` against that version's `imgui_impl_win32.cpp` to isolate
   Esoterica's adaptations.
3. Start from that version's `imgui_impl_sdl3.cpp` and apply the same adaptations.

**Multi-viewport support is required** — the vendored Win32 backend implements it
(`ImGui_ImplWin32_ViewportData`, `ImGuiPlatformMonitor` enumeration, per-viewport DPI), and the
editor's docking UI depends on it. `imgui_impl_sdl3.cpp` supports multi-viewport, so this is
configuration rather than new code, but verify it rather than assuming.

Note the renderer-side imgui backend is separate from the platform side and goes through the
engine's own RHI path, so Phase 5 already covers it. Confirm this rather than assuming; check how
`ImguiSystem.cpp` obtains its render backend.

### P6.4 — Keyboard and mouse input

**New:** `Code/Base/Input/InputDevices/Platform/InputDevice_KeyboardMouse_Linux.cpp`

The Win32 version (408 lines) uses raw input. SDL3 provides keyboard and mouse events directly.
Requirements:

- Map SDL scancodes to the engine's key enum. Use **scancodes, not keycodes**, so the mapping is
  keyboard-layout-independent — matching raw input's behaviour.
- Relative mouse motion for camera control (`SDL_SetWindowRelativeMouseMode`).
- Mouse capture and cursor visibility, matching the Win32 semantics.
- Mouse wheel, including horizontal.
- Character input for imgui text fields — SDL3 text input events, which correctly handle IME and
  dead keys.

Read the engine's key enum in `Code/Base/Input/` and build the mapping table exhaustively. A
partial table produces keys that silently do nothing, which is a frustrating class of bug to
chase later.

### P6.5 — Gamepad input

**New:** `Code/Base/Input/InputDevices/Platform/InputDevice_XboxController_Linux.cpp`

Only 72 lines on Win32 (XInput). SDL3's gamepad API is a straightforward replacement and supports
more controllers than XInput does.

Note the filename says `XboxController`, which is an upstream naming choice. Keep the name so the
files sit as siblings, even though SDL3 handles any gamepad — do not rename (Conventions rule 3).

### P6.6 — Swapchain surface creation

Completes Phase 5's bring-up step 9.

`RenderWindow::SetNativeWindowHandle( void* )` flows into
`RHI::SwapchainParameters::m_pNativeWindowHandle`. On Linux, pass the `SDL_Window*` and have the
Vulkan backend call `SDL_Vulkan_CreateSurface`.

This means `RHI_Vulkan.cpp` links against SDL3, which is a slightly unfortunate coupling but
matches how `RHI_Direct3D12.cpp` takes an `HWND`. Accept it; the alternative is an extra
abstraction layer for one call.

SDL3 windows intended for Vulkan must be created with `SDL_WINDOW_VULKAN`. Coordinate this with
P6.2's window creation.

### P6.7 — `EngineApplication_Linux`

**New:** `Code/Applications/Engine/Linux/EngineApplication_Linux.{h,cpp}`

Mirror `EngineApplication_Win32.{h,cpp}` (124 lines). `_tWinMain` becomes
`int main( int argc, char** argv )`.

Strip the Live++ hooks — `EE_ENABLE_LPP` stays unset on Linux, so the `#if EE_ENABLE_LPP` blocks
simply do not appear in the Linux file.

### P6.8 — First light

Run a map. Expect a long tail of issues: window resize and swapchain recreation, DPI scaling,
input focus, cursor clipping, fullscreen transitions, and imgui viewport behaviour under a
compositor.

Test under **both X11 and Wayland**. SDL3 abstracts them, but behaviour differs in practice —
particularly around window positioning, which multi-viewport imgui depends on. Wayland does not
let clients position their own windows, which can affect imgui viewports; find out early whether
this is a problem and record what you learn.

---

## Acceptance criteria

1. `Build/Linux_Release/EsotericaEngine` builds and links.
2. It opens a window and renders a map passed via `-map data://...`.
3. Keyboard, mouse, and gamepad input all work — camera control, and every key the engine binds.
4. Window resize recreates the swapchain without validation errors or leaks.
5. imgui renders, and multi-viewport windows can be dragged out of the main window.
6. Runs under both X11 and Wayland, with any behavioural differences recorded.
7. DPI scaling works on a HiDPI display.
8. Clean shutdown — no Vulkan validation errors, no leaks reported by
   `ReportDeviceMemoryLeaks`.
9. Frame timing is in a sane range compared to the same scene on Windows. Not a benchmark — just
   confirmation that nothing is pathologically wrong (e.g. accidental vsync-off busy-looping, or
   a per-frame device wait).
10. **The Windows MSBuild build still succeeds**, and the Windows `Engine` app is unchanged.
11. Every upstream file edited is in [TouchedFiles.md](../TouchedFiles.md) with status `done`.
12. `git diff --stat upstream/main -- Code/Base/Imgui/ImguiSystem.cpp` shows **1 line changed**.

## Do not

- Refactor `Win32Application` into a shared base.
- Hand-roll X11 or Wayland code. SDL3 owns this.
- Use SDL2.
- Parse `.rc` files or try to load Windows resource icons.
- Rename `InputDevice_XboxController_*` despite SDL3 being controller-agnostic.
- Map SDL keycodes where scancodes are correct.

## Notes for the next agent

Phase 7 needs from this phase:
- The `LinuxApplication` interface, since `EditorApplication_Linux` and the ResourceServer both
  derive from it.
- Whether imgui multi-viewport works under Wayland, since the editor depends on it heavily.

Record in [Progress.md](../Progress.md) the imgui version diffed, what Esoterica's adaptations
to the Win32 backend actually were, and the X11-vs-Wayland findings.
