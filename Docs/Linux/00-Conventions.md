# Conventions

**Read this every session before you touch code.** These rules keep `git merge upstream/main`
cheap. They matter more than elegance, and more than your own view of how the code should look.

These are rules about *the code*. For rules about *the workflow*, such as branching, commits,
and what counts as done, see [/AGENTS.md](../../AGENTS.md).

---

## Rule 1 - Prefer a new file to an edited file

Put a platform-specific implementation in a **new file with a `_Linux` suffix**, next to the
existing `_Win32` sibling.

```
Code/Base/Threading/Platform/Threading_Win32.cpp      <- upstream, do not touch
Code/Base/Threading/Platform/Threading_Linux.cpp      <- yours
```

Open every new `.cpp` with a platform guard so it is inert on Windows. This mirrors the upstream
convention:

```cpp
#ifdef __linux__
#include "..."

//-------------------------------------------------------------------------

namespace EE::Threading
{
    // ...
}
#endif
```

Give every new `.h` the same guard placement as its Win32 sibling. Upstream is inconsistent
here. `Platform_Win32.h` puts `#ifdef _WIN32` *before* `#pragma once`, and `Application_Win32.h`
puts it *after*. **Match the sibling you are mirroring**, so the two files diff cleanly against
each other.

## Rule 2 - Edits to upstream files are `#elif` additions only

A shared file usually needs to know about Linux only because it already has an `#if _WIN32`
include-switch. Add a sibling branch, and nothing else.

```cpp
// Code/Base/Esoterica.h, at line 55 - before:
#if _WIN32
#include "Platform/Platform_Win32.h"
#endif

// after:
#if _WIN32
#include "Platform/Platform_Win32.h"
#elif defined( __linux__ )
#include "Platform/Platform_Linux.h"
#endif
```

Such an edit has three constraints:

- **Two lines added, zero lines modified, zero lines removed.** If you start to rewrite the
  surrounding block, stop and reconsider the design.
- **Do not improve the existing guard.** Upstream uses both `#if _WIN32` and `#ifdef _WIN32`.
  Leave each one as you found it. Do not normalize them.
- **Record the edit in [TouchedFiles.md](TouchedFiles.md)** in the same commit. An edit that is
  missing from the registry is a bug.

[TouchedFiles.md](TouchedFiles.md) already lists every file expected to need this. **If your
task needs an edit to a file that is not on that list, your approach is probably wrong.** Stop
and escalate instead of widening the blast radius.

## Rule 3 - Change nothing you were not asked to change

While you work in this repository you may **not**:

- Reformat code, or run `clang-format` over an existing file.
- Fix compiler warnings in upstream code that your task does not touch.
- Rename anything.
- Reorder or tidy `#include` blocks.
- Convert `#if` to `#ifdef`, or the reverse.
- Replace a raw loop with an algorithm, or a macro with a `constexpr`.
- Delete dead code, commented-out code, or unused variables.
- Add or remove blank lines in an upstream file.
- Fix typos in upstream comments.

Each one of these causes a merge conflict later, and gains nothing. If you find a real upstream
bug, note it in [Progress.md](Progress.md) under "Upstream issues observed" and move on. Do not
fix it here. File it upstream as an issue instead. Upstream accepts bug reports, and rejects
large PRs.

## Rule 4 - Never disable something globally to make it compile

When a Windows-only middleware blocks your build, leave its `EE_ENABLE_*` define unset in the
*Linux build configuration*. See [03-Dependencies.md](03-Dependencies.md). Do not `#if 0` its
call sites, and do not strip its `#include` lines.

The optional middleware toggles are `EE_ENABLE_LPP`, `EE_ENABLE_SUPERLUMINAL`, and
`EE_ENABLE_NAVPOWER`. All three stay off unless the matching MSBuild user macro is set. Leaving
them unset in the ninja generator is enough, and needs no source change.

## Rule 5 - Upstream owns `Code/**/ThirdParty/`

Do not add, remove, or modify anything under `Code/Base/ThirdParty/`,
`Code/Engine/ThirdParty/`, or `Code/EngineTools/ThirdParty/`. Upstream vendors those directories
and will overwrite your changes.

Linux-only third-party code, such as SDL3, the Vulkan headers, VMA, and SPIRV-Reflect, comes in
as an external dependency under `External/`, which `.gitignore` covers. This matches how the
Windows dependencies already work. See [03-Dependencies.md](03-Dependencies.md).

If a vendored library really will not compile on Linux, fix it in the build generator with a
`-D`, a flag, or an exclusion. Do not edit the vendored source.

## Rule 6 - Do not touch `Data/`

Compiled resource data is already platform-neutral. `DataPath` hardcodes `'/'` as its delimiter
(`Code/Base/FileSystem/DataPath.h:47`), separate from the filesystem path delimiter. This port
has no data migration. If you think a data file needs a change, you have found a real design
problem. Escalate, do not edit.

## Rule 7 - Linux build tooling lives in two places only

```
Code/Scripts/NinjaGen/       <- the build generator (upstream already has this dir)
Docs/Linux/                  <- this plan
```

Plus `AGENTS.md` at the repo root, which holds the development workflow. Upstream has no such
file, so it is purely additive and cannot conflict.

Plus these root-level scripts, which mirror the existing `.bat` files:

```
DownloadDependencies.sh      <- mirrors DownloadDependencies.bat
RunReflection.sh             <- mirrors RunReflection.bat
CompileShaders.sh            <- mirrors CompileShaders.bat
```

Do not scatter `Makefile`, `CMakeLists.txt`, or shell script files through `Code/`.

## Rule 8 - Match the surrounding code style

The project ships `Code/.clang-format` (`BasedOnStyle: Microsoft`). Follow these points by hand
in new files:

- **Allman braces.** The opening brace goes on its own line, always.
- **Spaces inside parentheses**: `if ( x == y )`, `Foo( a, b )`. This is unusual and easy to
  forget. It is the most visible style marker in this codebase.
- **4-space indent, never tabs.** There is no column limit.
- **Pointer on the left**: `char const* pName`, not `char const *pName`.
- `AccessModifierOffset: -4`, so `public:` and `private:` sit one level out.
- Members take an `m_` prefix. Pointers take `p`. Statics take `s_`. Globals take `g_`.
- `const` goes on the right of the type: `char const*`, `String const&`, `bool const isFoo`.
- A section separator is exactly 73 hyphens after `//`:
  ```cpp
  //-------------------------------------------------------------------------
  ```
- Macros and public defines take an `EE_` prefix and use SCREAMING_CASE.
- Enums are `enum class` or `enum struct`. Both appear upstream, and `RHI.h` uses
  `enum struct`.

Do not run `clang-format` even on your own new files if the output would differ from the
hand-written style of the sibling `_Win32` file. Consistency with the neighbor beats conformance
to the config.

## Rule 9 - Every task updates the bookkeeping

- [TouchedFiles.md](TouchedFiles.md) lists any upstream file you edited, with the reason.
- [Progress.md](Progress.md) records what you did, and anything the next agent needs to know.
- [Blocked.md](Blocked.md) gets a row for anything you wrote but could not verify, and which
  machine would unblock it.

This keeps a chain of independent agent sessions coherent. Skipping it is the most expensive
shortcut available to you. See
[/AGENTS.md, Definition of done](../../AGENTS.md#definition-of-done) for the full checklist.

**Progress.md is where the investigation goes, not the PR description.** What you measured, what
misled you, and what the next session should not repeat all belong here. The PR describes the
change and how it solves the problem. See
[/AGENTS.md, What the PR description says](../../AGENTS.md#what-the-pr-description-says).

## Rule 10 - Report honestly

If a phase does not meet its acceptance criteria, say so plainly, and say which ones. Do not
mark a task complete because most of it works. Do not describe a stub as an implementation. A
truthful "Vulkan buffer creation done. Texture creation is stubbed and asserts" helps the next
agent far more than an optimistic summary.

---

## Naming reference

| Kind | Pattern | Example |
|---|---|---|
| Platform impl source | `<System>_Linux.cpp` | `Threading_Linux.cpp` |
| Platform impl header | `<System>_Linux.h` | `Platform_Linux.h` |
| Platform helper namespace | `EE::Platform::Linux` | mirrors `EE::Platform::Win32` |
| Application base class | `LinuxApplication` | mirrors `Win32Application` |
| App entry point | `<App>Application_Linux.cpp` | `EditorApplication_Linux.cpp` |
| RHI backend | `RHI_Vulkan.cpp` | mirrors `RHI_Direct3D12.cpp` |
| Compile guard | `__linux__` | never `LINUX`, never `__unix__` |

The stale upstream `NinjaGen.py` uses `-D__LINUX__`. **Do not use `__LINUX__`.** Use the
compiler-provided `__linux__`, which needs no `-D` and cannot get out of sync. Remove
`-D__LINUX__` from the toolchain flags when you rework that file. That counts as a
`NinjaGen.py` change, which is fine.
[01-UpstreamMerges.md](01-UpstreamMerges.md) explains why that file is a special case.

## Escalation triggers

Stop and ask a human when:

- Your task needs an edit to an upstream file that [TouchedFiles.md](TouchedFiles.md) does not
  list.
- Your task needs a public signature change in a shared header.
- Your task needs a change to anything under `Code/**/ThirdParty/`.
- Your task needs a change to `Data/`.
- A shared abstraction genuinely cannot express what Linux needs. For example, an `RHI.h`
  concept that is Direct3D-shaped in a way the survey missed.
- Two phases appear to conflict.
