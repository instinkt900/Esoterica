# Phase 7 - Editor and Tools

**Goal:** the full editor runs on Linux.

**Deliverable:** `./Build/Linux_Release/Esoterica.Applications.Editor` launches, the Resource
Server runs, and resource hot-reloading works end to end. **The binaries are named after their
projects on Linux**, as the Reflector, the ResourceCompiler and now the Engine are; this document
originally wrote `EsotericaEditor`.

**Prerequisites: Phase 6 is written, and it does not meet its goal.** Read the block below before
anything else. **Phase 7 does not depend on the part that is missing**, which is why it is the
work to do next.

**Rough cost:** 3-4 weeks.

**Read first:** [00-Conventions.md](../00-Conventions.md),
[TouchedFiles.md](../TouchedFiles.md).

---

> ## Start here
>
> ```bash
> python3 Code/Scripts/NinjaGen/NinjaGen.py
> ninja -f Build/Linux/Esoterica.ninja -k 0
> ```
>
> **Three translation units fail, and nothing else does.** That is the whole of Phase 7's build
> problem as of 2026-08-31:
>
> | File | Error | Task |
> |---|---|---|
> | `ResourceServerApplication.h:13` | `'shellapi.h' file not found` | P7.3 |
> | `ResourceServerUI.cpp:799` and `:811` | `no member named 'Win32' in namespace 'EE::Platform'` | P7.3, P7.4 |
> | `EditorUI.h:141`, `:157`, `:189` | `member access into incomplete type 'EE::EditorTool'` | P7.0 |
>
> The last one is **not** an editor problem. `EditorUI.h` uses `EE::EditorTool` in a template and
> never includes `EngineTools/Core/EditorTool.h`; MSVC supplies it transitively and clang does
> not. It is the "Missing includes that MSVC supplies transitively" category that
> [TouchedFiles.md](../TouchedFiles.md) already has a section for. **Fix it first** - it is one
> line and it unblocks the editor's own build.
>
> To run anything, see the "Start here" block in [Progress.md](../Progress.md). Two things there
> are not obvious and each cost a session: host validation has to be switched on by hand in
> `Esoterica.ini`, and `VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true` is what makes the
> Ubuntu 24.04 validation layers usable.

> ## What Phase 6 hands you, and how it shapes this phase
>
> ### The editor will not render on this machine, and that is not your bug
>
> **No GPU in the development machine can run the engine's geometry path.** The engine's whole
> geometry path is mesh shaders; neither real GPU here has `VK_EXT_mesh_shader`, the software
> rasteriser crashes compiling the shaders, and the NVIDIA part is refused for a missing
> extension. The RHI **drops every mesh draw** on such a device and says so once in the log, so a
> frame completes without its geometry.
>
> **The engine records and submits a complete frame with zero validation errors, and then the GPU
> hangs executing it.** That hang is unresolved. Some part of it is likely an artefact of the
> dropped mesh draws rather than a real defect, and **the two cannot be told apart without mesh
> shader hardware.**
>
> **So do not chase rendering on this machine.** Phase 7 is compile, link and tools work, and
> almost none of it needs a working frame. If the editor comes up with a blank or broken
> viewport, that is expected and already explained. Record it and move on. See the image layouts
> entry in [Progress.md](../Progress.md).
>
> ### What is genuinely ready
>
> **`LinuxApplication` is written and its interface is settled.** `EditorApplication_Linux` and
> the ResourceServer both derive from it. Read the P6.2 entry before you subclass it; in
> particular, `ProcessEvent` deliberately ignores what imgui's handler returns, and the window
> size it reports is in pixels while the position is in logical coordinates.
>
> **The imgui platform backend works, with multi-viewport verified on X11** - three imgui windows
> became three live SDL windows. Nothing has yet rendered imgui through the engine's own
> renderer.
>
> **imgui multi-viewport does not work under Wayland, and cannot be made to.**
> `ImGui_ImplSDL3_Init` enables `ImGuiBackendFlags_PlatformHasViewports` only for video drivers
> on its global-mouse white list, and `wayland` is not on it. **The editor's docking UI depends
> on viewports**, so an editor session on Wayland gets everything merged into one window. That is
> upstream imgui's own gate, not a port defect. This machine runs i3 on X11 and has no Wayland
> compositor, so Wayland is untested either way.
>
> **All four configurations link.** Debug, Release and Shipping all produce an engine binary;
> Shipping only started to on 2026-08-31.
>
> ### The ResourceServer is what the engine is waiting on too
>
> Until it builds, the engine has to be run with `-packaged`, which reads
> `Build/Linux_<configuration>/CompiledData` directly instead of going through the network
> resource provider. **That makes P7.3 the highest-value task in this phase**, and a reasonable
> place to start once the one-line `EditorUI.h` fix is in.
>
> Note also that `CompiledData` exists for `Linux_Release` only. A Shipping or Debug run has no
> data and fails at initialisation.

---

## What is actually left

Less than the phase's position in the list suggests, and **measured rather than estimated**: a
whole-tree `ninja -k 0` on 2026-08-31 failed in **three translation units and no others**. The
substance of the editor, `Code/Applications/Editor/EditorUI.cpp` and all of `Code/EngineTools/`,
is platform-neutral and has compiled since Phase 3. `EditorApplication_Win32.cpp` is only 127
lines, because the real logic lives in `EditorUI`.

What remains:

0. **One missing include in `EditorUI.h`.** A few minutes, and the editor's own build is then
   clean. See P7.0.
1. The Resource Server, a Win32 GUI app that spawns worker processes. **Start here after P7.0**:
   it is what lets the engine and the editor stop running with `-packaged`.
2. Native file dialogs, `SystemDialogs.cpp`, 552 lines of COM. This is the biggest *new* code.
3. `EditorApplication_Linux`, which is thin and mirrors the Phase 6 engine app.
4. The `OpenInExplorer` call sites. They have resolved since Phase 1, so this is about checking
   that they behave.

**Two of these do not need a window at all**, let alone a working frame: P7.0 and the compiling
half of P7.3. That is most of the value in this phase, and it is all reachable on this machine.

---

## Tasks

### P7.0 - The `EditorUI.h` include - **do this first**

`Code/Applications/Editor/EditorUI.h` uses `EE::EditorTool` in the `IsToolOpen` and
`GetToolOfType` templates and never includes `EngineTools/Core/EditorTool.h`. MSVC supplies it
transitively through another header; clang does not, so all three uses fail with `member access
into incomplete type`.

**One include, in the existing block.** It is the "Missing includes that MSVC supplies
transitively" category, which [TouchedFiles.md](../TouchedFiles.md) already has a section and a
precedent for - register it there in the same commit. Windows is unaffected: the header was
already being included, just not by name.

This is the only thing between here and a clean editor compile, so it is worth doing on its own
before the phase proper starts.

### P7.1 - `EditorApplication_Linux`

**New:** `Code/Applications/Editor/Linux/EditorApplication_Linux.{h,cpp}`

Mirror `EditorApplication_Win32.{h,cpp}`. The class derives from `LinuxApplication` (Phase 6,
P6.2) and overrides `Initialize`, `Shutdown`, `FatalError`, `GetBorderlessTitleBarInfo`,
`ResizeMainWindow`, `ProcessInputEvent`, and `ApplicationLoop`.

`EditorEngine`, declared in the same header, is platform-neutral. Copy it across with its shape
unchanged. Drop the `EE_ENABLE_LPP` overrides.

The editor uses a **borderless window with a custom title bar**, through
`GetBorderlessTitleBarInfo` and `InitOptions::Borderless`. That is the trickiest part. SDL3's
`SDL_SetWindowHitTest` gives the equivalent of Win32's `WM_NCHITTEST` handling. Expect to iterate
here. Window dragging, edge resizing, and maximize and restore all need to feel right, and this
is the most visible part of the editor's UX.

### P7.2 - `SystemDialogs_Linux.cpp`

**New:** `Code/EngineTools/Core/SystemDialogs_Linux.cpp`

**No edit to `Code/EngineTools/Core/SystemDialogs.cpp`.** Phase 3 excluded it in
`Code/Scripts/NinjaGen/Exclusions.txt` and wrote the halting sibling this task replaces. The
upstream file stays byte-identical.

The Windows implementation uses COM `IFileDialog` with `COMDLG_FILTERSPEC`. Read
`Code/EngineTools/Core/SystemDialogs.h` for the interface to implement: `FileDialog::Result`,
`FileDialog::ExtensionFilter`, and the open, save, and folder entry points.

The options, best first:

1. **XDG Desktop Portal** over D-Bus (`org.freedesktop.portal.FileChooser`). This is the correct
   modern answer. It works under Flatpak, and it uses the desktop's native dialog. But it is
   asynchronous, and D-Bus adds a dependency.
2. **`pfd` (portable-file-dialogs)**, which the project's README already lists as a third-party
   dependency. It calls out to `zenity`, `kdialog`, or `osascript`. It is much simpler,
   synchronous, and good enough for a tool. **Start here.**

Check whether `pfd` is vendored anywhere in the tree. The README lists it, but the survey did not
find it under `Code/**/ThirdParty/`. If it is missing, fetch it into `External/`, per Conventions
rule 5, not into `Code/`.

**`ExtensionFilter` holds a `WString m_filter`**, a wide string sized for the Windows
double-NUL-terminated filter format. On Linux you must translate that to the target dialog's
format. Prefer reading `m_extension` and `m_displayText` directly over parsing `m_filter`, if the
interface allows it. If the header genuinely forces you to consume `m_filter`, that is a
shared-header concern. **Escalate** instead of changing it.

### P7.3 - Resource Server

`Code/Applications/ResourceServer/` is a Win32 GUI application.
`ResourceServerApplication.cpp:432` is `_tWinMain`, and it has an imgui UI in
`ResourceServerUI.cpp`.

There are two decisions here.

**(a) GUI or headless?** The Resource Server compiles and serves resources. Its UI is a
monitoring convenience. A **headless mode** would be simpler, and it is arguably more useful on
Linux, for build agents and remote development. But the editor's workflow may expect to launch
and manage it.

Check how `EnsureResourceServerIsRunning` in `BaseModule.cpp` starts it, and whether anything
depends on it having a window. **The recommendation is GUI mode.** `LinuxApplication` and the
imgui backend already exist from Phase 6, so it is nearly free, and headless-only would diverge
in behavior from Windows.

**(b) Worker processes.** `ResourceServerWorker.cpp` uses `CreateProcess` to spawn
`EsotericaResourceCompiler` instances. The vendored `subprocess` library at
`Code/EngineTools/ThirdParty/subprocess/` may already provide a portable path. Check that before
you write `fork` and `execv` by hand. If `ResourceServerWorker.cpp` calls Win32 directly, it
needs a platform split. Add it to [TouchedFiles.md](../TouchedFiles.md) once you know.

Also note that `ResourceServerUI.cpp` calls `Platform::Win32::OpenInExplorer` twice. Route those
to the Phase 1 Linux implementation.

### P7.4 - `OpenInExplorer` verification

Six `EngineTools` call sites, plus two in the Resource Server:

- `Code/EngineTools/Resource/Tools/EditorTool_ResourceBrowser.cpp` (3 times)
- `Code/EngineTools/Resource/Tools/EditorTool_ResourceImporter.cpp`
- `Code/EngineTools/Widgets/Pickers/DataPathPicker.cpp`
- `Code/EngineTools/Widgets/Pickers/ResourcePickers.cpp`
- `Code/Applications/ResourceServer/ResourceServerUI.cpp` (2 times)

These have compiled since Phase 1. This task checks that they *work*. `xdg-open` on a directory
should open the file manager. On a file it should either select it in the file manager, or open
it in the associated application.

Note the difference: Win32's `OpenInExplorer` on a *file* selects it in Explorer, and `xdg-open`
on a file **opens** it. If selecting is the intent, `dbus-send` to
`org.freedesktop.FileManager1.ShowItems` is closer. Decide which behavior you want, and record
it.

### P7.5 - Resource hot-reload end to end

This is the payoff task, and it proves the Phase 3 file watcher under real conditions.

The loop: the editor runs, a source asset changes on disk, the file watcher fires, the Resource
Server recompiles, and the editor hot-reloads the resource.

Verify it with a real asset edit. That exercises the Phase 3 `inotify` watcher, the Resource
Server's worker spawning, the network path between server and editor (`ixWebSocket` and
`GameNetworkingSockets`), and the engine's resource reload path, all at once. Expect watcher bugs
here that the Phase 3 scratch test did not find, above all around editors that write by renaming
over the original. Most editors do, and that produces `IN_MOVED_TO` rather than `IN_CLOSE_WRITE`.

### P7.6 - Editor shakedown

Open the editor tools and use them: the resource browser, the resource importer, the animation
graph editor, the ragdoll editor, the map editor, and the property grids. Each is platform-neutral
code that has never run on Linux.

Expect issues in file path display, where delimiter differences show up in UI strings, in
drag-and-drop between imgui viewports, in the clipboard, in font rendering, and anywhere that
assumed a Windows path shape.

Record what you find rather than fixing everything at once. Some findings will be
platform-neutral upstream bugs. See Conventions rule 3.

---

## Acceptance criteria

**Criteria 3, 5, 8 and 9 need a window that draws, and criterion 10 needs a frame.** No GPU in
the current development machine renders the engine's geometry path; see the Phase 6 block at the
top. **Say which of these you could not check, rather than marking them met or failed.** The rest
- the builds, the Resource Server, hot reload, the file dialogs - do not need a rendered frame.

1. `Build/Linux_Release/Esoterica.Applications.Editor` builds, links, and launches.
2. `Build/Linux_Release/Esoterica.Applications.ResourceServer` builds, links, and runs. **The
   binary is named after its project**, like every other Linux binary in this port; this document
   originally wrote `EsotericaResourceServer`.
3. The editor's borderless window works: drag, resize from all edges, maximize, restore, and
   minimize.
4. Native file dialogs open, filter by extension, and return correct paths for open, save, and
   folder-select.
5. The resource browser lists resources, and `OpenInExplorer` opens a file manager.
6. Resource hot-reload works end to end on a real asset edit, **including the
   rename-over-original write pattern**.
7. The Resource Server compiles resources when the editor asks.
8. Every editor tool from P7.6 opens without crashing, and [Progress.md](../Progress.md) lists
   any issues.
9. imgui multi-viewport docking works. Tools can be dragged out into separate OS windows.
10. Both applications shut down cleanly, with no leaks and no validation errors.
11. **The Windows MSBuild build still succeeds**, and both Windows applications are unchanged.
12. Every upstream file you edited is in [TouchedFiles.md](../TouchedFiles.md) with status
    `done`.

## Do not

- Implement XDG portal support before you try `pfd`.
- Vendor `pfd` into `Code/**/ThirdParty/`.
- Change the interface in `SystemDialogs.h` without escalating.
- Refactor `EditorUI.cpp`. It is platform-neutral, and it already works.
- Fix platform-neutral editor bugs from P7.6 before you record them as upstream issues.
- Claim the editor works because it launched. Criterion 8 means opening the tools.

## Notes for the next agent

**Phase 6 left deliberate debt, and it is listed in
[Progress.md](../Progress.md#deferred-on-purpose).** Those are known, chosen shortcuts, not
things to rediscover. If you hit one, check that list before investigating.

This is the last planned phase. Record this in [Progress.md](../Progress.md):

- The `SystemDialogs` implementation you chose, and its limits.
- Whether the Resource Server runs with a GUI or headless, and why.
- Every editor tool issue from P7.6, split into "Linux port bug" and "upstream bug".
- What a Linux user must install to run the editor. That is the basis for Linux build
  instructions in the top-level `README.md`, which is worth adding once the port works.

## Beyond this phase

These are out of scope on purpose. They are recorded here so that nobody mistakes them for
oversights.

- **Profiling.** This port drops Optick. Tracy is the natural replacement. It is a separate
  optional workstream.
- **C++ hot reload**, the Live++ equivalent. There is no good Linux option, so this port does not
  pursue it.
- **Navmesh generation.** Navpower is Windows-only and licensed, and upstream already disables it
  by default.
- **Packaging.** No `.deb`, Flatpak, or AppImage. The engine is a developer tool, not a shipped
  product.
- **A Vulkan backend on Windows.** This becomes possible, because `RHI_Vulkan.cpp` is
  platform-neutral apart from surface creation. It would be useful for testing the Vulkan backend
  against the Direct3D one on identical hardware. Worth considering as a follow-up.
- **ARM64 Linux.** The hand-rolled SIMD math is x86-specific, using `<immintrin.h>`, SSE4.2, and
  AVX. Porting it would be a large separate project.
