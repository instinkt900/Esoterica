# Phase 3 - Resource Compiler

**Goal:** game data compiles on Linux.

**Deliverable:** `EsotericaResourceCompiler` builds and compiles the resources under `Data/`. Its
output is byte-identical to the Windows compiler's, or every difference has an explanation.

**Prerequisites:** Phases 1 and 2 complete.

**Rough cost:** 2-3 weeks.

**Read first:** [00-Conventions.md](../00-Conventions.md),
[TouchedFiles.md](../TouchedFiles.md), [03-Dependencies.md](../03-Dependencies.md).

---

## Why this is worth doing before the renderer

This phase makes the whole content pipeline work on Linux with no Vulkan at all. It is valuable
on its own, because Linux build agents can then compile game data. It also forces `Engine` and
`EngineTools` to compile in full. That is a large part of the codebase, 185k plus 193k lines,
and it surfaces portability problems early, while they are still cheap to fix.

`ResourceCompilerApplication.cpp` is a single-file `int main` app, but it links **`EngineTools`,
`Engine`, `GameTools`, `Game`, and `Base`**. Everything except the applications layer must
therefore compile here. This is the phase where most of the "does the codebase build on clang"
work happens.

## The blocker you will hit first

`Code/EngineTools/FileSystem/FileSystemWatcher.h:15` declares `FileSystem::Watcher` entirely
inside `#if _WIN32`. Two files then use it **unguarded**, as a by-value member:

- `Code/EngineTools/FileSystem/FileRegistry.h:282`
- `Code/Applications/ResourceServer/ResourceServer.h:216`

`FileRegistry.h` therefore does not compile on Linux at all, and `FileRegistry.cpp` belongs to
`Esoterica.Engine.Tools.vcxproj`. **This phase must implement the file watcher**, even though the
ResourceCompiler never uses it. There is no way to defer it to Phase 7.

---

## Tasks

### P3.1 - `FileSystemWatcher_Linux.cpp`

**New:** `Code/EngineTools/FileSystem/FileSystemWatcher_Linux.cpp`
**Edits:** `FileSystemWatcher.h:15`, and `FileSystemWatcher.cpp` to guard its `<windows.h>`
block.

The header edit is one line:

```cpp
#if _WIN32 || defined( __linux__ )
```

The class's private members already use platform-neutral types, so **they need no change**:

| Member | Win32 use | Linux use |
|---|---|---|
| `void* m_pDirectoryHandle` | `CreateFileW` handle | inotify fd, encoded. See below. |
| `void* m_pOverlappedEvent` | `OVERLAPPED` event | unused, or the watch-descriptor map |
| `uint8_t* m_pResultBuffer` | `ReadDirectoryChangesW` buffer | inotify read buffer |
| `unsigned long m_numBytesReturned` | bytes returned | bytes read |
| `bool m_requestPending` | async request in flight | unused |

**Handle encoding.** `IsWatching()` is an inline in the header that returns
`m_pDirectoryHandle != nullptr`, and inotify can legitimately return fd 0. Store
`(void*)( intptr_t )( fd + 1 )`, and subtract 1 on use. Put a clear comment at the top of the
Linux file to explain the offset. It is not obvious, and it will confuse the next reader.

**Behavior to match.** Read the header comment in `FileSystemWatcher.h`. The Win32
implementation *batches* notifications, because `ReadDirectoryChangesW` fires several events for
one logical operation. inotify has the same problem in a different shape. The requirements:

- Recursive watching. **inotify is not recursive.** You must `inotify_add_watch` every
  subdirectory, and maintain the watches as directories appear and disappear. This is most of
  the work, and the most common source of bugs in inotify code.
- Map the event types: `IN_CREATE`, `IN_DELETE`, `IN_MODIFY`, `IN_CLOSE_WRITE`, and
  `IN_MOVED_FROM` with `IN_MOVED_TO`, paired by cookie to synthesize a rename. Use the
  `IN_ISDIR` bit to tell the `File*` variants of `Watcher::Event::Type` from the `Directory*`
  ones.
- `m_oldPath` is set only for rename events. Fill it from the `IN_MOVED_FROM` and `IN_MOVED_TO`
  cookie pairing.
- `IN_Q_OVERFLOW` maps to `OnMassiveChangeDetected()`, which tells the caller to rescan by hand.
  Wire it up. `FileRegistry` depends on it for correctness under heavy change.
- `Update()` must be **non-blocking**. The Win32 version polls an overlapped request. Use a
  non-blocking fd (`IN_NONBLOCK`), or `poll` with a zero timeout.
- Watch the inotify watch limit (`/proc/sys/fs/inotify/max_user_watches`, often 8192). A large
  `Data/` tree can exhaust it. Detect `ENOSPC` from `inotify_add_watch`, and report it as a clear
  error that names the sysctl. Do not fail to watch part of the tree in silence.

### P3.2 - Compile `Esoterica.Engine.Runtime`

236 source files. Expect mostly mechanical clang issues. The survey found this coupling:

- The `Render/` subtree, 55 files that reference `RHI::`, compiles against `RHI.h` only, which is
  platform-neutral. The Phase 1 stub satisfies the linker.
- `Code/Engine/ThirdParty/box3d/` is portable C with its own CMake. It should compile as-is.
- `Code/Engine/ThirdParty/meshoptimizer/` is a 2-file wrapper over `External/MeshOptimizer/`,
  which you must build. See P3.5.
- The navmesh code sits behind `EE_ENABLE_NAVPOWER`, which stays unset. Confirm that the
  non-Navpower path compiles. Middleware that is off by default is often less well tested.

Expect a long tail of clang-against-MSVC differences. The usual ones, none of which justify
editing upstream code:

- Two-phase name lookup in templates. MSVC is lax, and clang is not. These are real errors, but
  they are *upstream* errors that also affect clang-cl on Windows. **Record them, and escalate
  instead of fixing them quietly**, because a fix here is a permanent diff.
- Missing `#include` lines that MSVC supplied transitively.
- Missing `typename` and `template` disambiguation keywords.

For each issue, ask whether the build generator can work around it with a `-D`, a `-Wno-`, or a
`-include`. If it can, do that. If it genuinely needs a source change, that is a new entry in
[TouchedFiles.md](../TouchedFiles.md) and a note in [Progress.md](../Progress.md). It is also
worth reporting upstream, since upstream plausibly cares about clang-cl compatibility.

### P3.3 - Compile `Esoterica.Engine.Tools`

155 source files. It holds the resource compilers and the import pipeline.

The vendored third-party code is all portable:

- `ufbx`, portable C, for FBX import
- `cgltf`, portable C
- `tinyexr`, portable C++
- `delabella`, portable C++
- `subprocess`. Check its Linux path. `ResourceServerWorker.cpp` uses it.

`Code/EngineTools/Core/SystemDialogs.cpp` is 552 lines of Windows COM with an unguarded
`<windows.h>`. **It is in `EngineTools`, so it must at least compile.** Apply the "wrap, do not
split" technique now: `#ifdef _WIN32` at the top, `#endif` at the bottom, two lines. Defer the
real Linux implementation to Phase 7. Confirm that nothing in the ResourceCompiler's call graph
opens a dialog. If something does, you need a minimal `SystemDialogs_Linux.cpp` stub here rather
than in Phase 7.

The six `OpenInExplorer` call sites in `EngineTools` also need to *compile*. They sit in UI code
that the ResourceCompiler never runs, but they still need a symbol that resolves. The Phase 1
`Platform::Linux::OpenInExplorer` provides it.

### P3.4 - Compile `Esoterica.Game.Runtime` and `Esoterica.Game.Tools`

These are small, 8,006 and 133 lines. They should be nearly free once `Engine` and `EngineTools`
compile.

### P3.5 - External dependencies

Extend `DownloadDependencies.sh`:

- **MeshOptimizer**, through CMake. Easy.
- **SQLite**, a system package or the vendored amalgamation.
- **ctt**, texture compression. This answers
  [open question 1](../Progress.md#open-questions), and it is the **least certain dependency in
  the whole port.**

**On `ctt`:** if it does not build on Linux, do not skip texture compression quietly. The
options, best first:

1. Port it, if the source is available and the work is bounded.
2. Swap in an equivalent compressor, such as `ispc_texcomp` or `bc7enc`, behind the same
   interface. This changes the compressed output bytes, which breaks the byte-identical
   acceptance criterion below and means Linux and Windows produce different data. **Escalate
   before you do this.**
3. Keep texture compression on Windows only, and have Linux produce uncompressed textures. This
   also breaks byte-identity. **Escalate.**

Whichever path you take, record the decision and its consequences in
[Progress.md](../Progress.md).

### P3.6 - Compile and link `EsotericaResourceCompiler`

One source file. Once the libraries compile, this should be short.

### P3.7 - Check that compiled data matches

Compile `Data/` on both platforms from the same commit, and compare:

```bash
find Data -name '*.<compiled-ext>' | sort | xargs sha256sum > /tmp/linux-data.sums
```

Byte-identical output is the goal, and it is achievable, because `DataPath` already hardcodes
`'/'` and serialized paths are therefore platform-neutral.

Investigate these differences rather than accepting them:

- **Float formatting or precision.** These should be identical, given `-fno-fast-math` and the
  same IEEE semantics. A difference means a flag is wrong.
- **Hash ordering.** If a compiler iterates an unordered container and writes results in
  iteration order, the output is nondeterministic *within* a platform too. That is an upstream
  bug worth reporting.
- **Texture compression.** See P3.5. If you substituted `ctt`, expect differences and explain
  them.
- **Path separators** in any serialized string. There should be none.

---

## Acceptance criteria

1. `libEsoterica.Engine.Runtime.so`, `libEsoterica.Engine.Tools.so`,
   `libEsoterica.Game.Runtime.so`, and `libEsoterica.Game.Tools.so` all build.
2. `Build/Linux_Release/EsotericaResourceCompiler` builds and links.
3. It compiles the full `Data/` tree without errors.
4. The compiled output is byte-identical to the Windows compiler's output for the same commit,
   **or** [Progress.md](../Progress.md) lists and explains every difference.
5. `FileSystem::Watcher` works on Linux. A scratch test that creates, modifies, renames, and
   deletes files in a watched directory tree, **including in a subdirectory created after
   watching started**, produces the correct `Watcher::Event` sequence.
6. The watcher reports `OnMassiveChangeDetected` on `IN_Q_OVERFLOW`. It does not drop events
   silently.
7. `FileRegistry.cpp` compiles, and its watcher-event handling
   (`FileRegistry.cpp:929`-1097) works against the Linux watcher.
8. Open question 1 (`ctt`) is answered and recorded.
9. **The Windows MSBuild build still succeeds**, unchanged.
10. Every upstream file you edited is in [TouchedFiles.md](../TouchedFiles.md) with status
    `done`.
11. `git diff --stat upstream/main -- Code/EngineTools/FileSystem/FileSystemWatcher.h` shows
    **1 line changed**.

## Do not

- Implement Linux file dialogs. Wrap `SystemDialogs.cpp` and move on. Phase 7 owns it.
- Touch anything under `Code/**/ThirdParty/`.
- Swap in a different texture compressor quietly. Escalate first.
- Fix clang-against-MSVC upstream errors without recording them.
- Change the member layout of `FileSystemWatcher.h`.

## Notes for the next agent

Record this in [Progress.md](../Progress.md):

- Every clang-against-MSVC issue you found, and how you handled each one.
- The `ctt` outcome.
- The result of the data-equivalence diff.
- Any inotify watch-limit tuning that the `Data/` tree needed.
