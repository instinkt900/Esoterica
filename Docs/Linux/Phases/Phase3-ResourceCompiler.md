# Phase 3 — Resource Compiler

**Goal:** game data compiles on Linux.

**Deliverable:** `EsotericaResourceCompiler` builds, and compiles the resources under `Data/`
producing output byte-identical to the Windows compiler's (or with every difference explained).

**Prerequisites:** Phases 1 and 2 complete.

**Rough cost:** 2–3 weeks.

**Read first:** [00-Conventions.md](../00-Conventions.md),
[TouchedFiles.md](../TouchedFiles.md), [03-Dependencies.md](../03-Dependencies.md).

---

## Why this is worth doing before the renderer

This phase makes the whole content pipeline work on Linux without a single line of Vulkan. It is
independently valuable — Linux build agents can compile game data — and it forces `Engine` and
`EngineTools` to compile in full, which is a large fraction of the codebase
(185k + 193k lines) and surfaces portability problems early, while they are cheap to fix.

`ResourceCompilerApplication.cpp` is a single-file `int main` app, but it links **`EngineTools`,
`Engine`, `GameTools`, `Game`, and `Base`**. So everything except the applications layer must
compile here. This is the phase where the bulk of the "does the codebase actually build on
clang" work happens.

## The blocker you will hit first

`FileSystem::Watcher` is declared entirely inside `#if _WIN32` in
`Code/EngineTools/FileSystem/FileSystemWatcher.h:15`, but it is used **unguarded** as a
by-value member in:

- `Code/EngineTools/FileSystem/FileRegistry.h:282`
- `Code/Applications/ResourceServer/ResourceServer.h:216`

So `FileRegistry.h` does not compile on Linux at all, and `FileRegistry.cpp` is a member of
`Esoterica.Engine.Tools.vcxproj`. **The file watcher must be implemented in this phase**, even
though the ResourceCompiler itself never uses it. There is no way to defer it to Phase 7.

---

## Tasks

### P3.1 — `FileSystemWatcher_Linux.cpp`

**New:** `Code/EngineTools/FileSystem/FileSystemWatcher_Linux.cpp`
**Edits:** `FileSystemWatcher.h:15`, `FileSystemWatcher.cpp` (guard its `<windows.h>` block)

The header edit is one line:

```cpp
#if _WIN32 || defined( __linux__ )
```

The class's private members are already platform-neutral types, so **no member changes are
needed**:

| Member | Win32 use | Linux use |
|---|---|---|
| `void* m_pDirectoryHandle` | `CreateFileW` handle | inotify fd, encoded — see below |
| `void* m_pOverlappedEvent` | `OVERLAPPED` event | unused, or the watch-descriptor map |
| `uint8_t* m_pResultBuffer` | `ReadDirectoryChangesW` buffer | inotify read buffer |
| `unsigned long m_numBytesReturned` | bytes returned | bytes read |
| `bool m_requestPending` | async request in flight | unused |

**Handle encoding.** `IsWatching()` is an inline in the header returning
`m_pDirectoryHandle != nullptr`, and inotify can legitimately return fd 0. Store
`(void*)( intptr_t )( fd + 1 )` and subtract 1 on use. Put a prominent comment at the top of the
Linux file explaining the offset — it is non-obvious and will confuse the next reader otherwise.

**Behaviour to match.** Read `FileSystemWatcher.h`'s header comment: the Win32 implementation
*batches* notifications because `ReadDirectoryChangesW` fires multiple events for one logical
operation. inotify has the same problem in a different shape. Requirements:

- Recursive watching. **inotify is not recursive** — you must `inotify_add_watch` every
  subdirectory and maintain watches as directories are created and deleted. This is the bulk of
  the work and the most common source of bugs in inotify code.
- Map the event types: `IN_CREATE`, `IN_DELETE`, `IN_MODIFY`, `IN_CLOSE_WRITE`,
  `IN_MOVED_FROM`/`IN_MOVED_TO` (pair by cookie to synthesise a rename), and the `IN_ISDIR` bit
  to distinguish the `File*` from the `Directory*` variants of `Watcher::Event::Type`.
- `m_oldPath` is set only for rename events — populate it from the `IN_MOVED_FROM`/`IN_MOVED_TO`
  cookie pairing.
- `IN_Q_OVERFLOW` maps to `OnMassiveChangeDetected()`, which tells the caller to rescan manually.
  Wire this up; `FileRegistry` depends on it for correctness under heavy change.
- `Update()` must be **non-blocking** — the Win32 version polls an overlapped request. Use a
  non-blocking fd (`IN_NONBLOCK`) or `poll` with zero timeout.
- Watch the inotify watch limit (`/proc/sys/fs/inotify/max_user_watches`, often 8192). A large
  `Data/` tree can exhaust it. Detect `ENOSPC` on `inotify_add_watch` and surface it as a clear
  error naming the sysctl, rather than silently failing to watch part of the tree.

### P3.2 — Compile `Esoterica.Engine.Runtime`

236 source files. Expect mostly-mechanical clang issues. Known coupling from the survey:

- The `Render/` subtree (55 files referencing `RHI::`) compiles against `RHI.h` only, which is
  platform-neutral. The Phase 1 stub satisfies the linker.
- `Code/Engine/ThirdParty/box3d/` is portable C with its own CMake — should compile as-is.
- `Code/Engine/ThirdParty/meshoptimizer/` is a 2-file wrapper over `External/MeshOptimizer/`,
  which must be built (see P3.5).
- Navmesh code is behind `EE_ENABLE_NAVPOWER`, which stays unset. Confirm the non-Navpower path
  compiles — a middleware that is "disabled by default" is often less well tested.

Expect a long tail of clang-vs-MSVC differences. The usual suspects, none of which justify
editing upstream code:

- Two-phase name lookup in templates — MSVC is lax, clang is not. Genuine errors, but they are
  *upstream* errors that also affect clang-cl on Windows. **Record them; escalate rather than
  fixing them silently**, because a fix here is a permanent diff.
- Missing `#include`s that MSVC supplied transitively.
- `typename` / `template` disambiguation keywords.

For each such issue, the decision is: can the build generator work around it (a `-D`, a
`-Wno-`, a `-include`)? If yes, do that. If it genuinely requires a source change, that is a new
entry in [TouchedFiles.md](../TouchedFiles.md) and warrants a note in
[Progress.md](../Progress.md) — and it is worth reporting upstream, since clang-cl compatibility
is plausibly something upstream cares about.

### P3.3 — Compile `Esoterica.Engine.Tools`

155 source files. Contains the resource compilers and the import pipeline.

Vendored third-party, all portable:
- `ufbx` — portable C, FBX import
- `cgltf` — portable C
- `tinyexr` — portable C++
- `delabella` — portable C++
- `subprocess` — check its Linux path; it is used by `ResourceServerWorker.cpp`

`Code/EngineTools/Core/SystemDialogs.cpp` is 552 lines of Windows COM with an unguarded
`<windows.h>`. **It is in `EngineTools`, so it must at least compile.** Apply the "wrap, don't
split" technique now — `#ifdef _WIN32` at the top, `#endif` at the bottom, two lines — and defer
the real Linux implementation to Phase 7. Verify nothing in the ResourceCompiler's call graph
actually invokes a dialog; if something does, you need a minimal `SystemDialogs_Linux.cpp` stub
here rather than in Phase 7.

Similarly, the six `OpenInExplorer` call sites in `EngineTools` need to *compile*. They are in
UI code the ResourceCompiler never runs, but they still need a resolvable symbol — the Phase 1
`Platform::Linux::OpenInExplorer` provides it.

### P3.4 — Compile `Esoterica.Game.Runtime` and `Esoterica.Game.Tools`

Small — 8,006 and 133 lines respectively. Should be nearly free once `Engine` and `EngineTools`
compile.

### P3.5 — External dependencies

Extend `DownloadDependencies.sh`:

- **MeshOptimizer** — CMake, trivial.
- **SQLite** — system package or vendored amalgamation.
- **ctt** — texture compression. This resolves
  [open question 1](../Progress.md#open-questions) and is the **highest-uncertainty dependency
  in the whole port.**

**On `ctt`:** if it does not build on Linux, do not silently skip texture compression. The
options, in order of preference:

1. Port it, if the source is available and the work is bounded.
2. Substitute an equivalent compressor (e.g. `ispc_texcomp`, `bc7enc`) behind the same interface
   — but note this changes compressed output bytes, which breaks the byte-identical acceptance
   criterion below and means Linux and Windows produce different data. **Escalate before doing
   this.**
3. Leave texture compression Windows-only, with Linux producing uncompressed textures. Also
   breaks byte-identity. **Escalate.**

Whichever path, record the decision and its consequences in [Progress.md](../Progress.md).

### P3.6 — Compile and link `EsotericaResourceCompiler`

Single source file. Once the libraries compile this should be short.

### P3.7 — Validate compiled data equivalence

Compile `Data/` on both platforms from the same commit and compare:

```bash
find Data -name '*.<compiled-ext>' | sort | xargs sha256sum > /tmp/linux-data.sums
```

Byte-identical output is the goal and is genuinely achievable, because `DataPath` already
hardcodes `'/'` so serialized paths are platform-neutral.

Differences to investigate rather than accept:

- **Float formatting or precision** — should be identical given `-fno-fast-math` and the same
  IEEE semantics. A difference here means a flag is wrong.
- **Hash ordering** — if any compiler iterates an unordered container and writes results in
  iteration order, output is nondeterministic *within* a platform too. That is an upstream bug
  worth reporting.
- **Texture compression** — see P3.5. If `ctt` was substituted, expect and explain differences.
- **Path separators** in any serialized string — should be none.

---

## Acceptance criteria

1. `libEsoterica.Engine.Runtime.so`, `libEsoterica.Engine.Tools.so`,
   `libEsoterica.Game.Runtime.so`, `libEsoterica.Game.Tools.so` all build.
2. `Build/Linux_Release/EsotericaResourceCompiler` builds and links.
3. It compiles the full `Data/` tree without errors.
4. Compiled output is byte-identical to the Windows compiler's output for the same commit, **or**
   every difference is enumerated and explained in [Progress.md](../Progress.md).
5. `FileSystem::Watcher` works on Linux: a scratch test that creates, modifies, renames, and
   deletes files in a watched directory tree — **including in a subdirectory created after
   watching started** — produces the correct `Watcher::Event` sequence.
6. The watcher reports `OnMassiveChangeDetected` on `IN_Q_OVERFLOW` rather than silently
   dropping events.
7. `FileRegistry.cpp` compiles and its watcher-event handling (`FileRegistry.cpp:929`–1097)
   works against the Linux watcher.
8. Open question 1 (`ctt`) is resolved and recorded.
9. **The Windows MSBuild build still succeeds**, unchanged.
10. Every upstream file edited is in [TouchedFiles.md](../TouchedFiles.md) with status `done`.
11. `git diff --stat upstream/main -- Code/EngineTools/FileSystem/FileSystemWatcher.h` shows
    **1 line changed**.

## Do not

- Implement Linux file dialogs. Wrap `SystemDialogs.cpp` and move on; Phase 7 owns it.
- Touch anything under `Code/**/ThirdParty/`.
- Silently substitute a different texture compressor. Escalate first.
- Fix clang-vs-MSVC upstream errors without recording them.
- Change `FileSystemWatcher.h`'s member layout.

## Notes for the next agent

Record in [Progress.md](../Progress.md):
- Every clang-vs-MSVC issue found, and how each was handled.
- The `ctt` outcome.
- The data-equivalence diff result.
- Any inotify watch-limit tuning the `Data/` tree required.
