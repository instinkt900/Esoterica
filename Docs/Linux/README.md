# Esoterica Linux Port

This directory is the authoritative plan and reference for adding Linux support to this
fork of [Esoterica](https://github.com/BobbyAnguelov/Esoterica).

## Prime directive

**This is a fork of an actively developed upstream that explicitly rejects large PRs.**
Every decision in this plan is subordinate to one goal: *keeping `git merge upstream/main`
painless, forever.* A Linux port that works but cannot absorb upstream changes is a failure.

Concretely that means: add files, don't edit them. When you must edit an upstream file,
the edit is a 2–3 line `#elif defined( __linux__ )` branch beside an existing `#if _WIN32`
branch, and it is recorded in [TouchedFiles.md](TouchedFiles.md).

## If you are an AI agent picking up work here

Start with **[/AGENTS.md](../../AGENTS.md)** — it covers workflow: branching, commits, the
definition of done, and when to escalate. This directory covers the substance of the work.

Then read, in this order, **every session**:

1. [00-Conventions.md](00-Conventions.md) — the rules. Non-negotiable. Violating these
   creates merge debt that someone pays later.
2. [Progress.md](Progress.md) — what has already been done, and what is in flight.
3. The phase document for your assigned task, in [Phases/](Phases/).

Then do exactly the task you were given. Do not opportunistically fix, tidy, reformat, or
"improve" upstream code you happen to be reading. See Conventions rule 3.

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

Each phase ends with something that demonstrably runs. Phases are mostly sequential;
where parallelism is possible it is called out in the phase doc.

| Phase | Deliverable | Rough cost |
|---|---|---|
| [0 — Build System](Phases/Phase0-BuildSystem.md) | `ninja` builds a Linux target from the existing `.vcxproj` files | 2–3 weeks |
| [1 — Base Platform Layer](Phases/Phase1-BasePlatform.md) | `Esoterica.Base` compiles and links on Linux, headless | 2–3 weeks |
| [2 — Reflector](Phases/Phase2-Reflector.md) | Reflection codegen runs on Linux | 1–2 weeks |
| [3 — Resource Compiler](Phases/Phase3-ResourceCompiler.md) | Game data compiles on Linux | 2–3 weeks |
| [4 — Shader Pipeline](Phases/Phase4-ShaderPipeline.md) | `.esh` → SPIR-V + reflection on Linux | 2–4 weeks |
| [5 — Vulkan RHI](Phases/Phase5-VulkanRHI.md) | Full-parity Vulkan backend behind `RHI.h` | 3–5 months |
| [6 — Windowing & Input](Phases/Phase6-WindowingInput.md) | `Engine` app renders a map on Linux | 3–4 weeks |
| [7 — Editor & Tools](Phases/Phase7-EditorTools.md) | Full editor on Linux | 3–4 weeks |

## Why this is more tractable than it looks

Findings from the initial survey of the codebase (2026-08-13, at commit `6813cf9`):

- **Total Win32-specific code is ~4,300 lines across 15 files.** The engine already uses a
  `Code/Base/<System>/Platform/<System>_Win32.cpp` convention.
- **Only 10 shared files contain a platform guard at all**, and 9 of those are a single
  `#if _WIN32` include-switch that takes an `#elif` sibling. See
  [TouchedFiles.md](TouchedFiles.md) for the exact list with line numbers.
- **`RHI.h` is genuinely API-agnostic.** Zero `ID3D12*`, `DXGI*`, or `D3D12_*` types appear
  in it. All 6,084 lines of Direct3D live in one file. The swapchain takes a
  `void* m_pNativeWindowHandle`. A Vulkan backend is an additive sibling file.
- **Math is hand-rolled SSE**, not DirectXMath. `Math_Win32.h` is 20 lines wrapping a single
  `_BitScanReverse64`.
- **`FileSystem::Path::s_pathDelimiter` is declared in the shared header and *defined in the
  platform .cpp*.** The Linux file defines it as `'/'` and no shared code changes.
- **`DataPath` already hardcodes `'/'`** (`DataPath.h:47`), so serialized resource paths are
  platform-neutral. **Compiled data is portable — there is no data migration problem.**
- **The imgui Win32 backend is a vendored copy of upstream `imgui_impl_win32.cpp`**, so
  upstream's `imgui_impl_sdl3.cpp` is a near-drop-in replacement rather than a rewrite.
- **`.vcxproj` files list every source explicitly** (Base 147, Engine 236, EngineTools 155
  `ClCompile` entries) with **zero `ExcludedFromBuild` entries**. This is what makes a
  vcxproj-derived build generator viable: upstream adding or moving source files needs no
  Linux-side change.

## Scope decisions already made

These were decided at planning time. Revisit only with a deliberate decision recorded here.

- **Build system: extend `Code/Scripts/NinjaGen/NinjaGen.py`.** Not CMake. Rationale in
  [02-Architecture.md](02-Architecture.md#build-system).
- **Renderer: full feature parity with the Direct3D 12 backend.** Including raytracing, mesh
  shaders, and variable rate shading. No reduced-feature Linux renderer.
- **Target: x86-64 Linux, Vulkan 1.3, clang.** No ARM, no GCC-primary, no Wayland-vs-X11
  hand-rolling (SDL3 handles it).

## Status

See [Progress.md](Progress.md). At the time of writing, **no implementation work has
started** — this directory is the plan only.
