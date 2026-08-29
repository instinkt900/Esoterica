# Dependencies

Every external dependency, and what happens to it on Linux.

`External/` is `.gitignore`d. On Windows, `DownloadDependencies.bat` fills it from a prebuilt
archive. Linux gets `DownloadDependencies.sh`, which builds from source into the same `External/`
layout. The property sheet to link flag mapping then stays parallel between the two platforms.

---

## Windows dependencies

Taken from `Code/PropertySheets/*.props` and the `.lib` files they reference.

| Dependency | Used by | Linux plan |
|---|---|---|
| **LLVM / libclang** (`libclang.lib`, `clangAST`, `clangBasic`, `clangLex`, about 18 `LLVM*.lib`) | Reflector, for C++ parsing | **Build from source, or use the official release tarball.** It is cross-platform. The version must match what upstream links against, so check `Code/PropertySheets/LLVM.props`. A distro `libclang-dev` is convenient, but its version drifts from upstream's. Prefer a pinned official tarball. |
| **DirectXShaderCompiler** (`dxcompiler.lib`) | Reflector, for HLSL compilation. Also `RHI_Direct3D12.cpp`. | **Build DXC from source for Linux.** It emits SPIR-V with `-spirv`, and produces `libdxcompiler.so`. DXIL signing (`dxil.dll`) is Windows-only, and does not matter for SPIR-V. Two patches are applied from `Code/Scripts/DXCPatches/`. `spirv-val` is built from the same clone's `external/SPIRV-Tools` and installed alongside `dxc`; see the row below. |
| **SPIRV-Tools** (`spirv-val`) | Checking Phase 4 acceptance criterion 3 | **Build from the SPIRV-Tools that DXC vendors**, and use that binary, at `External/DirectXShaderCompiler/bin/x64/spirv-val`. A distribution package is *not* interchangeable: Ubuntu 24.04 ships v2025.1, which wrongly rejects every mesh shader here on `VUID-CullPrimitiveEXT-CullPrimitiveEXT-07036`. The vendored v2026.3 is what DXC validates with internally, and it accepts all 46 stages. |
| **Freetype** | Font rasterization | `libfreetype-dev`, or build from source. Easy. |
| **SQLite** | Resource database | `libsqlite3-dev`, or the vendored amalgamation. Easy. |
| **GameNetworkingSockets** | Networking | Build from source with CMake. It needs protobuf, OpenSSL, and either libsodium or OpenSSL crypto. It is the most awkward of the build-it-yourself set. |
| **IXWebSocket** | Resource server websockets | Build from source with CMake. It needs zlib and OpenSSL. Straightforward. |
| **MeshOptimizer** | Mesh optimization | Build from source with CMake. `Code/Engine/ThirdParty/meshoptimizer/` holds only a 2-file wrapper (`meshoptimizer_esoterica.{h,cpp}`) over the real library in `External/MeshOptimizer/`, so the external build is genuinely needed. |
| **ctt** (`ctt_capi.lib`) | Texture compression | Build from source. Check that the upstream project has a Linux build path. If it does not, this is a real risk item. See the open questions below. |
| **RenderDoc** | In-app frame capture | **Keep.** RenderDoc supports Linux. Load `librenderdoc.so` with `dlopen` instead of `renderdoc.dll`. |
| **DbgHelp** | Stack walking, crash dumps | **Replace** with `libunwind` (or `backtrace_symbols`), plus `libdw` or `libdwarf` for symbols. Only `Platform_Win32.cpp` uses it. |
| **Optick** | Profiler | **Drop for now.** Optick's Linux support is limited, and its GUI is Windows-only. `EE_ENABLE_OPTICK` guards it, so leave that unset. Tracy is the natural later replacement. Treat that as a separate optional workstream, not part of this port. |
| **amd_ags** | AMD GPU extensions | **Drop.** It is Windows and Direct3D only by construction. |
| **WinPixEventRuntime** | PIX GPU markers | **Drop.** The Vulkan backend uses `VK_EXT_debug_utils` instead. |
| **SuperLuminal** | Profiler | **Drop.** Windows only. `EE_ENABLE_SUPERLUMINAL` guards it, so leave that unset. |
| **LivePP** | C++ hot reload | **Drop.** Windows only. `EE_ENABLE_LPP` guards it, so leave that unset. There is no Linux equivalent worth chasing. |
| **Navpower** | Navmesh | **Drop.** Windows only, licensed, and already off by default. `EE_ENABLE_NAVPOWER` guards it, so leave that unset. |
| **FBX SDK** | Mesh import | Already gone. `Esoterica.slnx` references `FBXSDK.props`, but `Code/PropertySheets/` does not contain it. Import now goes through the vendored `ufbx`, which is portable C. No action. |

### System libraries (Win32 imports that need no Linux equivalent)

`kernel32`, `advapi32`, `Shlwapi`, `userenv`, `ntdll`, `bcrypt`, `ws2_32`, `Xinput`, `D3D12`,
`dxgi`, and `dxguid`. libc, pthreads, SDL3, and Vulkan replace these, or nothing needs them.

---

## New Linux-only dependencies

| Dependency | Purpose | How to get it |
|---|---|---|
| **SDL3** | Windowing, input, gamepad, and the X11 or Wayland abstraction | Build from source, or use a distro package where SDL3 (not SDL2) exists. Pin the version. |
| **Vulkan headers and loader** | Vulkan API | `libvulkan-dev`. Not fetched into `External/`: the loader carries an ICD layer that has to match the installed drivers. **Plain loader, not `volk`** - see open question 3. |
| **VulkanMemoryAllocator (VMA)** | GPU allocator. Replaces `D3D12MemoryAllocator`. | Header only. `./DownloadDependencies.sh vma` puts `vk_mem_alloc.h` in `External/VMA/include/`. Pinned to `v3.4.0`. |
| **SPIRV-Reflect** | Shader reflection. Replaces `ID3D12ShaderReflection`. | `./DownloadDependencies.sh spirv_reflect` builds one C file into `External/SPIRV-Reflect/lib/libspirv-reflect.a`. Pinned to `vulkan-sdk-1.4.357.0`. |
| **libunwind and libdw** | Stack walking. Replaces DbgHelp. | `libunwind-dev`, `libdw-dev`. |
| **inotify** | File watching. Replaces `ReadDirectoryChangesW`. | Kernel API. No dependency. |
| **libuuid** *(optional)* | UUID generation. Replaces `CoCreateGuid`. | `uuid-dev`. The alternative is `getrandom(2)` plus a hand-written v4 UUID format, which needs no dependency. The engine only needs uniqueness, so prefer `getrandom`. |
| **XDG Desktop Portal** *(optional)* | Native file dialogs | Needed only in Phase 7, and only if the vendored `pfd` (portable-file-dialogs) is not enough. `pfd` calls `zenity` or `kdialog`, which is probably enough. |

Conventions rule 5 says none of these go into `Code/**/ThirdParty/`. They go in `External/`.

---

## Property sheet to link flag mapping

The build generator reads each `.vcxproj` file's `<Import Project="..\PropertySheets\X.props"/>`
elements to decide what to link. This is the table it implements.

| `.props` | Linux link flags | Notes |
|---|---|---|
| `Esoterica.props` | *(none)* | Defines and include dirs only |
| `Esoterica.Win32.props` | *(skipped)* | The generator replaces it, either with an `Esoterica.Linux.props` equivalent or with built-ins |
| `EA.props` | *(none)* | EASTL and EABase are header files plus vendored source |
| `Imgui.props` | *(none)* | Vendored source |
| `Box3D.props` | *(none)* | Vendored source, portable C |
| `LLVM.props` | `-lclang` plus the `LLVM*` set, or `llvm-config --libs` | Prefer `llvm-config` to list them |
| `DXC.props` | `-ldxcompiler` | Plus `-Wl,-rpath` for `libdxcompiler.so` |
| `FreeType.props` | `pkg-config --libs freetype2` | |
| `SQLite.props` | `-lsqlite3` | |
| `GameNetworkingSockets.props` | `-lGameNetworkingSockets` | Plus protobuf and OpenSSL |
| `ixWebSocket.props` | `-lixwebsocket -lz -lssl -lcrypto` | |
| `MeshOptimizer.props` | `-lmeshoptimizer` | |
| `CTT.props` | `-lctt_capi` | Waiting on the Linux build question below |
| `RenderDoc.props` | *(none)* | Header only. `./DownloadDependencies.sh renderdoc` fetches `renderdoc_app.h` from tag `v1.45`. The library is found at runtime with `dlopen`. |
| `AmdAgs.props` | *(skipped)* | Dropped |
| `WinPixEventRuntime.props` | *(skipped)* | Dropped |
| `Optick.props` | *(skipped)* | Dropped |
| `SuperLuminal.props` | *(skipped)* | Dropped |
| `LivePP.props` | *(skipped)* | Dropped |
| `NavPower.props` | *(skipped)* | Dropped |
| *(new)* SDL3 | `pkg-config --libs sdl3` | `Base` needs it from Phase 6 |
| *(new)* Vulkan | `pkg-config --libs vulkan` | `Base` needs it from Phase 5 |
| *(new)* VMA | *(none)* | Header only. `Base` needs it from Phase 5 |
| *(new)* SPIRVReflect | `-lspirv-reflect` | `Base` needs it from Phase 5 |
| *(new)* unwind | `-lunwind -ldw` | `Base` needs it from Phase 1 |

`DXC.props` also has MSBuild `Copy` targets (`DXC_CopyDLL`) that stage `dxcompiler.dll` and
`dxil.dll` into the output directory. The generator must do the equivalent for `.so` files, or
use `-Wl,-rpath,'$ORIGIN'` plus a staging step. Prefer `rpath`, because it has fewer moving
parts.

---

## `DownloadDependencies.sh`

This mirrors `DownloadDependencies.bat`, but it builds instead of unzipping. It must:

- Be idempotent. Skip anything it already built.
- Pin every version. An unpinned dependency drifts silently and produces a works-on-my-machine
  port.
- Produce the `External/<Name>/{inc,lib}/` layout that the `.props` sheets expect, so the
  generator's mapping table stays a direct translation.
- Print a clear summary of what it built and what it skipped.
- Check for the required system packages first, and fail with a message that lists the missing
  ones. Do not fail deep inside a nested CMake build.

Build LLVM and DXC last. Each takes tens of minutes, and everything else is fast, so failing
early on the cheap dependencies gives better feedback.

### DXC is pinned to a tag, and patched

`DXC_VERSION="v1.10.2605.37"`, which is commit `c4d8f4f9`. It is built from source rather than
downloaded, because its SPIR-V back end crashes on this engine's mesh shaders. The fixes live in
`Code/Scripts/DXCPatches/` and `fetch_dxc` applies them, in name order, before configuring. **A
patch that fails to apply is fatal**, because that means the pin moved and nobody re-checked the
fix.

The tag is the one the official prebuilt Linux binaries were built from, so the only difference
between this compiler and Microsoft's is the patches. See the 2026-08-28 decision entries in
[Progress.md](Progress.md) for why it is patched here rather than in a fork, and for the two
configure settings that keep a re-run incremental.

---

## Open questions to resolve during implementation

Answer each question in the phase that first needs it, then record the answer here. Questions
raised later, during implementation, are added to the table in
[Progress.md](Progress.md#open-questions) instead. Question 7, the indirect draws, is one of
those, and it is the one open question that blocks work today.

1. **`ctt` Linux support.** *(Phase 3)* Confirm that the texture-compression library builds on
   Linux. If it does not, the options are: find the upstream project and port it, swap in an
   equivalent compressor, or keep texture compression on Windows only and accept uncompressed
   textures on Linux for now. This is the least certain dependency in the list.
2. **LLVM version pinning.** *(Phase 2)* Read the exact version from `LLVM.props`, and confirm
   that the Reflector's use of `clangAST` compiles against it on Linux. Clang's AST C++ API is
   not stable across major versions.
3. ~~**`volk` or the Vulkan loader.**~~ *(Phase 5)* **Answered: the plain loader.** Nothing has
   been profiled yet, and nothing can be until the backend renders, so the default stands. It is
   one line in `Toolchain.SHEETS` plus an include if a profile ever argues otherwise.
4. **SDL3 availability.** *(Phase 6)* Confirm whether the target distributions package SDL3, or
   whether `DownloadDependencies.sh` must always build it.
5. **Does GameNetworkingSockets block the early phases?** *(Phase 1)* `Esoterica.Base` imports
   it, so it may block the first link. Check whether the linker can reach the networking code
   without it, or whether the build needs it before Phase 1 can finish.
