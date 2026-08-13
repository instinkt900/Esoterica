# Dependencies

Every external dependency, and what happens to it on Linux.

`External/` is `.gitignore`d and populated by `DownloadDependencies.bat` on Windows from a
prebuilt archive. Linux gets `DownloadDependencies.sh`, which builds from source into the same
`External/` layout so that the property-sheet → link-flag mapping stays parallel.

---

## Windows dependencies

Derived from `Code/PropertySheets/*.props` and the `.lib` files they reference.

| Dependency | Used by | Linux plan |
|---|---|---|
| **LLVM / libclang** (`libclang.lib`, `clangAST`, `clangBasic`, `clangLex`, ~18 `LLVM*.lib`) | Reflector — C++ parsing | **Build from source or use official release tarball.** Cross-platform. The version must match what upstream links against; check `Code/PropertySheets/LLVM.props`. Distro `libclang-dev` is convenient but the version will drift from upstream's — prefer a pinned official tarball. |
| **DirectXShaderCompiler** (`dxcompiler.lib`) | Reflector — HLSL compilation; `RHI_Direct3D12.cpp` | **Build DXC from source for Linux.** Emits SPIR-V with `-spirv`. Produces `libdxcompiler.so`. Note DXIL signing (`dxil.dll`) is Windows-only and irrelevant for SPIR-V. |
| **Freetype** | Font rasterisation | `libfreetype-dev`, or build from source. Trivial. |
| **SQLite** | Resource database | `libsqlite3-dev`, or the vendored amalgamation. Trivial. |
| **GameNetworkingSockets** | Networking | Build from source (CMake). Needs protobuf, OpenSSL, and either libsodium or OpenSSL crypto. The fiddliest of the "just build it" set. |
| **IXWebSocket** | Resource server websockets | Build from source (CMake). Needs zlib + OpenSSL. Straightforward. |
| **MeshOptimizer** | Mesh optimisation | Build from source (CMake). Note `Code/Engine/ThirdParty/meshoptimizer/` is only a 2-file wrapper (`meshoptimizer_esoterica.{h,cpp}`) over the real library in `External/MeshOptimizer/`, so the external build is genuinely required. |
| **ctt** (`ctt_capi.lib`) | Texture compression | Build from source. Verify the upstream project provides a Linux build path; if not, this is a genuine risk item — see Open Questions below. |
| **RenderDoc** | In-app frame capture | **Keep.** RenderDoc supports Linux; load `librenderdoc.so` instead of `renderdoc.dll` via `dlopen`. |
| **DbgHelp** | Stack walking, crash dumps | **Replace** with `libunwind` (or `backtrace_symbols`) plus `libdw`/`libdwarf` for symbolisation. Used only in `Platform_Win32.cpp`. |
| **Optick** | Profiler | **Drop initially.** Optick's Linux support is limited and its GUI is Windows-only. Guarded by `EE_ENABLE_OPTICK`; leave unset. Tracy is the natural later replacement — treat as a separate, optional workstream, not part of this port. |
| **amd_ags** | AMD GPU extensions | **Drop.** Windows/Direct3D only by construction. |
| **WinPixEventRuntime** | PIX GPU markers | **Drop.** Replaced by `VK_EXT_debug_utils` in the Vulkan backend. |
| **SuperLuminal** | Profiler | **Drop.** Windows only. Guarded by `EE_ENABLE_SUPERLUMINAL`; leave unset. |
| **LivePP** | C++ hot reload | **Drop.** Windows only. Guarded by `EE_ENABLE_LPP`; leave unset. No Linux equivalent worth pursuing. |
| **Navpower** | Navmesh | **Drop.** Windows only, licensed, and already disabled by default. Guarded by `EE_ENABLE_NAVPOWER`; leave unset. |
| **FBX SDK** | Mesh import | Already gone — `FBXSDK.props` is referenced by `Esoterica.slnx` but absent from `Code/PropertySheets/`. Import now goes through the vendored `ufbx`, which is portable C. No action. |

### System libraries (Win32 imports with no Linux analogue needed)

`kernel32`, `advapi32`, `Shlwapi`, `userenv`, `ntdll`, `bcrypt`, `ws2_32`, `Xinput`,
`D3D12`, `dxgi`, `dxguid` — all replaced by libc/pthreads/SDL3/Vulkan or simply not needed.

---

## New Linux-only dependencies

| Dependency | Purpose | Acquisition |
|---|---|---|
| **SDL3** | Windowing, input, gamepad, X11/Wayland abstraction | Build from source, or distro package where SDL3 (not SDL2) is available. Pin the version. |
| **Vulkan headers + loader** | Vulkan API | `vulkan-headers`, `libvulkan-dev`. Consider `volk` for function loading to avoid the loader's dispatch overhead — decide in Phase 5. |
| **VulkanMemoryAllocator (VMA)** | GPU allocator; replaces `D3D12MemoryAllocator` | Header-only. Vendor into `External/VMA/`. |
| **SPIRV-Reflect** | Shader reflection; replaces `ID3D12ShaderReflection` | Small, header+source. Vendor into `External/SPIRV-Reflect/`. |
| **libunwind + libdw** | Stack walking; replaces DbgHelp | `libunwind-dev`, `libdw-dev`. |
| **inotify** | File watching; replaces `ReadDirectoryChangesW` | Kernel API, no dependency. |
| **libuuid** *(optional)* | UUID generation; replaces `CoCreateGuid` | `uuid-dev`. Alternatively use `getrandom(2)` and format a v4 UUID by hand — fewer dependencies, and the engine only needs uniqueness. Prefer `getrandom`. |
| **XDG Desktop Portal** *(optional)* | Native file dialogs | Only needed in Phase 7, and only if the vendored `pfd` (portable-file-dialogs) proves insufficient. `pfd` shells out to `zenity`/`kdialog` and is likely good enough. |

Per Conventions rule 5, none of these are vendored into `Code/**/ThirdParty/`. They go in
`External/`.

---

## Property sheet → link flag mapping

The build generator reads each `.vcxproj`'s `<Import Project="..\PropertySheets\X.props"/>`
elements to decide what to link. This is the table it implements.

| `.props` | Linux link flags | Notes |
|---|---|---|
| `Esoterica.props` | *(none)* | Defines and include dirs only |
| `Esoterica.Win32.props` | *(skipped)* | Replaced by a new `Esoterica.Linux.props`-equivalent in the generator, or handled as generator built-ins |
| `EA.props` | *(none)* | EASTL/EABase are header + vendored source |
| `Imgui.props` | *(none)* | Vendored source |
| `Box3D.props` | *(none)* | Vendored source, portable C |
| `LLVM.props` | `-lclang` + the `LLVM*` set, or `llvm-config --libs` | Prefer `llvm-config` to enumerate |
| `DXC.props` | `-ldxcompiler` | Plus `-Wl,-rpath` for `libdxcompiler.so` |
| `FreeType.props` | `pkg-config --libs freetype2` | |
| `SQLite.props` | `-lsqlite3` | |
| `GameNetworkingSockets.props` | `-lGameNetworkingSockets` | Plus protobuf, OpenSSL |
| `ixWebSocket.props` | `-lixwebsocket -lz -lssl -lcrypto` | |
| `MeshOptimizer.props` | `-lmeshoptimizer` | |
| `CTT.props` | `-lctt_capi` | Pending the Linux build question below |
| `RenderDoc.props` | *(none)* | Header only; loaded via `dlopen` at runtime |
| `AmdAgs.props` | *(skipped)* | Dropped |
| `WinPixEventRuntime.props` | *(skipped)* | Dropped |
| `Optick.props` | *(skipped)* | Dropped |
| `SuperLuminal.props` | *(skipped)* | Dropped |
| `LivePP.props` | *(skipped)* | Dropped |
| `NavPower.props` | *(skipped)* | Dropped |
| *(new)* SDL3 | `pkg-config --libs sdl3` | Needed by `Base` from Phase 6 |
| *(new)* Vulkan | `-lvulkan` | Needed by `Base` from Phase 5 |
| *(new)* unwind | `-lunwind -ldw` | Needed by `Base` from Phase 1 |

Note also that `DXC.props` has MSBuild `Copy` targets (`DXC_CopyDLL`) that stage
`dxcompiler.dll` and `dxil.dll` into the output directory. The generator must reproduce the
equivalent for `.so` files, or rely on `-Wl,-rpath,'$ORIGIN'` plus a staging step. Prefer
`rpath` — fewer moving parts.

---

## `DownloadDependencies.sh`

Mirrors `DownloadDependencies.bat`, but builds rather than unzips. It should:

- Be idempotent — skip anything already built.
- Pin every version explicitly. An unpinned dependency will silently drift and produce a
  "works on my machine" port.
- Produce the same `External/<Name>/{inc,lib}/` layout the `.props` sheets expect, so the
  generator's mapping table stays a straight translation.
- Print a clear summary of what it built and what it skipped.
- Check for required system packages up front and fail with an actionable message listing the
  missing ones, rather than failing deep inside a nested CMake build.

Build LLVM and DXC last; they dominate wall-clock time (tens of minutes each) and everything
else is fast, so failing fast on the cheap dependencies is better feedback.

---

## Open questions to resolve during implementation

These are unresolved and should be answered by the phase that first needs them, then recorded
here.

1. **`ctt` Linux support.** *(Phase 3)* Confirm the texture-compression library builds on
   Linux. If it does not, options are: find the upstream project and port it, substitute an
   equivalent compressor, or leave texture compression Windows-only and accept uncompressed
   textures on Linux initially. This is the highest-uncertainty dependency in the list.
2. **LLVM version pinning.** *(Phase 2)* Read the exact version from `LLVM.props` and confirm
   the Reflector's use of `clangAST` compiles against it on Linux. Clang's AST C++ API is not
   stable across major versions.
3. **`volk` vs the Vulkan loader.** *(Phase 5)* Decide based on measured dispatch overhead.
   Default to the plain loader for simplicity; adopt `volk` only if profiling justifies it.
4. **SDL3 availability.** *(Phase 6)* Confirm whether the target distributions package SDL3,
   or whether `DownloadDependencies.sh` must always build it.
5. **GameNetworkingSockets necessity for early phases.** *(Phase 1)* `Esoterica.Base` imports
   it, so it may block the very first link. Check whether the networking code can be reached
   without it at link time, or whether it must be built before Phase 1 can finish.
