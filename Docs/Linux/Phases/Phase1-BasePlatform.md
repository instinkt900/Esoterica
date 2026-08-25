# Phase 1 — Base Platform Layer

**Goal:** `Esoterica.Base` compiles and links on Linux, headless.

**Deliverable:** `libEsoterica.Base.so` exists, and a trivial program that calls into it
(`Esoterica.Applications.Tester`) links and runs.

**Prerequisites:** Phase 0 complete.

**Rough cost:** 2–3 weeks.

**Read first:** [00-Conventions.md](../00-Conventions.md),
[02-Architecture.md § Platform abstraction layer](../02-Architecture.md#platform-abstraction-layer),
[TouchedFiles.md](../TouchedFiles.md).

---

## The critical design point: the RHI stub

`Code/Base/Render/RHI_Direct3D12.cpp` is a member of `Esoterica.Base.vcxproj` (line 568), and
**55 files across `Engine` and `EngineTools` call `RHI::` functions.** So `Base` must export the
full ~110-function RHI surface or nothing downstream links.

Therefore **Phase 1 must produce a stub `Code/Base/Render/RHI_Vulkan.cpp`** in which every
function declared in `RHI.h` is defined and immediately halts:

```cpp
#ifdef __linux__
#include "RHI.h"

//-------------------------------------------------------------------------
// Vulkan RHI backend
//-------------------------------------------------------------------------
// Phase 1 delivers signature-complete stubs so that Base exports the full RHI surface and
// everything downstream links. Phase 5 replaces these with real implementations.
//-------------------------------------------------------------------------

namespace EE::Render::RHI
{
    Context* CreateContext( ContextParameters const& parameters )
    {
        EE_UNIMPLEMENTED_FUNCTION();
        return nullptr;
    }

    // ... and so on for every function in RHI.h
}
#endif
```

This unblocks Phases 2, 3, and 4 entirely — they need `Base` and `EngineTools` to *link*, not to
render. It is the single most important structural decision in the plan's phasing.

Use `EE_UNIMPLEMENTED_FUNCTION()` (defined in `Esoterica.h`), not a silent return. A stub that
returns quietly will produce a mystifying failure in Phase 6; a stub that halts tells you
exactly which function Phase 5 still owes you.

**Do not** modify `RHI.h`. **Do not** guard `RHI_Direct3D12.cpp` — it is already Windows-only in
effect and the build generator excludes it by platform (add it to the generator's exclusion
logic in Phase 0 or here, by filename, since it lacks a `_Win32` suffix — note this explicitly
in `Progress.md`).

---

## Tasks

Tasks P1.1 through P1.8 are largely independent and can be parallelised across agents. P1.9 and
P1.10 depend on the rest.

### P1.1 — `Platform_Linux.h`, module API visibility

**New:** `Code/Base/Platform/Platform_Linux.h`
**Edits:** `Code/Base/Esoterica.h:55`, and five `_Module/API.h` files.

Mirror `Platform_Win32.h`. It must define:

| Macro | Win32 | Linux |
|---|---|---|
| `EE_FORCE_INLINE` | `__forceinline` | `__attribute__(( always_inline )) inline` |
| `EE_DEBUG_BREAK()` | `__debugbreak()` | `__builtin_debugtrap()` (clang) or `raise( SIGTRAP )` |
| `EE_DISABLE_OPTIMIZATION` | `__pragma( optimize( "", off ) )` | `_Pragma( "clang optimize off" )` |
| `EE_ENABLE_OPTIMIZATION` | `__pragma( optimize( "", on ) )` | `_Pragma( "clang optimize on" )` |

Match the sibling's guard placement: `Platform_Win32.h` puts `#pragma once` **before**
`#ifdef _WIN32`, so `Platform_Linux.h` puts `#pragma once` before `#ifdef __linux__` too.

For the five `API.h` files, the export macro becomes:

```cpp
#if EE_DLL
    #if ESOTERICA_BASE
        #define EE_BASE_API __attribute__(( visibility( "default" ) ))
    #else
        #define EE_BASE_API
    #endif
#else
    #define EE_BASE_API
#endif
```

The import case needs no attribute on ELF. Files:
`Code/Base/_Module/API.h`, `Code/Engine/_Module/API.h`, `Code/EngineTools/_Module/API.h`,
`Code/Game/_Module/API.h`, `Code/GameTools/_Module/API.h`. Land them in one commit.
(`Code/Applications/Tester/_Module/API.h` is **not** one of them — it contains only
`#pragma once`, and `EE_TESTER_API` is defined and used nowhere.)

### P1.2 — `Math_Linux.h`

**New:** `Code/Base/Math/Platform/Math_Linux.h`
**Edit:** `Code/Base/Math/Math.h:12`

Twenty lines. `Math_Win32.h` provides one function, `GetMostSignificantBit( uint64_t )`, via
`_BitScanReverse64`. Use `__builtin_clzll`:

```cpp
EE_FORCE_INLINE uint32_t GetMostSignificantBit( uint64_t value )
{
    if ( value == 0 )
    {
        return 0;
    }
    return 63u - (uint32_t) __builtin_clzll( value );
}
```

> **Note an upstream bug here.** `Math_Win32.h` casts the argument to `unsigned long`
> (`_BitScanReverse64( &index, (unsigned long) value )`), truncating to 32 bits and making the
> function wrong for values above `2^32`. **Do not replicate the bug, and do not fix the Win32
> file** (Conventions rule 3). Write the Linux version correctly, and record the discrepancy in
> [Progress.md](../Progress.md) under "Upstream issues observed" so it can be reported upstream.
> Flag it clearly — a silent behavioural difference between platforms is worse than either bug.

The project's SIMD (`Vector.h`, `Matrix.h`, `Quaternion.h`, `Plane.h`, `SIMD.h`,
`Quantization.h`) uses `<immintrin.h>` intrinsics directly, which clang supports natively. No
changes expected there; verify.

### P1.3 — `Threading_Linux.cpp`

**New:** `Code/Base/Threading/Platform/Threading_Linux.cpp`

Implement against `Code/Base/Threading/Threading.h`:

| Function | Linux mechanism |
|---|---|
| `GetProcessorInfo()` → `{ m_numPhysicalCores, m_numLogicalCores }` | Parse `/sys/devices/system/cpu/` or `/proc/cpuinfo`. `sysconf( _SC_NPROCESSORS_ONLN )` gives logical only; physical needs core-id deduplication. |
| `GetCurrentThreadID()` | `gettid()` / `syscall( SYS_gettid )` |
| `SetCurrentThreadName( char const* )` | `pthread_setname_np` — **note the 16-byte limit including NUL**; truncate rather than fail |
| `IsMainThread()` | compare against the tid captured in `Initialize` |
| `Initialize( char const* pMainThreadName )` / `Shutdown()` | store main tid, set its name |
| `SyncEvent::Signal/Reset/Wait/Wait(Milliseconds)` | `m_pNativeHandle` is a `void*`; allocate a pthread mutex+condvar pair, or use `eventfd`. Condvar is the better fit for the timed wait. |

`SyncEvent` is auto-reset or manual-reset depending on how `Reset()` is used — read
`Threading_Win32.cpp` and the call sites to determine which semantics Win32 `CreateEvent` was
configured for, and match exactly. Getting this wrong produces intermittent hangs that are very
expensive to debug later.

### P1.4 — `FileSystem_Linux.cpp` and `FileSystemPath_Linux.cpp`

**New:** `Code/Base/FileSystem/Platform/FileSystem_Linux.cpp`,
`Code/Base/FileSystem/Platform/FileSystemPath_Linux.cpp`

`FileSystemPath_Linux.cpp` must define:

```cpp
char const Path::s_pathDelimiter = '/';
```

plus `Path::EnsureCorrectPathStringFormat()`, `Path::GetFullPathString()` (use `realpath`), and
`Path::GetCorrectCaseForPath()`.

> **`GetCorrectCaseForPath` is a Windows concept.** On a case-sensitive filesystem the correct
> case *is* the case given. Implement it as a pass-through that returns the input unchanged, and
> comment why. Do not attempt to emulate case-insensitive resolution.

`FileSystem_Linux.cpp` implements: `Exists`, `IsReadOnly`, `IsExistingFile`,
`IsExistingDirectory`, `IsFileReadOnly`, `GetFileModifiedTime`, `WriteFileToDisk`,
`ReadBinaryFile` — via `stat`, `access`, `open`/`read`/`write`. See
`Code/Base/FileSystem/FileSystem.h` for the full exported surface and
`FileSystem_Win32.cpp` for semantics.

`GetFileModifiedTime` must return a value comparable across platforms in the same *units* the
resource system expects. Read how the return value is consumed (resource up-to-date checks)
before choosing between `st_mtime` seconds and `st_mtim` nanoseconds — a mismatch here causes
spurious or missed resource recompiles.

### P1.5 — `PlatformUtils_Linux.{h,cpp}`

**New:** `Code/Base/Platform/PlatformUtils_Linux.h`, `PlatformUtils_Linux.cpp`

Mirror `EE::Platform::Win32` as `EE::Platform::Linux`:

| Function | Linux mechanism |
|---|---|
| `GetShortPath` / `GetLongPath` | No 8.3 paths on Linux — pass through unchanged |
| `GetProcessID( char const* processName )` | Scan `/proc/*/comm` |
| `GetProcessPath( uint32_t processID )` | `readlink( "/proc/<pid>/exe" )` |
| `GetCurrentModulePath()` | `readlink( "/proc/self/exe" )` |
| `GetLastErrorMessage()` | `strerror_r( errno )` |
| `StartProcess( char const* exePath, char const* cmdLine )` | `fork` + `execv`; return the child pid |
| `KillProcess( uint32_t processID )` | `kill( pid, SIGTERM )` |
| `OpenInExplorer( char const* path )` | `xdg-open` |

Note `IsProcessRunning` is an inline in the header that calls `GetProcessID`; keep it inline in
the Linux header too.

**Callers matter here.** Outside the `_Win32` files, 21 call sites reference
`Platform::Win32::` directly (the sites inside `_Win32.cpp` files are fine — they compile out).
Twelve of the 21 already sit inside existing `#if _WIN32` blocks and need nothing; the nine
unguarded ones are handled per
[TouchedFiles.md](../TouchedFiles.md#small-additive-edits):

| Caller (sites) | Functions used | State | Handled in |
|---|---|---|---|
| `Code/Base/_Module/BaseModule.cpp` (6) | `GetCurrentModulePath`, `GetProcessID`, `GetProcessPath`, `KillProcess`, `StartProcess` | already fully guarded (`#if _WIN32` … `#else return false;`, lines 20–67) — **no change needed**; on Linux it simply declines to start a resource server | — (real Linux server startup is Phase 7) |
| `Code/Base/Profiling.cpp` (2) | `GetCurrentModulePath`, `StartProcess` | already `#if _WIN32` guarded | — |
| `Code/EngineTools/FileSystem/FileSystemWatcher.cpp` (4) | `GetLastErrorMessage` | already inside its `#if _WIN32` body (line 20); only the unguarded includes at lines 8–16 need guarding | 3 |
| `Code/Applications/Reflector/TypeReflection/Clang/ClangParser.cpp:75` (1) | `GetShortPath` | unguarded | 2 |
| `Code/EngineTools/Resource/Tools/EditorTool_ResourceBrowser.cpp` (3) | `OpenInExplorer` | unguarded | 3 |
| `Code/EngineTools/Resource/Tools/EditorTool_ResourceImporter.cpp` (1) | `OpenInExplorer` | unguarded | 3 |
| `Code/EngineTools/Widgets/Pickers/ResourcePickers.cpp` (1) | `OpenInExplorer` | unguarded | 3 |
| `Code/EngineTools/Widgets/Pickers/DataPathPicker.cpp` (1) | `OpenInExplorer` | unguarded | 3 |
| `Code/Applications/ResourceServer/ResourceServerUI.cpp` (2) | `OpenInExplorer` | unguarded | 7 |

None of this requires an edit to `BaseModule.cpp` — the original survey's claim that it needs
guard extension was wrong; it is listed "verified as needing no change" in
[TouchedFiles.md](../TouchedFiles.md).

### P1.6 — `Types_Linux.cpp`

**New:** `Code/Base/Types/Platform/Types_Linux.cpp`

Three functions:
- `UUID::GenerateID()` — Win32 uses `CoCreateGuid`. Prefer `getrandom(2)` and format a v4 UUID
  by hand over adding a `libuuid` dependency; the engine only needs uniqueness. Preserve the
  `static_assert( sizeof( GUID ) == sizeof( UUID ) )` intent by asserting `sizeof( UUID ) == 16`.
- `StringUtils::CompareInsensitive( char const*, char const* )` → `strcasecmp`
- `StringUtils::CompareInsensitive( char const*, char const*, size_t )` → `strncasecmp`

### P1.7 — `SystemLog_Linux.cpp`

**New:** `Code/Base/Logging/Platform/SystemLog_Linux.cpp`

One function, `SystemLog::TraceMessage`. Win32 writes to `OutputDebugStringA`; Linux writes to
`stderr`. Use `vsnprintf` in place of `_vsnprintf_s`, and `"\n"` rather than `"\r\n"`.

> The Win32 version has a bug: it guards the newline append with `numCharsWritten < 509` while
> the buffer is 2048 bytes, so messages between 509 and 2045 characters silently lose their
> newline. Write the Linux version correctly against the real buffer size; record the
> discrepancy in [Progress.md](../Progress.md); do not touch the Win32 file.

### P1.8 — `Platform_Linux.cpp` — crash handling and stack walking

**New:** `Code/Base/Platform/Platform_Linux.cpp`

The largest task in this phase (Win32 version is 249 lines). Implements:

| Win32 | Linux |
|---|---|
| `WalkStack( PCONTEXT )` via `StackWalk64` + `SymInitializeW` | `libunwind` (`unw_backtrace`) or `backtrace()`; symbolise with `libdw` or `dladdr` |
| `GenerateCrashDump( EXCEPTION_POINTERS* )` | Write a text report; core dumps are the kernel's job via `ulimit -c` / `core_pattern` |
| `VectoredExceptionHandler` | `sigaction` for `SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL`, `SIGABRT` |
| `Platform::Initialize()` / `Shutdown()` | install/remove handlers |

The `Symbol` type and `WalkStack` return shape are defined by the shared header — match them.

**Signal handlers must be async-signal-safe.** The Win32 code allocates strings and formats
messages inside the handler, which is legal on Windows but undefined behaviour in a POSIX signal
handler. Do not copy that structure literally: capture the backtrace in the handler using only
safe calls, and defer formatting, or accept the risk deliberately and comment it. This is a case
where mirroring the Win32 implementation exactly would be wrong.

### P1.9 — RHI stub

**New:** `Code/Base/Render/RHI_Vulkan.cpp`

Per the section at the top of this document. Every function in `RHI.h` — around 110 of them —
gets a signature-complete definition that calls `EE_UNIMPLEMENTED_FUNCTION()`.

This is mechanical and a good candidate for generation: parse the declarations out of `RHI.h`
and emit the stub bodies, then hand-check. Group the stubs with the same section comments
`RHI.h` uses, so Phase 5 can fill them in group by group and the file stays navigable.

Also ensure `Code/Base/Render/RenderWindow.cpp` compiles — it is platform-neutral (it only
passes `m_pNativeWindowHandle` through) and should need nothing.

### P1.10 — Link `Esoterica.Base` and run `Tester`

**Edits:** the three remaining upstream source edits, as
[TouchedFiles.md](../TouchedFiles.md) records them:

- `Code/Base/Settings/IniFile.cpp:6,152` — the file-wide `#if defined( _MSC_VER )` (line 4)
  wraps the *entire* implementation and closes at line 153, so on clang the file compiles to
  nothing and every `IniFile` symbol is undefined at link. Insert `#endif` after line 6 (the
  `#pragma warning( disable : 4866 )` line) and re-open `#if defined( _MSC_VER )` before the
  `#pragma warning( pop )` at line 152. Two lines added, zero modified.
- `Code/Base/Memory/Memory.h:23–27` — the non-Windows `#else` defines `EE_STACK_ALLOC` /
  `EE_STACK_ARRAY_ALLOC` as **empty** macros, so call sites become syntax errors. Give the
  `#else` a real `alloca` and add a Linux-guarded `#include <alloca.h>`.
- `Code/Base/Memory/Memory.cpp:231–252` — `VirtualMemoryReserve` / `VirtualMemoryCommit` /
  `VirtualMemoryFree` are **completely unguarded** (no `#else` exists;
  [open question 6](../Progress.md#open-questions) was answered 2026-08-25: there is no
  non-Windows path). Wrap the three functions in `#ifdef _WIN32` / `#else` / `#endif` and
  implement Linux as `mmap( MAP_PRIVATE | MAP_ANONYMOUS, PROT_NONE )` reserve,
  `mprotect( PROT_READWRITE )` commit, `munmap` free, with `__sync_add_and_fetch` in place of
  `InterlockedAdd64`. Keep `s_totalVirtualMemoryCommitted` (line 229) unguarded —
  `GetTotalRequestedMemory` (line 256) uses it on every platform.

Then resolve remaining compile/link errors. Expect issues in:
- `rpmalloc` — vendored and cross-platform, but confirm its Linux configuration path.
- `GameNetworkingSockets` — `Base` imports it, so it may block the first link. This is
  [open question 5](../Progress.md#open-questions). If it blocks, build it now.
- EASTL / EABase — vendored, cross-platform. Expect minor `-Wall` noise; do not fix upstream
  warnings, adjust flags instead.

Finally, make `Esoterica.Applications.Tester` build and run. It is an empty console app whose
only job here is to prove the `.so` loads and `Base` initialises.

---

## Acceptance criteria

1. `libEsoterica.Base.so` builds in Debug and Release; `libEsoterica.Base.a` in Shipping.
2. Zero remaining compile errors in `Esoterica.Base`.
3. `nm -D --defined-only libEsoterica.Base.so | grep -c ' T .*RHI'` accounts for every function
   declared in `RHI.h` — i.e. the stub is signature-complete. A link failure downstream in
   Phase 2/3 means it is not.
4. `Esoterica.Applications.Tester` links and runs to completion with exit code 0.
5. `Threading::Initialize` / `Shutdown`, `Platform::Initialize` / `Shutdown`,
   `FileSystem::Exists`, `UUID::GenerateID`, and `SystemLog::TraceMessage` all work — demonstrate
   with a temporary scratch program (do not commit it, or commit it under `Tester`).
6. A deliberately induced `SIGSEGV` produces a readable backtrace with symbol names.
7. Every upstream file edited appears in [TouchedFiles.md](../TouchedFiles.md) with status
   `done`.
8. **The Windows MSBuild build still succeeds**, unchanged.
9. `git diff --stat upstream/main -- Code/Base/Esoterica.h Code/Base/Math/Math.h` shows **2
   lines added per file, 0 modified**.

## Do not

- Modify `RHI.h`.
- Implement any real Vulkan calls — that is Phase 5.
- Touch `Application_Win32.*`, the imgui backend, or input devices — that is Phase 6.
- Refactor `Win32Application` into a shared base class.
- Fix the two upstream bugs noted in P1.2 and P1.7. Record them.
- Add `libuuid` if `getrandom` will do.

## Notes for the next agent

Record in [Progress.md](../Progress.md):
- Which `SyncEvent` reset semantics you matched, and how you determined it.
- What units `GetFileModifiedTime` returns and why.
- The resolution of open questions 5 and 6.
- Whether `GameNetworkingSockets` had to be built.
- The two upstream bugs, under "Upstream issues observed".
