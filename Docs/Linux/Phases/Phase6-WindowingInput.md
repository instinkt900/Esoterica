# Phase 6 - Windowing and Input

**Goal:** the standalone `Engine` application runs a map on Linux.

**Deliverable:** `./Build/Linux_Release/EsotericaEngine -map data://path_to_map.map` opens a
window and renders.

**Prerequisites:** Phase 5 through bring-up step 8, with bindless verified. Swapchain present,
step 9, is finished together with this phase.

**Rough cost:** 3-4 weeks.

**Read first:** [00-Conventions.md](../00-Conventions.md),
[02-Architecture.md, Application and entry points](../02-Architecture.md#application-and-entry-points).

---

## Approach: SDL3, and lean on upstream imgui

Three of the largest platform files handle windowing and input:

| File | LOC |
|---|---|
| `Code/Base/Imgui/Platform/ImguiPlatform_Win32.cpp` | 1,198 |
| `Code/Base/Application/Platform/Application_Win32.cpp` | 727 |
| `Code/Base/Input/InputDevices/Platform/InputDevice_KeyboardMouse_Win32.cpp` | 408 |

That is about 2,300 of the port's roughly 4,300 platform lines. The real cost is much lower than
it looks, because **`ImguiPlatform_Win32.cpp` is a vendored copy of upstream Dear ImGui's
`imgui_impl_win32.cpp`**, lightly adapted. The survey confirmed it. The file contains
`ImGui_ImplWin32_Data`, `ImGui_ImplWin32_ViewportData`,
`ImGui_ImplWin32_UpdateMonitors_EnumFunc`, and the standard DPI-awareness helpers, all with
upstream's naming.

The Linux equivalent is therefore **upstream's `imgui_impl_sdl3.cpp`**, adapted the same way. It
is a port of the adaptations, not a backend written from scratch. Diff the vendored file against
the matching upstream release of `imgui_impl_win32.cpp` first, to isolate exactly what Esoterica
changed. That diff is your real worklist.

SDL3 also absorbs keyboard, mouse, gamepad, multi-monitor, DPI, and the X11-against-Wayland
question. Do not hand-write any of it.

---

## Tasks

### P6.1 - Get SDL3

This answers [open question 4](../Progress.md#open-questions). Add an `sdl3` target to
`DownloadDependencies.sh`, and add `pkg-config --libs sdl3` to the generator's dependency
mapping. Pin the version. SDL3's API was still settling recently.

This is **SDL3**, not SDL2. Dear ImGui's `imgui_impl_sdl3.cpp` targets SDL3, and SDL3's Vulkan
surface creation (`SDL_Vulkan_CreateSurface`) is what Phase 5's swapchain needs.

### P6.2 - `LinuxApplication`

**New:** `Code/Base/Application/Platform/Application_Linux.{h,cpp}`

Mirror `Win32Application`. Its virtual interface is *almost* platform-neutral already. These
carry over unchanged:

`Initialize`, `Shutdown`, `ApplicationLoop`, `ResizeMainWindow`, `OnUserExitRequest`,
`FatalError`, `OnFirstShowMainWindow`, `ProcessWindowDestructionMessage`, `ReadWindowSettings`,
`WriteWindowSettings`, `RequestApplicationExit`, `GetBorderlessTitleBarInfo`

These are Win32-typed, and need SDL3 equivalents:

| Win32 | Linux |
|---|---|
| `Win32Application( HINSTANCE, char const* name, int iconResourceID, int splashResourceID, TBitFlags<InitOptions> )` | `LinuxApplication( char const* name, ... )`. No `HINSTANCE`. Icons load from a file, not from a resource ID. |
| `virtual LRESULT WindowMessageProcessor( HWND, UINT, WPARAM, LPARAM )` | `virtual bool ProcessEvent( SDL_Event const& )` |
| `virtual void ProcessInputMessage( UINT, WPARAM, LPARAM )` | `virtual void ProcessInputEvent( SDL_Event const& )` |
| `HICON GetIcon() const` | `SDL_Surface*`, or drop it |
| `LRESULT BorderlessWindowHitTest( POINT )` | The SDL3 hit-test callback, `SDL_SetWindowHitTest` |
| `TryCreateSplashScreen` and `DestroySplashScreen`, from a `.bmp` resource | An SDL borderless window with an image loaded from disk |

Carry over the `InitOptions` flags too, `StartMinimized` and `Borderless`.

**Windows resources have no Linux equivalent.** `Code/Applications/{Editor,Engine}/Win32/`
contain `.rc`, `.ico`, `.aps`, and `SplashScreen.bmp` files. On Linux, load the icon and splash
screen from files, or drop them at first. Do not try to parse `.rc` files.

**Do not refactor `Win32Application` into a shared base class.** It is a large edit to an
upstream file for a cosmetic benefit, it conflicts on every future merge, and it saves little
duplication. The app subclasses are 124 and 127 lines.

### P6.3 - imgui platform backend

**New:** `Code/Base/Imgui/Platform/ImguiPlatform_Linux.{h,cpp}`,
`Code/Base/Imgui/Platform/ImguiX_Linux.cpp`

**Edit:** `Code/Base/Imgui/ImguiSystem.cpp:12`. Change `#if _WIN32` to
`#if _WIN32 || defined( __linux__ )`, so that Linux uses the Freetype rasterizer too.

The procedure:

1. Find which Dear ImGui version is vendored at `Code/Base/ThirdParty/imgui/`.
2. Diff `ImguiPlatform_Win32.cpp` against that version's `imgui_impl_win32.cpp`, to isolate
   Esoterica's adaptations.
3. Start from that version's `imgui_impl_sdl3.cpp`, and apply the same adaptations.

**Multi-viewport support is required.** The vendored Win32 backend implements it, through
`ImGui_ImplWin32_ViewportData`, `ImGuiPlatformMonitor` enumeration, and per-viewport DPI, and the
editor's docking UI depends on it. `imgui_impl_sdl3.cpp` supports multi-viewport, so this is
configuration rather than new code. Verify it. Do not assume it.

The renderer-side imgui backend is separate from the platform side, and it goes through the
engine's own RHI path, so Phase 5 already covers it. Confirm this rather than assuming it. Check
how `ImguiSystem.cpp` gets its render backend.

### P6.4 - Keyboard and mouse input

**New:** `Code/Base/Input/InputDevices/Platform/InputDevice_KeyboardMouse_Linux.cpp`

The Win32 version, 408 lines, uses raw input. SDL3 provides keyboard and mouse events directly.
The requirements:

- Map SDL scancodes to the engine's key enum. Use **scancodes, not keycodes**, so the mapping
  does not depend on keyboard layout. That matches how raw input behaves.
- Relative mouse motion for camera control, through `SDL_SetWindowRelativeMouseMode`.
- Mouse capture and cursor visibility, matching the Win32 semantics.
- Mouse wheel, including horizontal.
- Character input for imgui text fields, through SDL3 text input events, which handle IME and
  dead keys correctly.

Read the engine's key enum in `Code/Base/Input/`, and build the mapping table completely. A
partial table produces keys that silently do nothing, which is a frustrating class of bug to
chase later.

### P6.5 - Gamepad input

**New:** `Code/Base/Input/InputDevices/Platform/InputDevice_XboxController_Linux.cpp`

The Win32 version is only 72 lines, using XInput. SDL3's gamepad API replaces it directly, and it
supports more controllers than XInput does.

The filename says `XboxController`, which is an upstream naming choice. Keep the name, so that
the files sit as siblings, even though SDL3 handles any gamepad. Do not rename it. See
Conventions rule 3.

### P6.6 - Swapchain surface creation

This finishes Phase 5's bring-up step 9.

`RenderWindow::SetNativeWindowHandle( void* )` flows into
`RHI::SwapchainParameters::m_pNativeWindowHandle`. On Linux, pass the `SDL_Window*`, and have the
Vulkan backend call `SDL_Vulkan_CreateSurface`.

This makes `RHI_Vulkan.cpp` link against SDL3. That coupling is slightly unfortunate, but it
matches how `RHI_Direct3D12.cpp` takes an `HWND`. Accept it. The alternative is an extra
abstraction layer for one call.

An SDL3 window meant for Vulkan must be created with `SDL_WINDOW_VULKAN`. Coordinate this with
the window creation in P6.2.

### P6.7 - `EngineApplication_Linux`

**New:** `Code/Applications/Engine/Linux/EngineApplication_Linux.{h,cpp}`

Mirror `EngineApplication_Win32.{h,cpp}`, which is 124 lines. `_tWinMain` becomes
`int main( int argc, char** argv )`.

Drop the Live++ hooks. `EE_ENABLE_LPP` stays unset on Linux, so the `#if EE_ENABLE_LPP` blocks
simply do not appear in the Linux file.

### P6.8 - First light

Run a map. Expect a long tail of issues: window resize and swapchain recreation, DPI scaling,
input focus, cursor clipping, fullscreen transitions, and imgui viewport behavior under a
compositor.

Test under **both X11 and Wayland**. SDL3 abstracts them, but they behave differently in
practice, above all around window positioning, which multi-viewport imgui depends on. Wayland
does not let a client position its own windows, which can affect imgui viewports. Find out early
whether that is a problem, and record what you learn.

---

## Acceptance criteria

1. `Build/Linux_Release/EsotericaEngine` builds and links.
2. It opens a window and renders a map passed with `-map data://...`.
3. Keyboard, mouse, and gamepad input all work, for camera control and for every key the engine
   binds.
4. Window resize recreates the swapchain with no validation errors and no leaks.
5. imgui renders, and multi-viewport windows can be dragged out of the main window.
6. It runs under both X11 and Wayland, and any behavior difference is recorded.
7. DPI scaling works on a HiDPI display.
8. Shutdown is clean. No Vulkan validation errors, and no leaks from `ReportDeviceMemoryLeaks`.
9. Frame timing is in a sane range next to the same scene on Windows. This is not a benchmark. It
   only confirms that nothing is badly wrong, such as an accidental vsync-off busy loop or a
   per-frame device wait.
10. **The Windows MSBuild build still succeeds**, and the Windows `Engine` app is unchanged.
11. Every upstream file you edited is in [TouchedFiles.md](../TouchedFiles.md) with status
    `done`.
12. `git diff --stat upstream/main -- Code/Base/Imgui/ImguiSystem.cpp` shows **1 line changed**.

## Do not

- Refactor `Win32Application` into a shared base.
- Hand-write X11 or Wayland code. SDL3 owns this.
- Use SDL2.
- Parse `.rc` files, or try to load Windows resource icons.
- Rename `InputDevice_XboxController_*`, even though SDL3 handles any controller.
- Map SDL keycodes where scancodes are correct.

## Notes for the next agent

Phase 7 needs two things from this phase:

- The `LinuxApplication` interface, because `EditorApplication_Linux` and the ResourceServer both
  derive from it.
- Whether imgui multi-viewport works under Wayland, because the editor depends on it heavily.

Record in [Progress.md](../Progress.md) which imgui version you diffed, what Esoterica's
adaptations to the Win32 backend actually were, and the X11-against-Wayland findings.
