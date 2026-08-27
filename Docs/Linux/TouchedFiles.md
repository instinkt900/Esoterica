# Upstream File Registry

Every upstream file this port modifies, why, and in which phase. **Keep this current.** It is
what makes the post-merge audit in [01-UpstreamMerges.md](01-UpstreamMerges.md) mechanical.

Status values: `planned` · `done` · `not needed` (checked, and confirmed unnecessary).

> **If your task needs an edit to a file that is not on this list, stop and escalate.**
> A full survey at commit `6813cf9` produced this list. A file outside it means the survey
> missed something, or your approach has drifted.

---

## Summary

| Category | Count |
|---|---|
| Files needing a 2-line `#elif` or `\|\|` addition | 7 |
| Files needing a whole-body guard wrap (2 lines) | 1 |
| Files needing a real edit | 3 |
| Files confirmed to need **no** change | 3 |
| **New** files added (no upstream conflict possible) | ~40 |

The `.vcxproj` files and `Esoterica.slnx` need **no change at all**. Linux sources are listed in
`Code/Scripts/NinjaGen/LinuxSources.txt`. Do not add them to the Visual Studio projects. It gains
nothing and guarantees conflicts.

---

## Real edits

| File | Change | Phase | Status |
|---|---|---|---|
| `Code/Scripts/NinjaGen/NinjaGen.py` | Large rewrite. A special case, see [01-UpstreamMerges.md](01-UpstreamMerges.md#special-case-codescriptsninjagenninjagenpy). | 0 | planned |
| `Code/EngineTools/FileSystem/FileSystemWatcher.h` | Line 15: change `#if _WIN32` to `#if _WIN32 \|\| defined( __linux__ )`. One line modified. The private members already use platform-neutral types (`void*`, `uint8_t*`, `unsigned long`), so they need no change. See the note below. | 3 | planned |
| `Code/EngineTools/FileSystem/FileSystemWatcher.cpp` | Wrap the unguarded `<windows.h>` include block (lines 8-16) in `#ifdef _WIN32`. The rest of the file already sits inside the `#if _WIN32` that starts at line 18. | 3 | planned |

## Two-line guard additions

Each of these files already has a platform guard. Add a sibling branch, and nothing else.

| File | Line | Existing | Add | Phase | Status |
|---|---|---|---|---|---|
| `Code/Base/Esoterica.h` | 55 | `#if _WIN32` includes `Platform/Platform_Win32.h` | `#elif defined( __linux__ )` includes `Platform/Platform_Linux.h` | 1 | planned |
| `Code/Base/Math/Math.h` | 12 | `#if _WIN32` includes `Platform/Math_Win32.h` | `#elif defined( __linux__ )` includes `Platform/Math_Linux.h` | 1 | planned |
| `Code/Base/Settings/IniFile.cpp` | 4 | `#if defined(_MSC_VER)` wraps the `#pragma warning` pair **and** the `mINI/ini.h` include | An `#else` branch that includes `ini.h` without the MSVC pragmas. Without it the include never happens on clang, and the file fails to compile. | 1 | planned |
| `Code/Base/Imgui/ImguiSystem.cpp` | 12 | `#if _WIN32` includes `imgui_freetype.h` | `#if _WIN32 \|\| defined( __linux__ )`. Linux uses Freetype too. | 6 | planned |
| `Code/Base/_Module/API.h` | 5 | `__declspec(dllexport)` and `dllimport` | An `#elif` branch that uses `__attribute__(( visibility( "default" ) ))` | 1 | planned |
| `Code/Engine/_Module/API.h` | - | as above | as above | 1 | planned |
| `Code/EngineTools/_Module/API.h` | - | as above | as above | 1 | planned |
| `Code/Game/_Module/API.h` | - | as above | as above | 1 | planned |
| `Code/GameTools/_Module/API.h` | - | as above | as above | 1 | planned |
| `Code/Applications/Tester/_Module/API.h` | - | as above | as above | 1 | planned |

*(The six `API.h` files get separate rows because each is an independent edit. They are one
logical change, so land them in one commit.)*

## Whole-body guard wrap

Use "wrap, do not split" to make an unguarded Windows-only translation unit inert on Linux. Add
`#ifdef _WIN32` as the first line and `#endif` as the last. That is **two lines added, zero
modified**, which is the cheapest possible diff. Then add a `_Linux.cpp` sibling that implements
the same interface.

| File | Change | Phase | Status |
|---|---|---|---|
| `Code/EngineTools/Core/SystemDialogs.cpp` | Wrap the whole body in `#ifdef _WIN32`. It is 552 lines of COM `IFileDialog` code, with an unguarded `<windows.h>` and `<shobjidl.h>` include at line 5. New sibling: `SystemDialogs_Linux.cpp`. | 7 | planned |

## Confirmed to need no change

Checked during the survey. Recorded so that nobody investigates them again.

| File | Why no change is needed |
|---|---|
| `Code/Base/Memory/Memory.h` | The `#ifdef _WIN32` at line 18 (`EE_STACK_ALLOC`) **already has an `#else` branch** for non-Windows. |
| `Code/Base/Memory/Memory.cpp` | The `#ifdef _WIN32` at line 13 already guards its `<windows.h>` include. The only `VirtualAlloc` use (`PageAllocator`, near line 234) sits inside it. During Phase 1, confirm that the guarded region has a working `#else`, or that the function is Windows-only by design. **Flagged for a check, not yet confirmed.** |
| `Code/Base/Profiling.cpp` | Both `#if _WIN32` guards (lines 8 and 36) are already there. `OpenProfiler()` becomes a no-op on Linux, which is correct because this port drops Optick. |
| `Code/Base/FileSystem/FileSystemPath.h` | This header declares `s_pathDelimiter`, and the platform `.cpp` defines it. The Linux `.cpp` defines `'/'`, and this header stays untouched. It is the model example of the intended pattern. |
| `Code/Base/Render/RHI.h` | Contains no Direct3D types. The Vulkan backend is a new sibling `.cpp`. **Do not modify this header.** |
| `Esoterica.slnx`, all `*.vcxproj` | The Linux sources live in `LinuxSources.txt`, and `SyncUpstream.py` only reads these files. No project-file changes. |
| `Data/**` | `DataPath` hardcodes `'/'`, separate from the filesystem delimiter. Compiled data is portable. |

## Repository configuration

| File | Change | Phase | Status |
|---|---|---|---|
| `.gitignore` | Add `Build/`. The existing entry is lowercase `build/`. Linux filesystems are case-sensitive, so git currently does **not** ignore `Build/`, the MSBuild and ninja output directory. Confirmed with `git check-ignore`. | 0 | planned |
| `.gitignore` | Add `__pycache__/`. Running the build generator writes bytecode next to it. One line appended, nothing modified. | 0 | done |

## New build generator files

Additive, so no upstream conflict is possible. Listed here because they sit in an upstream
directory, `Code/Scripts/NinjaGen/`, which Conventions rule 7 designates for build tooling.

| New file | Purpose | Phase |
|---|---|---|
| `Code/Scripts/NinjaGen/SyncUpstream.py` | Reads `Esoterica.slnx` and the `.vcxproj` files. Writes and checks `UpstreamProjects.txt` | 0 |
| `Code/Scripts/NinjaGen/SourceLists.py` | The three-list format, and the build model built from it | 0 |
| `Code/Scripts/NinjaGen/SourceLists_Test.py` | Checks for both of the above | 0 |
| `Code/Scripts/NinjaGen/UpstreamProjects.txt` | Generated snapshot of the Visual Studio projects. Never hand-edited | 0 |
| `Code/Scripts/NinjaGen/Exclusions.txt` | Upstream sources the Linux build drops | 0 |
| `Code/Scripts/NinjaGen/LinuxSources.txt` | Sources this fork adds | 0 |

## Root-level scripts (new files that mirror the `.bat` equivalents)

| New file | Mirrors | Phase |
|---|---|---|
| `DownloadDependencies.sh` | `DownloadDependencies.bat` | 0 |
| `RunReflection.sh` | `RunReflection.bat` | 2 |
| `CompileShaders.sh` | `CompileShaders.bat` | 4 |

---

## Note: the `FileSystemWatcher` handle encoding

`FileSystemWatcher.h` stores its OS handle as `void* m_pDirectoryHandle`, and the header has an
inline `IsWatching() const { return m_pDirectoryHandle != nullptr; }`. An `inotify` file
descriptor is an `int`, and fd `0` is legal in principle. Storing the raw fd would therefore
make `IsWatching()` wrong when the fd is 0.

**Store `(void*)( intptr_t )( fd + 1 )`** and subtract 1 on use. This keeps the upstream header
edit to a single line, and it leaves the inline accessor alone. Document the offset at the top
of `FileSystemWatcher_Linux.cpp`. It is not obvious, and it will otherwise confuse the next
reader.

The alternative is to change the member to a union, or to add a platform-specific member block.
Both mean a larger header edit for no functional gain, so this plan rejects them.
