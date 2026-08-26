# Upstream File Registry

Every upstream file this port modifies, why, and in which phase. **Keep this current.** It is
what makes the post-merge audit in [01-UpstreamMerges.md](01-UpstreamMerges.md) mechanical.

Status values: `planned` · `done` · `not needed` (verified unnecessary).

> **If your task requires editing a file that is not on this list, stop and escalate.**
> The list below was derived from a full survey at commit `6813cf9` and re-verified line by
> line at that same commit on 2026-08-25 (upstream had not moved). A file outside it is a
> signal that either the survey missed something or your approach has drifted.

---

## Summary

| Category | Count |
|---|---|
| Files needing a small additive guard / call-site edit | 17 |
| Files needing a whole-body or region guard wrap | 1 |
| Files needing a substantive edit | 4 |
| Windows-only files excluded from the Linux build **by filename** (no source edit) | 2 |
| Files verified as needing **no** change | 7 |
| **New** files added (no upstream conflict possible) | ~40 |

The `.vcxproj` files and `Esoterica.slnx` need **no changes at all** — the build generator
discovers `*_Linux.cpp` files by globbing the directory beside each `*_Win32.cpp`. Do not add
Linux sources to the Visual Studio projects; it buys nothing and guarantees conflicts.

---

## Substantive edits

| File | Change | Phase | Status |
|---|---|---|---|
| `Code/Scripts/NinjaGen/NinjaGen.py` | Substantial rewrite across P0.1–P0.10. Special case — see [01-UpstreamMerges.md](01-UpstreamMerges.md#special-case-codescriptsninjagenninjagenpy). The P0.1 slice (`.slnx` parsing) landed in PR #1 (2026-08-26); the P0.2 slice (`.vcxproj`
parsing) landed in PR #2 (2026-08-26); P0.3–P0.10 not started. | 0 | planned |
| `Code/Base/Memory/Memory.h` | The `#else` branch at lines 23–27 already exists but is a non-functional stub: it defines `EE_STACK_ALLOC` / `EE_STACK_ARRAY_ALLOC` as **empty** macros, so call sites of the form `auto x = EE_STACK_ARRAY_ALLOC( T, n );` (e.g. `DebugDrawing.cpp`, `Embed.cpp`, `AnimationPose.cpp`) become syntax errors on Linux. Give the `#else` a real `alloca` and add a Linux-guarded `#include <alloca.h>`. Two lines modified (the stub definitions only), include added. Completing an existing non-working platform branch, not new platform awareness. | 1 | planned |
| `Code/Base/Memory/Memory.cpp` | `VirtualMemoryReserve` / `VirtualMemoryCommit` / `VirtualMemoryFree` (lines 231–252) call `VirtualAlloc` / `VirtualFree` / `InterlockedAdd64` **completely unguarded** — the only `#ifdef _WIN32` in the file (line 13) covers just the `<windows.h>` include, and there is no `#else`. Add `#ifdef _WIN32` / `#else` / `#endif` around the three functions (additions only) with a Linux implementation in the `#else` branch: `mmap( MAP_PRIVATE | MAP_ANONYMOUS, PROT_NONE )` for reserve, `mprotect( PROT_READWRITE )` for commit, `munmap` for free, `__sync_add_and_fetch` in place of `InterlockedAdd64`. The static `s_totalVirtualMemoryCommitted` (line 229) stays unguarded — `GetTotalRequestedMemory` (line 256) uses it on all platforms, which is why the implementations stay in this file rather than a sibling. | 1 | planned |
| `Code/EngineTools/FileSystem/FileSystemWatcher.h` | Line 15: `#if _WIN32` → `#if _WIN32 \|\| defined( __linux__ )`. One line modified. The class's private members are already platform-neutral types (`void*`, `uint8_t*`, `unsigned long`), so no member changes are needed — see the note below. | 3 | planned |

## Small additive edits

Most of these already have a platform guard; the edit is a sibling `#elif` / `#else` branch next
to the existing `#if`. The rest add a guard around an existing unguarded Windows-only line in a
shared file. Every diff in this group is a handful of added lines and at most one modified line.

| File | Line | Edit | Phase | Status |
|---|---|---|---|---|
| `Code/Base/Esoterica.h` | 55 | existing `#if _WIN32` → `Platform/Platform_Win32.h`; add `#elif defined( __linux__ )` → `Platform/Platform_Linux.h` | 1 | planned |
| `Code/Base/Math/Math.h` | 12 | same pattern → `Platform/Math_Linux.h` | 1 | planned |
| `Code/Base/Settings/IniFile.cpp` | 4–6, 152 | the `#if defined( _MSC_VER )` at line 4 wraps the **entire file** (the pragma pair, the `mINI/ini.h` include *and* the whole implementation); it closes at line 153, so on clang nothing in this file ever compiles. Insert `#endif` after line 6 (the `#pragma warning( disable : 4866 )` line) and re-open `#if defined( _MSC_VER )` before the `#pragma warning( pop )` at line 152. Two lines added, zero modified. | 1 | planned |
| `Code/Base/Imgui/ImguiSystem.cpp` | 12 | existing `#if _WIN32` → `imgui_freetype.h`; widen to `#if _WIN32 \|\| defined( __linux__ )` — Freetype is used on Linux too | 6 | planned |
| `Code/Base/_Module/API.h` | 5 | existing `#if EE_DLL` with the `__declspec` branch; add a sibling branch using `__attribute__(( visibility( "default" ) ))` | 1 | planned |
| `Code/Engine/_Module/API.h` | — | as above | 1 | planned |
| `Code/EngineTools/_Module/API.h` | — | as above | 1 | planned |
| `Code/Game/_Module/API.h` | — | as above | 1 | planned |
| `Code/GameTools/_Module/API.h` | — | as above | 1 | planned |
| `Code/Applications/Reflector/ReflectorApplication.cpp` | 5, 7 | unguarded `<windows.h>` / `<consoleapi2.h>` — both dead (nothing in the file uses them); wrap the two includes in `#ifdef _WIN32` / `#endif` | 2 | planned |
| `Code/Applications/Reflector/TypeReflection/Clang/ClangParser.cpp` | 75 | unguarded `Platform::Win32::GetShortPath( )` call; wrap the call in `#if _WIN32` / `#elif defined( __linux__ )` where Linux uses the path as-is (the line-5 `PlatformUtils_Win32.h` include is harmless — the header self-guards) | 2 | planned |
| `Code/EngineTools/FileSystem/FileSystemWatcher.cpp` | 8–16 | the body already sits inside `#if _WIN32` (line 20 → `#endif` line 270); the unguarded `NOMINMAX` / `WIN32_LEAN_AND_MEAN` defines and `<windows.h>` include at lines 8–16 do not. Wrap them in `#ifdef _WIN32` / `#endif`. Two lines added. | 3 | planned |
| `Code/EngineTools/Resource/Tools/EditorTool_ResourceBrowser.cpp` | 545, 1044, 1101 | three unguarded `Platform::Win32::OpenInExplorer( )` calls; guard each site with `#if _WIN32` / `#elif defined( __linux__ )` (the `#elif` calls the `Platform::Linux` sibling) | 3 | planned |
| `Code/EngineTools/Resource/Tools/EditorTool_ResourceImporter.cpp` | 847 | same as above | 3 | planned |
| `Code/EngineTools/Widgets/Pickers/ResourcePickers.cpp` | 330 | same as above | 3 | planned |
| `Code/EngineTools/Widgets/Pickers/DataPathPicker.cpp` | 142 | same as above | 3 | planned |
| `Code/Applications/ResourceServer/ResourceServerUI.cpp` | 799, 811 | same as above | 7 | planned |

*(The five `API.h` files are listed separately because each is an independent edit, but they are
one logical change and should land in one commit. `Code/Applications/Tester/_Module/API.h` is
**not** one of them — it contains only `#pragma once` and `EE_TESTER_API` is defined and used
nowhere; see "Verified as needing no change".)*

Each of these files except `ReflectorApplication.cpp` also carries an unguarded
`#include "Base/Platform/PlatformUtils_Win32.h"`; that include is harmless on Linux (the header
self-guards its body), so only the `Platform::Win32::` call sites need the `#elif`. The Linux sibling `Platform/PlatformUtils_Linux.h` is a new file (Phase 1)
and provides the `#elif`-branch implementations.

## Whole-body guard wrap

The "wrap, don't split" technique: to make an unguarded Windows-only translation unit inert on
Linux, add `#ifdef _WIN32` as the first line and `#endif` as the last. **Two lines added, zero
modified** — the cheapest possible diff — then add a `_Linux.cpp` sibling implementing the same
interface.

| File | Change | Phase | Status |
|---|---|---|---|
| `Code/EngineTools/Core/SystemDialogs.cpp` | Wrap entire body in `#ifdef _WIN32`. It is 552 lines of COM `IFileDialog` code with an unguarded `<windows.h>` / `<shobjidl.h>` include at line 5. New sibling: `SystemDialogs_Linux.cpp`. | 7 | planned |

## Excluded by filename (no source edit)

Upstream translation units the Linux build generator skips by filename. The files are never
modified; they are listed here so the exclusion rule is auditable in the post-merge audit.

| File | Reason | Phase |
|---|---|---|
| `Code/Base/Render/RHI_Direct3D12.cpp` | 6,084 lines of Direct3D 12 backend (`Esoterica.Base.vcxproj` line 568). Not on the Linux path; the Vulkan backend is a new sibling file, not a rename. | 0 |
| `Code/Applications/ResourceServer/ResourceServerApplication.cpp` | Windows app entry: `HINSTANCE` constructor, `_tWinMain`, unguarded `<shobjidl_core.h>` (`Esoterica.Applications.ResourceServer.vcxproj` line 103). Has **no** `_Win32` suffix, so the generator cannot find it by convention — it must be excluded by explicit filename. Phase 7 defines what replaces it on Linux. | 7 |

## Verified as needing no change

Checked during the survey. Recorded so nobody re-investigates.

| File | Why no change is needed |
|---|---|
| `Code/Base/Profiling.cpp` | Both `#if _WIN32` guards (lines 8, 36) are already present. `OpenProfiler()` degrades to a no-op on Linux, which is correct given Optick is dropped. |
| `Code/Base/_Module/BaseModule.cpp` | Verified 2026-08-25: the `PlatformUtils_Win32.h` include is already guarded (lines 9–11) and `EnsureResourceServerIsRunning` carries a full `#if _WIN32` / `#else return false;` / `#endif` around its process-starting body (lines 20–67), so it compiles on Linux and simply declines to start a server. (`Memory.h` and `Memory.cpp`, formerly listed here, are **not** in this category — see "Substantive edits".) |
| `Code/Applications/Tester/_Module/API.h` | Contains only `#pragma once`. `EE_TESTER_API` is defined nowhere and used nowhere, so there is nothing to port. |
| `Code/Base/FileSystem/FileSystemPath.h` | `s_pathDelimiter` is declared here and *defined in the platform `.cpp`*. The Linux `.cpp` defines `'/'`; this header is untouched. Model example of the intended pattern. |
| `Code/Base/Render/RHI.h` | Contains zero Direct3D types. The Vulkan backend is a new sibling `.cpp`. **This header must not be modified.** |
| `Esoterica.slnx`, all `*.vcxproj` | The generator globs `*_Linux.cpp` from disk. No project-file changes. |
| `Data/**` | `DataPath` hardcodes `'/'` independent of the filesystem delimiter. Compiled data is portable. |

## Repository configuration

| File | Change | Phase | Status |
|---|---|---|---|
| `.gitignore` | Add `Build/`. The existing entry is `build/` (lowercase); Linux filesystems are case-sensitive, so the MSBuild-and-ninja output directory `Build/` is currently **not** ignored. Verified with `git check-ignore`. | 0 | planned |

## Root-level scripts (new files, mirroring the `.bat` equivalents)

| New file | Mirrors | Phase |
|---|---|---|
| `DownloadDependencies.sh` | `DownloadDependencies.bat` | 0 |
| `RunReflection.sh` | `RunReflection.bat` | 2 |
| `CompileShaders.sh` | `CompileShaders.bat` | 4 |

---

## Note: the `FileSystemWatcher` handle encoding

`FileSystemWatcher.h` stores its OS handle as `void* m_pDirectoryHandle` and the header has an
inline `IsWatching() const { return m_pDirectoryHandle != nullptr; }`. An `inotify` file
descriptor is an `int`, and fd `0` is legitimate in principle, so storing the raw fd would make
`IsWatching()` wrong in the fd-0 case.

**Store `(void*)( intptr_t )( fd + 1 )`** and subtract 1 on use. This keeps the upstream header
edit to a single line and avoids touching the inline accessor. Document the offset prominently
at the top of `FileSystemWatcher_Linux.cpp` — it is non-obvious and will otherwise confuse
whoever reads it next.

The alternative — changing the member to a union or adding a platform-specific member block —
is a larger header edit for no functional gain, and is rejected.
