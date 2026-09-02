# Esoterica Linux Port

This directory holds the plan and reference for adding Linux support to this fork of
[Esoterica](https://github.com/BobbyAnguelov/Esoterica).

## Prime directive

**This is a fork of an actively developed upstream that rejects large PRs.** Every decision in
this plan serves one goal: *keep `git merge upstream/main` cheap, forever.* A Linux port that
works but cannot absorb upstream changes has failed.

In practice: add files, do not edit them. When you must edit an upstream file, make the edit a
2-3 line `#elif defined( __linux__ )` branch next to an existing `#if _WIN32` branch, and record
it in [TouchedFiles.md](TouchedFiles.md).

## If you are an AI agent picking up work here

Start with **[/AGENTS.md](../../AGENTS.md)**. It covers workflow: branching, commits, the
definition of done, and when to escalate. This directory covers the substance of the work.

Then read these, in order, **every session**:

1. [00-Conventions.md](00-Conventions.md) - the rules. They are not negotiable. A violation
   creates merge debt that someone else pays later.
2. [Progress.md](Progress.md) - what is done, and what is in flight.
3. [Blocked.md](Blocked.md) - what is written but not verified, and which machine unblocks it.
4. The phase document for your task, in [Phases/](Phases/).

Then do the task you were given, and nothing else. Do not fix, tidy, reformat, or improve
upstream code that you read along the way. See Conventions rule 3.

## Document map

| Document | Purpose |
|---|---|
| [/AGENTS.md](../../AGENTS.md) | *(repo root)* Workflow: branching, commits, definition of done, escalation |
| [00-Conventions.md](00-Conventions.md) | Porting rules, code style, what you may and may not touch |
| [01-UpstreamMerges.md](01-UpstreamMerges.md) | How to merge upstream, and how to keep merges cheap |
| [02-Architecture.md](02-Architecture.md) | Target design: platform layer, build system, RHI |
| [03-Dependencies.md](03-Dependencies.md) | Every Windows dependency and its Linux replacement |
| [04-BuildAndRun.md](04-BuildAndRun.md) | How to build and run each target on Linux |
| [TouchedFiles.md](TouchedFiles.md) | Registry of every upstream file this port modifies |
| [Progress.md](Progress.md) | Running log of completed and in-flight work |
| [Blocked.md](Blocked.md) | What is written but not verified, indexed by the machine that unblocks it |
| [Phases/](Phases/) | Per-phase task specifications. [Phase 8](Phases/Phase8-Completion.md) is the remaining-work list |
| `ForkReview.md` | *(not written yet)* How far this fork has diverged, and whether any of it could go upstream. [P8.7](Phases/Phase8-Completion.md#p87---fork-review) |

## Phases

Each phase ends with something that runs. The phases are mostly sequential. Where you can work
in parallel, the phase doc says so.

| Phase | Deliverable | Rough cost |
|---|---|---|
| [0 - Build System](Phases/Phase0-BuildSystem.md) | `ninja` builds a Linux target from source lists synced off the `.vcxproj` files | 2-3 weeks |
| [1 - Base Platform Layer](Phases/Phase1-BasePlatform.md) | `Esoterica.Base` compiles and links on Linux, headless | 2-3 weeks |
| [2 - Reflector](Phases/Phase2-Reflector.md) | Reflection codegen runs on Linux | 1-2 weeks |
| [3 - Resource Compiler](Phases/Phase3-ResourceCompiler.md) | Game data compiles on Linux | 2-3 weeks |
| [4 - Shader Pipeline](Phases/Phase4-ShaderPipeline.md) | `.esh` to SPIR-V plus reflection on Linux | 2-4 weeks |
| [5 - Vulkan RHI](Phases/Phase5-VulkanRHI.md) | Vulkan backend behind `RHI.h`, at full parity | 3-5 months |
| [6 - Windowing and Input](Phases/Phase6-WindowingInput.md) | `Engine` app renders a map on Linux | 3-4 weeks |
| [7 - Editor and Tools](Phases/Phase7-EditorTools.md) | Full editor on Linux | 3-4 weeks |
| [8 - Completion and Fork Review](Phases/Phase8-Completion.md) | The Windows build runs, the engine simulates, the debt is swept, and the fork is assessed | unknown; P8.1 is why |

**Phase 8 was added on 2026-09-02**, after Phase 7's editor shakedown. The original plan ended at
Phase 7 and was right that the port would work by then; what it had no place for was the work
left over **after** the thing works. See [Why this phase exists](Phases/Phase8-Completion.md#why-this-phase-exists).

## Why this is smaller than it looks

Findings from the first survey of the codebase (2026-08-13, at commit `6813cf9`):

- **Win32-specific code totals about 4,300 lines across 15 files.** The engine already uses a
  `Code/Base/<System>/Platform/<System>_Win32.cpp` convention.
- **Only 10 shared files contain a platform guard at all.** In 9 of them the guard is a single
  `#if _WIN32` include-switch that takes an `#elif` sibling. [TouchedFiles.md](TouchedFiles.md)
  lists them with line numbers.
- **`RHI.h` is API-agnostic.** It contains no `ID3D12*`, `DXGI*`, or `D3D12_*` types. All 6,084
  lines of Direct3D live in one file. The swapchain takes a `void* m_pNativeWindowHandle`. A
  Vulkan backend is therefore a new sibling file, not a refactor.
- **The math code is hand-rolled SSE**, not DirectXMath. `Math_Win32.h` is 20 lines that wrap a
  single `_BitScanReverse64`.
- **The shared header declares `FileSystem::Path::s_pathDelimiter`, and the platform .cpp
  defines it.** The Linux file defines it as `'/'`, and no shared code changes.
- **`DataPath` already hardcodes `'/'`** (`DataPath.h:47`), so serialized resource paths are
  platform-neutral. **Compiled data is portable. There is no data migration problem.**
- **The imgui Win32 backend is a vendored copy of upstream `imgui_impl_win32.cpp`.** Upstream
  `imgui_impl_sdl3.cpp` is therefore close to a drop-in replacement, not a rewrite.
- **The `.vcxproj` files list every source explicitly** (Base 147, Engine 236, EngineTools 155
  `ClCompile` entries) with **no `ExcludedFromBuild` entries**. That makes them a good
  machine-readable record of what upstream builds. `SyncUpstream.py` turns them into
  `UpstreamProjects.txt`, and checks that copy is current on every build.
- **Upstream churns about five source-list entries a year.** 107 commits in total, and four in
  the last twelve months touched a `.vcxproj` source list. Keeping the Linux source list in
  three reviewed text files therefore costs almost nothing, and it removes the filename
  heuristics a live-parsing generator needs. See
  [02-Architecture.md](02-Architecture.md#decision-three-source-lists-checked-against-the-vcxproj-files).

## Scope decisions already made

These were decided at planning time. Change one only with a deliberate decision recorded here.

- **Build system: extend `Code/Scripts/NinjaGen/NinjaGen.py`.** Not CMake. See the reasoning in
  [02-Architecture.md](02-Architecture.md#build-system).
- **The Linux source list lives in three reviewed text files**, synced off the `.vcxproj` files
  by `SyncUpstream.py` rather than derived live on every build. Amended 2026-08-27; the original
  plan derived it live. [Progress.md](Progress.md) records why.
- **Renderer: full feature parity with the Direct3D 12 backend.** This includes raytracing, mesh
  shaders, and variable rate shading. There is no reduced-feature Linux renderer.
- **Target: x86-64 Linux, Vulkan 1.3, clang.** No ARM, no GCC as the primary compiler, and no
  hand-written Wayland or X11 code. SDL3 handles the display server.

## Status

**Live state is in [Progress.md](Progress.md).** This section is the shape of the thing; that
file is the detail, and [Blocked.md](Blocked.md) is what is written but not yet verified.

**The engine renders on Linux, correctly.** `Esoterica.Applications.Engine` draws the pbrdemo map
with geometry, textures, normals, image-based lighting, direct lighting, shadows, a sky and a
reflective ground plane - on an RTX 3090, with host validation on, **zero validation messages and
no message-ID filters at all**, no device memory leaked and a clean shutdown.

**The editor works too.** It opens the map, every tool opens, multi-viewport docking works,
resource hot reload works end to end on a real asset edit, and both applications shut down clean.

**Phases 0 to 7 are done. Phase 8 is what is left**, and it is verification and debt rather than
porting. The whole tree builds in Debug and Release, and nothing in it fails to compile.

| Phase | State |
|---|---|
| 0 - 4 | Done. `ninja` builds the tree, reflection and resource compilation run, DXC builds from source, and all 46 shader stages compile and validate as SPIR-V |
| 5 - Vulkan RHI | Written, merged and **run**. All seventeen groups including P5.17. Criterion 8 is met for forward shading, cascaded shadows, GTAO, SMAA and debug draw; mesh picking is owed to P8.4. **Raytracing has never executed and nothing can make it** - it has no callers on either backend. See [P8.3](Phases/Phase8-Completion.md#p83---raytracing-or-the-decision-not-to) |
| 6 - Windowing and input | Written. SDL3, `LinuxApplication`, the imgui platform backend, keyboard, mouse and gamepad, the surface and the swapchain. The engine opens a window and renders a map |
| 7 - Editor and tools | **Done.** Both applications build, link and run; the server serves over the network; the editor shakedown of 2026-09-02 met criteria 5, 6, 8, 9 and 10. Four items remain and they are in [Blocked.md](Blocked.md) |
| 8 - Completion and fork review | **In flight.** Nothing has been built on Windows, nothing has been seen to *simulate*, and the fork has never been measured against upstream |

### There is almost no unported code left

Worth stating plainly, because the remaining-work list is long and it reads worse than it is.
Every `*_Win32.*` file has a `_Linux` sibling - 27 of them - and
`Code/Scripts/NinjaGen/Exclusions.txt` drops **five** things: the Direct3D 12 backend, its memory
allocator, three Windows entry points, and the Windows-only BuildGenerator. **No engine subsystem
is excluded.** Physics, animation, the entity system, navmesh and networking all compile and ship
in the Linux build.

What is left in Phase 8 is verification debt, deliberate shortcuts, and one feature that is dead
code on both backends.

### Two development machines, and only one can render

**The port is built on a laptop whose GPU cannot run the engine's geometry path.** Its Intel UHD
620 has no `VK_EXT_mesh_shader`, so the RHI drops every mesh draw, later passes read what the
geometry path never wrote, and the GPU is lost a few seconds in. **Everything above about a
correct frame was measured on a second machine with an RTX 3090.**

Do not chase rendering on the first machine. [Blocked.md](Blocked.md) lists what is waiting for
the second one, and what is waiting for a Windows machine instead - two different queues that are
easy to confuse.

### What has never been checked at all

Two things, and they are the whole of Phase 8's risk.

**No Windows build has been run since this port started.** "`main` builds on both Windows and
Linux" is the invariant every phase's acceptance criteria ends with, and it is the largest
unmeasured risk in the project. Ten shader files are edited for **both** platforms, with no
`__linux__` branch to hide behind. See the Windows queue in [Blocked.md](Blocked.md) and
[P8.1](Phases/Phase8-Completion.md#p81---the-windows-build).

**The engine has rendered, and it has never simulated.** Every measurement in this port is a
static scene held for about thirty seconds. No physics body has been seen to move, no animation
graph has been evaluated, and **"Play Map" has never been pressed.** That is
[P8.2](Phases/Phase8-Completion.md#p82---runtime-shakedown), and it is the largest functional
unknown after Windows.

### Things that will surprise you

- **imgui multi-viewport cannot work under Wayland.** `ImGui_ImplSDL3_Init` enables it only for
  video drivers on its global-mouse white list, and `wayland` is not on it. The editor's docking
  UI depends on viewports, so a Wayland session gets one merged window. That is upstream imgui's
  own gate, not a port defect. See the P6.3 entry in [Progress.md](Progress.md).
- **Validation is off unless the ini says so**, and the Ubuntu 24.04 layers need
  `VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true` - which disables only the layer's stale
  bundled spirv-val, not any real check. The "Start here" block in [Progress.md](Progress.md) has
  both lines and three other things that each cost a session to find.
- **`HLSL_STATIC_ASSERT` is compiled out on SPIR-V.** Every shared-struct size check is absent on
  Linux and present on Windows.
- **Read [Deferred on purpose](Progress.md#deferred-on-purpose) before investigating anything that
  looks wrong.** It lists the shortcuts that were chosen rather than missed.
