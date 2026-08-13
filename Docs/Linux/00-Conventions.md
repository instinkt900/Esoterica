# Conventions

**Read this every session before touching code.** These rules exist to keep
`git merge upstream/main` cheap. They are more important than elegance, and more important
than your own judgement about how the code "should" look.

These are rules about *the code*. For rules about *the workflow* — branching, commits, what
counts as done — see [/AGENTS.md](../../AGENTS.md).

---

## Rule 1 — Prefer new files to edited files

Platform-specific implementations go in a **new file with a `_Linux` suffix**, placed beside
the existing `_Win32` sibling.

```
Code/Base/Threading/Platform/Threading_Win32.cpp      <- upstream, do not touch
Code/Base/Threading/Platform/Threading_Linux.cpp      <- yours
```

Every new `.cpp` opens with a platform guard so it is inert on Windows, mirroring the
upstream convention:

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

Every new `.h` uses the same guard placement as its Win32 sibling. Note that upstream is
inconsistent here — `Platform_Win32.h` puts `#ifdef _WIN32` *before* `#pragma once`, while
`Application_Win32.h` puts it *after*. **Match whichever sibling you are mirroring**, so the
two files diff cleanly against each other.

## Rule 2 — Edits to upstream files are `#elif` additions only

When a shared file must know about Linux, it is almost always because it already has an
`#if _WIN32` include-switch. Add a sibling branch. Nothing else.

```cpp
// Code/Base/Esoterica.h, at line 55 — before:
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

Constraints on such an edit:

- **Two lines added, zero lines modified, zero lines removed.** If you find yourself
  rewriting the surrounding block, stop and reconsider the design.
- **Do not "improve" the existing guard.** `#if _WIN32` and `#ifdef _WIN32` are both used
  upstream. Leave each as you found it; do not normalise.
- **Record it in [TouchedFiles.md](TouchedFiles.md)** in the same commit. An edit that is not
  in the registry is a bug.

The complete set of files expected to need this is already enumerated in
[TouchedFiles.md](TouchedFiles.md). **If your task requires editing a file that is not on
that list, that is a signal your approach is wrong.** Stop and escalate rather than
expanding the blast radius.

## Rule 3 — Change nothing you were not asked to change

Specifically, while working in this repository you may **not**:

- Reformat code, or run `clang-format` over an existing file.
- Fix compiler warnings in upstream code that are unrelated to your task.
- Rename anything.
- Reorder or tidy `#include` blocks.
- Convert `#if` to `#ifdef` or vice versa.
- Replace a raw loop with an algorithm, a macro with a `constexpr`, etc.
- Delete dead code, commented-out code, or unused variables.
- Add or remove blank lines in an upstream file.
- Fix typos in upstream comments.

Every one of these produces a merge conflict later, for zero functional gain. If you spot a
genuine upstream bug, note it in [Progress.md](Progress.md) under "Upstream issues observed"
and move on. Do not fix it here — file it upstream as an issue instead
(upstream accepts bug reports; it rejects large PRs).

## Rule 4 — Never disable something globally to make it compile

If a Windows-only middleware blocks your build, the fix is to leave its `EE_ENABLE_*` define
unset in the *Linux build configuration* (see [03-Dependencies.md](03-Dependencies.md)) — not
to `#if 0` its call sites, and not to strip its `#include`s.

The optional middleware toggles are `EE_ENABLE_LPP`, `EE_ENABLE_SUPERLUMINAL`,
`EE_ENABLE_NAVPOWER`. All are already off unless the corresponding MSBuild user macro is
set, so simply not setting them in the ninja generator is sufficient and requires no source
change at all.

## Rule 5 — Upstream owns `Code/**/ThirdParty/`

Do not add, remove, or modify anything under `Code/Base/ThirdParty/`,
`Code/Engine/ThirdParty/`, or `Code/EngineTools/ThirdParty/`. Upstream vendors those and
will overwrite your changes.

Linux-only third-party code (SDL3, Vulkan headers, VMA, SPIRV-Reflect, …) is acquired as an
external dependency into `External/` — which is `.gitignore`d — exactly like the existing
Windows dependencies. See [03-Dependencies.md](03-Dependencies.md).

If a vendored library genuinely will not compile on Linux, the fix goes in the build
generator (a `-D`, a flag, an exclusion), not in the vendored source.

## Rule 6 — Do not touch `Data/`

Compiled resource data is already platform-neutral: `DataPath` hardcodes `'/'` as its
delimiter (`Code/Base/FileSystem/DataPath.h:47`), independent of the filesystem path
delimiter. There is no data migration in this port. If you believe a data file needs
changing, you have found a real design problem — escalate, don't edit.

## Rule 7 — Linux build tooling lives in two places only

```
Code/Scripts/NinjaGen/       <- the build generator (upstream already has this dir)
Docs/Linux/                  <- this plan
```

Plus `AGENTS.md` at the repo root, which holds development workflow. Upstream has no such file,
so it is purely additive and cannot conflict.

Plus these root-level scripts, which mirror the existing `.bat` files:

```
DownloadDependencies.sh      <- mirrors DownloadDependencies.bat
RunReflection.sh             <- mirrors RunReflection.bat
CompileShaders.sh            <- mirrors CompileShaders.bat
```

Do not scatter `Makefile`s, `CMakeLists.txt`, or shell scripts through `Code/`.

## Rule 8 — Match the surrounding code style

The project ships `Code/.clang-format` (`BasedOnStyle: Microsoft`). The salient points, which
you must follow by hand in new files:

- **Allman braces** — opening brace on its own line, always.
- **Spaces inside parentheses**: `if ( x == y )`, `Foo( a, b )`. This is unusual and easy to
  forget; it is the single most visible style marker in this codebase.
- **4-space indent, never tabs.** No column limit.
- **Pointer on the left**: `char const* pName`, not `char const *pName`.
- `AccessModifierOffset: -4` — `public:` / `private:` outdented one level.
- Members prefixed `m_`. Pointers prefixed `p`. Statics `s_`. Globals `g_`.
- `const` on the right of the type: `char const*`, `String const&`, `bool const isFoo`.
- Section separators are exactly 73 hyphens after `//`:
  ```cpp
  //-------------------------------------------------------------------------
  ```
- Macros and public defines are `EE_`-prefixed and `SCREAMING_CASE`.
- Enums are `enum class` / `enum struct` (both appear upstream; `RHI.h` uses `enum struct`).

Do not run `clang-format` even on your own new files if it would produce output that differs
from the hand-written style of the sibling `_Win32` file. Consistency with the neighbour
beats conformance to the config.

## Rule 9 — Every task updates the bookkeeping

- [TouchedFiles.md](TouchedFiles.md) lists any upstream file you edited, with the reason.
- [Progress.md](Progress.md) records what you did and anything the next agent needs to know.

This is how a chain of independent agent sessions stays coherent. Skipping it is the most
expensive shortcut available to you. See
[/AGENTS.md § Definition of done](../../AGENTS.md#definition-of-done) for the full checklist.

## Rule 10 — Report honestly

If a phase's acceptance criteria are not met, say so plainly and say which ones. Do not mark
a task complete because most of it works. Do not describe a stub as an implementation. A
truthful "Vulkan buffer creation done; texture creation is stubbed and asserts" is far more
useful to the next agent than an optimistic summary.

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

Note that upstream's stale `NinjaGen.py` uses `-D__LINUX__`. **Do not use `__LINUX__`.** Use
the compiler-provided `__linux__`, which needs no `-D` and cannot get out of sync. Remove
the `-D__LINUX__` from the toolchain flags when you rework that file (recorded as a
`NinjaGen.py` change, which is fine — see [01-UpstreamMerges.md](01-UpstreamMerges.md) on
why that file is a special case).

## Escalation triggers

Stop and ask a human when:

- Your task requires editing an upstream file not listed in [TouchedFiles.md](TouchedFiles.md).
- Your task requires changing a public signature in a shared header.
- Your task requires modifying anything under `Code/**/ThirdParty/`.
- Your task requires a change to `Data/`.
- A shared abstraction genuinely cannot express what Linux needs (e.g. an `RHI.h` concept
  that is Direct3D-shaped in a way the survey missed).
- Two phases appear to conflict.
