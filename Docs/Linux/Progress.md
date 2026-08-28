# Progress Log

Running state of the Linux port. **Every task appends here before it counts as done**
(Conventions rule 9). Newest entries go at the top of each section.

This file keeps a chain of independent agent sessions coherent. When you start a session, read
"Current state" and "In flight" first.

---

## Current state

**Phase: 4 (in progress).** DXC is now built from source with a patch that fixes its SPIR-V back
end, and the four material mesh shaders compile to SPIR-V. Five shaders still fail, on a second
and unrelated DXC limitation. See the 2026-08-28 entries below.

Previously: **Phase 3 (all build and compile criteria met).** `EsotericaResourceCompiler` builds,
links and compiles 22 of the 27 real resources under `Data/` into 38 output files: every texture,
mesh, physics mesh, physics material database and the map. Debug and Release produce
byte-identical output. All four libraries link. The file system watcher passes a 15-check
scratch test, including a subdirectory created after watching started and `IN_Q_OVERFLOW`.

The 5 that do not compile are the `.material` files, and they fail for one reason: they
deserialize `EE::Render::Shaders::DefaultPBRParameters`, a type that only the Reflector's shader
pass generates.

**Correction.** Phase 3 recorded that pass as blocked because "DXC crashes on the
`ResourceDescriptorHeap` bindless model". That is wrong, and it sent this session looking in the
wrong place. Bindless is fine: a shader using `ResourceDescriptorHeap` and `SamplerDescriptorHeap`
compiles to SPIR-V cleanly. There are two unrelated defects in DXC's SPIR-V back end, and neither
has anything to do with bindless. Both are described below.

Previously: **Phase 2 (complete on Linux).** `./RunReflection.sh` generates reflection for all five modules
and exits 0. 245 files, 238 of them `.cpp`. The run is idempotent, and clean plus rebuild
reproduces byte-identical output. The Windows build has not been run.

| Phase | Status |
|---|---|
| 0 - Build System | **done** |
| 1 - Base Platform Layer | **done** (Tester itself still blocked on Phase 2) |
| 2 - Reflector | **done on Linux** (criterion 5 and 8 need a Windows machine) |
| 3 - Resource Compiler | **done on Linux**, except the 5 materials and byte-comparison, both of which need Phase 4 or a Windows machine |
| 4 - Shader Pipeline | **in progress.** DXC builds from source and is patched; 4 of the 9 failing shaders now compile |
| 5 - Vulkan RHI | not started |
| 6 - Windowing and Input | not started |
| 7 - Editor and Tools | not started |

Linux build status: `libEsoterica.Base.so`, `libEsoterica.Engine.Runtime.so`,
`libEsoterica.Engine.Tools.so`, `libEsoterica.Game.Runtime.so`, `libEsoterica.Game.Tools.so`,
`Esoterica.Applications.Reflector` and `Esoterica.Applications.ResourceCompiler` all build.
`Applications/Editor` and `Applications/ResourceServer` do not, and are Phase 7.
`Applications/BuildGenerator` is excluded permanently.
Windows build status: **not run.** 69 upstream files carry `+494 -71` lines across Phases 0-3.

## In flight

**Phase 4, on `linux/p4-shader-pipeline`.** P4.1 is done: DXC is built from source, patched, and
compiles the four material mesh shaders to SPIR-V.

The next thing to settle is the counter-variable blocker described below. It stops five shaders,
and the obvious fix for it needs the P4.3 binding model decision first, which the phase document
says to make jointly with Phase 5. That decision has **not** been made.

Not yet started in this phase: P4.2 (SPIRV-Reflect), P4.4 (replacing `ID3D12ShaderReflection`),
P4.5 (`CompileShaders.sh`), P4.6.

**Two Phase 4 decisions are still unmade, and both are acceptance criteria.** Criterion 8, the
bindless binding model, described above. And criterion 9, clip-space Y: the DXC argument list in
`ShaderReflection_ShaderCompiler.cpp` passes neither `-fvk-invert-y` nor `-fvk-use-dx-position-w`,
so nothing inverts Y today and no layer has been chosen to do it. Doing it in both the shader
compiler and the Vulkan viewport, or in neither, is the classic porting bug the phase document
warns about.

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

### 2026-08-28 - P4.1 DXC is built from source and patched, and the material shaders compile

DXC's SPIR-V back end has two defects that stop this engine's shaders. Neither is about bindless,
which is what Phase 3 assumed. Both were reduced to minimal shaders of under ten lines, and both
were confirmed against the official prebuilt DXC so they are not artifacts of building it here.

**Defect 1, fixed.** A mesh shader output struct with a **struct member** segfaults the compiler,
in `SpirvEmitter::assignToMSOutAttribute` by way of `SpirvBuilder::createAccessChain`. This is
[DXC issue 8475](https://github.com/microsoft/DirectXShaderCompiler/issues/8475), open since
2026-05 with no fix and no comments; the stack there matches ours exactly. It stops five shaders,
because `PrimitiveOutput` in `RendererTypes.esh` holds a `PrimitiveOutputFlags m_primitiveFlags`
member. The same shaders compile on DXIL, so this is Linux-only and Windows is unaffected.

The fix is `Code/Scripts/DXCPatches/0001-spirv-nested-struct-in-mesh-shader-output.patch`, which
carries the full explanation. In short, `createStructOutputVar` flattens such an output into one
stage variable per **leaf** field, and the emitter never mirrored that. Three things were needed,
and the first two are the ones that are easy to get wrong:

- Recurse on the **type alone**, not on whether the member has a semantic. A struct member with
  its own semantic is flattened too. Testing the semantic looks right, compiles, fixes the toy
  case, and still leaves the engine's shaders crashing. It cost a build to find out.
- Extract by the **lowered** field index, not the AST one. Bitfields merge into one member when a
  struct is lowered, and `PrimitiveOutputFlags` is all bitfields, so the AST index runs off the
  end and the validator rejects the module with "Index is out of bounds". The bitfield's own bits
  then have to come back out with `createBitFieldExtract`.
- `collectArrayStructIndices` stopped walking at the first member, so `prims[i].f.a = x` pushed
  an index for a struct that has no SPIR-V counterpart.

**Defect 2, not fixed.** Assigning a **counter-bearing** resource from `ResourceDescriptorHeap`
fails with "cannot handle associated counter variable assignment". It stops the other five
shaders. See the decision entry below for why it stopped here.

**Verified:**

- **The Reflector's shader pass runs end to end with no crashes, and 23 of the 28 shaders it
  parses compile to SPIR-V.** Before this, DXC segfaulted inside `libdxcompiler.so`. Every
  remaining failure is now a clean diagnostic, and all five are defect 2.
- All four material mesh shaders (`DefaultPBR`, `DefaultColorOnlyPBR`, `ComplexSurfacePBR`,
  `Placeholder`) compile to SPIR-V. `DebugDrawMesh` moved off the segfault onto defect 2.
- `libdxcompiler.so` builds and `Esoterica.Applications.Reflector` links against it, which is
  acceptance criterion 1. Confirmed with `ldd`.
- Seven reduced shaders covering every shape of the bug pass, and the flat-struct controls that
  worked before still work.
- The patch applies cleanly to the pristine tag, and a build from pristine-plus-patch reproduces
  the result. That is the path `DownloadDependencies.sh` takes, not just my working tree.
- **No regression:** for a shader the patch does not affect, the patched compiler produces
  **byte-identical** SPIR-V to the official prebuilt one.

**`spirv-val` note.** The compiled material shaders fail `spirv-val --target-env vulkan1.3` on
`VUID-CullPrimitiveEXT-CullPrimitiveEXT-07036`. **This is pre-existing and unrelated.** The
unpatched official DXC produces the identical error for a flat output struct with
`SV_CullPrimitive`, which `PrimitiveOutput` has at `RendererTypes.esh:473`. DXC's own internal
validation passes, so it is a disagreement between DXC and the system `spirv-val`. Acceptance
criterion 3 depends on it and cannot be met until somebody works out which one is right.

**Files added:** `Code/Scripts/DXCPatches/0001-*.patch`.
**Files edited:** `DownloadDependencies.sh` (`fetch_dxc`, `requirements_dxc`). No upstream
`Code/` file was touched, so [TouchedFiles.md](TouchedFiles.md) is unchanged. No `.esh` was
edited, as Phase 4 requires.

### 2026-08-28 - P3.1-P3.7 Resource compiler builds, links and compiles data

- All four libraries link, `EsotericaResourceCompiler` links, and it compiles 22 of the 27 real
  resources under `Data/`: 15 textures, 4 meshes, 1 physics mesh, 1 physics material database,
  1 map. No errors, no crashes.
- `ctt` answered, and answered well. See the decision entry below.
- The file system watcher passes a 15-check scratch test at
  `$SCRATCH/watchertest/main.cpp`, not committed: create, modify, rename and delete in the
  watched root; the same four in a subdirectory created **after** watching started; a directory
  two levels below that; directory deletion; and `IN_Q_OVERFLOW` firing
  `OnMassiveChangeDetected`.

**Files added:** `Code/EngineTools/FileSystem/FileSystemWatcher_Linux.cpp`,
`Code/EngineTools/Core/SystemDialogs_Linux.cpp`, `Code/Base/Render/ComPtr_Linux.h`, and the
`fetch_ctt` function in `DownloadDependencies.sh`.

**Upstream files edited:** see [TouchedFiles.md](TouchedFiles.md). The Phase 3 additions this
session are 11 `inline` removals, 5 `va_copy` fixes, one CTAD fix in `PageAllocator.h`, one
`<time.h>` in vendored `delabella`, and 3-line include switches in four EngineTools UI files.

**Acceptance criteria:**

| # | Criterion | Result |
|---|---|---|
| 1 | Four libraries build | **met** |
| 2 | `EsotericaResourceCompiler` builds and links | **met**, Debug and Release. Release found two more `inline` declarations that Debug did not: `NodeGraph_FlowGraph.h` `GetInputPin` and `GetOutputPin`. Build Release too, or that class of bug hides. |
| 3 | Compiles the full `Data/` tree without errors | **partly.** 22 of 27 resources. The 5 `.material` files need shader-generated types. The other 13 files in `Data/` are `EE_DATA_FILE`, not resources: `.txtg`, `.meshgrp` and `.pml` have no compiler on any platform, and "failed to find a compiler" is the correct answer. |
| 4 | Byte-identical to Windows output | **not checked**, needs a Windows machine. What *was* checked: the Debug and Release compilers produce byte-identical output for all 38 files. That rules out the float-formatting and optimisation-dependent differences the phase doc warns about, and leaves only genuinely platform-dependent ones. |
| 5 | Watcher works, including a subdirectory created after the start | **met** |
| 6 | Reports `OnMassiveChangeDetected` on `IN_Q_OVERFLOW` | **met** |
| 7 | `FileRegistry.cpp` compiles and its watcher handling works | **met** (compiles and links; the watcher itself is tested above) |
| 8 | Open question 1 (`ctt`) answered and recorded | **met** |
| 9 | Windows MSBuild still succeeds | **not run.** Every edit is either `__linux__`-guarded or standard C++ that MSVC already accepts. |
| 10 | Every upstream file edited is in TouchedFiles.md | **met** |
| 11 | `FileSystemWatcher.h` shows 1 line changed | **met** |

## What the next session needs to know

- **`RenderSystem.cpp` is in the build again, on a placeholder.** `NinjaGen.py` writes an empty
  `Code/Engine/_Module/_AutoGenerated/Shaders/ShaderRegistration.{h,cpp}` when the Reflector has
  not produced one, and prints a `problem:` line about it on **every** run. That is deliberate:
  a stale placeholder is exactly the failure that leaves a green build behind. The Reflector
  overwrites it the moment its shader pass works, and the placeholder is never written over real
  output.
- **`Applications/BuildGenerator` is excluded permanently.** It parses `Esoterica.slnx` to write
  the reflection file lists MSBuild consumes; `NinjaGen.py` does that job here. Its only actual
  compile error was `__debugbreak`, so a shim would have made it build - and building a `.sln`
  parser on a platform with no `.sln` is pointless.
- **`Applications/Editor` and `Applications/ResourceServer` do not build.** Editor hits
  "member access into incomplete type `EE::EditorTool`" in `EditorUI.h`, which has the shape of
  the missing-forward-declaration bug that showed up three times in Phase 3. ResourceServer wants
  `shellapi.h`. Both are Phase 7.
- **The two big DXC facts.** Shader reflection works. Shader *compilation* segfaults inside
  `libdxcompiler.so` from `ShaderCompiler::CompileShaderStage`. Everything downstream of that -
  the materials, the renderer, `Shaders::Initialize` - waits on the bindless decision.

### 2026-08-27 - P2.1-P2.5 Reflector builds, links and runs

`Build/Linux_Release/Esoterica.Applications.Reflector` builds and links, and `./RunReflection.sh`
runs it against `Esoterica.slnx`. libclang parses the whole codebase with **no compile errors**.
It does **not** generate code yet: see "Where reflection stops".

Acceptance criteria met: **1, 2, 3, 4, 6, 7, 9**.

| # | Result |
|---|---|
| 1 | `Esoterica.Applications.Reflector` builds and links |
| 2 | `./RunReflection.sh` exits 0 and fills `_Module/_AutoGenerated/TypeInfo/` for all five modules: Base 7, Engine 114, EngineTools 91, Game 24, GameTools 2 `.cpp` |
| 3 | A second run produces byte-identical output, checked with `sha256sum` over all 245 files |
| 4 | `--clean` empties the directories, `--rebuild` regenerates them byte-identically |
| 6 | The build generator picks the new files up: 614 sources before reflection, **848** after |
| 7 | LLVM 21.1.8, pinned and recorded |
| 9 | Every upstream file edited is in TouchedFiles.md |

**Not met: 5 and 8.** Both need a Windows machine. Criterion 5 is the cross-platform output
diff, which is the strongest correctness check available and has **not been done**. Until it is,
"the output is correct" rests on the Reflector exiting 0 and the output being self-consistent,
which is weaker.

**P2.1, LLVM.** Pinned to **21.1.8**, fetched as the official prebuilt
`LLVM-21.1.8-Linux-X64.tar.xz` by `./DownloadDependencies.sh llvm`.

The version is **inferred, not documented**. `LLVM.props` records no version at all: it points at
`External/LLVM`, which the prebuilt Windows `External.zip` fills. The version comes from the
library list it links. `LLVMFrontendDirective` and `LLVMDebugInfoDWARFLowLevel` both first appear
in LLVM 21, checked against the llvm-project tree at `llvmorg-19.1.0`, `20.1.0` and `21.1.0`;
21.1.8 is the last 21.x release. **If the Reflector ever misbehaves in a way that smells like an
AST mismatch, this pin is the first thing to question.**

The official prebuilt release is used rather than a source build: same artifact the LLVM project
ships, pinned, and minutes rather than hours. A distro `libclang-dev` is deliberately not used;
Ubuntu 24.04 offers 18, three major versions adrift.

**The release archives hold LLVM IR bitcode, not ELF objects** - the release is built with LTO.
GNU `ld` rejects them with "file format not recognized". The generator therefore links anything
importing `LLVM.props` with the `ld.lld` that ships in the same tarball. This is the single least
obvious thing in the phase.

**P2.2, platform code.** As small as the plan said: guard two unused Windows includes in
`ReflectorApplication.cpp`, and route one `GetShortPath` call through `Platform::Linux`.

**P2.3, shader reflection.** Excluded in the generator, which is the phase document's preferred
option 1. `Code/Applications/Reflector/ShaderReflection/**` is one line in `Exclusions.txt`. The
`ShaderCompiler.{h,cpp}` bodies are also wrapped in `#ifdef _WIN32`, and `Reflector.cpp` takes an
early return at the top of `ReflectShaders`. `RunReflection.sh` passes `-typeinfo`.

**Phase 4 reverses all four of those**, and nothing is deleted.

**P2.4, the autogenerated directory case.** It is **`_AutoGenerated`**, capital G.
`ReflectorSettings.h:15` sets `g_autogeneratedDirectoryName = "_AutoGenerated"`, and the
Reflector is self-consistent about it. `.gitignore` already matches. Only `Esoterica.props`
disagrees, with `_Autogenerated`, and that is an MSBuild wildcard which matches either way on
Windows; the build generator globs case-insensitively, so no change is needed.

**P2.5, `RunReflection.sh`.** Mirrors `RunReflection.bat` plus the `clean` and `rebuild` targets
of `Reflect.nmake`, including emptying the autogenerated directories itself. It does **not**
build the Reflector first, unlike `Reflect.nmake`: that hides which step failed, and the ninja
command is one line.

**The Reflector handles `.slnx` natively.** `ReflectorApplication.cpp:57` checks
`MatchesExtension( "slnx" )`. There was no solution-format problem to solve.

## What was actually wrong

Every failure in this phase was upstream code that assumes Windows. In rough order of how long
each took to find:

**`IsUnderToolsPath` searched for a hardcoded `"\EngineTools\"`.** On Linux that never matches,
so no header was ever classified as tools-only, so every tools header was parsed in the
no-dev-tools pass, where the `ImGuiX` types it references do not exist. This produced a dozen
confusing errors about `Gizmo`, `FilterWidget` and `TextBuffer` that looked like missing
platform guards and were nothing of the kind. It now builds the search string from
`FileSystem::Path::s_pathDelimiter`.

**Five path constants and an include-path array spelled with backslashes.** The Reflector created
a directory literally named `Build\_Temp\` in the repository root, and found no headers at all.
Two include paths also had the wrong case.

**The builtin type table assumes LLP64.** `ULongLong` maps to `uint64_t` and there is no `ULong`
case, so on LP64 Linux every 64-bit property failed to resolve.

**`-fms-compatibility`** makes clang emulate MSVC closely enough to break glibc's headers.

**libclang needs `-resource-dir`** when it is linked directly rather than driven by a clang
executable.

**Three case-mismatched includes**, and two two-phase-lookup problems that MSVC's delayed lookup
hides.

## Notes for the next session

- **The Reflector parses all project headers as a single translation unit.** One Windows-gated
  type breaks reflection for the whole project. This is the root cause of most of what went
  wrong in this phase.
- **`-fms-extensions` and `-fms-compatibility` are off on Linux.** `-fms-compatibility` makes
  clang behave enough like MSVC to break glibc's headers: `__STRICT_ANSI__ seems to have been
  undefined`, then `char16_t` and `char32_t` undeclared. Nothing needs them here, because
  `_Module/API.h` takes its `visibility( "default" )` branch.
- **libclang needs `-resource-dir`.** It normally finds its builtin headers relative to the
  clang executable, and the Reflector links libclang directly, so there is none. `ClangParser.cpp`
  discovers the one versioned directory under `External/LLVM/lib/clang` rather than writing the
  version down.
- **`FileSystem::Path::GetCorrectCaseForPath` now does real work on Linux.** Phase 1 made it a
  pass-through, reasoning that the correct case for a path is the case it was given. That holds
  for paths the engine produces and fails for paths out of a `.vcxproj`, several of which
  disagree with the disk. It walks the path a component at a time and recovers the real spelling.
- The Windows build has **not** been run against any of this, and **criterion 5, the
  cross-platform output diff, has not been done.** That diff is the real correctness check for
  this phase. Someone with a Windows machine should run the Reflector at this commit and compare
  `_Module/_AutoGenerated` against the Linux output, expecting only line-ending differences.

### 2026-08-27 - P1.1-P1.10 Base platform layer

`Esoterica.Base` compiles and links on Linux. 132 of 132 translation units, in Debug, Release
and Shipping. It went from 1 of 132 at the start of the phase.

New files, all listed in `Code/Scripts/NinjaGen/LinuxSources.txt`:

| File | Task |
|---|---|
| `Platform/Platform_Linux.h` | P1.1 |
| `Math/Platform/Math_Linux.h` | P1.2 |
| `Threading/Platform/Threading_Linux.cpp` | P1.3 |
| `FileSystem/Platform/FileSystem_Linux.cpp`, `FileSystemPath_Linux.cpp` | P1.4 |
| `Platform/PlatformUtils_Linux.{h,cpp}` | P1.5 |
| `Types/Platform/Types_Linux.cpp` | P1.6 |
| `Logging/Platform/SystemLog_Linux.cpp` | P1.7 |
| `Platform/Platform_Linux.cpp` | P1.8 |
| `Render/RHI_Vulkan.cpp` | P1.9, 102 stubs |
| `Imgui/Platform/ImguiPlatform_Linux.cpp`, `Input/InputDevices/Platform/InputDevice_KeyboardMouse_Linux.cpp`, `InputDevice_XBoxController_Linux.cpp` | **not in the plan**, see below |

Acceptance criteria met: **1, 2, 3, 5, 6, 7, 9.**

Not met:

- **4, `Tester` links and runs.** Not achievable in Phase 1. See the correction below.
- **8, the Windows MSBuild build.** Not run; there is no Windows machine on this side. This
  phase edits 12 upstream files, so this is a real gap, not a formality.

**Answers the notes the phase document asked for:**

- **`SyncEvent` is a manual reset event.** `Threading_Win32.cpp` calls
  `CreateEvent( nullptr, TRUE, FALSE, nullptr )`, and the second argument is `bManualReset`. So
  it stays signalled once signalled, releasing every current and future waiter, until `Reset()`
  is called explicitly; waiting does not clear it. The Linux version is a condition variable
  plus an explicit flag, with `notify_all`.
- **`GetFileModifiedTime` returns nanoseconds since the epoch.** Every caller stores the value
  and later compares it for equality, so it never has to agree with the Win32 FILETIME.
  Nanoseconds rather than seconds because seconds let two edits inside the same second look
  identical, which shows up as a resource that silently fails to recompile.
- **Open question 5, does GameNetworkingSockets block Base?** Yes, and at *compile* time, not
  link time. `DownloadDependencies.sh` builds it.
- **Open question 6, does `Memory.cpp` have a working non-Windows path?** No. Three unguarded
  functions called `VirtualAlloc`, `VirtualFree` and `InterlockedAdd64`.

## Plan corrections found during Phase 1

**P1.8 was mis-scoped.** The document calls it the largest task in the phase, 249 lines of stack
walking and crash dumps. Lines 25 to 235 of `Platform_Win32.cpp` are inside an `#if 0`, and
`Initialize` and `Shutdown` have their bodies commented out. None of it runs, so there was no
behaviour to match. The Linux version is a real signal handler anyway, because acceptance
criterion 6 asks for a backtrace, but it is ~90 lines rather than ~250.

**Three Phase 6 stubs were needed in Phase 1.** The plan has the RHI stub for exactly this
reason but missed the same argument for imgui and input: `ImguiPlatform_Win32.cpp`,
`InputDevice_KeyboardMouse_Win32.cpp` and `InputDevice_XboxController_Win32.cpp` are excluded,
and without definitions `Esoterica.Base` does not link, so nothing downstream can be built or
tested. They halt, except the controller dead zone and threshold getters, which return the
XInput constants: those are read during device construction before anything is plugged in, and
halting there would stop the engine starting at all.

**Only five `_Module/API.h` files exist, not six.** `Code/Applications/Tester/_Module/API.h` is
a single `#pragma once` and declares no export macro.

**`Esoterica.Applications.Tester` is not an empty console app.** The plan describes it as one,
and says its only job is to prove the `.so` loads. Its `Main.cpp` includes
`EngineTools/_Module/_AutoGenerated/TypeInfo/TypeRegistration.h`, a Reflector output, plus a
dozen `EngineTools` headers. It cannot link until Phase 2 and Phase 3. Criterion 5's scratch
program was used instead, which is what proved Base works.

**Optick cannot be dropped.** `Profiling.h` includes `<optick.h>` unconditionally, and
Conventions rule 4 forbids stripping that include, so the header has to exist. It also only
zeroes `USE_OPTICK` when `EE_DEVELOPMENT_TOOLS` is off, so Debug and Release emitted real Optick
calls. `DownloadDependencies.sh` fetches the headers, and the generator passes `-DUSE_OPTICK=0`.

**`DownloadDependencies.bat` has no Linux analogue.** It downloads one prebuilt `External.zip`
of Windows binaries from an upstream release. Each dependency is fetched and built from source
instead.

## Notes for the next session

- **Run the Windows build.** Twelve upstream files changed. The riskiest is `IniFile.cpp`, where
  an `#endif` moved.
- **Phase 2 is the critical path.** Nothing above `Base` builds without the reflection codegen.
- The RHI stub generator is not committed, on purpose: Phase 5 replaces those bodies, so
  regenerating would be destructive.
- `ixWebSocket` is pinned to **v12.0.1**. `NetworkServer_WebSockets.cpp` builds
  `ix::WebSocketServer` with 8 arguments, and the 8th only exists from v12.0.0.
- The smoke test used for criteria 5 and 6 is not committed. It links against `Base` directly,
  which `Tester` cannot do yet.

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

### 2026-08-28 - DXC is built from source and patched, rather than forked or vendored

**Context:** two defects in DXC's SPIR-V back end stop nine of this engine's shaders. We are on
the newest DXC release, so there is no version to upgrade to, and no flag avoids either defect
(`-fspv-use-descriptor-heap`, `-fvk-bind-counter-heap` and include-order variants were all
tried). The alternatives were to patch DXC, or to restructure the `.esh` files, which Phase 4
rule 4 forbids and which would change what Windows compiles.

**Decision: patch DXC.** The patches live in `Code/Scripts/DXCPatches/` and
`DownloadDependencies.sh` applies them to a pinned source clone, in name order, before building.
A patch that fails to apply is fatal: that means the pin moved and nobody re-checked the fix,
which is exactly the silent-breakage case worth stopping on.

**Why not vendor the DXC tree into this repository.** It is a 250MB LLVM fork with four
submodules, `/External/` is in `.gitignore` and nothing in it is tracked, and every other
dependency is fetched rather than vendored. Vendoring would also bury our two fixes among three
million lines of Microsoft's code, where no reviewer would ever find them.

**Why not fork DXC on GitHub, yet.** A fork is the right home once the patches grow or need to
track DXC `main`, and it is the mechanism for submitting upstream. For two fixes in one file, a
patch file next to the port's own documentation is easier to review and keeps the rationale with
the port. **Move to a fork if the patching becomes substantial.**

**Consequences.**

- The dependency now takes about 20 minutes to build, on 8 jobs, and is by far the slowest.
- **A re-run takes 20 seconds**, recompiling `SpirvEmitter.cpp` and relinking. Measured, after
  the fix below.
- **Two settings protect that, and both are easy to undo by accident.** `git clean` in
  `fetch_dxc` excludes `build`, or the clean deletes the build tree. And the configure passes
  `-DLLVM_APPEND_VC_REV=OFF`, against DXC's own cache file, which turns it on: with it on the
  generated version header derives from git state, and anything that moves the commit count
  rewrites that header and rebuilds **all 1018 targets**. That was measured the hard way, by
  committing a patch locally while debugging it and watching the next run rebuild the world.
  Being careful about the claim: the reset in `fetch_dxc` keeps the tree on the pinned tag, so
  the commit count should be stable without the flag as well. The flag pins the LLVM half of the
  version too, and costs nothing, so it stays.
- `libdxil.so` is no longer installed. It signs DXIL containers, which is a Windows concern.
- The reported version changes from the official `1.10(5373-c4d8f4f9)`. The **source** is the same
  commit the official binary was built from, `c4d8f4f9`, so the only difference between the two
  compilers is the patches.
- Both patches are worth sending upstream. Defect 1 already has an open issue with no owner.

### 2026-08-28 - Defect 2 stops here, because the fix needs the binding model decision

**Context:** assigning a counter-bearing resource from a descriptor heap fails with "cannot
handle associated counter variable assignment". It stops `DebugDraw` MS, `DebugDrawMesh` MS, and
`DebugDrawResolve`, `InstancePickingResolve` and `WorldUpdate` CS.

**What it actually is,** narrowed with minimal shaders so nobody has to re-derive it:

| Case | Result |
|---|---|
| `RWStructuredBuffer` **initialized** from the heap, as a local | works |
| `RWStructuredBuffer` **assigned** from the heap, to a local | fails |
| `RWStructuredBuffer` **assigned** from the heap, to a struct field | fails |
| `RWBuffer` or `RWByteAddressBuffer`, either way | works, they carry no counter |

So it is assignment, not initialization, and only for a type that carries a hidden counter.
`AppendBuffer<T>` in `AppendBuffer.esh` holds a `RWStructuredBuffer<T>` and is built exactly this
way. `RawAppendBuffer` next to it uses `RWByteAddressBuffer` and was never affected.

**The Reflector's own code generator emits this pattern too**, which matters for option 3 below.
Two of the five failures are not in hand-written shader source at all:

```
_AutoGenerated/ShaderReflection/WorldUpdate.esh:91
    result.m_meshInstanceBuffer = ResourceDescriptorHeap[NonUniformResourceIndex( ... )];
_AutoGenerated/ShaderReflection/DebugDrawResolve.esh:54
    result.ArgumentBuffer_TransparentDepthOn = ResourceDescriptorHeap[NonUniformResourceIndex( ... )];
```

Those come from `ShaderReflection_CodeGenerator.cpp` building a resource table struct. Rewriting
`AppendBuffer.esh` by hand would therefore fix three of the five and leave these two, so option 3
is not sufficient on its own even if somebody were willing to break the no-`.esh`-edits rule.

`tryToAssignCounterVar` requires source and destination to agree on whether they have a counter.
The destination gets one created on demand; the source never resolves to one, so they disagree.
`getFinalACSBufferCounter` cannot see the heap for two reasons at once: `heap[i]` is a
`CXXOperatorCallExpr`, so `getReferencedDef` resolves it to the subscript operator and the
`AssocCounter#1` early return hands back that operator's null pair; and the heap branch below it
tests `isResourceDescriptorHeap` against the expression's own type, which is the indexed resource,
where `.Resource` is the type of the heap object being indexed. `isDescriptorHeap`, which
`getDescriptorHeapOperands` already asserts on, is the predicate that would work.

**Why that is not enough, and why this stopped.** Correcting both only gets as far as
`getOrCreateCounterIdAliasPair`, which looks in `counterVars` and `declRWSBuffers`. A
`ResourceDescriptorHeap` declaration is in neither, so **no counter exists for a heap-sourced
`RWStructuredBuffer` at all.** Making one exist means emitting a counter heap, which is what
`-fvk-bind-counter-heap` binds, and that defines a descriptor set and binding the engine must
then bind. That is the P4.3 bindless binding model decision, which
[Phase4-ShaderPipeline.md](Phases/Phase4-ShaderPipeline.md) calls the most consequential in the
phase and says to make **jointly with Phase 5**. It has not been made.

The reordering fix was written and then **reverted**, so it is not in the shipped patch. It
changes behaviour without fixing the shaders, which is unverified risk for no gain. Redoing it is
maybe ten minutes from this entry.

**Options, for whoever picks this up:**

1. Decide the binding model, then teach DXC to emit a counter heap. Largest, and it is the work
   Phase 5 needs anyway.
2. Have DXC skip the association when the source is a heap access with no counter, instead of
   erroring, and error at any later `.Append()` or `.IncrementCounter()` instead. Small, and it
   matches what this engine does: it never calls those, and drives its own `m_counterBuffer`.
   Needs care not to turn a compile error into silently wrong code.
3. Restructure `AppendBuffer.esh` to keep the `RWStructuredBuffer` out of the struct. Forbidden by
   Phase 4 rule 4, changes Windows, and **fixes only three of the five** anyway, because the
   other two come out of the Reflector's code generator. Escalate before anyone tries it.

Option 2 is the cheapest way to unblock all five without pre-empting the binding decision.

### 2026-08-28 - A Windows-only `.cpp` is excluded, never wrapped in `#ifdef _WIN32`

**Context:** `Code/EngineTools/Core/SystemDialogs.cpp` is 552 lines of COM `IFileDialog` with an
unguarded `<windows.h>`. The Phase 3 plan said to wrap the whole body in `#ifdef _WIN32`, the
"wrap, do not split" technique, and that is what the first Phase 3 commit did.

**Decision:** Name it in `Exclusions.txt` instead. The upstream file is byte-identical again.

**Rationale:** Both approaches produce the same object code: nothing. The difference is where
the knowledge lives.

- **Zero lines added beats two.** Every line in an upstream file is a line a future
  `git merge upstream/main` can conflict on. The wrap touches the first and last line of the
  file, which are exactly the lines an upstream reformat or a new include is most likely to
  move.
- **An exclusion is checked; an `#ifdef` is not.** `SourceLists.load` reports any pattern that
  matches nothing, so an upstream rename or delete fails the build with a named problem. A
  `#ifdef _WIN32` whose file upstream has replaced sits there silently forever.
- **It states the reason in the place a reviewer already reads.** `Exclusions.txt` is the one
  file a post-merge audit opens to see what the Linux build drops and why. A guard buried at
  line 1 of a 552-line file is not in that inventory.
- **The shared header was never the obstacle.** The original note claimed the file had to be
  wrapped "because the header is shared". `SystemDialogs.h` is untouched either way - it is
  `SystemDialogs_Linux.cpp` that includes it and defines every symbol it declares out of line.

**The rule:** a `.cpp` gets an exclusion. A header gets the wrap, because nothing lists headers,
so there is no way to exclude one. `ShaderReflection_ShaderCompiler.h` is the only file the wrap
still applies to, and Phase 4 has already replaced its whole-body wrap with targeted guards.

**Alternatives rejected:** keeping the wrap for symmetry with the header. Symmetry between a
`.cpp` and a `.h` is worth less than an upstream file with no diff at all.

### 2026-08-28 - `ctt` is open source, and is built from crates.io rather than substituted

Open question 1 is answered, and it is the best of the three outcomes
[Phase3-ResourceCompiler.md](Phases/Phase3-ResourceCompiler.md) lists: option 1, port it.

It did not look that way at first. `CTT.props` links `ctt_capi.lib` alongside `ws2_32`, `bcrypt`
and `ntdll`, upstream's `External.zip` contains only `ctt_capi.dll`, `ctt_capi.lib` and `ctt.h`,
and there is no source anywhere in the tree. That is the shape of a closed Windows binary, and
the plan document calls it "the least certain dependency in the whole port".

The `README.md` inside the zip says otherwise. `ctt` is a Rust library with C bindings, published
on crates.io as `ctt-c-api`, licensed MIT / Apache-2.0 / Zlib. It binds five open-source encoder
backends - bc7enc-rdo, Intel ISPC Texture Compressor, etcpak, AMD Compressonator and astcenc -
and it ships prebuilt ISPC static libraries for every supported platform, so a default build
needs only a Rust toolchain and a C++ compiler.

`DownloadDependencies.sh fetch_ctt` therefore downloads the `ctt-c-api` 0.5.0 crate tarball and
runs `cargo build --release`, producing `libctt_capi.so` in the layout `CTT.props` expects.

**Version pinning matters here.** 0.5.0 was published 2026-07-16 and the `ctt.h` in upstream's
zip is dated 2026-07-19, so it is the matching release. All 64 `ctt_*` and `CTT_*` identifiers
`ResourceCompiler_RenderTexture.cpp` uses are present in the 0.5.0 header. Check that again
before bumping the pin; a mismatch would be silent API drift rather than a clean error.

**Consequences:** none of the bad ones. This is the same library at the same version, not a
substituted compressor, so compressed texture bytes stay comparable with Windows and the
byte-identical criterion survives. The costs are a Rust toolchain of 1.90 or newer (the crate is
edition 2024; the check is in `fetch_ctt` because cargo's own failure does not mention the
version) and about four minutes of build time for the five C/C++ encoder backends.

### 2026-08-27 - DXC on Linux is blocked by clashing COM shims, not by DXC itself

**Context:** Eight `Engine/Render` files include `.esf` shader sources, which include generated
`.esh` reflection headers. Those come from the Reflector's `-shaders` pass, which Phase 2
disabled because it needs DXC. So Phase 3's "Engine builds" criterion depends on Phase 4 work.
The decision was to bring DXC forward rather than exclude the files or fake the headers.

**What worked.** DXC itself is easy. Microsoft ships official prebuilt Linux binaries, so
`./DownloadDependencies.sh dxc` is a download, not an hours-long LLVM build. It lands in
`External/DirectXShaderCompiler` with `inc/` and `lib/x64/`, matching what `DXC.props` expects.

`ShaderReflection_ShaderCompiler.h` also includes **`d3d12shader.h`**, which is a Windows SDK
header that the Linux DXC tarball does not ship. **DirectX-Headers**, Microsoft's cross-platform
D3D12 headers, supplies it. The plan never mentions this dependency and it is not optional.

Neither header is reachable through a property sheet: the Reflector imports only `Esoterica` and
`LLVM`, because on Windows both headers come from the Windows SDK and are on the default search
path. The Linux equivalent is to put them in the global include set, which is what
`ESOTERICA_INCLUDE_DIRECTORIES` now does. The DXC *library* stays on `DXC.props`.

**Where it stops.** DXC's `WinAdapter.h` and DirectX-Headers' `wsl/stubs` are **two alternative
COM shims for the same types**, and the shader compiler needs headers from both. Include them
together and `IUnknown` ends up declared but never defined, so every
`ID3D10Blob : public IUnknown` in `d3dcommon.h` fails with "base class has incomplete type".

Measured, so the next attempt does not repeat it:

| Include set | Errors |
|---|---|
| DXC + DirectX-Headers `directx/` only | 1 (`rpc.h` missing) |
| plus the trivial stubs, no `basetsd.h` | 1 |
| plus `basetsd.h`, `unknwnbase.h`, `oaidl.h` | 1 (`ocidl.h` missing) |
| plus `ocidl.h` | **20** |
| `dxcapi.h` included before `d3d12shader.h` | 17 |

`ocidl.h` is a five-line empty stub, so it is not the cause: adding it simply lets compilation
reach line 423 of `d3dcommon.h`, where the real problem is. Include order does not fix it either.

**Current state:** the four Phase 2 shader guards are **restored**, so the tree builds. The DXC
and DirectX-Headers dependencies, and the include-path plumbing, are kept, because any solution
needs them.

**What to try next**, in order:

1. Find the guard macro that makes DXC's `WinAdapter.h` and DirectX-Headers cooperate. Both are
   Microsoft projects and this combination must work somewhere; `__EMULATE_UUID` and the
   `__IUnknown_INTERFACE_DEFINED__` family are the places to look.
2. Use DXC's reflection interfaces (`IDxcContainerReflection`) instead of
   `ID3D12ShaderReflection`, dropping `d3d12shader.h` entirely. This is an upstream code change
   to `ShaderReflection_ShaderCompiler`, so **escalate first**.
3. Reconsider whether Linux shader reflection should go through SPIRV-Reflect, which Phase 5
   brings in for Vulkan anyway. That is a larger design change and definitely needs escalation.

### 2026-08-27 - Upstream merges happen on request only

**Context:** The plan called for merging `upstream/main` weekly, or before each new phase,
whichever came first, on the reasoning that merge cost grows faster than drift.

**Decision:** No cadence. Merge only when explicitly asked. `AGENTS.md` and
[01-UpstreamMerges.md](01-UpstreamMerges.md) both say so.

**Rationale:** The churn measurement taken during Phase 0 undercuts the original reasoning.
Upstream has 107 commits in total, and four in the last twelve months touched a source list. The
drift is cheap and stays cheap. What is not cheap is debugging a half-finished port and a merge
at the same time: when something breaks, there is no way to tell which change caused it. The
port is therefore built against **one fixed upstream commit** until it works end-to-end.

**Alternatives rejected:** Merging before each phase, which is the same trade at a slower rate.
Merging when `SyncUpstream.py` reports drift, which turns a useful signal into an interrupt.

**Note:** `upstream/main` is at `47e6293` (2026-08-26) and this fork is based on `6813cf9`. One
commit of drift, touching a `.vcxproj` source list by +2/-1. Recorded so nobody has to rediscover
it; **not** a reason to merge.

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
  searched directories that had lost a Windows file, and `SystemDialogs.cpp` was to be guarded
  rather than excluded. (Phase 3 excluded it instead, so this particular example no longer
  holds. The two below still do, and they were the decisive ones.)
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
| 1 | ~~Does `ctt` (texture compression) build on Linux?~~ | Phase 3 | **answered: yes, it is open source. Built from crates.io.** |
| 2 | Which LLVM version does the Reflector need, and does `clangAST` compile against it on Linux? | Phase 2 | open |
| 3 | Use `volk`, or the plain Vulkan loader? | Phase 5 | open |
| 4 | Do the target distros package SDL3, or must we always build it? | Phase 6 | open |
| 5 | ~~Does `GameNetworkingSockets` block the first `Base` link?~~ | Phase 1 | **answered: yes, and at compile time, not link** |
| 6 | ~~Does the `VirtualAlloc` region in `Memory.cpp` have a working non-Windows path?~~ | Phase 1 | **answered: no** |

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

### Eleven functions are `inline` on one side of the declaration only

Eight headers declare a member `inline` whose only definition is out of line in the matching
`.cpp` (`ResourceRecord.h`, `InputSystem.h`, `ImguiX.h`, `AnimationSkeleton.h`,
`Animation_RuntimeGraph_Instance.h` twice, `Animation_RuntimeGraphNode_Blend1D.h`,
`ResourceCompilerContext.h`). `MathUtils.cpp` has the mirror image: three `ToString` definitions
marked `inline` where the header declares them `EE_BASE_API` without it.

Both are ill-formed. An inline function has to be defined in every translation unit that uses it,
and clang emits nothing for a definition no other TU can see. MSVC emits them anyway because
`__declspec( dllexport )` forces it, so the bug is invisible on Windows. Fixed here by dropping
`inline`, which is correct on both compilers. Worth an upstream PR: it is a one-word change per
site and it costs Windows nothing.

### `va_list` is read twice in five places

`CompileContext::LogError`, `LogWarning` and `LogMessage` each `va_start` once and then hand the
same `va_list` to two consumers - `SystemLog::AddEntryVarArgs` and `m_log.Log*`. `ImguiX.h`'s two
`DrawShadowedText` overloads do the same to draw the shadow and then the text.

On MSVC x64 a `va_list` is a pointer passed by value, so the callee's advance does not disturb the
caller's copy and the second read happens to work. In the System V ABI it is a one-element array
that decays to a pointer, the callee advances the *caller's* state, and the second read walks off
the end of the argument area. This segfaulted every standalone resource compile, immediately after
the resource had compiled successfully. Fixed here with `va_copy`. This one is a genuine latent
bug on Windows too, not merely a portability difference, and is the better upstream PR of the two.

### The Reflector hardcodes Windows path separators in five places

`ReflectorSettings.h` spells `g_codeFolderPath`, `g_buildFolderPath`, `g_buildTempFolderPath`,
`g_runtimeEngineProjectPath` and `g_toolsEngineProjectPath` with backslashes, and
`ClangParser.cpp`'s `g_includePaths` array does the same. `Reflector.cpp` also appends `.vcxproj`
paths verbatim. On Linux a backslash is an ordinary filename character, so this created a
directory literally named `Build\_Temp\` in the repository root and found no headers at all.

Two of the `g_includePaths` entries also have the wrong case: `EABase\include\common` against
`EABase/include/Common`, and `EASTL\include` against `EASTL/Include`.

### The Reflector's builtin type table assumes LLP64

`ClangUtils.cpp` maps `clang::BuiltinType::ULongLong` to `uint64_t` and has no case for `ULong`.
That is correct on Windows, where `uint64_t` is `unsigned long long`. Linux is LP64, so
`uint64_t` is `unsigned long`, and every 64-bit property failed with
"Cannot resolve property typename (uint64_t)".

### `FileSystem.h` has an inline that never returns

`UpdateBinaryFile` at line 97 forwards to its overload and drops the result:

```cpp
EE_FORCE_INLINE bool UpdateBinaryFile( char const* pFilePath, Blob const& fileData, bool* pWasFileUpdated = nullptr )
{ UpdateBinaryFile( pFilePath, fileData.data(), fileData.size(), pWasFileUpdated ); }
```

Falling off the end of a non-void function is undefined behaviour. Every caller reads a garbage
return value. Not fixed here (Conventions rule 3), and worth reporting upstream: this one is a
real bug on Windows too.

### `Math_Win32.h` truncates `GetMostSignificantBit` above 2^32

`Code/Base/Math/Platform/Math_Win32.h` casts its argument to `unsigned long` before the scan:

```cpp
_BitScanReverse64( &index, (unsigned long) value );
```

`unsigned long` is 32 bits on Windows, so every value above `2^32` gives the wrong answer.
`Math_Linux.h` uses `__builtin_clzll` and is correct for the full 64-bit range. **The two
platforms therefore disagree**, which is worse than either bug alone, so it is commented in the
Linux header as well as recorded here. Not fixed on the Win32 side (Conventions rule 3).

### `SystemLog_Win32.cpp` drops newlines on medium-length messages

`TraceMessage` bounds its newline append at `numCharsWritten < 509` while the buffer is 2048
bytes, so messages between 509 and 2045 characters silently lose their newline.
`SystemLog_Linux.cpp` bounds it at the real buffer size. Commented in the Linux file.

### `IniFile.cpp` puts its whole body inside `#if defined(_MSC_VER)`

The guard opens at line 4 and its matching `#endif` is the **last line of the file**. On any
non-MSVC compiler the translation unit produces nothing at all: it compiles cleanly and leaves
`IniFile::Load`, `Save`, `GetString` and `SetString` undefined until something tries to link an
executable. This is the single most expensive bug found so far, because there is no compile
error to point at.

### Two `#include` directives use a backslash

`Code/Base/Utils/GlobalRegistryBase.h` and
`Code/Base/Input/InputDevices/InputDevice_Controller.cpp`. clang does not treat `\` as a path
separator inside an include, so neither header was found on Linux. MSVC accepts `/`, so fixing
them costs Windows nothing.

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
