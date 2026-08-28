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
| Files needing a whole-body guard wrap (2 lines) | 0 - the one candidate is an exclusion instead |
| Files needing a real edit | 5 |
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
| `Code/EngineTools/FileSystem/FileSystemWatcher.h` | Line 15: change `#if _WIN32` to `#if _WIN32 \|\| defined( __linux__ )`. One line modified. **Needed in Phase 2, not Phase 3:** the Reflector parses every project header in one translation unit, so a Windows-gated `Watcher` class breaks reflection for all of EngineTools. | 2 (was planned for 3) | done |
| `Code/Base/Utils/GlobalRegistryBase.h` | Line 2: `#include "Base\Esoterica.h"` to `#include "Base/Esoterica.h"`. One character. clang does not treat `\` as a path separator in an include, so on Linux the header is simply not found. MSVC accepts `/`, so Windows is unaffected. | 0 | done |
| `Code/Base/Input/InputDevices/InputDevice_Controller.cpp` | Line 2: `#include "Base\Math\Vector.h"` to `#include "Base/Math/Vector.h"`. Same reason as above. | 0 | done |
| `Code/Base/Types/Color.h` | Hoist `struct ByteColor` out of the anonymous union that holds it. A named type may not be declared inside an anonymous union in standard C++; MSVC allows it as an extension. Layout, member names and Windows behaviour are unchanged. | 1 | done |
| `Code/Base/TypeSystem/TypeInstance.h` | One line: `template<typename T> friend class TTypeInstance;`. `TTypeInstance`'s copy constructor reads `m_pInstance` through a `TypeInstance const&`, and standard C++ only allows a derived class to reach a protected member through an object of its own type. MSVC permits it without the friend. | 1 | done |
| `Code/Base/Resource/ResourcePtr.h` | A forward declaration of `EE::TResourcePtr` at `EE` scope, plus `template<typename T> friend class EE::TResourcePtr;`. Same protected-access rule as above. The qualification matters: `TResourcePtr` lives in `EE`, not in `EE::Resource`. | 1 | done |
| `Code/Base/Memory/Memory.h` | Line 18: `#ifdef _WIN32` to `#if defined( _WIN32 ) \|\| defined( __linux__ )`. One line modified. The existing `#else` defines the stack allocators as empty, so `EE_STACK_ALLOC` and `EE_STACK_ARRAY_ALLOC` expanded to nothing. `alloca` works on both platforms; `Platform_Linux.h` supplies `<alloca.h>`. | 1 | done |
| `Code/Base/Memory/Memory.cpp` | `#elif defined( __linux__ )` for `<sys/mman.h>`, and an `#else` inside each of `VirtualMemoryReserve`, `VirtualMemoryCommit` and `VirtualMemoryFree`. `mmap` with `PROT_NONE` and `MAP_NORESERVE` is the `MEM_RESERVE` equivalent, `mprotect` is `MEM_COMMIT`, `munmap` is `MEM_RELEASE`, and `__atomic_fetch_add` replaces `InterlockedAdd64`. Windows lines untouched. | 1 | done |
| `Code/Base/Resource/ResourceTypeID.h` | Line 26: `template<eastl_size_t S>` to `template<int S>`. `eastl::fixed_string` declares its size parameter as `int`, so deducing an `eastl_size_t` from `TInlineString<9>` fails. MSVC accepts the narrowing during deduction. | 1 | done |

## Two-line guard additions

Each of these files already has a platform guard. Add a sibling branch, and nothing else.

| File | Line | Existing | Add | Phase | Status |
|---|---|---|---|---|---|
| `Code/Base/Esoterica.h` | 55 | `#if _WIN32` includes `Platform/Platform_Win32.h` | `#elif defined( __linux__ )` includes `Platform/Platform_Linux.h` | 1 | **done** (2 added, 0 modified) |
| `Code/Base/Math/Math.h` | 12 | `#if _WIN32` includes `Platform/Math_Win32.h` | `#elif defined( __linux__ )` includes `Platform/Math_Linux.h` | 1 | **done** (2 added, 0 modified) |
| `Code/Base/Settings/IniFile.cpp` | 4 | `#if defined(_MSC_VER)` wraps the pragmas, the `ini.h` include **and the entire file body** - the matching `#endif` is the last line | Close the guard right after the pragmas, and reopen it around the trailing `#pragma warning( pop )`. **2 lines added, 0 modified.** The plan expected a compile failure; the actual symptom was worse: the translation unit produced *nothing* on clang, so `IniFile::Load`, `Save`, `GetString` and `SetString` were silently undefined until an executable tried to link. | 1 | **done** |
| `Code/Base/Imgui/ImguiSystem.cpp` | 12 | `#if _WIN32` includes `imgui_freetype.h` | `#if _WIN32 \|\| defined( __linux__ )`. Linux uses Freetype too. | 6 | planned |
| `Code/Base/_Module/API.h` | 5 | `__declspec(dllexport)` and `dllimport` | A `#if defined( __linux__ )` branch using `__attribute__(( visibility( "default" ) ))`, placed first so the existing `#if`/`#ifdef` becomes an `#elif`. 2 added, 1 modified. | 1 | **done** |
| `Code/Engine/_Module/API.h` | - | as above | as above | 1 | **done** |
| `Code/EngineTools/_Module/API.h` | - | as above | as above | 1 | **done** |
| `Code/Game/_Module/API.h` | - | as above | as above | 1 | **done** |
| `Code/GameTools/_Module/API.h` | - | as above | as above | 1 | **done** |
| ~~`Code/Applications/Tester/_Module/API.h`~~ | - | - | **No change needed.** The file is one line, `#pragma once`, and declares no export macro. The survey assumed six API.h files; there are five. | 1 | **not needed** |

*(The `API.h` files get separate rows because each is an independent edit. They are one logical
change, so land them in one commit. Note that only **five** need changing, not six.)*

**On the ELF import case.** The plan expected separate export and import branches. ELF needs
only one: `visibility( "default" )` on an imported declaration is correct and harmless, so a
single `#if defined( __linux__ )` branch covers both and keeps the diff to 2 added, 1 modified.

## Missing includes that MSVC supplies transitively

MSVC's standard headers pull in more than they promise, so this codebase relies on includes it
never writes. libstdc++ and libc++ do not. Each fix is **2 lines added, 0 modified**.

| File | Change | Phase | Status |
|---|---|---|---|
| `Code/Base/Threading/Threading.h` | Add `#include <thread>` and `#include <condition_variable>`. The file uses `std::thread` and `std::condition_variable` but includes only `<mutex>` and `<shared_mutex>`. | 1 | done |
| `Code/Base/Render/HandleAllocator.h` | Wrap `#include <intrin.h>` in `#if _WIN32`, with an `#else` including `<immintrin.h>`. The MSVC header is where `_tzcnt_u64` and `_lzcnt_u64` come from on Windows; clang has them in `<immintrin.h>`. Guarded rather than deleted, per Conventions rule 3. | 1 | done |

## Phase 3 - Resource Compiler

| File | Change | Phase | Status |
|---|---|---|---|
| `Code/Engine/Entity/EntitySystem.h` | Forward-declare `class Entity;`. It is used in the inline body of `CreateAdditionalRequiredComponents` and never declared. MSVC's delayed lookup finds it; standard C++ needs it visible. 1 line added. | 3 | done |
| `Code/Base/Resource/ResourcePtr.h` | Add `TResourcePtr<T>::operator=( ResourceID const& )`. Assigning a `ResourceID` was ambiguous: it converts to `ResourcePtr`, which then matches both the existing `operator=` and the implicit copy assignment reached through the converting constructor. An exact match removes the choice, with no behaviour change. | 3 | done |
| `Code/Engine/Debug/Widgets/SystemLogWidget.cpp` | `SomeChildrenChecked = -1` to `uint64_t( -1 )`. The enum has a `uint64_t` underlying type, so `-1` narrows, which MSVC accepts and standard C++ does not. Same bits. | 3 | done |
| `Code/Engine/Physics/Components/Component_PhysicsShape.h` | Forward-declare `class PhysicsWorld;`. Only `friend class PhysicsWorld;` declarations introduce the name elsewhere, and a friend declaration does not make a name findable by ordinary lookup. Fixed 5 files at once. | 3 | done |
| `Code/Engine/Entity/EntityMap.h` | `#include "Entity.h"`. A template member dereferences `Entity*` in a non-dependent expression, so clang needs the complete type at the point of definition; MSVC defers to instantiation. No include cycle: `Entity.h` does not include `EntityMap.h`. | 3 | done |
| `Code/Engine/Render/Imgui/ImguiRenderer.cpp` | Two sites: `T alignas( 32 ) x;` to `alignas( 32 ) T x;`. `alignas` is a declaration specifier and belongs before the type. MSVC accepts either order. | 3 | done |
| `Code/Engine/Entity/EntityInitializationContext.h` | Forward-declare `class EntityMap;` inside `namespace EE::EntityModel`. `InitializationContext` says `friend EntityMap;`, and unqualified lookup escaped to the `class EntityMap;` in namespace `EE` at the top of the file - a phantom, since the real class is `EE::EntityModel::EntityMap`. The friendship applied to a class nobody defines. | 3 | done |
| `Code/Engine/Render/Device/DeviceRenderView.cpp` | `"base/Render/RHI.h"` to `"Base/Render/RHI.h"`. Case mismatch. | 3 | done |
| `Code/Engine/**`, `Code/EngineTools/**` (6 files) | `<eastl/...>` to `<EASTL/...>`. Case mismatches; the directory is `EASTL`. | 3 | done |
| `Code/Base/Types/Path.h` | `TPath::IsParentOf` called `potentialChild.size()` and `potentialChild.m_path[i]`. `TPath` has neither: the accessor is `Size()` and the member is `m_elements`. The function has evidently never been instantiated, so MSVC never looked inside it. Fixing it cleared 96 of EngineTools' 110 errors. | 3 | done |
| `Code/EngineTools/Entity/EntityEditor/EntityEditor_EntityItem.h`, `EntityEditor_UndoableAction.h`, `Code/EngineTools/Game/ResourceEditors/ResourceEditor_Hitbox.h` | Forward-declare `class EntityWorld;` in namespace `EE`. All three use it as a pointer without declaring it; MSVC finds it through another translation unit's includes. Declared rather than including `EntityWorld.h`, which would pull the whole engine world into a tools header. Cleared 20 errors. | 3 | done |
| `Code/Base/FileSystem/FileSystemPath.h`, `DataPath.h`, `DataFileExtension.h` | `template<size_t S>` / `template<eastl_size_t S>` to `template<int S>` on the `TInlineString<S>` overloads. `eastl::fixed_string` declares its size parameter as `int`, so deducing any other type from a `TInlineString<N>` fails. Same fix as `ResourceTypeID.h` in Phase 2. | 3 | done |
| `Code/Base/Resource/ResourcePtr.h` (2) | Add `TResourcePtr<T>::operator=( nullptr_t )`. Assigning `nullptr` was ambiguous for the same reason as `ResourceID`. | 3 | done |
| `Code/Engine/Render/Device/DeviceRenderWorld.cpp` | `Math::Max( 1ULL, ...size() )` to `Math::Max( size_t( 1 ), ... )`. `Math::Max` deduces one type from both arguments; on Windows `size_t` *is* `unsigned long long` so they agree, and on LP64 Linux it is `unsigned long`. | 3 | done |
| `Code/EngineTools/Game/ResourceEditors/ResourceEditor_Hitbox.h` (2) | Forward-declare `class Hitbox;`. Used as a pointer at line 163 and never declared, which broke the class and cascaded into a dozen "cannot initialize object parameter" errors that looked like a broken inheritance chain. | 3 | done |
| `Code/EngineTools/Import/Formats/FBX.cpp` | `"EngineTools/Import/importedSkeleton.h"` to `ImportedSkeleton.h`. Case mismatch. | 3 | done |
| `Code/Base/Math/MathUtils.cpp` | Drop `inline` from the three `ToString` definitions. `MathUtils.h` declares them `EE_BASE_API` without it, so the `.cpp` definition is the only one, and an `inline` function that no other TU can see is emitted nowhere. MSVC exports it anyway through `__declspec( dllexport )`. | 3 | done |
| `Code/Base/Resource/ResourceRecord.h`, `Code/Base/Input/InputSystem.h`, `Code/Base/Imgui/ImguiX.h`, `Code/Engine/Animation/AnimationSkeleton.h`, `Animation_RuntimeGraph_Instance.h` (2), `Animation_RuntimeGraphNode_Blend1D.h`, `Code/EngineTools/Resource/ResourceCompilerContext.h` | The same defect, mirrored: 8 declarations marked `inline` whose only definition is out of line in the matching `.cpp`. That is ill-formed - an inline function must be defined in every TU that uses it - and clang emits nothing. Dropping `inline` is correct on both compilers. | 3 | done |
| `Code/Base/Render/PageAllocator.h` | `TArrayView( ... )` to `TArrayView<T>( ... )`. Class template argument deduction through the alias makes clang build `eastl::span`'s implicit deduction guides with the alias's fixed extent, and `span( T (&)[N] ) -> span<T, N>` then forms an array of `size_t( -1 )` elements. `m_data` is `TArrayView<T>`, so nothing was being deduced. | 3 | done |
| `Code/EngineTools/Resource/ResourceCompilerContext.cpp` | `va_copy` in `LogError`, `LogWarning` and `LogMessage`. Each reads one `va_list` twice, once for `SystemLog::AddEntryVarArgs` and once for `m_log.Log*`. On MSVC x64 a `va_list` is a pointer passed by value, so the second read works; in the System V ABI it is an array that decays, the callee advances the caller's own state, and the second read segfaults. This crashed every standalone resource compile. | 3 | done |
| `Code/Base/Imgui/ImguiX.h` (2) | `va_copy` in both `DrawShadowedText` overloads, which draw twice from one `va_list`. Same defect as above; it would crash the editor on Linux rather than the compiler. | 3 | done |
| `Code/EngineTools/Resource/Tools/EditorTool_ResourceBrowser.cpp`, `EditorTool_ResourceImporter.cpp`, `Code/EngineTools/Widgets/Pickers/DataPathPicker.cpp`, `ResourcePickers.cpp` | Add a 3-line `#if defined( __linux__ )` include of `PlatformUtils_Linux.h` next to the existing `PlatformUtils_Win32.h` include. The 5 `Platform::Win32::OpenInExplorer` call sites are left alone: `PlatformUtils_Linux.h` aliases `namespace Win32 = Linux`, which keeps the diff to the include block rather than putting a guard around every call in editor UI code. | 3 | done |
| `Code/EngineTools/ThirdParty/delabella/delabella.cpp` | Add an `#else` branch including `<time.h>` beside the `#ifdef _WIN32` `<windows.h>` block. The non-Windows path of `uSec()` already calls `clock_gettime( CLOCK_MONOTONIC, ... )` and nothing declared it. Vendored third-party, but the fix is upstream-shaped. | 3 | done |

## Phase 4 (brought forward) - Shader pipeline

| File | Change | Phase | Status |
|---|---|---|---|
| `Code/Applications/Reflector/ShaderReflection/ShaderReflection_ShaderCompiler.h` | Replace the Phase 2 whole-body wrap with targeted guards. `d3d12shader.h` and `<wrl/client.h>` go behind `#if _WIN32`; Linux takes `ComPtr_Linux.h` instead. | 4 | done |
| `Code/Applications/Reflector/ShaderReflection/ShaderReflection_ShaderCompiler.cpp` | Unwrapped. `-spirv -fspv-target-env=vulkan1.3` on Linux instead of DXC's default DXIL. `-Qembed_debug` and `-Fd` are DXIL-only and make libdxcompiler **segfault** on the SPIR-V path, so they are Windows-only. | 4 | done |
| `Code/Applications/Reflector/ShaderReflection/ShaderReflection_ShaderCompiler.cpp` (2) | `DXC_ARG_BINDING_MODEL`, the P4.3 bindless binding model, on Linux only. Pins the resource and sampler heaps to set 1 and shifts root parameter bindings by register type, so that every shader agrees on where they are and the Vulkan backend has a fixed contract to bind against. Empty on Windows. | 4 | done |
| `Code/Applications/Reflector/ShaderReflection/ShaderReflection_CodeGenerator.cpp` | Replace `std::stable_sort( doc.begin(), doc.end(), ... )`. `pugi::xml_node_iterator` is bidirectional and `stable_sort` needs random access. **The comparator was also wrong**: it returned `stricmp`'s `int`, true for a difference in *either* direction, which is not a strict weak ordering and so was undefined behaviour on Windows too. | 4 | done |
| `Code/Base/Render/RHI.esh` | `HLSL_STATIC_ASSERT` becomes a no-op under `__spirv__`. DXC's SPIR-V back end does not implement `_Static_assert`. **The plan predicted no `.esh` file would need edits; that was wrong.** | 4 | done |
| `Code/Applications/Reflector/ShaderReflection/ComPtr_Linux.h` | **New file.** A minimal `Microsoft::WRL::ComPtr` replacement. DXC's own `CComPtr` lacks `Get`, `GetAddressOf` and `ReleaseAndGetAddressOf`, which is what the eleven call sites use. | 4 | done |

## Phase 2 - Reflector

| File | Change | Phase | Status |
|---|---|---|---|
| `Code/Applications/Reflector/ReflectorApplication.cpp` | Wrap `<windows.h>` and `<consoleapi2.h>` in `#if _WIN32`. Neither is used: the file calls no Windows API. Guarded rather than deleted, per Conventions rule 3. | 2 | done |
| `Code/Applications/Reflector/TypeReflection/Clang/ClangParser.cpp` | `#elif defined( __linux__ )` for `PlatformUtils_Linux.h`, and an `#if _WIN32` / `#else` around the single `Platform::Win32::GetShortPath` call. `GetShortPath` works around `MAX_PATH`; the Linux version returns its input unchanged. | 2 | done |
| `Code/Applications/Reflector/Reflector.cpp` | Guard the `ShaderReflection_ShaderCompiler.h` include; `#if !_WIN32` early return at the top of `ReflectShaders` (**Phase 4 reverses this**); `system( "cls" )` to `clear`; normalise MSBuild backslashes in header and dependency paths; resolve header path case. | 2 | done |
| `Code/Applications/Reflector/Reflector.cpp` (2) | `IsUnderToolsPath` searched for a hardcoded `"\\EngineTools\\"`, so on Linux no header was ever classified as tools-only. It now builds the search string from `FileSystem::Path::s_pathDelimiter`. Without this, every tools header was parsed in the no-dev-tools pass and failed on `ImGuiX` types that only exist when `EE_DEVELOPMENT_TOOLS` is set. | 2 | done |
| `Code/Applications/Reflector/ReflectorSettings.h` | Spell the five path constants per platform (`g_codeFolderPath`, `g_buildFolderPath`, `g_buildTempFolderPath`, `g_runtimeEngineProjectPath`, `g_toolsEngineProjectPath`). A backslash is a separator on Windows and an ordinary filename character here, so the Reflector was creating a directory literally named `Build\_Temp\`. | 2 | done |
| `Code/Applications/Reflector/TypeReflection/Clang/ClangUtils.h` | `<clang/AST/Ast.h>` to `<clang/AST/AST.h>`. Case mismatch; the real header is `AST.h`. | 2 | done |
| `Code/Applications/Reflector/TypeReflection/Clang/ClangUtils.cpp` | Add `BuiltinType::ULong` and `Long` cases, guarded to non-Windows. Windows is LLP64 so `uint64_t` is `unsigned long long`; Linux is LP64 where it is `unsigned long`, and without these every 64-bit property failed to resolve. | 2 | done |
| `Code/Applications/Reflector/TypeReflection/Clang/ClangVisitors_TranslationUnit.cpp` | `"Clangvisitors_Structure.h"` to `"ClangVisitors_Structure.h"`. Case mismatch. | 2 | done |
| `Code/Applications/Reflector/TypeReflection/TypeReflection_CodeGenerator.cpp` | `<eastl/sort.h>` to `<EASTL/sort.h>`. Case mismatch. | 2 | done |
| `Code/Base/Utils/CommandLineParser.h` | Forward-declare `operator<<( std::ostream&, String const& )` above the template that uses it. The definition is at the bottom of the file, and two-phase lookup cannot reach it: `String` is an eastl type, so ADL searches `eastl`, not `EE`. | 2 | done |
| `Code/EngineTools/Resource/ResourceCompilerContext.h` | Forward-declare `class Compiler;`. Only a `friend class Compiler;` declares it today, and a friend declaration does not make a name findable by ordinary lookup. | 2 | done |

## Whole-body guard wrap

"Wrap, do not split" makes an unguarded Windows-only file inert on Linux: `#ifdef _WIN32` as
the first line, `#endif` as the last. Two lines added, zero modified.

**Use it only on a header.** A `.cpp` needs no edit at all - name it in
[Exclusions.txt](../../Code/Scripts/NinjaGen/Exclusions.txt) and the Linux build never sees it,
which is zero lines added. An exclusion is also self-checking: a pattern that stops matching is
reported as a problem, whereas a stale `#ifdef` sits in the file forever. A header cannot be
excluded, because nothing lists headers, so a header that must not be parsed on Linux is wrapped.

A wrapped or excluded `.cpp` still needs a `_Linux.cpp` sibling, listed in
[LinuxSources.txt](../../Code/Scripts/NinjaGen/LinuxSources.txt), to supply the symbols its
shared header declares.

| File | Change | Phase | Status |
|---|---|---|---|
| `Code/Applications/Reflector/ShaderReflection/ShaderReflection_ShaderCompiler.h` | Wrap the body in `#ifdef _WIN32`. It includes `d3d12shader.h` and `dxcapi.h`, and it is the **only** shader reflection header that needs DXC. **Phase 4 reverses this.** | 2 | done |
| `Code/Applications/Reflector/ShaderReflection/ShaderReflection_ShaderCompiler.cpp` | As above. | 2 | done |

## Confirmed to need no change

Checked during the survey. Recorded so that nobody investigates them again.

| File | Why no change is needed |
|---|---|
| ~~`Code/Base/Memory/Memory.h`~~ | **The survey was wrong; this needed an edit.** See "Real edits". The `#else` branch exists but defines `EE_STACK_ALLOC` and `EE_STACK_ARRAY_ALLOC` as *nothing*, so every call site failed with "expected expression". |
| ~~`Code/Base/Memory/Memory.cpp`~~ | **Checked in Phase 1; it did need an edit.** See "Real edits". `VirtualMemoryReserve`, `VirtualMemoryCommit` and `VirtualMemoryFree` sit *outside* the `_WIN32` guard and call `VirtualAlloc`, `VirtualFree` and `InterlockedAdd64` directly. This answers open question 6: there was no working non-Windows path. |
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
| `.gitignore` | Add `Build/`, `compile_commands.json`, `.ninja_deps`, `.ninja_log`. The ninja build writes all four, and none were ignored. Appended, nothing modified. | 0 | done |

## New build generator files

Additive, so no upstream conflict is possible. Listed here because they sit in an upstream
directory, `Code/Scripts/NinjaGen/`, which Conventions rule 7 designates for build tooling.

| New file | Purpose | Phase |
|---|---|---|
| `Code/Scripts/NinjaGen/SyncUpstream.py` | Reads `Esoterica.slnx` and the `.vcxproj` files. Writes and checks `UpstreamProjects.txt` | 0 |
| `Code/Scripts/NinjaGen/SourceLists.py` | The three-list format, and the build model built from it | 0 |
| `Code/Scripts/NinjaGen/Toolchain.py` | Property sheet and compiler flag translation | 0 |
| `Code/Scripts/NinjaGen/Checks.py` | The few checks a green build would not catch | 0 |
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
