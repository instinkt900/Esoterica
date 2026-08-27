# Progress Log

Running state of the Linux port. **Every task appends here before it counts as done**
(Conventions rule 9). Newest entries go at the top of each section.

This file keeps a chain of independent agent sessions coherent. When you start a session, read
"Current state" and "In flight" first.

---

## Current state

**Phase: 0 (in progress).** P0.1 to P0.8 and P0.10 are done. `ninja` builds Linux targets and
reaches the compiler. It does not compile yet, which is the expected end state for Phase 0. Only
P0.9, `DownloadDependencies.sh`, is left.

| Phase | Status |
|---|---|
| 0 - Build System | in progress (P0.1-P0.8 and P0.10 done, P0.9 open) |
| 1 - Base Platform Layer | not started |
| 2 - Reflector | not started |
| 3 - Resource Compiler | not started |
| 4 - Shader Pipeline | not started |
| 5 - Vulkan RHI | not started |
| 6 - Windowing and Input | not started |
| 7 - Editor and Tools | not started |

Linux build status: **does not build.**
Windows build status: **unchanged from upstream** (no edits landed yet).

## In flight

**P0.9** - `DownloadDependencies.sh`. Independent of the generator. Phase 0 needs only libunwind,
libdw, Freetype and SQLite; the rest defer to the phase that first needs them.

After that, Phase 0 is done and **Phase 1** starts on the worklist below.

---

## Completed work

<!--
Append one entry per completed task, newest first. Format:

### YYYY-MM-DD - P<phase>.<task> <short title>
- What you did, concretely.
- Files added: ...
- Upstream files edited: ... (must match TouchedFiles.md)
- Acceptance criteria met: which ones, and which not.
- Anything the next agent needs to know.
-->

### 2026-08-27 - P0.5-P0.8, P0.10 Ninja emitter, flags, linking

`python3 Code/Scripts/NinjaGen/NinjaGen.py` writes `Build/Linux/Esoterica.ninja` and
`compile_commands.json`. `ninja -f Build/Linux/Esoterica.ninja` then reaches the compiler.
**Nothing links, and only one translation unit compiles.** That is the expected end state for
Phase 0, and the error output is Phase 1's worklist. See below.

- **P0.5 flags.** `Toolchain.py` translates `Esoterica.props`. `-std=c++20`, `-std=c17`,
  `-fno-exceptions`, `-msse4.2 -mavx`, `-g`, `-fPIC`, `-O0` or `-O2`, `-flto` in Shipping,
  `-fvisibility=hidden` on shared libraries. `-Wall -Wextra` and **no `-Werror`**, per the phase
  document. `NOMINMAX`, `WIN32_LEAN_AND_MEAN` and `_CRT_SECURE_NO_WARNINGS` are dropped;
  `NDEBUG` is kept in every configuration, because upstream conditions it on `$(Platform)`, not
  on `$(Configuration)`.
- **P0.6 linking.** All 14 imported property sheets are mapped. `pkg-config` for Freetype,
  plain `-l` otherwise, `llvm-config` for LLVM. The six dropped sheets are named in the table
  with no flags, so they are recognised rather than forgotten, and **none contributes an
  `EE_ENABLE_*` define** (Conventions rule 4).
- **P0.7 layout.** `Build/Linux_<Config>/` for binaries, `Build/_Temp/Linux_<Config>/<Project>/`
  for objects, derived from the repo-relative source path. Nine configurations: the three from
  the `.slnx`, plus ASan, TSan and UBSan on Debug and Release. MSan is dropped.
- **P0.8 `compile_commands.json`.** Generated with `ninja -t compdb` against the Debug rules
  only, using the real Linux toolchain. 603 entries.
- **P0.10 `.gitignore`.** Added `Build/`, `compile_commands.json`, `.ninja_deps`, `.ninja_log`.
  None of the four was ignored: the existing entry is lowercase `build/`.

Files added:

- `Code/Scripts/NinjaGen/Toolchain.py` - the property sheet and flag translation.
- `Code/Scripts/NinjaGen/Checks.py` - 20 checks, exits 0. See the decision below on
  what belongs there.

`Code/Scripts/NinjaGen/NinjaGen.py` is rewritten. It is the one upstream file this port replaces
wholesale; see [01-UpstreamMerges.md](01-UpstreamMerges.md).

**Upstream files edited, beyond `NinjaGen.py`:** two, both one character, both registered in
[TouchedFiles.md](TouchedFiles.md).

| File | Change |
|---|---|
| `Code/Base/Utils/GlobalRegistryBase.h` | `#include "Base\Esoterica.h"` to `"Base/Esoterica.h"` |
| `Code/Base/Input/InputDevices/InputDevice_Controller.cpp` | `#include "Base\Math\Vector.h"` to `"Base/Math/Vector.h"` |

clang does not treat `\` as a path separator inside an include, so on Linux neither header was
found. The first accounted for 43 not-found errors in `Esoterica.Base` on its own. MSVC accepts
`/`, so Windows is unaffected. This needed escalation, because neither file was in
the registry.

Acceptance criteria met: **1, 2, 3, 4, 5, 6, 7, 8, 9, 11.** Not met: **10**, the Windows MSBuild
build, which has not been run. There is no Windows machine on this side.

## The Phase 1 worklist

From `ninja -f Build/Linux/Esoterica.ninja Build/Linux_Debug/libEsoterica.Base.so`. 132
translation units attempted, 131 failed, 1 compiled clean
(`Code/Base/ThirdParty/mpack/mpack.c`).

**No error is a generator fault.** There are no missing include paths, no bad flags, and no
malformed rules. Every one traces to a platform implementation that does not exist yet, and each
already has a row in [TouchedFiles.md](TouchedFiles.md).

| Errors | Message | Root cause | Fix |
|---|---|---|---|
| 1451 | `'__declspec' attributes are not enabled` | `EE_BASE_API` and friends expand to `__declspec(dllexport)` | The six `_Module/API.h` files need their `__attribute__(( visibility( "default" ) ))` branch |
| 502 | `unknown type name 'size_t'` | `Esoterica.h:55` includes `Platform/Platform_Win32.h` under `#if _WIN32` and has no `#elif` | `Platform_Linux.h`, and the 2-line guard addition |
| 254 | `unknown type name 'va_list'` | as above | as above |
| 199 | `use of undeclared identifier 'EE_DEBUG_BREAK'` | defined in `Platform_Win32.h` | `Platform_Linux.h` must define it |

The macros clang names in its expansion notes, in order: `EE_BASE_API` (1473), `EE_TRACE_HALT` (107),
`EE_UNIMPLEMENTED_FUNCTION` (106), `EE_ASSERT` (92), then the vendored `EASTL_ATOMIC_*` (88),
`PUGIXML_API` and `PUGIXML_CLASS` (32), and `ENKI_ASSERT` (9). The vendored ones key off
`_MSC_VER` inside `Code/**/ThirdParty/`, which Conventions rule 5 puts out of bounds; fix them
with a `-D` in the generator, not by editing the source.

Do **not** add `-fdeclspec` to make these go away. It hides the `_Module/API.h` work rather than
doing it.

What the next session needs to know:

- **`NinjaGen.py` runs `SyncUpstream.py` in check mode first and stops on a non-zero exit.**
  There is deliberately no flag to skip it.
- **The generator reports problems but does not fail on them.** 11 against this tree, all
  expected: `External/RenderDoc` and `llvm-config` are deferred dependencies, and
  `Esoterica.Applications.Engine` has no sources until Phase 6.
- **`clang/AST/Ast.h` is not found** when building the Reflector. That is Phase 2's LLVM
  dependency, and note the casing: the real header is `clang/AST/AST.h`, so this may be another
  case mismatch. Check when LLVM is installed.
- Sanitizer builds exist for Debug and Release only. `Build/Linux_Debug_ASan/` and so on.
- **A whole-tree `ninja` run gets killed on a small machine.** `ninja -n` walks all 588 Debug
  edges and the graph is valid, but a real build at the default `-j8` on 7 GB of RAM dies partway
  with no ninja summary line, which looks like the OOM killer. clang holds a lot of memory per
  translation unit at `-O0` with `-g`. Use `-j2` or `-j4` on a small machine. This is an
  environment limit, not a generator problem: `Esoterica.Base` on its own completes reliably and
  is what Phase 1 targets.

### 2026-08-27 - P0.1-P0.4 Source lists and the upstream sync tool

The Linux build now has a source model. It reads three text files, not XML. It writes no ninja
file yet; that is P0.5 to P0.8.

| File | Maintained | Contents |
|---|---|---|
| `Code/Scripts/NinjaGen/UpstreamProjects.txt` | generated | 13 projects, 620 sources, per-configuration types, references, property sheets |
| `Code/Scripts/NinjaGen/Exclusions.txt` | by hand | 4 globs, each with a reason |
| `Code/Scripts/NinjaGen/LinuxSources.txt` | by hand | empty. Phase 1 adds the first entries |

- **P0.1 sync tool.** `SyncUpstream.py --update` reads `Esoterica.slnx` and the `.vcxproj`
  files and rewrites `UpstreamProjects.txt`. With no arguments it re-derives the list and exits
  1 with a unified diff if the committed copy is stale. It is the only thing in the build that
  reads XML. It honors `<Build Project="false"/>`, `<Build Solution="Shipping|*"
  Project="false"/>`, and reads configuration names from `<Configurations>`.
- **P0.2 per-configuration facts.** `ConfigurationType` and property sheet imports are both read
  per configuration. `Esoterica.Base` is `DynamicLibrary` in Debug and Release and
  `StaticLibrary` in Shipping, and it drops `ixWebSocket.props` in Shipping.
- **P0.3 exclusions.** `Exclusions.txt` holds 4 globs: `**/*_Win32.cpp`, `**/Win32/**`,
  `Code/Base/Render/RHI_Direct3D12.cpp`, and `Code/Base/ThirdParty/D3D12MemoryAllocator/**`.
  A glob that matches nothing is reported as a problem.
- **P0.4 autogenerated globbing.** `_Module/_Autogenerated/{TypeInfo,Shaders}/*.cpp` per
  project, case-insensitively. These are Reflector outputs, so they stay globbed.

Files added:

- `Code/Scripts/NinjaGen/SourceLists.py` - the list format and the build model.
  `python3 Code/Scripts/NinjaGen/SourceLists.py` prints the model and reports problems.
- `Code/Scripts/NinjaGen/SyncUpstream.py` - the only reader of the Visual Studio projects.
- `Code/Scripts/NinjaGen/Checks.py` - the generator's checks. No test framework.
- The three list files above.

Upstream files edited: **none.** `NinjaGen.py` is untouched and still stale. P0.5 rewrites it.

Acceptance criteria met: part of 2 (`Esoterica.Base` keeps 132 sources and excludes 15, which
accounts for all 147 `ClCompile` entries), 4, 7, and 9. Criteria 1, 3, 5, 6, 8, 10 and 11 need
the emitter, which is P0.5 to P0.8.

What the next session needs to know:

- **Run `SyncUpstream.py` in check mode at the top of `NinjaGen.py`** and stop on a non-zero
  exit. The check is what makes the lists safe, and it is worthless if nothing runs it.
- **`Esoterica.Applications.Engine` has zero sources.** Its only source was
  `Win32/EngineApplication_Win32.cpp`. Phase 6 adds the Linux entry point. The emitter must not
  treat an empty source list as an error.
- **`Esoterica.Scripts.Reflect` is `ConfigurationType: Makefile`** and is never built directly.
  Skip it rather than warning about it.
- **14 property sheets are imported** by at least one project: `AmdAgs`, `Box3D`, `CTT`, `DXC`,
  `Esoterica`, `FreeType`, `GameNetworkingSockets`, `LLVM`, `LivePP`, `MeshOptimizer`,
  `RenderDoc`, `SQLite`, `WinPixEventRuntime`, `ixWebSocket`. **None are mapped to link flags
  yet.** That is P0.6. `LivePP` is imported, so P0.6 must skip it without defining
  `EE_ENABLE_LPP` (Conventions rule 4).
- `Esoterica.slnx` also lists `EA.props`, `FBXSDK.props`, `Imgui.props`, `NavPower.props`,
  `Optick.props` and `SuperLuminal.props`, but **no `.vcxproj` imports them.** P0.6 needs no
  mapping for those.
- Headers are no longer listed anywhere. The `REFLECT` rule's dependency set should be a
  `**/*.h` glob per project. Phase 2 needs this.

---

## Decisions made during implementation

Record any decision that a future reader would otherwise have to work out from the code. Give
the reasoning, not just the outcome.

<!--
### YYYY-MM-DD - <decision>
**Context:** ...
**Decision:** ...
**Rationale:** ...
**Alternatives rejected:** ...
-->

### 2026-08-27 - The build is the test. Check only what a green build would hide

**Context:** The first cut of the generator carried 520 lines of checks against 1441 lines of
generator, 27% of the Python. Most of it re-checked things the build proves anyway: that a rule
passes `-std=c++20`, that `Esoterica.Base` resolves to a `.so`, that link order is topological,
that ninja parses the file.

**Decision:** One `Code/Scripts/NinjaGen/Checks.py`, 192 lines, 20 checks. **Add a check only
when the failure would leave a green build behind.** Everything else is the compiler's job.

What that leaves:

| Check | Why the build cannot catch it |
|---|---|
| Sync drift detection | The whole safety mechanism of the three-list design. It fails silently until an upstream merge, months later. |
| Determinism of the ninja file | Building twice never reveals a non-deterministic build file. Acceptance criterion 6. |
| Stale exclusion globs | A glob that stopped matching silently readmits a file. |
| Glob semantics | A wrong glob silently includes or excludes sources, and the build may still succeed. |
| Property sheet coverage | An unmapped sheet surfaces much later as a link error naming nothing useful. |
| No `.vcxproj` modified | The prime directive. Nothing about a successful build says the project files are untouched. |
| `-ffast-math` never appears | It changes float behaviour without failing anything. |

**Rationale:** A compile error is louder, more specific and cheaper than any assertion that
duplicates it. Time spent asserting what the compiler already tells you is time not spent
getting Linux to build, which is the actual goal.

**Alternatives rejected:** Full coverage of the generator, which is what the first cut drifted
towards. No checks at all, which would drop the drift detection that makes the three-list design
safe in the first place.

### 2026-08-27 - Sanitizers on Debug and Release, never on Shipping

**Context:** The stale upstream generator built ASan, MSan and TSan variants of all three
configurations.
**Decision:** ASan, TSan and UBSan on Debug and Release. None on Shipping. MSan is dropped
entirely.
**Rationale:** Shipping is the `-flto` configuration. Instrumenting it measures a binary nobody
ships and takes far longer to build. MSan needs an instrumented libc++ to give usable output,
and without one it buries the reader in false positives from the standard library.
**Alternatives rejected:** Sanitizing every configuration, which triples the size of the ninja
file to describe builds nobody runs.

### 2026-08-27 - One compile rule per project and configuration

**Context:** Compiler flags vary by project and by configuration, and ninja has no scoping
between the two.
**Decision:** Emit a `cc_<Project>_<Config>` and `cxx_<Project>_<Config>` rule with the flags
baked into the command, so each build edge is a single line.
**Rationale:** The alternative is a per-edge variable, which adds an indented line to every one
of the 5300 edges and roughly doubles the file. 234 rules cost nothing to parse.
**Alternatives rejected:** One rule per configuration with per-edge flag variables.

### 2026-08-27 - Three source lists, not a source list derived from the `.vcxproj` files live

**Context:** The plan said the generator should read the `.vcxproj` files on every build, so that
upstream source changes need no Linux-side work. The first implementation did that, and needed
filename heuristics to decide what was Windows-only.

Those heuristics were wrong in both directions, silently:

- `Code/EngineTools/Core/SystemDialogs_Linux.cpp` would not have been found. The rule only
  searched directories that had lost a Windows file, and `SystemDialogs.cpp` gets guarded rather
  than excluded.
- `Code/Base/Render/RHI_Direct3D12.cpp` was **in** the Linux build. It carries no platform
  suffix and no `#if _WIN32` guard, so nothing kept it out. Same for
  `Code/Base/ThirdParty/D3D12MemoryAllocator/`.
- `Code/Base/Render/RHI_Vulkan.cpp`, which Phase 5 adds, would not have been found either. It
  has no platform token in its name.

**The measurement that settled it.** Upstream has 107 commits in total: 63 in 2022, 28 in 2023,
4 in 2024, 1 in 2025, 11 in 2026. In the twelve months to 2026-08-27, four commits touched a
`.vcxproj` source list, and the churn was `+1/-0`, `+1/-0`, `+3/-3`, and `+265/-137`
(`5d02c90`, "Push Esoterica DX12").

**Decision:** The build reads three text files. `UpstreamProjects.txt` is generated by
`SyncUpstream.py` and never hand-edited. `Exclusions.txt` and `LinuxSources.txt` are
hand-maintained. Every build first runs `SyncUpstream.py` in check mode and stops if the
generated list is stale.

**Rationale:** Live parsing saved about five lines of list editing a year, and saved nothing on
`5d02c90`, because 265 new files need classifying by hand whatever the build system does. In
exchange it introduced a class of silent wrong answers. An explicit list cannot make that
mistake, and `Exclusions.txt` records *why* each file is dropped, which no filename convention
can. The check keeps the one real benefit of live parsing: a new upstream source stops the build
with a named error rather than joining it unnoticed.

**Alternatives rejected:** Live parsing with better heuristics; the heuristics are the problem,
not their quality. A one-time bootstrap with no ongoing check; that turns upstream drift back
into a silent failure, which is the thing worth preventing.

### 2026-08-27 - Path case is corrected once, at sync time

**Context:** Seven `.vcxproj` entries disagree with the case of the file on disk. MSBuild ignores
case. Linux does not.
**Decision:** `SyncUpstream.py` writes the on-disk spelling into `UpstreamProjects.txt`. The
build never sees the mismatch.
**Rationale:** Conventions rule 3 forbids fixing upstream files the task does not need, and rule
5 says to fix a vendored-code problem in the build generator. Doing it at sync time rather than
at build time means the correction is visible in a reviewed file instead of happening invisibly
on every run.
**Alternatives rejected:** Editing the `.vcxproj` files, which
[TouchedFiles.md](TouchedFiles.md) rules out. Resolving case on every build, which hides the
problem.

### 2026-08-27 - Headers are globbed, not listed

**Context:** The `REFLECT` rule needs a header dependency set. `<ClInclude>` entries would supply
it.
**Decision:** `SyncUpstream.py` ignores `<ClInclude>`. Phase 2 globs `**/*.h` per project
instead.
**Rationale:** A header list is pure maintenance with no decisions in it: nothing is ever
excluded from a dependency set for being Windows-only, because an unused header costs nothing.
Listing them would roughly double `UpstreamProjects.txt` and add a merge conflict surface for no
gain.

---

## Open questions

Carried from
[03-Dependencies.md](03-Dependencies.md#open-questions-to-resolve-during-implementation). Move a
question to "Decisions made" once you answer it.

| # | Question | Blocks | Status |
|---|---|---|---|
| 1 | Does `ctt` (texture compression) build on Linux? | Phase 3 | open |
| 2 | Which LLVM version does the Reflector need, and does `clangAST` compile against it on Linux? | Phase 2 | open |
| 3 | Use `volk`, or the plain Vulkan loader? | Phase 5 | open |
| 4 | Do the target distros package SDL3, or must we always build it? | Phase 6 | open |
| 5 | Does `GameNetworkingSockets` block the first `Base` link, or can we defer it? | Phase 1 | open |
| 6 | Does the `VirtualAlloc` region in `Memory.cpp` (`PageAllocator`, near line 234) have a working non-Windows path? | Phase 1 | open |

Answered:

- **Is the autogenerated directory `_Autogenerated` or `_AutoGenerated`?** It is `_AutoGenerated`
  on disk, which is what `Reflect.nmake` and `.gitignore` use. `Esoterica.props` writes
  `_Autogenerated`. The generator globs case-insensitively, so both work.

- **How often does upstream actually change its source lists?** About five entries a year. 107
  commits in total: 63 in 2022, 28 in 2023, 4 in 2024, 1 in 2025, 11 in 2026. Four commits in
  the twelve months to 2026-08-27 touched a `.vcxproj` source list: `+1/-0`, `+1/-0`, `+3/-3`,
  and `+265/-137`. This settled the build system design; see "Decisions made" above.

---

## Upstream issues observed

Bugs and oddities in upstream code. **Do not fix them here** (Conventions rule 3). Record them,
and file them upstream as issues if they matter.

<!-- ### <file>:<line> - <description> -->

Noted during the first survey, in `Code/Scripts/NinjaGen/NinjaGen.py`. This port rewrites that
stale build script, so these get fixed as a side effect rather than as upstream fixes:

- It parses `Esoterica.sln`, which the repo no longer contains. The project moved to
  `Esoterica.slnx`.
- `cpp_rule` calls `toolchain.compiler_c` instead of `compiler_cpp`.
- `-fsanitize-address` is not a valid flag. It should be `-fsanitize=address`.
- It declares `-std=c++17`, but the project needs C++20.

Also noted, and not fixed:

- `Code/Applications/BuildGenerator/` does not work. It emits rule references with no rule
  definitions, and it parses the legacy `.sln` GUID format. Left alone on purpose.
- `Esoterica.slnx` references `Docs/docs/CodingGuidelines.md`, which the repository does not
  contain.

### Path case mismatches between the `.vcxproj` files and the disk

Found on 2026-08-27. MSBuild ignores case, so Windows builds fine. On Linux the file is simply
not found. `SyncUpstream.py` writes the on-disk spelling into `UpstreamProjects.txt` and warns.
**Not fixed in the `.vcxproj` files** (Conventions rule 3, TouchedFiles.md).

| Project | Listed | On disk | Affects the build |
|---|---|---|---|
| `Esoterica.Base` | `ThirdParty/enkits/TaskScheduler.cpp` | `ThirdParty/EnkiTS/TaskScheduler.cpp` | yes |
| `Esoterica.Engine.Runtime` | `Navmesh/NavPower.cpp` | `Navmesh/Navpower.cpp` | yes |
| `Esoterica.Base` | `ThirdParty/enkits/TaskScheduler.h` | `ThirdParty/EnkiTS/TaskScheduler.h` | no, header |
| `Esoterica.Base` | `ThirdParty/enkits/TaskScheduler_Esoterica.h` | `ThirdParty/EnkiTS/TaskScheduler_Esoterica.h` | no, header |
| `Esoterica.Base` | `ThirdParty/enkits/LockLessMultiReadPipe.h` | `ThirdParty/EnkiTS/LockLessMultiReadPipe.h` | no, header |
| `Esoterica.Applications.Reflector` | `Resources/Resource.h` | `Resources/resource.h` | no, header |
| `Esoterica.Applications.BuildGenerator` | `Resources/Resource.h` | `Resources/resource.h` | no, header |

The header rows no longer reach the build, because `SyncUpstream.py` ignores `<ClInclude>`. They
are recorded so nobody investigates them twice.

### `RHI_Direct3D12.cpp` has no platform guard

`Code/Base/Render/RHI_Direct3D12.cpp` is 6084 lines of Direct3D 12 with no `#if _WIN32` at the
top and no platform suffix in its name. The same is true of the vendored
`Code/Base/ThirdParty/D3D12MemoryAllocator/`. Nothing about the file marks it as Windows-only,
so nothing but an explicit entry in `Exclusions.txt` keeps it out of a Linux build.

This is fine for upstream, which is Windows-only. Recorded because it is the single clearest
argument against deciding what to build from filenames.

The case mismatches are worth reporting upstream. They cost nothing on Windows, so upstream will
not notice them on its own.

---

## Merge notes

See [01-UpstreamMerges.md](01-UpstreamMerges.md#merge-notes) for the sync-point table. Record
hard conflict resolutions there, not here.
