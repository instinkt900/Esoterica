# Phase 0 - Build System

**Goal:** `ninja` produces Linux binaries from the existing `.vcxproj` files, and no `.vcxproj`
changes.

**Deliverable:** `python3 Code/Scripts/NinjaGen/NinjaGen.py` writes
`Build/Linux/Esoterica.ninja` and `compile_commands.json`. Running `ninja` then gets *some* way
into compiling `Esoterica.Base`. It will not link yet, and it will not compile every file. That
is expected. Phase 1 makes it compile.

**Prerequisites:** none. This is the first phase.

**Rough cost:** 2-3 weeks.

**Read first:** [00-Conventions.md](../00-Conventions.md),
[02-Architecture.md, Build system](../02-Architecture.md#build-system),
[03-Dependencies.md](../03-Dependencies.md).

---

## Why this comes first

You cannot check anything without a build. Every later phase states its acceptance criteria as
"this compiles" or "this runs", so the generator is the foundation. It is also the piece that
most directly decides whether upstream merges stay cheap, which is this project's main
constraint.

## Scope boundary

**In scope:** the source lists and sync tool, the generator, the dependency-acquisition script,
and `.gitignore`.

**Out of scope:** making anything compile. Do not write a single `_Linux.cpp` in this phase. Do
not stub out Win32 code to get further. Compile errors from missing platform implementations are
the *expected output* of Phase 0, and the input to Phase 1.

---

## Tasks

### P0.1 - The source lists and the sync tool

The Linux build reads three text files under `Code/Scripts/NinjaGen/`, not XML:

| File | Maintained | Contents |
|---|---|---|
| `UpstreamProjects.txt` | **generated** | Per project: the `.vcxproj` path, `ConfigurationType` per configuration, `.slnx` build exclusions, project references, property sheets, and every source upstream builds. |
| `Exclusions.txt` | by hand | Globs for upstream sources the Linux build drops, each with a comment saying why. |
| `LinuxSources.txt` | by hand | Sources this fork adds, grouped by `[Project]`. |

`SyncUpstream.py` is the only thing that reads `Esoterica.slnx` and the `.vcxproj` files.

- `python3 Code/Scripts/NinjaGen/SyncUpstream.py --update` rewrites `UpstreamProjects.txt`.
- `python3 Code/Scripts/NinjaGen/SyncUpstream.py` re-derives it and **exits 1 with a diff** if
  the committed copy is stale.

Requirements:

- Use `xml.etree.ElementTree`. Add no new Python dependencies.
- Honor `<Build Project="false"/>` (never build) and
  `<Build Solution="Shipping|*" Project="false"/>` (exclude from that configuration only), with
  `*` as a wildcard on either side of the `|`.
- Read the configuration and platform names from `<Configurations>`. Do not hardcode them.
- **Correct filename case at sync time.** Several `.vcxproj` entries disagree with the case of
  the file on disk. MSBuild ignores case and Linux does not. Write the on-disk spelling into
  `UpstreamProjects.txt` so the correction happens once, not on every build.
- **Drop a listed source that no file matches**, with a warning. It is a stale `.vcxproj` entry,
  and keeping it only makes ninja fail with `missing and no known rule to make it`.
- Ignore `<ClInclude>`. Headers are only needed as the `REFLECT` rule's dependency set, and a
  glob gives that without a list anybody has to maintain.
- **Deterministic output.** Same input, byte-identical file. Sort projects and sources.

Note that `Code/Scripts/Reflect/Esoterica.Scripts.Reflect.vcxproj` is an NMAKE wrapper project
(`Reflect.nmake`), not a real C++ project. Its `ConfigurationType` is `Makefile`. Record it as
such and never build it directly. Detect it by its `ConfigurationType`, not by name.

### P0.2 - `ConfigurationType` and property sheets, per configuration

Both vary by configuration in this codebase, and the stale upstream script read each once and
got it wrong.

- `Esoterica.Base` is `DynamicLibrary` in Debug and Release, and `StaticLibrary` in Shipping.
  Three separate `<PropertyGroup Condition="'$(Configuration)|$(Platform)'=='...'">` blocks
  declare it.
- Property sheet imports sit in `<ImportGroup Condition="...">` blocks, one per configuration.
  `Esoterica.Base` imports `ixWebSocket.props` in Debug and Release but not in Shipping.

Map `DynamicLibrary` to a `.so`, `StaticLibrary` to a `.a`, `Application` to an executable, and
`Makefile` to "never built".

### P0.3 - Exclusions

`Exclusions.txt` decides what the Linux build drops. Globs match the repo-relative path, where
`**/` is zero or more leading directories, `**` is anything, and `*` is anything except `/`.

The starting set:

```
**/*_Win32.cpp                              the platform implementations
**/Win32/**                                 the application entry point directories
Code/Base/Render/RHI_Direct3D12.cpp         6084 lines, no platform suffix, no #if _WIN32 guard
Code/Base/ThirdParty/D3D12MemoryAllocator/**   AMD's D3D12 allocator. VMA replaces it in Phase 5
```

**Report any glob that matches nothing.** It is almost always a leftover from an upstream
rename, and a stale exclusion is how a file quietly rejoins the build.

Every entry carries a comment saying why. That comment is the thing a future reader needs and
that no filename convention can supply.

### P0.4 - Autogenerated source globbing

`Esoterica.props` adds these as MSBuild wildcards, so the `.vcxproj` files do not list them:

```
_Module/_Autogenerated/TypeInfo/*.cpp
_Module/_Autogenerated/Shaders/*.cpp
```

These are Reflector outputs, regenerated on every reflection run, so they stay **globbed** rather
than listed. Glob both, per project. The directories do not exist before the Reflector has run
(Phase 2). Handle their absence without failing.

Upstream has a case inconsistency here. `Esoterica.props` writes `_Module\_Autogenerated\`,
`Reflect.nmake` writes `_Module\_AutoGenerated\`, and `.gitignore` has `**/_AutoGenerated/*`. On
a case-sensitive filesystem those are different directories. **The real one on disk is
`_AutoGenerated`.** Glob case-insensitively so both spellings work.

### P0.5 - Compiler and linker flags

Translate `Esoterica.props` and `Esoterica.Win32.props` into clang flags.

Required:

| MSBuild setting | clang equivalent |
|---|---|
| `LanguageStandard: stdcpp20` | `-std=c++20` |
| `LanguageStandard_C: stdc17` | `-std=c17` |
| `ExceptionHandling: false`, `_HAS_EXCEPTIONS=0` | `-fno-exceptions -D_HAS_EXCEPTIONS=0` |
| `FloatingPointModel: Precise` | the clang default. Do **not** pass `-ffast-math`. |
| `/bigobj` | not needed |
| `AdditionalIncludeDirectories: $(SolutionDir)Code` | `-ICode` |
| `$(ProjectName.ToUpper().Replace('.','_'))` | `-DESOTERICA_BASE` and so on. **Required** for the export and import switch. |
| `EE_DEBUG=1`, `EE_RELEASE=1`, `EE_SHIPPING=1` | same |
| `EE_DLL` (Debug and Release only) | `-DEE_DLL` |
| `NDEBUG`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`, `_CRT_SECURE_NO_WARNINGS` | `-DNDEBUG` only. The rest are Windows-only. |
| `Optimization: Disabled` or `MaxSpeed` | `-O0` or `-O2` |
| `WholeProgramOptimization` (Shipping) | `-flto` |
| `TreatWarningAsError: true` | `-Werror`. **See the warning note below.** |
| `DebugInformationFormat: ProgramDatabase` | `-g` |
| `DynamicLibrary` | `-fPIC -shared`, output `.so` |
| `StaticLibrary` | `ar rcs`, output `.a` |

Add these too. They have no MSBuild equivalent:

- `-fvisibility=hidden` on `.so` targets, so that the visibility attribute added in Phase 1
  means something.
- `-Wl,-rpath,'$ORIGIN'` on executables, so they find sibling `.so` files. This replaces the
  MSBuild `Copy` targets in `DXC.props` and similar sheets.
- `-msse4.2 -mavx`. The project's hand-rolled SIMD math assumes these. The stale script had
  this right.

**On `-Werror`:** `Esoterica.props` sets `WarningLevel: EnableAllWarnings` with
`TreatWarningAsError: true`, and a long `DisableSpecificWarnings` list of MSVC warning numbers
that have no clang equivalents. Do **not** translate that list. Start with `-Wall -Wextra` and
**no** `-Werror`. Get a mostly clean build, then decide what to turn on. Chasing
`-Weverything -Werror` parity in Phase 0 will eat the whole phase for no gain, and Conventions
rule 3 forbids fixing upstream warnings anyway.

### P0.6 - Library linking

Implement the property sheet to link flag mapping from
[03-Dependencies.md](../03-Dependencies.md#property-sheet-to-link-flag-mapping).

Use `pkg-config` where it exists (`freetype2`, and later `sdl3`). Use an explicit `-l`
otherwise. Skip the dropped sheets (`AmdAgs`, `WinPixEventRuntime`, `Optick`, `SuperLuminal`,
`LivePP`, `NavPower`). Above all, **do not define their `EE_ENABLE_*` macros**. See Conventions
rule 4.

### P0.7 - Output layout, configurations, sanitizers

```
Build/Linux_Debug/  Build/Linux_Release/  Build/Linux_Shipping/
Build/_Temp/Linux_<Config>/<ProjectName>/
```

Mirror the MSBuild layout, which `Esoterica.props` sets as
`Build/$(Platform)_$(Configuration)/`, so that tooling and habits carry over.

Keep the ASan, TSan, and UBSan variants. **Drop MSan.** It needs an instrumented libc++ to
produce usable output, and without one it buries you in false positives. Fix the
`-fsanitize-address` typo. It must be `-fsanitize=address`.

Derive object file paths from a path *relative to the repo root*, not from the absolute source
path. The stale script embeds absolute paths, which breaks the build directory.

### P0.8 - `compile_commands.json`

Generate it with `ninja -t compdb` against the real Linux toolchain, not `clang-cl`. This gives
working clangd in editors, which speeds up every later phase. Treat it as required, not
optional.

### P0.9 - `DownloadDependencies.sh`

See
[03-Dependencies.md, DownloadDependencies.sh](../03-Dependencies.md#downloaddependenciessh).

This phase needs only these. Defer the rest to the phase that first needs them:

- **libunwind, libdw**, which Phase 1 needs
- **Freetype, SQLite**, which are system packages and easy

Defer LLVM (Phase 2), DXC (Phase 4), Vulkan, VMA, and SPIRV-Reflect (Phase 5), and SDL3
(Phase 6). Build the script so that it can fetch one dependency at a time
(`./DownloadDependencies.sh llvm`) as well as all of them. Full builds of LLVM and DXC take tens
of minutes each, and you should not pay that in Phase 0.

Check for the required system packages first, and fail with one message that lists everything
missing. Do not fail deep inside a nested build.

### P0.10 - `.gitignore`

Add `Build/`. The existing entry is lowercase `build/`, and Linux filesystems are
case-sensitive, so git currently tracks the build output directory. Confirmed with
`git check-ignore`.

---

## Acceptance criteria

Each one is checkable. All must pass.

1. `python3 Code/Scripts/NinjaGen/NinjaGen.py` exits 0 and writes
   `Build/Linux/Esoterica.ninja`.
2. The generated ninja file has a compile rule for every `.cpp` in `Esoterica.Base.vcxproj`
   **except** those `Exclusions.txt` covers. Kept plus excluded must account for all 147
   `ClCompile` entries.
3. The `Esoterica.Base` compile commands include `-DESOTERICA_BASE`, `-DEE_DLL` (Debug),
   `-ICode`, `-std=c++20`, and `-fno-exceptions`.
4. `Esoterica.Base` resolves to a `.so` target in Debug and Release, and a `.a` target in
   Shipping.
5. The generator writes `compile_commands.json`, and `clangd` can resolve
   `#include "Base/Esoterica.h"` from any source file.
6. A re-run with no changes produces a byte-identical ninja file, and
   `SyncUpstream.py --update` produces a byte-identical `UpstreamProjects.txt`. Both must be
   deterministic, so that they diff cleanly.
7. `git status --porcelain` shows no change to any `.vcxproj` file, and none to
   `Esoterica.slnx`.
8. `git check-ignore Build/foo` succeeds.
9. `python3 Code/Scripts/NinjaGen/SyncUpstream.py` exits 0 on a clean tree, and exits 1 with a
   diff when a `.vcxproj` gains a source. `SourceLists.py` reports no problems: no stale
   exclusion glob, no missing Linux source, no unknown project section.
10. **The Windows MSBuild build still succeeds**, unchanged.
11. `ninja -f Build/Linux/Esoterica.ninja` reaches the compiler and emits compile errors from
    *missing platform implementations*, not from generator bugs such as bad flags, missing
    include paths, or malformed rules. Save that error output. It is Phase 1's worklist.

## Do not

- Write any `_Linux.cpp`. That is Phase 1.
- Modify any `.vcxproj` file, or `Esoterica.slnx`.
- Modify `Code/Applications/BuildGenerator/`. It is dead. Leave it.
- Chase `-Weverything -Werror` parity with the MSVC warning configuration.
- Add CMake.
- Add Python dependencies beyond the standard library and the vendored
  `Code/Scripts/NinjaGen/ninja_syntax.py`.

## Notes for the next agent

Record this in [Progress.md](../Progress.md):

- The compile-error output from criterion 11, or where you saved it.
- Which `.props` sheets you mapped, and which are still unmapped.
- Any `.slnx` or `.vcxproj` construct you had to special-case.
- Any source you added to `Exclusions.txt` beyond the starting set, and why.
