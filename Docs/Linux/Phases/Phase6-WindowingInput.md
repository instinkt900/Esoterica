# Phase 6 - Windowing and Input

**Goal:** the standalone `Engine` application runs a map on Linux.

**Deliverable:** `./Build/Linux_Release/Esoterica.Applications.Engine -map data://path_to_map.map -packaged`
opens a window and renders. The binary is named after its project, like the Reflector and the
ResourceCompiler, and `-packaged` is needed until the ResourceServer builds in Phase 7.

> ## Status: P6.1 to P6.8 are written. **The engine reaches its frame loop and stops at
> `CmdExecuteIndirect`.**
>
> Open question 8 is answered, so every shader and every pipeline now builds. The one thing
> between this port and a drawn frame is
> [P5.17](Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change---scheduled-not-started).
> Four hardware gaps remain, and the driver tolerates all of them with validation off. Read the
> P6.x and open question 8 entries in [Progress.md](../Progress.md) first; they carry every
> measurement and every decision this phase made.

**Prerequisites:** Phase 5, all sixteen groups, which are written and merged. **This phase is
where the Vulkan backend ran for the first time**, and doing so found four real defects in it.
Expect more. Read the P5.x entries in [Progress.md](../Progress.md) too; each one ends with a
"Not verified" list.

**A rendered frame still needs [P5.17](Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change---scheduled-not-started),
which is deliberately scheduled after this phase.** Every engine render pass draws through
`CmdExecuteIndirect`, and `CmdExecuteIndirect` refuses the engine's command signatures at the
line. The window, the input, the swapchain and imgui all come up without it. **Geometry does
not.** Read acceptance criterion 2 with that in mind.

**[Open question 8](../Progress.md#open-questions) is answered and out of the way.** The
engine's `Buffer<uint64_t>` had no Vulkan spelling; the shaders now read `Buffer<uint2>` and
pack. No C++ changed, because the RHI already created every one of those buffers as `RG32_UInt`.
P5.17 is now the only thing left in front of a drawn frame.

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
is a port of the adaptations, not a backend written from scratch.

**Correction from P6.3: there is no matching upstream release to diff against.**
`Code/Base/ThirdParty/imgui/imgui.cpp` is byte-identical to `v1.92.9b-docking`, and
`ImguiPlatform_Win32.cpp` was last synced from a backend of roughly 1.89.1 in 2022. The Linux
backend is built from `v1.92.9b-docking`'s `imgui_impl_sdl3.cpp`, which matches the core, with
Esoterica's *adaptations* ported by hand. See the P6.3 entry in [Progress.md](../Progress.md);
it lists every one of them.

SDL3 also absorbs keyboard, mouse, gamepad, multi-monitor, DPI, and the X11-against-Wayland
question. Do not hand-write any of it.

---

## Tasks

### P6.1 - Get SDL3 — **done**

`./DownloadDependencies.sh sdl3` builds `release-3.4.14` from source into `External/SDL3/`, and
`Esoterica.Base` links `-lSDL3`.

**Open question 4 is answered: always build it.** Ubuntu 24.04 LTS, the development target,
packages no SDL3 at all. **The sheet therefore points at `External/`, not at
`pkg-config --libs sdl3`** as this task originally planned: there is no `sdl3.pc` to find. X11
and Wayland are forced on in the CMake configure, because SDL silently drops a video backend
whose headers are missing.

This is **SDL3**, not SDL2. Dear ImGui's `imgui_impl_sdl3.cpp` targets SDL3, and SDL3's Vulkan
surface creation (`SDL_Vulkan_CreateSurface`) is what Phase 5's swapchain needs.

### P6.2 - `LinuxApplication` — **done**

**New:** `Code/Base/Application/Platform/Application_Linux.{h,cpp}`

**One difference matters to anyone touching `ProcessEvent`.** `Win32Application` returns early
when imgui's handler returns non-zero; `LinuxApplication` calls imgui and **ignores the answer**.
`imgui_impl_sdl3.cpp` returns `true` for every event it recognises, including
`SDL_EVENT_WINDOW_CLOSE_REQUESTED`, so an early return swallows the application's own exit.

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

### P6.3 - imgui platform backend — **done**

**Multi-viewport is verified, not assumed:** three imgui windows became three live SDL windows.
**But it will not work under Wayland at all** - `ImGui_ImplSDL3_Init` sets
`ImGuiBackendFlags_PlatformHasViewports` only for video drivers on its global-mouse white list,
and `wayland` is not on it. Phase 7 needs to know. imgui also gates `ViewportsEnable` on *both*
backend halves, so a missing `ImGuiBackendFlags_RendererHasViewports` silently merges every
window into the main one.

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

### P6.4 - Keyboard and mouse input — **done**

**105 scancodes map to 105 distinct `InputID`s, one to one**, checked by feeding every scancode
through the device. **`SDL_SetWindowRelativeMouseMode` is deliberately not called**, though this
task asks for it: enabling it on any button press would hide and warp the cursor and break every
imgui drag, and the device cannot tell a camera drag from a slider drag. The cost is that mouse
deltas stop at the screen edge where raw input does not. **P6.8 should decide it against a live
camera.**

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

### P6.5 - Gamepad input — **done**

**SDL's stick Y is inverted relative to XInput** and is negated in the device; without that every
controller would be inverted on Linux only. Verified end to end with an SDL virtual joystick.

**New:** `Code/Base/Input/InputDevices/Platform/InputDevice_XBoxController_Linux.cpp` - note the
capital B, which matches the shared header rather than the Win32 sibling. Upstream is
inconsistent; Conventions rule 3 says not to rename either.

The Win32 version is only 72 lines, using XInput. SDL3's gamepad API replaces it directly, and it
supports more controllers than XInput does.

The filename says `XboxController`, which is an upstream naming choice. Keep the name, so that
the files sit as siblings, even though SDL3 handles any gamepad. Do not rename it. See
Conventions rule 3.

### P6.6 - Swapchain surface creation — **done**

**`m_pNativeWindowHandle` is an `SDL_Window*` on Linux, and `RHI_Vulkan.cpp` makes the surface
from it.** That revises the first half of P5.3's answer, which said the application would create
the surface and hand it over. There is nowhere for the application to do that:
`EngineModule::InitializeModule` calls `RenderSystem::Initialize`, which creates the `VkInstance`,
and `SetNativeWindowHandle` three lines later, with nothing the application owns in between.

`CreateSwapchain` calls `Platform::Linux::CreateVulkanSurface`, in `PlatformUtils_Linux.cpp` -
**the one file in the engine that knows both SDL3 and Vulkan**. The handles cross as `void*`, so
`Base/Render` still includes no window system header, which is what P5.3 actually required. See
the 2026-08-30 decision in [Progress.md](../Progress.md).

**The application still drives swapchain recreation, not the RHI.** `Engine.cpp:754` and
`ImguiRenderer.cpp:91` compare the window size against `GetSwapchainSize()` and call
`Window::ResizeSwapchain`, each waiting the graphics queue idle first. `AcquireNextImage` and
`QueuePresent` tolerate `VK_SUBOPTIMAL_KHR` and `VK_ERROR_OUT_OF_DATE_KHR`.

**The swapchain image count did fail first, exactly as predicted.** The Intel UHD 620 and
llvmpipe both report a `minImageCount` of 3. `RHI::MaxPendingFrames` is now 3 on Linux through a
four-line `#if defined( __linux__ )` branch that leaves Windows bit for bit unchanged. It was
escalated, approved and registered in [TouchedFiles.md](../TouchedFiles.md).

**Two more defects came out of running it**, both fixed in `RHI_Vulkan.cpp`: no surface on this
machine offers an RGBA format, only BGRA; and `Window::DestroySwapchain` destroys command pools
before command buffers, which Direct3D 12 allows and Vulkan does not.

### P6.7 - `EngineApplication_Linux` — **done**

**New:** `Code/Applications/Engine/Linux/EngineApplication_Linux.{h,cpp}`

The binary builds, links and starts. **Two build system defects came out of running it**, both
in the generator: the `External/` rpath was repository relative, and shared libraries had no
soname, so nothing ran outside the repository root. Both are fixed.

**A third defect was Phase 5's:** `CreateContext` never enabled the 16-bit shader feature bits,
which the engine's shaders need. Direct3D 12 has no equivalent step, so P5.1 had nothing to
mirror.

**Run it with `-packaged`** until the ResourceServer builds in Phase 7. Without it the engine
uses the network resource provider and tries to start `EsotericaResourceServer.exe`.

### P6.8 - First light — **done as far as this machine allows, and the goal is not met**

**Read the P6.8 entry in [Progress.md](../Progress.md) before running anything.** It carries the
command line, the measurements and the two things that stop the engine.

#### Run it with validation on

```bash
printf '[Render:RHI]\nEnable_Host_Validation = true\n' > Build/Linux_Release/Esoterica.ini

VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true \
  ./Build/Linux_Release/Esoterica.Applications.Engine \
  -map data://demo/render/pbr/pbrdemo.map -packaged
```

**The stale SPIRV-Tools is no longer a reason to lose validation.**
`VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true` turns off only the layer's bundled
spirv-val, which is where the bug lives, and leaves every other check on. Ubuntu 24.04's
`vulkan-validationlayers 1.4.309.0` needs no replacing. `VK_LAYER_MESSAGE_ID_FILTER` takes a
comma separated VUID list, which is how several walls were surveyed in one run.

#### What P6.8 fixed

- **The `VK_ERROR_UNKNOWN` is explained**, and it was not P5.7's. Mesa's `spirv_to_nir` refuses
  the 64-bit sampled image DXC emits for `Buffer<uint64_t>`. It is **open question 8**.
- **Five feature defects in `CreateContext`**, the same class as P6.7's 16-bit finding:
  `storageInputOutput16`, `shaderInt64`, `shaderSubgroupExtendedTypes`,
  `shaderDemoteToHelperInvocation` and the two 64-bit atomic bits. Two optional extensions are
  now asked for as well.
- **Startup survives a device without mesh shaders.** `CreateShader` skips a Task or Mesh module,
  `CreatePipeline` returns an empty mesh pipeline, and `CmdSetPipeline` is where the halt moved.
  `Shaders::Initialize` now runs to the end instead of stopping at shader 14 of 28.

#### What is left, and what it needs

**Open question 8 first.** Nothing in the frame runs until `Buffer<uint64_t>` has a Vulkan
spelling. It sits in front of P5.17, not behind it.

**Then a machine with a current GPU.** Four gaps are hardware, and none of the three GPUs here
clears them: `VK_KHR_fragment_shader_barycentric`, `shaderSharedInt64Atomics`,
`storageInputOutput16` and `VK_EXT_mesh_shader`. The MX250 is refused earlier still, for
`VK_EXT_mutable_descriptor_type`. The table in the P6.8 entry has the detail.

**Then the long tail this task was always about.** Window resize and swapchain recreation, DPI
scaling, input focus, cursor clipping, fullscreen transitions, and imgui viewport behaviour under
a compositor. Also settle `SDL_SetWindowRelativeMouseMode` against a live camera; see P6.4.

**Test under both X11 and Wayland.** This machine runs **i3 on X11** and has no Wayland
compositor, so nothing so far says how Wayland behaves. Two things are known in advance:
imgui viewports **will not be enabled at all** under Wayland (see P6.3), and i3 ignores
`SDL_SetWindowPosition` on the main window while honouring it on the borderless viewport windows.
A tiling window manager also makes acceptance criterion 7 and the client-driven half of criterion
4 untestable here; a floating window manager or a second session is needed for those.

---

## Acceptance criteria

Status as of P6.8, which is the end of the phase's work on this machine. **Only criterion 1 and
the bookkeeping ones are met.** P6.8 got the engine further without moving any of the rest: it
now creates every shader in the engine instead of stopping at the fourteenth, and still reaches
no frame loop.

1. **Met.** `Build/Linux_Release/Esoterica.Applications.Engine` builds and links.
2. **Not met.** It opens a window but does not render a map. **This one is split.** The
   window, the swapchain, the frame loop and imgui are this phase's and are done. **Geometry
   needs [P5.17](Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change---scheduled-not-started)
   and [open question 8](../Progress.md#open-questions)**, and mesh shader hardware, which this
   machine does not have. Say which half you met.
3. **Half met.** Keyboard, mouse and gamepad are implemented and each is tested at the device
   level. "Works for camera control" needs a running engine.
4. **Met for the RHI, not for the application.** Resize recreates the swapchain with no
   validation errors and no leaks, proved by a scratch application. The engine has not reached a
   frame loop yet, and the client-driven half is untestable under a tiling window manager.
5. **Not met.** imgui multi-viewport is proved on the platform side - three imgui windows became
   three live SDL windows - but nothing has rendered imgui through the engine's own renderer.
6. **Not met.** X11 only so far, under i3. **imgui viewports will not be enabled under Wayland
   at all**; see P6.3.
7. **Not met and not testable here.** A tiling window manager ignores client sizing, and this
   machine has no HiDPI display.
8. **Met for the RHI path.** Shutdown is clean with validation on. `ReportDeviceMemoryLeaks` has
   not been exercised through the engine.
9. **Not met.** No frame loop in the engine yet.
10. **Met throughout.** One upstream file is edited in this whole phase,
    `Code/Base/Render/RHI.h:31`, and it adds a Linux-only branch that leaves the Windows value
    verbatim. `Code/Base/Imgui/ImguiSystem.cpp:12` is the only other, and it is criterion 12.
11. **Met.** Both edited upstream files are in [TouchedFiles.md](../TouchedFiles.md) with status
    `done`.
12. **Met.** `git diff --stat upstream/main -- Code/Base/Imgui/ImguiSystem.cpp` shows 1 line
    changed.

## Do not

- Refactor `Win32Application` into a shared base.
- Hand-write X11 or Wayland code. SDL3 owns this.
- Use SDL2.
- Parse `.rc` files, or try to load Windows resource icons.
- Rename `InputDevice_XboxController_*`, even though SDL3 handles any controller.
- Map SDL keycodes where scancodes are correct.

## Notes for the next agent

Phase 7 needs three things from this phase:

- **The `LinuxApplication` interface**, because `EditorApplication_Linux` and the ResourceServer
  both derive from it. It is written and its shape is in the P6.2 entry.
- **imgui multi-viewport does not work under Wayland.** `ImGui_ImplSDL3_Init` enables it only for
  video drivers on its global-mouse white list, and `wayland` is not on it. The editor's docking
  UI depends on viewports. This is not a port defect and not something the port can fix; it is
  upstream imgui's own gate.
- **`ImguiPlatform_Linux.cpp` keeps upstream's formatting in its vendored region**, deliberately,
  so it can be re-synced with a `diff`. That breaks Conventions rule 8 and a banner in the file
  says why. Do not reformat it, and do not copy the pattern to files that are not vendored.

Phase 7 also inherits **the way to run with validation on**, which P6.8 found:
`VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true`, plus a hand-written `[Render:RHI]` section
in the configuration's `Esoterica.ini`. Both are in the P6.8 entry.

Everything this phase learned is in the P6.x entries in [Progress.md](../Progress.md), with the
measurements. The short version of what is worth knowing before touching any of it:

- `Platform::SetMainWindowHandle` holds an `SDL_Window*`. `RHI_Vulkan.cpp` makes the
  `VkSurfaceKHR` from it through `Platform::Linux::CreateVulkanSurface`.
- An `SDL_Event` reaches the input devices **by pointer**, in `GenericMessage::m_data0`. Nothing
  may queue that message for later.
- `LinuxApplication::ProcessEvent` ignores what imgui's handler returns. Read P6.2 before
  changing it.
- The vendored `ImguiPlatform_Win32.cpp` is about three years behind the imgui core beside it.
  Do not treat it as a reference for current imgui behaviour.
