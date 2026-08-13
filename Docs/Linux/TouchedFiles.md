# Upstream File Registry

Every upstream file this port modifies, why, and in which phase. **Keep this current.** It is
what makes the post-merge audit in [01-UpstreamMerges.md](01-UpstreamMerges.md) mechanical.

Status values: `planned` · `done` · `not needed` (verified unnecessary).

> **If your task requires editing a file that is not on this list, stop and escalate.**
> The list below was derived from a full survey at commit `6813cf9`; a file outside it is a
> signal that either the survey missed something or your approach has drifted.

---

## Summary

| Category | Count |
|---|---|
| Files needing a 2-line `#elif` / `\|\|` addition | 7 |
| Files needing a whole-body guard wrap (2 lines) | 1 |
| Files needing a substantive edit | 3 |
| Files verified as needing **no** change | 3 |
| **New** files added (no upstream conflict possible) | ~40 |

The `.vcxproj` files and `Esoterica.slnx` need **no changes at all** — the build generator
discovers `*_Linux.cpp` files by globbing the directory beside each `*_Win32.cpp`. Do not add
Linux sources to the Visual Studio projects; it buys nothing and guarantees conflicts.

---

## Substantive edits

| File | Change | Phase | Status |
|---|---|---|---|
| `Code/Scripts/NinjaGen/NinjaGen.py` | Substantial rewrite. Special case — see [01-UpstreamMerges.md](01-UpstreamMerges.md#special-case-codescriptsninjagenninjagenpy). | 0 | planned |
| `Code/EngineTools/FileSystem/FileSystemWatcher.h` | Line 15: `#if _WIN32` → `#if _WIN32 \|\| defined( __linux__ )`. One line modified. The class's private members are already platform-neutral types (`void*`, `uint8_t*`, `unsigned long`), so no member changes are needed — see the note below. | 3 | planned |
| `Code/EngineTools/FileSystem/FileSystemWatcher.cpp` | Wrap the unguarded `<windows.h>` include block (lines ~8–16) in `#ifdef _WIN32`. The rest of the file is already inside `#if _WIN32` from line 18. | 3 | planned |

## Two-line guard additions

Each of these already has a platform guard; add a sibling branch and nothing else.

| File | Line | Existing | Add | Phase | Status |
|---|---|---|---|---|---|
| `Code/Base/Esoterica.h` | 55 | `#if _WIN32` → `Platform/Platform_Win32.h` | `#elif defined( __linux__ )` → `Platform/Platform_Linux.h` | 1 | planned |
| `Code/Base/Math/Math.h` | 12 | `#if _WIN32` → `Platform/Math_Win32.h` | `#elif defined( __linux__ )` → `Platform/Math_Linux.h` | 1 | planned |
| `Code/Base/Settings/IniFile.cpp` | 4 | `#if defined(_MSC_VER)` wraps the `#pragma warning` pair **and** the `mINI/ini.h` include | `#else` branch that includes `ini.h` without the MSVC pragmas. Without this the include never happens on clang and the file fails to compile. | 1 | planned |
| `Code/Base/Imgui/ImguiSystem.cpp` | 12 | `#if _WIN32` → `imgui_freetype.h` | `#if _WIN32 \|\| defined( __linux__ )` — Freetype is used on Linux too | 6 | planned |
| `Code/Base/_Module/API.h` | 5 | `__declspec(dllexport)` / `dllimport` | `#elif` branch using `__attribute__(( visibility( "default" ) ))` | 1 | planned |
| `Code/Engine/_Module/API.h` | — | as above | as above | 1 | planned |
| `Code/EngineTools/_Module/API.h` | — | as above | as above | 1 | planned |
| `Code/Game/_Module/API.h` | — | as above | as above | 1 | planned |
| `Code/GameTools/_Module/API.h` | — | as above | as above | 1 | planned |
| `Code/Applications/Tester/_Module/API.h` | — | as above | as above | 1 | planned |

*(The six `API.h` files are listed separately because each is an independent edit, but they are
one logical change and should land in one commit.)*

## Whole-body guard wrap

The "wrap, don't split" technique: to make an unguarded Windows-only translation unit inert on
Linux, add `#ifdef _WIN32` as the first line and `#endif` as the last. **Two lines added, zero
modified** — the cheapest possible diff — then add a `_Linux.cpp` sibling implementing the same
interface.

| File | Change | Phase | Status |
|---|---|---|---|
| `Code/EngineTools/Core/SystemDialogs.cpp` | Wrap entire body in `#ifdef _WIN32`. It is 552 lines of COM `IFileDialog` code with an unguarded `<windows.h>` / `<shobjidl.h>` include at line 5. New sibling: `SystemDialogs_Linux.cpp`. | 7 | planned |

## Verified as needing no change

Checked during the survey. Recorded so nobody re-investigates.

| File | Why no change is needed |
|---|---|
| `Code/Base/Memory/Memory.h` | The `#ifdef _WIN32` at line 18 (`EE_STACK_ALLOC`) **already has an `#else` branch** for non-Windows. |
| `Code/Base/Memory/Memory.cpp` | The `#ifdef _WIN32` at line 13 already guards its `<windows.h>` include. Only `VirtualAlloc` use (`PageAllocator`, ~line 234) is inside it; confirm during Phase 1 that the guarded region has a working `#else` or that the function is Windows-only by design. **Flagged for verification, not yet confirmed.** |
| `Code/Base/Profiling.cpp` | Both `#if _WIN32` guards (lines 8, 36) are already present. `OpenProfiler()` degrades to a no-op on Linux, which is correct given Optick is dropped. |
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
