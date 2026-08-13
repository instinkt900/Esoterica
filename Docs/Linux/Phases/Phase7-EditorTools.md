# Phase 7 — Editor & Tools

**Goal:** the full editor runs on Linux.

**Deliverable:** `./Build/Linux_Release/EsotericaEditor` launches, the Resource Server runs, and
resource hot-reloading works end to end.

**Prerequisites:** Phase 6 complete.

**Rough cost:** 3–4 weeks.

**Read first:** [00-Conventions.md](../00-Conventions.md),
[TouchedFiles.md](../TouchedFiles.md).

---

## What is actually left

Less than the phase's position in the list suggests. The editor's substance —
`Code/Applications/Editor/EditorUI.cpp` and the whole of `Code/EngineTools/` — is
platform-neutral and already compiling since Phase 3. `EditorApplication_Win32.cpp` is only 127
lines because the real logic lives in `EditorUI`.

What remains:

1. Native file dialogs (`SystemDialogs.cpp`, 552 lines of COM) — the biggest item.
2. `EditorApplication_Linux` — thin, mirrors Phase 6's engine app.
3. The Resource Server, which is a Win32 GUI app (`_tWinMain` + imgui) and spawns worker
   processes.
4. `OpenInExplorer` call sites — already resolvable since Phase 1, needs verifying they behave.

---

## Tasks

### P7.1 — `EditorApplication_Linux`

**New:** `Code/Applications/Editor/Linux/EditorApplication_Linux.{h,cpp}`

Mirror `EditorApplication_Win32.{h,cpp}`. The class derives from `LinuxApplication` (Phase 6,
P6.2) and overrides `Initialize`, `Shutdown`, `FatalError`, `GetBorderlessTitleBarInfo`,
`ResizeMainWindow`, `ProcessInputEvent`, `ApplicationLoop`.

`EditorEngine` (declared in the same header) is platform-neutral — copy it across unchanged in
shape. Drop the `EE_ENABLE_LPP` overrides.

Note the editor uses a **borderless window with a custom title bar**
(`GetBorderlessTitleBarInfo` and `InitOptions::Borderless`), which is the trickiest part. SDL3's
`SDL_SetWindowHitTest` provides the equivalent of Win32's `WM_NCHITTEST` handling. Expect
iteration here — window dragging, edge resizing, and the maximise/restore behaviour all need to
feel right, and this is the most visible part of the editor's UX.

### P7.2 — `SystemDialogs_Linux.cpp`

**New:** `Code/EngineTools/Core/SystemDialogs_Linux.cpp`
**Edit:** `Code/EngineTools/Core/SystemDialogs.cpp` — should already be wrapped in `#ifdef _WIN32`
from Phase 3, P3.3. Verify.

The Windows implementation uses COM `IFileDialog` with `COMDLG_FILTERSPEC`. Read
`Code/EngineTools/Core/SystemDialogs.h` for the interface to implement — `FileDialog::Result`,
`FileDialog::ExtensionFilter`, and the open/save/folder entry points.

Options, in order of preference:

1. **XDG Desktop Portal** via D-Bus (`org.freedesktop.portal.FileChooser`). The correct modern
   answer, works under Flatpak, respects the desktop's native dialog. But it is asynchronous and
   D-Bus adds a dependency.
2. **`pfd` (portable-file-dialogs)**, which the project already lists as a third-party dependency
   in its README. It shells out to `zenity`/`kdialog`/`osascript`. Much simpler, synchronous,
   good enough for a tool. **Start here.**

Check whether `pfd` is actually vendored anywhere in the tree — the README lists it but the survey
did not locate it under `Code/**/ThirdParty/`. If it is absent, acquire it into `External/`
(Conventions rule 5), not into `Code/`.

**Note `ExtensionFilter` holds a `WString m_filter`** — a wide string, sized for the Windows
double-NUL-terminated filter format. On Linux you will need to translate that to the target
dialog's format. Prefer reading `m_extension` and `m_displayText` directly over parsing
`m_filter`, if the interface allows it. If the header genuinely forces you to consume `m_filter`,
that is a shared-header concern — **escalate** rather than changing it.

### P7.3 — Resource Server

`Code/Applications/ResourceServer/` is a Win32 GUI application:
`ResourceServerApplication.cpp:432` is `_tWinMain`, and it has an imgui UI
(`ResourceServerUI.cpp`).

Two decisions to make here:

**(a) GUI or headless?** The Resource Server's job is to compile and serve resources; the UI is a
monitoring convenience. A **headless mode** would be simpler and is arguably more useful on Linux
(build agents, remote development). But the editor's workflow may expect to launch and manage it.

Check how `BaseModule.cpp`'s `EnsureResourceServerIsRunning` starts it and whether anything depends
on it having a window. **Recommended: implement GUI mode**, since `LinuxApplication` and the imgui
backend already exist from Phase 6, making it nearly free — and headless-only would be a
behavioural divergence from Windows.

**(b) Worker processes.** `ResourceServerWorker.cpp` uses `CreateProcess` to spawn
`EsotericaResourceCompiler` instances. The vendored `subprocess` library
(`Code/EngineTools/ThirdParty/subprocess/`) may already provide a portable path — check before
writing `fork`/`execv` by hand. If `ResourceServerWorker.cpp` calls Win32 directly, it needs a
platform split; add it to [TouchedFiles.md](../TouchedFiles.md) when you know.

Also relevant: `ResourceServerUI.cpp` calls `Platform::Win32::OpenInExplorer` twice. Route to the
Phase 1 Linux implementation.

### P7.4 — `OpenInExplorer` verification

Six `EngineTools` call sites plus two in the Resource Server:

- `Code/EngineTools/Resource/Tools/EditorTool_ResourceBrowser.cpp` (×3)
- `Code/EngineTools/Resource/Tools/EditorTool_ResourceImporter.cpp`
- `Code/EngineTools/Widgets/Pickers/DataPathPicker.cpp`
- `Code/EngineTools/Widgets/Pickers/ResourcePickers.cpp`
- `Code/Applications/ResourceServer/ResourceServerUI.cpp` (×2)

These compile since Phase 1. This task is verifying they *work* — `xdg-open` on a directory should
open the file manager, and on a file should either select it in the file manager or open it in the
associated application. Note Win32's `OpenInExplorer` on a *file* selects it in Explorer, whereas
`xdg-open` on a file **opens** it. If selecting is the intent, `dbus-send` to
`org.freedesktop.FileManager1.ShowItems` is the closer equivalent — decide and record which
behaviour you chose.

### P7.5 — Resource hot-reload end to end

The payoff task, and the one that proves Phase 3's file watcher under real conditions.

The loop: editor runs → a source asset changes on disk → the file watcher fires → the Resource
Server recompiles → the editor hot-reloads the resource.

Verify with a real asset edit. This exercises the Phase 3 `inotify` watcher, the Resource Server's
worker spawning, the network path between server and editor (`ixWebSocket` /
`GameNetworkingSockets`), and the engine's resource reload path all at once. Expect to find
watcher bugs here that the Phase 3 scratch test did not — particularly around editors that write
via rename-over-original (most of them do), which produces `IN_MOVED_TO` rather than
`IN_CLOSE_WRITE`.

### P7.6 — Editor shakedown

Open the editor tools and use them: resource browser, resource importer, animation graph editor,
ragdoll editor, map editor, property grids. Each is platform-neutral code that has never actually
run on Linux.

Expect issues in: file path display (delimiter differences in UI strings), drag-and-drop between
imgui viewports, clipboard, font rendering, and any place that assumed a Windows path shape.

Record what you find rather than fixing everything immediately — some findings may be
platform-neutral upstream bugs (Conventions rule 3).

---

## Acceptance criteria

1. `Build/Linux_Release/EsotericaEditor` builds, links, and launches.
2. `Build/Linux_Release/EsotericaResourceServer` builds, links, and runs.
3. The editor's borderless window works: drag, resize from all edges, maximise, restore,
   minimise.
4. Native file dialogs open, filter by extension, and return correct paths for open, save, and
   folder-select.
5. The resource browser lists resources and `OpenInExplorer` opens a file manager.
6. Resource hot-reload works end to end on a real asset edit, **including the
   rename-over-original write pattern**.
7. The Resource Server compiles resources on request from the editor.
8. Each editor tool from P7.6 opens without crashing, with any issues enumerated in
   [Progress.md](../Progress.md).
9. imgui multi-viewport docking works — tools can be dragged out into separate OS windows.
10. Clean shutdown of both applications; no leaks, no validation errors.
11. **The Windows MSBuild build still succeeds** and both Windows applications are unchanged.
12. Every upstream file edited is in [TouchedFiles.md](../TouchedFiles.md) with status `done`.

## Do not

- Implement XDG portal support before trying `pfd`.
- Vendor `pfd` into `Code/**/ThirdParty/`.
- Change `SystemDialogs.h`'s interface without escalating.
- Refactor `EditorUI.cpp` — it is platform-neutral and already works.
- Fix platform-neutral editor bugs you discover in P7.6 without recording them as upstream
  issues first.
- Claim the editor "works" on the basis of it launching. Criterion 8 means actually opening the
  tools.

## Notes for the next agent

This is the last planned phase. Record in [Progress.md](../Progress.md):
- The `SystemDialogs` implementation chosen and its limitations.
- Whether the Resource Server runs GUI or headless, and why.
- Every editor tool issue found in P7.6, separated into "Linux port bug" and "upstream bug".
- What a Linux user needs installed to run the editor — this is the basis for Linux build
  instructions in the top-level `README.md`, which is worth adding once the port works.

## Beyond this phase

Things deliberately left out of scope, recorded so they are not mistaken for oversights:

- **Profiling.** Optick is dropped; Tracy would be the natural replacement. A separate,
  optional workstream.
- **Hot-reload of C++** (Live++ equivalent). No good Linux option; not pursued.
- **Navmesh generation.** Navpower is Windows-only and licensed. Already disabled by default
  upstream.
- **Packaging.** No `.deb`, Flatpak, or AppImage. The engine is a developer tool, not a shipped
  product.
- **A Vulkan backend on Windows.** Now possible — `RHI_Vulkan.cpp` is platform-neutral apart from
  surface creation — and would be genuinely useful for testing the backend against the Direct3D
  one on identical hardware. Worth considering as a follow-up.
- **ARM64 Linux.** The hand-rolled SIMD math is x86-specific (`<immintrin.h>`, SSE4.2/AVX).
  Porting it would be a substantial separate project.
