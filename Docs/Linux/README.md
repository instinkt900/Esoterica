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
3. The phase document for your task, in [Phases/](Phases/).

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
| [TouchedFiles.md](TouchedFiles.md) | Registry of every upstream file this port modifies |
| [Progress.md](Progress.md) | Running log of completed and in-flight work |
| [Phases/](Phases/) | Per-phase task specifications |

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

See [Progress.md](Progress.md). **Phases 0 to 4 are done on Linux.** The resource compiler
compiles every resource under `Data/`, DXC builds from source, and all 46 shader stages compile
and validate as SPIR-V.

**Phase 5, the Vulkan RHI, is written and merged.** All sixteen groups are on `main`, and **none
of them has ever run**: no Linux binary can reach `RHI::CreateContext` until Phase 6 lands. Treat
the whole backend as compile-verified and run-unverified.

**The frame does not draw yet, and the fix is scheduled.** Every engine command signature sets
root constants and binds root descriptors per command, and no Vulkan indirect draw can do either.
Open question 7 is now answered: the shader reads its own command's root data out of the argument
buffer, indexed by `DrawIndex`. That is
[P5.17](Phases/Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change---scheduled-not-started),
and it comes **after** Phase 6, because it cannot be tested until the engine runs.

**Phase 6, windowing and input, has started, and Esoterica has rendered on Linux.** P6.1 to P6.6
are done: SDL3 builds from source, `LinuxApplication` opens a window and runs an event loop, the
imgui platform backend runs on SDL3 with multi-viewport verified on X11, keyboard, mouse and
gamepad input all work, and **a `VkSurfaceKHR` made from the SDL window drives a swapchain that
cleared and presented twelve frames with no Vulkan validation errors**. `Base` has no Phase 6
stubs left.

**This carries the port's first real edit to an upstream file.** `RHI::MaxPendingFrames` is 3 on
Linux, because the Intel UHD 620 and llvmpipe both report a swapchain `minImageCount` of 3. Four
lines added, zero modified, and Windows is bit for bit unchanged. It was escalated, approved and
registered in [TouchedFiles.md](TouchedFiles.md).

**P6.7 is done too: the engine binary builds, links and starts.**
`Build/Linux_Release/Esoterica.Applications.Engine -map data://... -packaged` reads its settings,
loads compiled data, opens a window and creates a Vulkan device. **Criterion 1 is met.** It does
not render a map yet: DXC emits invalid SPIR-V for `CullPrimitiveEXT` in the `DebugDraw` mesh
shader, and `Shaders::Initialize` creates every shader module at startup, so that one shader
blocks any machine without `VK_EXT_mesh_shader`. P6.8, first light, owns both.

**imgui viewports will not work under Wayland.** `ImGui_ImplSDL3_Init` enables them only for
video drivers on its global-mouse white list, which does not include `wayland`. The editor
depends on them, so Phase 7 needs to know. See the P6.3 entry in [Progress.md](Progress.md).

**The surface question is settled and written.** `Platform::SetMainWindowHandle` holds an
`SDL_Window*`, and `RHI_Vulkan.cpp` creates the `VkSurfaceKHR` itself through a Linux-only
`Platform` function. See the decision entry in [Progress.md](Progress.md).
