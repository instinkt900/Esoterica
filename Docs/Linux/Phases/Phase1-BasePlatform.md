# Phase 1 - Base Platform Layer

**Goal:** `Esoterica.Base` compiles and links on Linux, headless.

**Deliverable:** `libEsoterica.Base.so` exists, and a small program that calls into it
(`Esoterica.Applications.Tester`) links and runs.

**Prerequisites:** Phase 0 complete.

**Rough cost:** 2-3 weeks.

**Read first:** [00-Conventions.md](../00-Conventions.md),
[02-Architecture.md, Platform abstraction layer](../02-Architecture.md#platform-abstraction-layer),
[TouchedFiles.md](../TouchedFiles.md).

---

## The critical design point: the RHI stub

`Code/Base/Render/RHI_Direct3D12.cpp` belongs to `Esoterica.Base.vcxproj` (line 568), and **55
files across `Engine` and `EngineTools` call `RHI::` functions.** `Base` must therefore export
the full RHI surface of about 110 functions, or nothing downstream links.

So **Phase 1 must produce a stub `Code/Base/Render/RHI_Vulkan.cpp`**. Every function that
`RHI.h` declares gets a definition that halts immediately:

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

This unblocks Phases 2, 3, and 4 completely. They need `Base` and `EngineTools` to *link*, not
to render. It is the most important structural decision in the plan's phasing.

Use `EE_UNIMPLEMENTED_FUNCTION()`, which `Esoterica.h` defines. Do not return silently. A stub
that returns quietly causes a baffling failure in Phase 6. A stub that halts tells you exactly
which function Phase 5 still owes you.

**Do not** modify `RHI.h`. **Do not** guard `RHI_Direct3D12.cpp`. It is already Windows-only in
effect, and the build generator excludes it by platform. It has no `_Win32` suffix, so add it to
the generator's exclusion logic by filename, in Phase 0 or here, and note that in `Progress.md`.

---

## Tasks

Tasks P1.1 to P1.8 are mostly independent, so agents can run them in parallel. P1.9 and P1.10
depend on the rest.

### P1.1 - `Platform_Linux.h`, module API visibility

**New:** `Code/Base/Platform/Platform_Linux.h`
**Edits:** `Code/Base/Esoterica.h:55`, and all six `_Module/API.h` files.

Mirror `Platform_Win32.h`. It must define:

| Macro | Win32 | Linux |
|---|---|---|
| `EE_FORCE_INLINE` | `__forceinline` | `__attribute__(( always_inline )) inline` |
| `EE_DEBUG_BREAK()` | `__debugbreak()` | `__builtin_debugtrap()` (clang), or `raise( SIGTRAP )` |
| `EE_DISABLE_OPTIMIZATION` | `__pragma( optimize( "", off ) )` | `_Pragma( "clang optimize off" )` |
| `EE_ENABLE_OPTIMIZATION` | `__pragma( optimize( "", on ) )` | `_Pragma( "clang optimize on" )` |

Match the sibling's guard placement. `Platform_Win32.h` puts `#ifdef _WIN32` **before**
`#pragma once`, so `Platform_Linux.h` puts `#ifdef __linux__` before `#pragma once` too.

In the six `API.h` files, the export macro becomes:

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

The import case needs no attribute on ELF. The files are `Code/Base/_Module/API.h`,
`Code/Engine/_Module/API.h`, `Code/EngineTools/_Module/API.h`, `Code/Game/_Module/API.h`,
`Code/GameTools/_Module/API.h`, and `Code/Applications/Tester/_Module/API.h`. Land them in one
commit.

### P1.2 - `Math_Linux.h`

**New:** `Code/Base/Math/Platform/Math_Linux.h`
**Edit:** `Code/Base/Math/Math.h:12`

Twenty lines. `Math_Win32.h` provides one function, `GetMostSignificantBit( uint64_t )`, using
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

> **There is an upstream bug here.** `Math_Win32.h` casts the argument to `unsigned long`
> (`_BitScanReverse64( &index, (unsigned long) value )`). That truncates to 32 bits, so the
> function is wrong for values above `2^32`. **Do not copy the bug, and do not fix the Win32
> file** (Conventions rule 3). Write the Linux version correctly, and record the difference in
> [Progress.md](../Progress.md) under "Upstream issues observed" so someone can report it
> upstream. Flag it clearly. A silent behavior difference between platforms is worse than
> either bug alone.

The project's SIMD code (`Vector.h`, `Matrix.h`, `Quaternion.h`, `Plane.h`, `SIMD.h`,
`Quantization.h`) uses `<immintrin.h>` intrinsics directly, and clang supports them natively.
Expect no changes there, but confirm it.

### P1.3 - `Threading_Linux.cpp`

**New:** `Code/Base/Threading/Platform/Threading_Linux.cpp`

Implement against `Code/Base/Threading/Threading.h`:

| Function | Linux mechanism |
|---|---|
| `GetProcessorInfo()`, returning `{ m_numPhysicalCores, m_numLogicalCores }` | Parse `/sys/devices/system/cpu/` or `/proc/cpuinfo`. `sysconf( _SC_NPROCESSORS_ONLN )` gives the logical count only. The physical count needs core-id deduplication. |
| `GetCurrentThreadID()` | `gettid()`, or `syscall( SYS_gettid )` |
| `SetCurrentThreadName( char const* )` | `pthread_setname_np`. **Note the 16-byte limit, including the NUL.** Truncate instead of failing. |
| `IsMainThread()` | Compare against the tid captured in `Initialize`. |
| `Initialize( char const* pMainThreadName )` and `Shutdown()` | Store the main tid, and set its name. |
| `SyncEvent::Signal`, `Reset`, `Wait`, `Wait(Milliseconds)` | `m_pNativeHandle` is a `void*`. Allocate a pthread mutex and condvar pair, or use `eventfd`. A condvar fits the timed wait better. |

`SyncEvent` is auto-reset or manual-reset, depending on how the code uses `Reset()`. Read
`Threading_Win32.cpp` and the call sites to find out which mode Win32 `CreateEvent` used, and
match it exactly. Getting this wrong produces intermittent hangs that cost a great deal to debug
later.

### P1.4 - `FileSystem_Linux.cpp` and `FileSystemPath_Linux.cpp`

**New:** `Code/Base/FileSystem/Platform/FileSystem_Linux.cpp`,
`Code/Base/FileSystem/Platform/FileSystemPath_Linux.cpp`

`FileSystemPath_Linux.cpp` must define:

```cpp
char const Path::s_pathDelimiter = '/';
```

It also needs `Path::EnsureCorrectPathStringFormat()`, `Path::GetFullPathString()` (use
`realpath`), and `Path::GetCorrectCaseForPath()`.

> **`GetCorrectCaseForPath` is a Windows concept.** On a case-sensitive filesystem, the correct
> case *is* the case given. Implement it as a pass-through that returns the input unchanged, and
> comment why. Do not try to emulate case-insensitive resolution.

`FileSystem_Linux.cpp` implements `Exists`, `IsReadOnly`, `IsExistingFile`,
`IsExistingDirectory`, `IsFileReadOnly`, `GetFileModifiedTime`, `WriteFileToDisk`, and
`ReadBinaryFile`, using `stat`, `access`, `open`, `read`, and `write`. See
`Code/Base/FileSystem/FileSystem.h` for the full exported surface, and `FileSystem_Win32.cpp`
for the semantics.

`GetFileModifiedTime` must return a value in the same *units* the resource system expects, and
comparable across platforms. Read how the callers use the return value, which is for resource
up-to-date checks, before you choose between `st_mtime` seconds and `st_mtim` nanoseconds. A
mismatch here causes spurious or missed resource recompiles.

### P1.5 - `PlatformUtils_Linux.{h,cpp}`

**New:** `Code/Base/Platform/PlatformUtils_Linux.h`, `PlatformUtils_Linux.cpp`

Mirror `EE::Platform::Win32` as `EE::Platform::Linux`:

| Function | Linux mechanism |
|---|---|
| `GetShortPath` and `GetLongPath` | Linux has no 8.3 paths. Pass the input through unchanged. |
| `GetProcessID( char const* processName )` | Scan `/proc/*/comm`. |
| `GetProcessPath( uint32_t processID )` | `readlink( "/proc/<pid>/exe" )` |
| `GetCurrentModulePath()` | `readlink( "/proc/self/exe" )` |
| `GetLastErrorMessage()` | `strerror_r( errno )` |
| `StartProcess( char const* exePath, char const* cmdLine )` | `fork` and `execv`. Return the child pid. |
| `KillProcess( uint32_t processID )` | `kill( pid, SIGTERM )` |
| `OpenInExplorer( char const* path )` | `xdg-open` |

`IsProcessRunning` is an inline in the header that calls `GetProcessID`. Keep it inline in the
Linux header too.

**The callers matter here.** About 30 call sites reference `Platform::Win32::` directly. Most
sit inside other `_Win32.cpp` files, which is fine because they compile out. These are in
platform-neutral code, and need attention in this phase or a later one:

| Caller | Functions used | Phase |
|---|---|---|
| `Code/Base/_Module/BaseModule.cpp` | `GetCurrentModulePath`, `GetProcessID`, `GetProcessPath`, `KillProcess`, `StartProcess` | 1 |
| `Code/Base/Profiling.cpp` | `GetCurrentModulePath`, `StartProcess` | already `#if _WIN32` guarded, no change |
| `Code/EngineTools/**` (6 files) | `OpenInExplorer` | 7 |
| `Code/Applications/ResourceServer/ResourceServerUI.cpp` | `OpenInExplorer` | 7 |
| `Code/Applications/Reflector/TypeReflection/Clang/ClangParser.cpp` | `GetShortPath` | 2 |

`BaseModule.cpp` already has `#ifdef _WIN32` guards at lines 9 and 20, around the
`EnsureResourceServerIsRunning` body. Prefer extending the existing guard to
`#if _WIN32 || defined( __linux__ )` and calling through a platform-neutral alias. Do not
duplicate the function body. **If that needs more than a 2-line change, escalate.** This is
exactly the kind of edit that grows if you are not careful.

### P1.6 - `Types_Linux.cpp`

**New:** `Code/Base/Types/Platform/Types_Linux.cpp`

Three functions:

- `UUID::GenerateID()`. Win32 uses `CoCreateGuid`. Prefer `getrandom(2)` plus a hand-written v4
  UUID format over adding a `libuuid` dependency, because the engine only needs uniqueness.
  Keep the intent of `static_assert( sizeof( GUID ) == sizeof( UUID ) )` by asserting
  `sizeof( UUID ) == 16`.
- `StringUtils::CompareInsensitive( char const*, char const* )`, which maps to `strcasecmp`.
- `StringUtils::CompareInsensitive( char const*, char const*, size_t )`, which maps to
  `strncasecmp`.

### P1.7 - `SystemLog_Linux.cpp`

**New:** `Code/Base/Logging/Platform/SystemLog_Linux.cpp`

One function, `SystemLog::TraceMessage`. Win32 writes to `OutputDebugStringA`. Linux writes to
`stderr`. Use `vsnprintf` instead of `_vsnprintf_s`, and `"\n"` instead of `"\r\n"`.

> The Win32 version has a bug. It guards the newline append with `numCharsWritten < 509`, but
> the buffer is 2048 bytes, so messages between 509 and 2045 characters silently lose their
> newline. Write the Linux version correctly against the real buffer size. Record the difference
> in [Progress.md](../Progress.md). Do not touch the Win32 file.

### P1.8 - `Platform_Linux.cpp`, crash handling and stack walking

**New:** `Code/Base/Platform/Platform_Linux.cpp`

This is the largest task in the phase. The Win32 version is 249 lines. It implements:

| Win32 | Linux |
|---|---|
| `WalkStack( PCONTEXT )`, using `StackWalk64` and `SymInitializeW` | `libunwind` (`unw_backtrace`), or `backtrace()`. Resolve symbols with `libdw` or `dladdr`. |
| `GenerateCrashDump( EXCEPTION_POINTERS* )` | Write a text report. Core dumps are the kernel's job, through `ulimit -c` and `core_pattern`. |
| `VectoredExceptionHandler` | `sigaction` for `SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL`, and `SIGABRT`. |
| `Platform::Initialize()` and `Shutdown()` | Install and remove the handlers. |

The shared header defines the `Symbol` type and the `WalkStack` return shape. Match them.

**Signal handlers must be async-signal-safe.** The Win32 code allocates strings and formats
messages inside the handler. That is legal on Windows, but it is undefined behavior in a POSIX
signal handler. Do not copy that structure. Capture the backtrace in the handler using only safe
calls, and defer the formatting. Or accept the risk deliberately and comment it. This is a case
where copying the Win32 implementation exactly would be wrong.

### P1.9 - RHI stub

**New:** `Code/Base/Render/RHI_Vulkan.cpp`

Follow the section at the top of this document. Every function in `RHI.h`, about 110 of them,
gets a signature-complete definition that calls `EE_UNIMPLEMENTED_FUNCTION()`.

This is mechanical work, and a good candidate for generation. Parse the declarations out of
`RHI.h`, emit the stub bodies, then check them by hand. Group the stubs under the same section
comments that `RHI.h` uses, so that Phase 5 can fill them in group by group and the file stays
easy to navigate.

Also confirm that `Code/Base/Render/RenderWindow.cpp` compiles. It is platform-neutral, because
it only passes `m_pNativeWindowHandle` through, so it should need nothing.

### P1.10 - Link `Esoterica.Base` and run `Tester`

**Edit:** `Code/Base/Settings/IniFile.cpp:4`. Add an `#else` branch that includes
`Base/ThirdParty/mINI/ini.h` without the MSVC `#pragma warning` pair. The include currently sits
*inside* `#if defined(_MSC_VER)`, so on clang it never happens and the file cannot compile.

Then resolve the remaining link errors. Expect problems in:

- `Code/Base/Memory/Memory.cpp`. Confirm that the `VirtualAlloc` region (`PageAllocator`, near
  line 234) has a working non-Windows path. `mmap` with `PROT_NONE`, then `mprotect`, is the
  equivalent of `MEM_RESERVE` then `MEM_COMMIT`. This is
  [open question 6](../Progress.md#open-questions).
- `rpmalloc`. It is vendored and cross-platform, but confirm its Linux configuration path.
- `GameNetworkingSockets`. `Base` imports it, so it may block the first link. This is
  [open question 5](../Progress.md#open-questions). If it blocks, build it now.
- EASTL and EABase. Both are vendored and cross-platform. Expect minor `-Wall` noise. Do not fix
  upstream warnings. Adjust the flags instead.

Finally, make `Esoterica.Applications.Tester` build and run. It is an empty console app, and its
only job here is to prove that the `.so` loads and `Base` initializes.

---

## Acceptance criteria

1. `libEsoterica.Base.so` builds in Debug and Release. `libEsoterica.Base.a` builds in Shipping.
2. `Esoterica.Base` has no remaining compile errors.
3. `nm -D --defined-only libEsoterica.Base.so | grep -c ' T .*RHI'` accounts for every function
   that `RHI.h` declares, which means the stub is signature-complete. A link failure downstream
   in Phase 2 or 3 means it is not.
4. `Esoterica.Applications.Tester` links, runs to completion, and exits 0.
5. `Threading::Initialize` and `Shutdown`, `Platform::Initialize` and `Shutdown`,
   `FileSystem::Exists`, `UUID::GenerateID`, and `SystemLog::TraceMessage` all work. Show this
   with a temporary scratch program. Do not commit it, or commit it under `Tester`.
6. A deliberate `SIGSEGV` produces a readable backtrace with symbol names.
7. Every upstream file you edited appears in [TouchedFiles.md](../TouchedFiles.md) with status
   `done`.
8. **The Windows MSBuild build still succeeds**, unchanged.
9. `git diff --stat upstream/main -- Code/Base/Esoterica.h Code/Base/Math/Math.h` shows **2
   lines added per file, and 0 modified**.

## Do not

- Modify `RHI.h`.
- Implement any real Vulkan calls. That is Phase 5.
- Touch `Application_Win32.*`, the imgui backend, or the input devices. That is Phase 6.
- Refactor `Win32Application` into a shared base class.
- Fix the two upstream bugs noted in P1.2 and P1.7. Record them.
- Add `libuuid` if `getrandom` will do.

## Notes for the next agent

Record this in [Progress.md](../Progress.md):

- Which `SyncEvent` reset semantics you matched, and how you worked it out.
- What units `GetFileModifiedTime` returns, and why.
- The answers to open questions 5 and 6.
- Whether you had to build `GameNetworkingSockets`.
- The two upstream bugs, under "Upstream issues observed".
