# Progress Log

Running state of the Linux port. **Every task appends here before it counts as done**
(Conventions rule 9). Newest entries go at the top of each section.

This file keeps a chain of independent agent sessions coherent. When you start a session, read
"Current state" and "In flight" first.

---

## Current state

**Phase: 6. P6.1 to P6.8 are written. The phase does not meet its acceptance criteria and
cannot on this machine.** P6.8 root-caused the `VK_ERROR_UNKNOWN`, fixed five real defects in
`CreateContext`, and found a wall that needs a shader change. See the P6.8 entry.

### Start here

```bash
python3 Code/Scripts/NinjaGen/NinjaGen.py
ninja -f Build/Linux/Esoterica.ninja Build/Linux_Release/Esoterica.Applications.Engine

printf '[Render:RHI]\nEnable_Host_Validation = true\n' > Build/Linux_Release/Esoterica.ini

VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true \
  ./Build/Linux_Release/Esoterica.Applications.Engine \
  -map data://demo/render/pbr/pbrdemo.map -packaged
```

**Four things about that, each of which cost a session to find:**

- **`-packaged` is required.** Without it the engine uses the network resource provider and tries
  to start `EsotericaResourceServer.exe`, which is Phase 7. `-packaged` reads
  `Build/Linux_<configuration>/CompiledData` directly, which is what Phase 3 filled.
- **Validation is off unless the ini says otherwise.** `RenderSettings::m_enableHostValidation`
  defaults to false and only a Debug build forces it on. The generated `Esoterica.ini` is empty
  because `Settings::SaveSettings` skips every property still at its default, so the section has
  to be written by hand. The key names come from the reflected `Category` and `FriendlyName`.
  That is upstream behaviour, not a Linux defect.
- **`VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true` replaces
  `VK_LOADER_LAYERS_DISABLE='*'`.** It turns off only the validation layer's bundled spirv-val,
  which is where the stale SPIRV-Tools lives, and leaves every other check on. P6.7 turned the
  layers off entirely and lost validation with them. No newer layer package is needed.
  `VK_LAYER_MESSAGE_ID_FILTER=<vuid>[,<vuid>]` silences one VUID at a time, which is how P6.8
  surveyed several walls in one run.
- **The binary is `Esoterica.Applications.Engine`**, named after its project like the Reflector
  and the ResourceCompiler, not `EsotericaEngine` as the phase document originally wrote.

**Where it stops: the first shader that declares a capability this hardware does not have.**
On the Intel UHD 620 that is `storageInputOutput16`, in `DebugDraw`'s pixel stage. Behind it are
three more hardware gaps and one wall that no hardware fixes; see the table in the P6.8 entry.

### The one that needs a decision

**`Buffer<uint64_t>` cannot be expressed in Vulkan as the engine uses it.** DXC's SPIR-V backend
emits `OpTypeImage %ulong Buffer 2 0 0 1 R64ui`, a 64-bit sampled image. The RHI creates the
matching buffer view with `RG32_UInt` (`DeviceRenderWorld.cpp:604`), which is what Direct3D 12
wants: a typed buffer load there returns two 32-bit words and HLSL packs them into a `uint64_t`.
The two do not agree in Vulkan, `VK_FORMAT_R64_UINT` is not a uniform texel buffer format on this
hardware, and Mesa's `spirv_to_nir` refuses the type outright. **This was the `VK_ERROR_UNKNOWN`.**

**The fix is a shader change, like [P5.17](Phases/Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change---scheduled-not-started).**
Six sites read one: `SpatialHash.esh:185` and `:207`, `LightCulling_CullLights.esf:125` and
`:126`, `InstanceCulling.esf:45`, `InstancePickingResolve.esf:16`, and `MaterialShaderPBR.esh:117`.
The last puts it in every material pixel shader, so the whole frame is on it. **Escalated, not
started.** It is open question 8.

### What is behind that

**Esoterica renders on Linux.** P6.6 made a `VkSurfaceKHR` from the `SDL_Window*` and cleared and
presented twelve frames with **no Vulkan validation errors**. Every Phase 5 RHI call in that path
had never executed before. Running the backend for the first time found **four defects in it**,
all fixed; see the P6.6 and P6.7 entries.

**The engine binary builds, links and starts.** It reads its settings, loads compiled data, opens
a window and creates a Vulkan device. **Phase 6 acceptance criterion 1 is met.**

**The window, input and imgui layers are all done and each was proved by running it.** SDL3
`release-3.4.14` builds from source, `LinuxApplication` runs a window and an event loop, the imgui
platform backend has **multi-viewport verified** - three imgui windows became three live SDL
windows - and keyboard, mouse and gamepad all work: a **complete scancode table, 105 scancodes to
105 distinct `InputID`s**, and a full `InputSystem` pass driven by an SDL virtual gamepad.
**Open question 4 is answered: the port always builds SDL3, because Ubuntu 24.04 LTS packages
none.**

**The port now edits one upstream file that is not a pure include switch.**
`RHI::MaxPendingFrames` is 3 on Linux, because the Intel UHD 620 and llvmpipe both report a
swapchain `minImageCount` of 3. Four lines added, zero modified, Windows bit for bit unchanged.
Escalated, approved, made, and registered in [TouchedFiles.md](TouchedFiles.md).

**`EE_UNIMPLEMENTED_FUNCTION` is gone from `Base` outside `RHI_Vulkan.cpp`.** The three there are
Phase 5's; two more in `Triangle.h` and `Encoding.cpp` are upstream's own.

**Two findings from P6.3 that later phases need.** First, the vendored `ImguiPlatform_Win32.cpp`
is about three years behind the imgui core beside it: the core is `v1.92.9b-docking`, the backend
is roughly 1.89.1. The Linux backend is therefore built from `v1.92.9b-docking`'s
`imgui_impl_sdl3.cpp`, and its vendored region keeps upstream's formatting so it can be
re-synced. Second, **imgui will not enable viewports on Wayland**: `ImGui_ImplSDL3_Init` sets
`ImGuiBackendFlags_PlatformHasViewports` only for video drivers on its global-mouse white list,
and `wayland` is not on it. The editor's docking UI depends on viewports, so Phase 7 needs to
know.

**The Phase 5 / Phase 6 surface question is answered, and P6.6 implements it.**
`Platform::SetMainWindowHandle` holds the `SDL_Window*`, and **`RHI_Vulkan.cpp` creates the
`VkSurfaceKHR` itself**, through a new Linux-only `Platform::CreateVulkanSurface` in
`Platform_Linux.cpp`. That edits no upstream file, and `Base/Render` still includes no window
system header. It revises the second half of P5.3's answer, so read the two decision entries
together. **Not written yet: P6.6 owns it.**

**Phase 5 remains merged and incomplete, but it is no longer unrun.** All sixteen groups are on
`main`. **The parts P6.6 and P6.7 exercised are verified** - context, queues, command pools and
buffers, the swapchain, barriers, a render pass, submit and present - and everything else is
still compile-verified only. Each P5.x entry's "Not verified" list stands apart from those.

**Open question 7 is answered, and the work is scheduled as P5.17.** The shader reads its own
command's root data out of the argument buffer, indexed by `DrawIndex`. **It is deliberately
sequenced after Phase 6 bring-up**, because nothing here can be tested until the engine runs. Read
the decision entry and
[P5.17](Phases/Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change---scheduled-not-started)
before starting it. **The frame still cannot draw until it lands.**

**All 16 groups are written. 15 of them are real; P5.13 is the exception and cannot be finished
here.** Phase 5's own note says this count is the single most important piece of state.
**Verification is now partial rather than absent**: the P6.6 and P6.7 bring-up exercised P5.1,
P5.2, P5.3, P5.4, P5.7's shader creation, P5.8 and P5.9, and found four defects across them. No
other group has executed.

**3 `EE_UNIMPLEMENTED_FUNCTION` remain, none of them a whole function.** One is the indirect
refusal from open question 7. The other two are markers that name a caller if it ever appears: a
sampler border colour Vulkan cannot express without `VK_EXT_custom_border_color`, and the
static-sampler path the binding model does not use.

**"Written" is not "works".** Four of the sixteen groups cannot be exercised on any hardware in
this machine, and one cannot be finished at all:

| Group | Why it is unverifiable here |
|---|---|
| P5.13 indirect draws | Open question 7. Half written by decision, and the frame is built on it. |
| P5.14 mesh shaders | Neither real GPU here has `VK_EXT_mesh_shader`. |
| P5.15 variable rate shading | Switched off to match the Direct3D 12 backend, which reports `NotSupported`. |
| P5.16 raytracing | No caller anywhere, no way to build a shader table on either backend, and only `llvmpipe` has the extensions. |

**Neither GPU in this development machine supports mesh shaders**, so the engine's debug draw
cannot run here. `VK_EXT_mesh_shader` is present only on `llvmpipe`, the software rasteriser. The
Intel UHD 620 and the NVIDIA MX250 both lack it, and both would lack it on Direct3D 12 as well, so
this is the hardware being below the engine's bar rather than a port problem. See the P5.14 entry.

**The frame cannot draw. The decision is made, the work is not done.** Every render
pass is built on `CmdExecuteIndirect` - `RenderPass_ForwardShading` uses it six times,
`RenderPass_CascadedShadow` twice, `RenderPass_DebugDraw` six times - and **the engine's command
signatures cannot be expressed by any Vulkan indirect draw.** A Direct3D 12 command signature sets
root constants and binds root descriptors per command; Vulkan's indirect draws read draw arguments
and nothing else, and a compute pre-pass does not help because a pre-pass cannot bind a descriptor
either. **The answer is a shader change, and it is P5.17.** P5.13 landed its mechanical half by
decision and refuses the rest at the line; see the P5.13 entry, the decision entry, and P5.17.

**Superseded: `m_pNativeWindowHandle` is an `SDL_Window*` on Linux, and `RHI_Vulkan.cpp` makes
the surface from it** through `Platform::Linux::CreateVulkanSurface`. P5.3 said the application
would create the surface and hand it over, and there turned out to be nowhere for it to do that.
See the 2026-08-30 decision entry. **The application does still drive swapchain recreation, not
the RHI**, which is the other answer Phase 6 was promised, and that half of P5.3 stands.

**A Vulkan queue does not execute its submits in order and a Direct3D 12 queue does**, so every
submit now waits on the value the previous submit on that queue signalled. The engine depends on
the Direct3D guarantee and `RHI.h` gives it no way to ask for it. See the P5.3 entry.

**The render pass is opened by the first draw, not by `CmdSetRenderTargets`.** The engine records
image layout barriers between the two and a barrier may not run inside dynamic rendering, so the
begin is deferred. See the P5.9 entry; anything that changes `CmdSetRenderTargets` or a draw has
to keep that order.

**The `DataFormat` to `VkFormat` mapping is complete, and there is exactly one of it.** All 99
formats map, the three `DeviceCapabilities` format arrays are filled from the device, and every
texture, buffer view and pipeline attachment format reads the same function.

**Both Phase 4 decisions are now implemented, each in exactly one place.** Clip-space Y is
inverted in `CmdSetViewport` with a negative viewport height, and nowhere else. Heap set 1 is
bound in `CmdSetPipeline`, not in `BeginCommandBuffer`. `CmdSetViewport` has run; the heap bind
has not, because nothing has set a pipeline yet.

**The bindless heap now exists in code.** `CreateContext` builds set 1 exactly as the Phase 4
binding model specifies, and `GetBufferHandle` returns an index into it. **One correction to that
recorded decision was needed**, on a flag Vulkan does not allow where the entry put it; it is
written up below and it changes nothing the shaders can see.

**Phase 5 was written against the compiler and link only, on purpose**, because nothing on Linux
could reach `RHI::CreateContext` until Phase 6 provided an entry point. See the 2026-08-28
decision entry. **That has now happened**, and the first execution found four defects in four
different groups. **Treat every P5.x entry as compile-verified and run-unverified except where
the P6.6 and P6.7 entries say otherwise** - which is context, queues, command pools and buffers,
the swapchain, barriers, a render pass, submit and present.

Previously: **Phase 4 (done on Linux).** DXC is built from source with three patches that fix its SPIR-V back
end, and **all 46 shader stages compile and pass `spirv-val`**. `./CompileShaders.sh` exits 0 and
fills `_AutoGenerated/Shaders/`; the generated C++ compiles and links. Validate with
`External/DirectXShaderCompiler/bin/x64/spirv-val --target-env vulkan1.3 --scalar-block-layout`
- not the one on `PATH`, and not without the layout flag. Both matter; see the entries below.

**No known correctness bugs are outstanding.** Defect 3 is fixed by patch 0003: bitfields that
differ only in signedness now merge, so `MeshCluster` is 32 bytes and `RenderView` 352, matching
Direct3D and C++. DXC carries three patches, all in its SPIR-V back end.

**Both binding decisions are made and written for Phase 5 to implement against**: the bindless
binding model, and clip-space Y, which the Vulkan viewport inverts. Phase 3's five `.material`
resources compile now too, which closes the last Phase 3 gap that did not need Windows. See the
2026-08-28 entries below.

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
| 3 - Resource Compiler | **done on Linux.** The 5 materials compile as of Phase 4's defect 2 fix; only the byte-comparison against Windows remains |
| 4 - Shader Pipeline | **done on Linux** (criteria 6 and 10 need a Windows machine). DXC builds from source with three patches; all 46 shader stages compile, validate and link with layouts matching Direct3D, and `CompileShaders.sh` runs them |
| 5 - Vulkan RHI | **all 16 groups written and merged to `main`, and the backend has now run.** P6.6 and P6.7 executed it for the first time and found four defects, all fixed. Context, queues, command pools and buffers, the swapchain, barriers, a render pass, submit and present are **verified**; the rest is still compile-verified only. 3 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 103, and none is a whole function. P5.13 is finished by P5.17, which still comes after P6.8. Criteria 5 to 10 are now checkable |
| 6 - Windowing and Input | **started.** P6.1 to P6.7 done: SDL3 builds from source, `LinuxApplication` runs a window and an event loop, imgui and all three input devices work, the port renders on Linux, and **the engine binary builds, links and starts**. Criterion 1 is met. P6.8, first light, is next, and it owns the `VK_ERROR_UNKNOWN` compute pipeline that blocks a map |
| 7 - Editor and Tools | not started |

Linux build status: `libEsoterica.Base.so`, `libEsoterica.Engine.Runtime.so`,
`libEsoterica.Engine.Tools.so`, `libEsoterica.Game.Runtime.so`, `libEsoterica.Game.Tools.so`,
`Esoterica.Applications.Reflector` and `Esoterica.Applications.ResourceCompiler` all build.
`Applications/Editor` and `Applications/ResourceServer` do not, and are Phase 7.
`Applications/BuildGenerator` is excluded permanently.
Windows build status: **not run.** 69 upstream files carry `+494 -71` lines across Phases 0-3.

## In flight

> ### **Phase 6 is merged. P6.8 is the one branch open.**
>
> PRs #43 to #49 all landed, so a session that checks out `main` finds P6.1 to P6.7.

| Branch | PR | What |
|---|---|---|
| `linux/p6.8-first-light` | open | The `VK_ERROR_UNKNOWN` root cause, five `CreateContext` feature defects, and the mesh-shader startup path |

---

**Phase 5 is merged and has now run.** All seventeen branches went in through PRs #24 to #41,
ending with `p5.16-raytracing`. P6.6 and P6.7 executed the backend for the first time and found
four defects in it, all fixed on the stack above. **What is still unverified is most of it**:
every group's "Not verified" list stands except for the parts the P6.6 entry names.

**[P5.17](Phases/Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change---scheduled-not-started)
is still the last piece of Phase 5, and it still comes after P6.8.** It is the indirect draw
shader change that makes the frame draw geometry, and it cannot be tested until the engine
reaches a frame loop - which it does not yet, because `vkCreateComputePipelines` fails first.

---

## `ALL_COMMANDS` sites

Phase 5's "do not" list says to record every temporary `ALL_COMMANDS` barrier rather than leave it
to be found later. All four sites are in `RHI_Vulkan.cpp`:

| Site | Why it is there | What narrowing it needs |
|---|---|---|
| `QueueDeviceWait`, wait `stageMask` (P5.2) | `ID3D12CommandQueue::Wait` blocks the whole queue, and this has to mean the same thing. | Knowledge of what the waiting submit does, which the caller does not pass. |
| `QueueSubmit`, signal `stageMask` (P5.2) | The signalled timeline value has to mean "everything in this submit finished". | Nothing. `ALL_COMMANDS` is arguably correct here rather than lazy, since that is the semantic. |
| `VulkanPipelineStage`, `PipelineStage::All` (P5.9) | `D3D12_BARRIER_SYNC_ALL` means every stage, and so does this. The reference returns it the same way, as an early return rather than one bit among many. | Nothing. It is the meaning of the flag. |
| `VulkanAccess`, `ResourceAccess::Common` (P5.9) | `D3D12_BARRIER_ACCESS_COMMON` is "any access", which Vulkan spells `MEMORY_READ` plus `MEMORY_WRITE`. `DeviceTextureState` starts every texture at `Common`, so this is the source mask of the first barrier on any texture. | The engine would have to say what it actually did, which the tracker does not record. Narrowing it is a change on the engine side, not here. |
| `RecordQueueOrderingWait`, both masks (P5.3) | A Direct3D 12 queue runs its command lists in submission order and a Vulkan queue does not, so every submit waits on the previous submit's timeline value. "The previous submit finished" is the whole meaning of the wait. | Nothing. It is the semantic, the way `QueueSubmit`'s signal mask is. |
| `RecordClearVisibilityBarrier`, destination masks (P5.10) | A clear has to be visible to whatever reads it next, and the engine's own barrier after a clear names a shader write as the source, which does not cover a Vulkan transfer write. Nothing at the call site says who the reader is. | The reader. In practice it is a compute dispatch, an indirect argument fetch or a copy to a host buffer, and naming those three would narrow it. Confirm against a captured frame first. |

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

### 2026-08-31 - P6.8 First light. **The `VK_ERROR_UNKNOWN` is explained, and it is not P5.7's**

**Not first light.** The engine still does not render a map, and it cannot on this machine. What
this task did instead: root-caused the `VK_ERROR_UNKNOWN`, fixed five real defects in
`CreateContext`, made startup survive a device without mesh shaders, and measured exactly what
stops the engine here. Read [the "Start here" block](#start-here) before running anything.

#### Validation works now, and the stale SPIRV-Tools is no longer a reason to lose it

P6.7 ran with `VK_LOADER_LAYERS_DISABLE='*'`, which turns validation off. It does not have to be.

```bash
VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true
```

`debug_disable_spirv_val` is a setting the Khronos layer already has. It switches off the layer's
bundled spirv-val, which is where the stale SPIRV-Tools that rejects the `DebugDraw` mesh shader
lives, and leaves every other check on. **No newer layer package is needed**, and Ubuntu 24.04's
`vulkan-validationlayers 1.4.309.0` is fine with that one flag. `VK_LAYER_MESSAGE_ID_FILTER`
takes a comma separated VUID list and silences those messages, which is how the remaining walls
below were found in one run rather than one rebuild each.

Host validation also has to be switched on. It defaults to false and only a Debug build forces
it. The generated `Esoterica.ini` is empty because `Settings::SaveSettings` skips any property
still at its default, so the section is written by hand:

```ini
[Render:RHI]
Enable_Host_Validation = true
```

The key names come from the reflected `Category` and `FriendlyName`, with spaces replaced by
underscores (`Settings.cpp:11`). **The empty ini is upstream behaviour, not a Linux defect.**

#### The `VK_ERROR_UNKNOWN` is `Buffer<uint64_t>`, and it is open question 8

`vkCreateComputePipelines` failed for `InstancePickingResolve`. With validation on, Mesa says
what it is:

```
SPIR-V offset 4620: SPIR-V parsing FAILED:
    glsl_type_is_texture(type->glsl_image)
spirv_to_nir failed (VK_ERROR_UNKNOWN)
```

The instruction is `%type_buffer_image = OpTypeImage %ulong Buffer 2 0 0 1 R64ui`, from
`Buffer<uint64_t>` in the HLSL. **This is not a P5.7 defect.** P5.7 builds the pipeline
correctly; the module it is handed cannot be lowered.

The RHI creates the matching buffer with `RHI::DataFormat::RG32_UInt`
(`DeviceRenderWorld.cpp:604`), which is right for Direct3D 12: a typed buffer load there returns
two 32-bit words and HLSL packs them into a `uint64_t`. DXC's SPIR-V backend does not do that. It
emits a 64-bit sampled image, whose sampled type has to match the view's format, and
`VK_FORMAT_R64_UINT` is not a uniform texel buffer format on this hardware. Mesa refuses the type
before any of that matters.

**The fix is a shader change on both backends, the same shape as
[P5.17](Phases/Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change---scheduled-not-started):**
read the pages as `Buffer<uint2>` and assemble the `uint64_t` in the shader. Six sites:

| File | Line |
|---|---|
| `Code/Engine/Render/Shaders/SpatialHash.esh` | 185, 207 |
| `Code/Engine/Render/Shaders/Renderer/LightCulling_CullLights.esf` | 125, 126 |
| `Code/Engine/Render/Shaders/Renderer/InstanceCulling.esf` | 45 |
| `Code/Engine/Render/Shaders/Picking/InstancePickingResolve.esf` | 16 |
| `Code/Engine/Render/Shaders/Renderer/MaterialShaderPBR.esh` | 117 |

The last one puts a `Buffer<uint64_t>` in **every material pixel shader**, so the whole frame is
on it. **Escalated, not started.**

#### Five defects in `CreateContext`, all the same class as P6.7's 16-bit finding

The engine's shaders declare capabilities that Direct3D 12 folds into the shader model and Vulkan
gates one at a time. `vkCreateShaderModule` rejects any the device did not enable. P6.7 found the
first three; validation named five more.

| Enabled now | Who needs it | Intel UHD 620 |
|---|---|---|
| `storageInputOutput16` | a `uint16_t` bindless handle crossing a stage interface | **no** |
| `shaderInt64` | `uint64_t` in `InstancePickingResolve` | yes |
| `shaderSubgroupExtendedTypes` | a wave operation on a 16- or 64-bit operand | yes |
| `shaderDemoteToHelperInvocation` | HLSL `discard` under Shader Model 6.6 | yes |
| `shaderBufferInt64Atomics`, `shaderSharedInt64Atomics` | a 64-bit `InterlockedMin` or `InterlockedAdd` | yes, **no** |

Two optional extensions are now asked for when the device has them, chained the way the mesh
shader and shading rate blocks already are: `VK_EXT_shader_image_atomic_int64` and
`VK_KHR_fragment_shader_barycentric`. Neither has an RHI entry point to assert in, so a device
without one fails at the shader that declares it.

**`storageInputOutput16` is worth its own note.** A bindless handle is a `uint16_t`
(`RHI.esh:91-96`), and shaders pass handles down the stage interface -
`DebugDraw.esf`'s `DebugDrawPrimitiveOutput` carries a `TextureHandle` from the mesh stage to the
pixel stage. Vulkan gates 16-bit stage I/O separately from 16-bit buffer access. Direct3D 12 does
not: `Native16BitShaderOps` covers every use at once.

#### Startup now survives a device without mesh shaders

`Shaders::Initialize` creates every shader in the engine at startup, mesh stages included,
whatever the device supports. Three changes, all in `RHI_Vulkan.cpp`:

- **`CreateShader` skips `vkCreateShaderModule` for a Task or Mesh stage** when the device has no
  `VK_EXT_mesh_shader`, and leaves the handle null. Reflection still runs, because it reads the
  SPIR-V and not the module, so the root signature is built either way.
- **`CreatePipeline( MeshPipelineParameters )` returns a pipeline with a null handle** instead of
  asserting. `SurfaceShader`'s constructor builds one at startup for every shader, so the assert
  stopped the engine before it had a frame loop over a pass it may never run. **`CmdSetPipeline`
  asserts instead**, which names the pass.
- **`SetVulkanObjectName` ignores a null handle.** Naming `VK_NULL_HANDLE` is a validation error,
  and the guard belongs in the shared function rather than at each call site.

With these, `Shaders::Initialize` runs to the end on a device with no mesh shader support.

#### What this machine cannot do

Measured on all three GPUs with `vulkaninfo` and confirmed by running. Counts are shader modules
rejected in one startup.

| Gap | Modules | Intel UHD 620 | NVIDIA MX250 | llvmpipe |
|---|---|---|---|---|
| `VK_KHR_fragment_shader_barycentric` | 17 | no | no | no |
| `shaderSharedInt64Atomics` | 4 | no | yes | yes |
| `storageInputOutput16` | 2 | no | no | no |
| `VK_EXT_mesh_shader` | debug draw | no | no | yes |
| `VK_EXT_mutable_descriptor_type` | all of them | yes | **no** | yes |

**No GPU here can run the engine's shaders.** The MX250 is refused at device selection for
`VK_EXT_mutable_descriptor_type`, which the binding model requires. llvmpipe and the UHD 620 both
lack `storageInputOutput16` and barycentrics. **And open question 8 stops every device, including
ones that pass this table.**

**Barycentrics look like a development tools cost, and a Shipping build does not escape them.**
`MaterialShaderPBR.esh:82` adds `SV_Barycentrics` to the material pixel shader only when
`EE_DEVELOPMENT_TOOLS` is set, for a wireframe overlay. **But the Reflector always defines it
when it compiles shaders** (`ShaderReflection_ShaderCompiler.cpp:21` and `:23`, reached from
`COMMON_DXC_ARGUMENTS` at `:72`), and the SPIR-V it embeds is one variant shared by every build
configuration. So the barycentric capability is in the bytecode whatever the engine is built as.
Dropping it means changing the Reflector, not the build configuration.

**The Shipping Engine binary does not link**, and never has. This is a defect in
`NinjaGen.py`, not upstream's: Shipping builds static archives, and the link line is
`libEsoterica.Engine.Runtime.a libEsoterica.Base.a libEsoterica.Game.Runtime.a`.
`Game.Runtime` references `EE::Animation::GraphController`, which lives in `Engine.Runtime`,
already scanned by the time the linker reaches it. Archive order, or `--start-group`. The object
files are all there and the source lists match Release exactly, 766 each. **Not caused by this
task, and fixable here.**

#### Files

- Files added: none.
- Files changed: `Code/Base/Render/RHI_Vulkan.cpp`. The port owns it; it is not upstream.
- Upstream files edited: **none.** [TouchedFiles.md](TouchedFiles.md) is unchanged.

#### Acceptance criteria

Criterion 1 stays met. **Nothing else moved.** The engine gets further - `Shaders::Initialize`
now runs to the end where it used to stop at shader 14 of 28 - but it still reaches no frame
loop, so criteria 2 to 9 are exactly as P6.7 left them. Criteria 10 to 12 stay met: this task
edited no upstream file.

#### For the next session

- **Answer open question 8 before anything else.** Nothing in the frame runs until
  `Buffer<uint64_t>` has a Vulkan spelling, and it sits in front of P5.17 rather than behind it.
- **Run with validation on.** The two lines at the top of this entry cost a session to find and
  turn every remaining wall into a named message instead of a `VK_ERROR_UNKNOWN`.
- **Do not read `VK_ERROR_UNKNOWN` from Mesa as a compile failure.** Here it meant
  `spirv_to_nir` refused a type. `INTEL_DEBUG=cs` dumps the shaders that did compile, which is
  how the failing one was narrowed down by elimination.
- **A machine with a current GPU is needed to finish Phase 6.** The four gaps above are hardware,
  not port defects. Any device with `VK_EXT_mutable_descriptor_type`, `VK_EXT_mesh_shader`,
  `VK_KHR_fragment_shader_barycentric` and `storageInputOutput16` clears the table.

### 2026-08-30 - P6.7 `EngineApplication_Linux`. **The engine binary exists and runs**

**`Build/Linux_Release/Esoterica.Applications.Engine` builds, links and starts.** It reads its
settings, loads compiled data, opens a window, creates a Vulkan device and gets as far as
compiling shaders. **Phase 6 acceptance criterion 1 is met.** It does not yet render a map; the
two things stopping it are named below and neither is P6.7's.

`EngineApplication_Linux.{h,cpp}` mirror `EngineApplication_Win32.{h,cpp}`. `_tWinMain` becomes
`int main( int argc, char** argv )`, which also drops the `__argc` and `__argv` fetch, and the
Live++ hooks are absent because `EE_ENABLE_LPP` stays unset on Linux. `ProcessInputEvent` is the
one line the P6.4 entry specified. The constructor passes no icon and no splash screen: the
Windows build reaches both through a `.rc` file, and Phase 6 says not to parse those.

#### Two build system defects, both found by running the binary

**1. The `External/` rpath was repository relative, so a binary only ran from the repository
root.** `-L` is resolved by the linker, which ninja runs from the root, so a relative path is
fine there. An **rpath is resolved by the loader against the working directory**, so it only
worked by accident. `Toolchain.py` now emits `-Wl,-rpath,'$ORIGIN/../../<directory>'`; every
output directory is `Build/Linux_<configuration>/`, two levels below the root. P6.2 predicted
this and left it for P6.7.

**2. A shared library had no soname, so its dependents recorded a path.** The dependency
libraries are passed to the linker as repository-relative paths, and with no soname the linker
copies that path verbatim into the dependent's `DT_NEEDED`. The loader then resolves
`Build/Linux_Release/libEsoterica.Base.so` against the working directory, and `$ORIGIN` never
gets a say. `NinjaGen.py` now passes `-Wl,-soname,lib<Project>.so` for every shared library, so
`DT_NEEDED` carries a bare filename that `$ORIGIN` finds.

Together these are why the engine failed with *"error while loading shared libraries"* from its
own output directory. `ldd` from `/tmp` now resolves everything.

#### A Phase 5 defect, found by running it: 16-bit shader types were never enabled

**`CreateContext` did not enable the 16-bit feature bits, and the engine's shaders need them.**
`MeshData.esh`, `CommonPacking.esh`, `XeGTAO.esh`, `RendererTypes.esh` and several `.esf` files
declare `float16_t` and `uint16_t`; DXC emits the matching SPIR-V capabilities, and
`vkCreateShaderModule` rejects a module whose capabilities the device did not enable. The first
shader it saw was `InstancePickingResolve`, and validation said:

```
vkCreateShaderModule(): SPIR-V Capability Int16 was declared, but one of the following
requirements is required (VkPhysicalDeviceFeatures::shaderInt16).
```

**Direct3D 12 has no equivalent step.** `Native16BitShaderOps` is a capability a driver either
has or does not, with nothing to switch on, so P5.1 had nothing to mirror and the gap was
invisible until a shader was created.

`shaderInt16`, `shaderFloat16`, `storageBuffer16BitAccess`,
`uniformAndStorageBuffer16BitAccess` and `storagePushConstant16` are now asked for when the
device has them, and a warning names any that are missing. Asked for rather than required,
because a device without them fails at the shader that needs one, which names the shader.

#### **What stops the engine rendering a map, and neither is Phase 6's**

With the above fixed, the engine reaches `Shaders::Initialize` and halts on the **`DebugDraw`
mesh shader**:

```
vkCreateShaderModule(): pCreateInfo->pCode (spirv-val produced an error):
[VUID-CullPrimitiveEXT-CullPrimitiveEXT-07036] According to the Vulkan spec BuiltIn
CullPrimitiveEXT variable needs to be a boolean value array. ID <10> (OpVariable) is not a
bool scalar.
```

**The SPIR-V is not the problem, and the validation layer is.** This is the *same* stale
SPIRV-Tools that Phase 4 already ran into, now inside the Vulkan validation layer rather than in
`/usr/bin/spirv-val`. Ubuntu 24.04 ships `vulkan-validationlayers 1.4.309.0`, which carries a
SPIRV-Tools old enough to have the bug Phase 4 documented: it reads an
`OpVariable %_ptr_Output__arr_bool_uint_64 Output` - an array of 64 bools, which is exactly what
the spec asks for - and reports that it "is not a bool scalar".

**Measured, not assumed.** With `VK_LOADER_LAYERS_DISABLE='*'`, `vkCreateShaderModule` accepts
the `DebugDraw` mesh shader and the engine walks straight past it. The driver has no complaint;
only the layer does.

**So there is no fourth DXC defect.** Phase 4's conclusion stands, and its rule stands with it:
*a distribution's SPIRV-Tools is not interchangeable with the one the compiler validates
against.* This is the second time that has cost a session, so it is worth stating in the form it
now takes: **the Vulkan validation layers on Ubuntu 24.04 cannot be used on the mesh shader
stages.** P6.8 must either disable them, or install a newer set, and record which.

#### What actually blocks the engine, then

Two things, and neither is what the paragraph above looked like:

1. **`vkCreateComputePipelines` returns `VK_ERROR_UNKNOWN` for `InstancePickingResolve`.** That
   is the first shader after the mesh stages, and it is the real wall. `VK_ERROR_UNKNOWN` from
   Intel's ANV usually means the driver refused to compile the module, so the next step is to
   run that one shader through the driver with `VK_LOADER_LAYERS_DISABLE` unset and a newer
   validation layer, or through `spirv-val` and the driver's own shader cache diagnostics. This
   is a Phase 5 group's first execution, so treat it as a P5.7 defect until shown otherwise.
2. **`Shaders::Initialize` creates every shader module at startup**, mesh shaders included,
   whatever the device supports. That is not fatal by itself - the driver accepts the module -
   but it means the engine pays for shaders it can never dispatch, and any future driver that is
   stricter about an unsupported stage would stop here. Worth knowing; not urgent.

**Both belong to P6.8 and Phase 5's completion, not to P6.7.** P6.7's deliverable is the binary,
and the binary exists.

#### Verification

- Files added: `Code/Applications/Engine/Linux/EngineApplication_Linux.{h,cpp}`.
- Files edited: `Code/Scripts/NinjaGen/LinuxSources.txt` (the new source),
  `Code/Scripts/NinjaGen/Toolchain.py` (the `$ORIGIN` rpath, and the SDL3 sheet for
  `Esoterica.Applications.Engine`, whose `.cpp` reads `SDL_Event` fields),
  `Code/Scripts/NinjaGen/NinjaGen.py` (the soname), `Code/Base/Render/RHI_Vulkan.cpp` (the
  16-bit features).
- **Upstream files edited: none.**
- Build: `Checks.py` passes. `ninja -k 0` fails on `Esoterica.Applications.Editor` and
  `Esoterica.Applications.ResourceServer` only, which is where it failed before. **A third
  executable now builds**, next to the Reflector and the ResourceCompiler.
- Run: `./Build/Linux_Release/Esoterica.Applications.Engine -map
  data://demo/render/pbr/pbrdemo.map -packaged`, **from any directory**, reads its settings,
  loads compiled data, opens a window, selects the Intel UHD 620 and builds both descriptor
  pools before halting at the `DebugDraw` mesh shader.
- Acceptance criteria: **criterion 1 is met.** Criterion 2 is not; see above. Criterion 10 is
  untouched: both new files are guarded with `#ifdef __linux__`, they live in a directory no
  `.vcxproj` references, and the two generator changes are Linux-only tooling.

#### The `-packaged` flag is needed, and that is not a defect

Without it the engine uses the **network** resource provider, which tries to start
`EsotericaResourceServer.exe`. The ResourceServer is Phase 7 and does not build on Linux yet.
`-packaged` selects `PackagedResourceProvider`, which reads
`Build/Linux_<configuration>/CompiledData` directly - the data Phase 3 compiled. Nothing needs
changing; P6.8 and Phase 7 should simply expect the flag until the ResourceServer exists.

**A related upstream observation, not fixed here.** When `Engine::Initialize` fails, the
following `Engine::Shutdown` segfaults in `RenderSystem::WaitAllQueuesIdle`, because it tears
down systems that were never initialized. That is upstream behaviour on both platforms and it
only shows up on a failed start; it is recorded under "Upstream issues observed".

### 2026-08-30 - P6.6 The Vulkan surface. **Esoterica renders on Linux**

**The port drew its first frame.** A window opens, `RHI::CreateContext` picks a device, a
`VkSurfaceKHR` is made from the `SDL_Window*`, a swapchain is created, and twelve frames are
cleared and presented to the screen with **no Vulkan validation errors**. The swapchain recreates
on resize and tears down clean. Every Phase 5 RHI call in that path had never executed before
today.

**It needed one change to an upstream file that was not on the registry.** That was escalated,
approved and made: `RHI::MaxPendingFrames` is 3 on Linux. See below, and
[TouchedFiles.md](TouchedFiles.md).

#### Bring-up order, and what each step found

Phase 5 wrote all of this against the compiler. Running it turned up three real defects and one
measured limit. In the order they appeared:

1. **`RHI::CreateContext` worked first time.** It skipped the NVIDIA MX250 for a missing
   `VK_EXT_mutable_descriptor_type`, chose the Intel UHD 620, warned about the absent
   `VK_EXT_mesh_shader` exactly as P5.14 said it would, and built both descriptor pools. Nothing
   needed fixing.

2. **Defect: the surface offers no RGBA format.** `CreateSwapchain` asked for `RGBA8_sRGB` then
   `RGBA8_UNorm`, which is what DXGI hands out, and asserted. **Every surface on this machine
   offers only `VK_FORMAT_B8G8R8A8_SRGB` and `VK_FORMAT_B8G8R8A8_UNORM`** - measured on the Intel
   UHD 620, the NVIDIA MX250 and llvmpipe alike. So `BGRA8_sRGB` and `BGRA8_UNorm` are now
   candidates after the two the caller asked for. **The swap costs nothing and needs no shader
   change**: a Vulkan format names its components in memory order, and a shader's red output
   lands in the format's red component wherever that byte sits. The sRGB preference is kept, and
   `DataFormat` already had both spellings.

3. **Measured limit: `minImageCount` is 3, and `MaxPendingFrames` is 2.** See the escalation.

4. **Defect: `Window::DestroySwapchain` destroys command pools before command buffers.** That is
   fine on Direct3D 12, where an allocator and a command list are independent objects, and it is
   a validation error on Vulkan, where destroying a `VkCommandPool` frees its buffers and a later
   `vkFreeCommandBuffers` on the dead pool is invalid. `RenderWindow.cpp:53` does exactly this.
   **P5.4 assumed the other order** - its comment said "the engine destroys buffers first; see
   `RenderSystem::Shutdown`" - and that is true of `RenderSystem` and false of `Window`.

   Fixed in `RHI_Vulkan.cpp`, not upstream: `VulkanCommandPool` now knows the buffers it
   allocated and nulls their handles as it is destroyed, so `DestroyCommandBuffer` skips a free
   that already happened and never reads a pool it no longer owns. Either order now works.

#### `RHI::MaxPendingFrames` is 3 on Linux. **Escalated, approved, and made**

**This is the one thing Phase 6 predicted by name, and it is real.** `RHI.h:31` set
`MaxPendingFrames = 2`. Measured on this machine:

| Surface | `minImageCount` | `maxImageCount` |
|---|---|---|
| Intel UHD Graphics 620 | **3** | unlimited |
| NVIDIA GeForce MX250 | 2 | 8 |
| llvmpipe | **3** | unlimited |

`minImageCount` is a hard minimum, so `CreateSwapchain` gets three images back and halts on its
own check, with the message P5.3 wrote for this exact case:

```
[Error][Rendering][RHI/CreateSwapchain] The surface needs 3 swapchain images and RHI::MaxPendingFrames is 2.
```

`Swapchain::m_renderTargets` is a `TArray<Texture*, MaxPendingFrames>`, and `AcquireNextImage`
returns an index into it, so there is no way to absorb this in the backend.

**The edit, exactly, at `RHI.h:31`. Four lines added, zero modified:**

```cpp
#if defined( __linux__ )
        MaxPendingFrames = 3, // Several Linux drivers report a minImageCount of 3
#else
        MaxPendingFrames = 2, // Set this value to 2 for double buffering, or 3 for triple buffering.
#endif
```

The existing line survives verbatim inside the `#else`, so **the Windows build is bit for bit
unchanged** and stays double buffered. `git diff --stat upstream/main -- Code/Base/Render/RHI.h`
reports `4 ++++` with no deletions. That is the shape Conventions rule 2 asks for. What made it an
escalation is only that `Code/Base/Render/RHI.h` was not in
[TouchedFiles.md](TouchedFiles.md); **it was escalated, approved, made, and is registered there
now.**

**With it, everything above passes.** Without it, nothing after `CreateContext` runs at all.

The one alternative that touches no upstream file was to give the backend three real swapchain
images, render into two of its own, and blit into the acquired image at present time. That is a
full screen copy every frame to avoid four lines, and it was not taken.

#### What actually ran

Recorded here because Phase 5 could never state it. `CmdBarrier` on a swapchain image in both
directions, `CmdSetRenderTargets` opening and closing a dynamic render pass with a clear load
action, `CmdSetViewport`, `EndCommandBuffer`, `QueueSubmit`, `AcquireNextImage`, `QueuePresent`,
`WaitQueueIdle`, and the whole create and destroy path for the context, queues, command pools,
command buffers, swapchain and its render targets. **The Vulkan validation layers were on
throughout and said nothing.**

Also confirmed incidentally: Phase 1's crash handler works. A null dereference during bring-up
printed a symbolised backtrace and re-raised, which is what it was written to do.

#### Verification

- Files edited: `Code/Base/Platform/PlatformUtils_Linux.{h,cpp}` (the two surface functions),
  `Code/Base/Render/RHI_Vulkan.cpp` (the surface call, the BGRA candidates, the command pool
  teardown fix).
- **Upstream files edited: one.** `Code/Base/Render/RHI.h:31`, 4 added and 0 modified,
  registered in [TouchedFiles.md](TouchedFiles.md). Escalated and approved before the edit.
- Build: `Checks.py` passes. `ninja -k 0` fails on `Esoterica.Applications.Editor` and
  `Esoterica.Applications.ResourceServer` only, which is where it failed before.
- Run: a scratch application deriving from `LinuxApplication` (not committed) reported every
  check passing, including that all three swapchain images were acquired across twelve frames and
  that the swapchain survived a resize.
- Acceptance criteria: **criterion 4 is met** - resize recreates the swapchain with no validation
  errors. **Criterion 8 is met for this path** - shutdown is clean with validation on. Criterion 1
  is not: there was still no engine binary when this was written, which is P6.7. Criterion 10
  is untouched.

#### For P6.7

- `EngineApplication_Linux::ProcessInputEvent` is one line; see the P6.4 entry for the exact
  `GenericMessage`.
- The `Esoterica.Applications.Engine` project needs the SDL3 sheet in `LINUX_ONLY_SHEETS` as soon
  as its `.cpp` reads an SDL event field.
- The engine paces frames on `RenderSystem`'s frame semaphores. A harness without them reuses the
  swapchain's acquire semaphores while they are still pending, and validation says so. That is
  not a defect; it is a reminder that `AcquireNextImage` is only sound inside the engine's own
  frame pacing.

### 2026-08-30 - P6.5 Gamepads. The last Phase 6 stub in `Base` is gone

**`InputDevice_XBoxController_Linux.cpp` is a real device, on SDL3's gamepad API.** It replaces
the Phase 1 stub, and with it **`Base` has no `EE_UNIMPLEMENTED_FUNCTION` left outside
`RHI_Vulkan.cpp`.** The three that remain there are Phase 5's, and two more are upstream's own in
`Triangle.h` and `Encoding.cpp`.

**`InputSystem::Initialize()` works now.** It constructs two `XBoxControllerInputDevice`s and
calls `Initialize` on each, and that used to halt, which meant nothing could touch `InputSystem`
at all. P6.4 had to drive its device directly for that reason. That obstacle is gone.

**The name stays `InputDevice_XBoxController_Linux.cpp`**, per Conventions rule 3 and the phase
document, even though SDL3 handles any gamepad.

#### Three things that differ from XInput, all of them real

1. **The vertical axes are negated.** SDL follows the joystick convention, where pushing the
   stick down gives a positive Y. XInput's `sThumbLY` is positive upwards, and the engine is
   written against XInput. **Without the negation every controller would be inverted on Linux
   only**, which is the sort of defect that survives a long time because it looks like a user
   setting. Proved in both directions, on both sticks.
2. **Triggers use a different raw range.** XInput reports a byte, 0 to 255; SDL reports 0 to
   `SDL_JOYSTICK_AXIS_MAX`. Both normalize to 0..1, so only the divisor changes and
   `GetDefaultTriggerThreshold` keeps XInput's 30/255. All three dead zone values are unchanged
   from the Win32 device.
3. **Face buttons are named by position, not by letter.** `SDL_GAMEPAD_BUTTON_SOUTH` is the
   XInput A button, and "south" is exactly what `Controller_FaceButtonDown` means, so the mapping
   is more direct than XInput's.

#### Where the `SDL_Gamepad*` lives, and why it is not a member

**In a file-static array, indexed by hardware controller index.** `XBoxControllerInputDevice` has
no member to hold one and `InputDevice_XBoxController.h` is an upstream file, so adding one would
be an unregistered edit to a shared header - an escalation trigger. XInput needs no such storage:
it polls a slot number and the OS owns the connection.

The array holds four entries; `InputSystem` creates `s_maxControllers`, which is 2. An index
outside the array is treated as permanently disconnected rather than as an error.

**Hot plug is handled by re-reading the slot every frame.** `SDL_GetGamepads` returns a list, and
a device unplugged earlier in that list shifts the rest down, so the joystick ID at a slot is not
stable and cannot be cached. When it changes, the old handle is closed and the new one opened.

#### The gamepad subsystem initializes itself

`SDL_InitSubSystem( SDL_INIT_GAMEPAD )` is called from `XBoxControllerInputDevice::Initialize`
rather than from `LinuxApplication::Run`, so anything holding an `InputSystem` gets working
gamepads without knowing about SDL. SDL reference counts subsystems, so both devices doing it is
correct. **This replaces P6.2's note that P6.5 would add `SDL_INIT_GAMEPAD` to `Run`**; that
comment is updated in place.

**There are no `SDL_EVENT_GAMEPAD_*` cases in `LinuxApplication::ProcessEvent` either.** The
device polls, the way the XInput sibling does, and `SDL_UpdateGamepads` picks up plug and unplug
on its own. P6.3's placeholder comment is updated to say so.

#### Verification

- Files replaced: `Code/Base/Input/InputDevices/Platform/InputDevice_XBoxController_Linux.cpp`,
  which was a Phase 1 stub.
- Files edited: `Code/Base/Application/Platform/Application_Linux.cpp` (two stale comments),
  `Code/Scripts/NinjaGen/LinuxSources.txt` (the file moves out of the stub group, and
  `Application_Linux.cpp` moves to the Phase 6 group where it belongs).
- **Upstream files edited: none.**
- Build: `Checks.py` passes. `ninja -k 0` fails on `Esoterica.Applications.Editor` and
  `Esoterica.Applications.ResourceServer` only, which is where it failed before.
- **Run, end to end through `InputSystem`, with a scratch harness (not committed). All checks
  pass.** No gamepad is plugged into this machine, so the harness attaches an
  **SDL virtual joystick** (`SDL_AttachVirtualJoystick`) and drives it with
  `SDL_SetJoystickVirtualButton` and `SDL_SetJoystickVirtualAxis`. What it covers:
  - `InputSystem::Initialize` returns true and reports one connected controller.
  - All 14 buttons, one at a time, each checked to raise its own `InputID` **and nothing else**.
  - Stick Y inverted correctly in both directions, on both sticks: SDL -32768 becomes engine
    +1.000, SDL +32767 becomes engine -1.000.
  - Stick X passes through unchanged, and the two sticks are independent.
  - Triggers press and release independently.
  - Dead zones read 0.2395, 0.2652 and 0.1176, which are XInput's 7849, 8689 and 30 normalized.
  - Unplugging the virtual pad disconnects the device and drops the controller count to zero.
- **A note for anyone writing a similar harness.** SDL generates the mapping `lefttrigger:a4`
  for a virtual pad, which maps the full joystick range onto the trigger's 0..32767. A released
  trigger is therefore joystick axis **-32768**, not 0; setting 0 reads back as a half-pulled
  trigger. That cost a failing check before it was understood, and it is the harness rather than
  the device.
- Acceptance criteria: criterion 3 is met at the device level for keyboard, mouse and gamepad.
  "Works for camera control" still needs a running engine, which is P6.7 and P6.8. Criterion 10
  is untouched.

#### Not done, and not needed

Rumble, LEDs, gyro, touchpads and battery. The engine's `ControllerDevice` has no concept of any
of them, and XInput's device exposes none either. SDL3 offers them all; adding them would be a
feature this port does not owe.

### 2026-08-30 - P6.4 Keyboard and mouse. The mapping table is complete and proved complete

**`InputDevice_KeyboardMouse_Linux.cpp` is a real device.** It replaces the Phase 1 stub of four
halting functions. **The scancode table is complete: 105 scancodes map to 105 distinct
`InputID`s, one to one, with none missing and none duplicated.** That is checked by running it,
not by reading it.

**Scancodes, not keycodes**, as the phase document requires. A scancode names the physical key and
does not move with the layout, which is what the Win32 sibling gets from raw input. `SDL_SCANCODE_W`
is `Keyboard_W` on AZERTY too.

#### How an `SDL_Event` reaches the device

**By pointer, in `GenericMessage::m_data0`.** `GenericMessage` is four `uint64_t` and an
`SDL_Event` is 128 bytes, so it cannot be copied in. This is safe because
`InputSystem::ForwardInputMessageToInputDevices` dispatches synchronously, from inside
`LinuxApplication`'s event loop, while the event is still on the stack. The Win32 sibling passes
an `HRAWINPUT` handle through the same field, so the shape is not new.

**This is the contract P6.7 has to honour.** `EngineApplication_Linux::ProcessInputEvent` is one
line:

```cpp
m_engine.GetInputSystem()->ForwardInputMessageToInputDevices( { (uint64_t) &event, 0, 0, 0 } );
```

Nothing may queue that message for later.

#### What got simpler, and what got harder

**Simpler.** The Win32 device spends 60 lines in `ConvertKeyMessageToInputID` fixing up
`VK_SHIFT`, `VK_CONTROL`, `VK_MENU` and every numpad key from the message's scan code and
extended bit, because a Windows virtual key does not distinguish left from right, or numpad from
cursor block. Scancodes already do. That whole function is a hash lookup here.

Wheel deltas arrive in notches, so there is no `WHEEL_DELTA` to divide by. A "natural scrolling"
setting arrives as `SDL_MOUSEWHEEL_FLIPPED` rather than negated values, and is undone, so the
engine sees what Windows reports.

**Auto-repeat is dropped.** `SDL_EVENT_KEY_DOWN` repeats while a key is held and raw input does
not, so the Win32 sibling never sees one. `Press` on an already held key would restart its state.

#### Two behaviour gaps, both recorded rather than guessed at

1. **Mouse deltas stop at the screen edge; on Windows they do not.** The Win32 device reads raw
   input, which has no cursor and no bounds. `SDL_EVENT_MOUSE_MOTION`'s `xrel` and `yrel` follow
   the pointer. X11 takes an implicit pointer grab on button press, so a camera drag keeps
   receiving motion outside the *window*, but the *screen* edge still stops it.

   **`SDL_SetWindowRelativeMouseMode` is the fix, and P6.4 deliberately does not call it.**
   Turning it on for any button press would hide and warp the cursor and break every imgui drag,
   and the device cannot tell a camera drag from a slider drag. imgui's own backend already calls
   `SDL_CaptureMouse`, so a second owner of capture would fight it. **P6.8 should decide this
   against a live camera**, which is the first time anyone can see whether it matters.

2. **`m_charKeyPressed` only fills while text input is active.** SDL3 delivers
   `SDL_EVENT_TEXT_INPUT` only after `SDL_StartTextInput`, and the only caller of that is imgui's
   backend, when a text field has focus. `WM_CHAR` always arrives on Windows. **Nothing in the
   engine reads `GetCharKeyPressed()`** - the grep is empty - so this blocks nothing today.

   Non-ASCII is dropped rather than truncated. The field is one byte and `SDL_EVENT_TEXT_INPUT`
   is UTF-8; the Win32 sibling truncates a `WM_CHAR` code point to a `char`, which is the same
   loss written differently. imgui does its own text input and is unaffected.

#### Verification

- Files replaced: `Code/Base/Input/InputDevices/Platform/InputDevice_KeyboardMouse_Linux.cpp`,
  which was a Phase 1 stub.
- Files edited: `Code/Scripts/NinjaGen/LinuxSources.txt`, which now lists the file under Phase 6
  rather than under the stubs.
- **Upstream files edited: none.**
- Build: `Checks.py` passes. `ninja -k 0` fails on `Esoterica.Applications.Editor` and
  `Esoterica.Applications.ResourceServer` only, which is where it failed before.
- **Run, with a scratch harness (not committed). 20 checks, all passing.** It drives the device
  through an `InputDevice*`, because `InputDevice` declares the overrides public and access is
  checked on the static type:
  - Every scancode from 1 to `SDL_SCANCODE_COUNT` is fed in and the resulting `InputID`s
    collected. **105 scancodes produce 105 distinct IDs**; every ID from `Keyboard_A` to
    `Keyboard_RAlt` is produced by exactly one scancode.
  - Left and right Shift are distinct, and `Numpad4` is not the Left arrow. Those are the two
    cases the Win32 fix-up code exists for.
  - Press, hold, release; auto-repeat ignored.
  - All five mouse buttons; the vertical and horizontal wheel; `SDL_MOUSEWHEEL_FLIPPED` undone.
  - Two motion events in one frame accumulate, and the delta resets on the next.
  - Char input; non-ASCII dropped; focus loss clears held keys.
- **`InputSystem` cannot be used end to end yet.** `InputSystem::Initialize` constructs two
  `XBoxControllerInputDevice`s and calls `Initialize` on each, and that is still the P6.5 stub,
  which halts. The harness drives the keyboard and mouse device directly for that reason. P6.5
  removes the obstacle.
- Acceptance criteria: criterion 3 is half met. Keyboard and mouse are implemented and tested at
  the device level; gamepad is P6.5, and "works for camera control" needs a running engine, which
  is P6.7 and P6.8. Criterion 10 is untouched: the file is guarded with `#ifdef __linux__` and no
  `.vcxproj` lists it.

### 2026-08-30 - P6.3 imgui platform backend. Multi-viewport works, and the Win32 copy is stale

**The imgui platform backend runs on SDL3, and multi-viewport is verified rather than assumed.**
`ImguiPlatform_Linux.{h,cpp}` replace the Phase 1 stub, `ImguiX_Linux.cpp` is the sibling of
`ImguiX_Win32.cpp`, and `LinuxApplication::ProcessEvent` now calls imgui first.

**Proved by running it.** Three imgui windows became three real SDL windows, each with a live
`SDL_Window*` found from its viewport, one of them at 1100,200 which is outside the main window.
Shutdown destroyed all three. Under i3 on X11.

#### The vendored Win32 backend is three years behind the imgui core it sits next to

**This changes the plan for this task.** [Phase6-WindowingInput.md](Phases/Phase6-WindowingInput.md)
says to diff `ImguiPlatform_Win32.cpp` against "the matching upstream release" of
`imgui_impl_win32.cpp` and treat that as the worklist. There is no matching release:

- `Code/Base/ThirdParty/imgui/imgui.cpp` is **byte-identical to `v1.92.9b-docking`**, bumped
  upstream on 2026-08-06.
- `ImguiPlatform_Win32.cpp` was last touched on 2026-07-19 and last named a version in
  "Upgrade to DearImgui 1.89.1", 2022-11-29. Its functions still match roughly 1.89: it has
  `ImGui_ImplWin32_VirtualKeyToImGuiKey`, which upstream renamed to `KeyEventToImGuiKey`, and
  lacks `WndProcHandlerEx` and `AdjustWindowRect`.

So this task starts from **`v1.92.9b-docking`'s `imgui_impl_sdl3.cpp`**, which matches the
vendored core, and ports Esoterica's *adaptations* rather than mirroring a stale file. A
1.89-era backend against a 1.92 core would be wrong: 1.92 changed the font and texture contract
(`ImGuiBackendFlags_RendererHasTextures`).

#### Esoterica's adaptations, and what each became on SDL3

| Win32 adaptation | Linux |
|---|---|
| Wrapped in `EE::ImGuiX::Platform`, guarded, reformatted into house style | Same wrapper and guards. **Not reformatted**; see below |
| `Init`/`Shutdown`/`NewFrame` folded into `ImguiSystem::InitializePlatform`/`ShutdownPlatform`/`PlatformNewFrame` | Same, but as thin calls into the vendored functions rather than an inlined copy |
| Window read from `Platform::GetMainWindowHandle()`, not passed to `Init` | Same |
| Gamepad and XInput removed; the engine owns gamepads | Same. `ImGui_ImplSDL3_UpdateGamepads` and friends are gone |
| DPI awareness left to `Win32Application` | Left to `LinuxApplication`, which sets `SDL_WINDOW_HIGH_PIXEL_DENSITY` |
| Backend data through `EE::New` / `EE::Delete` | Same |
| A child wnd proc forwards input to `InputSystem`, so viewport windows keep feeding the engine | **Not needed.** SDL has one event queue, and `LinuxApplication::ProcessEvent` already forwards every input event whatever window it came from |
| A window class registered for viewport windows, with the app icon | Not needed. SDL has no window classes |
| `EE_BASE_API intptr_t WindowMessageProcessor(...)` | `EE_BASE_API bool ProcessEvent( SDL_Event const& )` |

**Two adaptations are new, and neither has a Win32 counterpart:**

1. **The `NewFrame` time step is removed.** Upstream's `ImGui_ImplSDL3_NewFrame` sets
   `io.DeltaTime` from `SDL_GetPerformanceCounter`. `ImguiSystem::StartFrame` sets it from the
   engine clock and then calls `PlatformNewFrame`, so upstream's version would overwrite it every
   frame. Verified: the engine's 0.01667 survives.
2. **`ProcessEvent`'s return value is ignored by the caller**, and it guards against a null
   context. Both matter, and the second is written up under P6.2's file below.

#### **The return value of the two backends does not mean the same thing**

`Win32Application::WindowMessageProcessor` returns early when
`ImGuiX::Platform::WindowMessageProcessor` returns non-zero. **Copying that would break the
application.** A wnd proc returns non-zero only for a message it truly consumed, and
`imgui_impl_win32.cpp` returns 0 for nearly everything. `imgui_impl_sdl3.cpp` returns `true` for
every event it recognises, including `SDL_EVENT_WINDOW_CLOSE_REQUESTED` and both focus events. An
early return there swallows the application's own close and stops input reaching the engine.
`LinuxApplication::ProcessEvent` therefore calls imgui and ignores the answer, which is what
upstream's own SDL3 examples do.

`ProcessEvent` also returns false when there is no imgui context or backend.
`ImGui_ImplSDL3_ProcessEvent` asserts in that case, and `LinuxApplication` pumps events for any
subclass, including one that never starts imgui.

#### The vendored region keeps upstream's formatting, and that is deliberate

**This breaks Conventions rule 8, knowingly.** The vendored region of `ImguiPlatform_Linux.cpp`
is left at upstream's indentation, at column zero rather than indented into the namespace, so
that

```
diff <upstream imgui_impl_sdl3.cpp> <the vendored region>
```

still works. Every deliberate change carries an `EE:` comment for exactly that reason. The
argument is the finding above: `ImguiPlatform_Win32.cpp` was reformatted into house style and is
now three years behind the core it sits beside. Rule 8's own rationale is consistency with the
neighbour; here the neighbour's approach is what produced the drift. A banner at the top of the
file says all of this, so nobody has to rediscover it.

Everything Esoterica wrote in that file, and all of `ImguiPlatform_Linux.h` and
`ImguiX_Linux.cpp`, is in house style.

#### `PlatformHandleRaw` is null on Linux

`ImGui_ImplSDL3_SetupPlatformHandles` fills `PlatformHandleRaw` only on Windows and macOS, and
puts the `SDL_WindowID` in `PlatformHandle`. So `ImguiX_Linux.cpp`'s window controls look the
window up with `SDL_GetWindowFromID` where the Win32 sibling casts `PlatformHandleRaw` to an
`HWND`. Minimize, maximize, restore and close map to `SDL_MinimizeWindow`, `SDL_MaximizeWindow`,
`SDL_RestoreWindow` and a pushed `SDL_EVENT_WINDOW_CLOSE_REQUESTED`; there is no SDL equivalent
of `SendMessage( hwnd, WM_CLOSE )`.

#### The renderer half is confirmed separate, and it gates viewports

The phase document asked for this to be confirmed rather than assumed. It is separate:
`ImguiRenderer` goes through the RHI, and nothing in `ImguiPlatform_Linux.cpp` touches it.

**But imgui gates `ImGuiConfigFlags_ViewportsEnable` on both halves** (`imgui.cpp:11791`): the
platform backend must set `ImGuiBackendFlags_PlatformHasViewports` *and* the renderer must set
`ImGuiBackendFlags_RendererHasViewports`. With only the platform half, imgui silently disables
viewports for the frame and every window merges into the main one. That cost an hour here, and
it is the first thing to check if the editor's windows will not detach in Phase 7.

#### Verification

- Files added: `Code/Base/Imgui/Platform/ImguiPlatform_Linux.h`,
  `Code/Base/Imgui/Platform/ImguiX_Linux.cpp`.
- Files replaced: `Code/Base/Imgui/Platform/ImguiPlatform_Linux.cpp`, which was a Phase 1 stub of
  three halting functions.
- Files edited: `Code/Base/Application/Platform/Application_Linux.cpp` (the imgui hook),
  `Code/Scripts/NinjaGen/LinuxSources.txt`.
- **Upstream files edited: one.** `Code/Base/Imgui/ImguiSystem.cpp:12`, `#if _WIN32` to
  `#if _WIN32 || defined( __linux__ )`. `git diff --stat upstream/main` shows **1 line changed**,
  which is Phase 6 acceptance criterion 12. Registered in [TouchedFiles.md](TouchedFiles.md).
  **It turns out to be cosmetic**: `imconfig.h` defines `IMGUI_ENABLE_FREETYPE` unconditionally,
  so `imgui_freetype.cpp` was already compiled into `libEsoterica.Base.so` before this change.
- Build: `Checks.py` passes. `ninja -k 0` fails on `Esoterica.Applications.Editor` and
  `Esoterica.Applications.ResourceServer` only, which is where it failed before. Both are Phase 7.
- Run, with a scratch subclass (not committed): the backend reports itself as
  `imgui_impl_sdl3 (3.4.14; 3.4.14) (x11)`; `ImGuiBackendFlags_PlatformHasViewports` and
  `HasMouseCursors` are set and `HasGamepad` is not, which is the intended removal; one monitor is
  enumerated at 1920x1080, DPI scale 1.0; `io.DisplaySize` is 958x1042 with framebuffer scale
  1.0; `io.DeltaTime` keeps the engine's value; draw data is produced; and three imgui windows
  became three live SDL windows at the positions imgui asked for, destroyed cleanly on shutdown.
  The harness fakes the two renderer flags, because it has no renderer.
- Acceptance criteria: **criterion 12 is met.** Criterion 5 needs the renderer and a running
  engine, so it is P6.7 and P6.8. Criterion 10, the Windows build, is untouched: the one upstream
  edit adds a Linux branch to an existing `#if` and changes nothing Windows compiles.

#### Still open

- **Wayland is untested.** This machine runs i3 on X11 and has no Wayland compositor, so nothing
  here says how viewports behave under one. P6.8 owns it. Note that
  `ImGui_ImplSDL3_Init` sets `bd->IsWayland` and the mouse capture and global state white list
  excludes wayland, so `ImGuiBackendFlags_PlatformHasViewports` **will not be set on Wayland at
  all**. Read `ImGui_ImplSDL3_Init` before assuming the editor's docking UI works there.
- **i3 honoured the viewport positions**, which was the worry Phase 6 recorded about tiling and
  Wayland window placement. It ignores `SDL_SetWindowPosition` on the main window and honours it
  on the viewport windows, which are borderless utility windows.

### 2026-08-30 - P6.2 `LinuxApplication`, and a Phase 5 / Phase 6 conflict to settle

**`LinuxApplication` is written and it runs.** `Application_Linux.{h,cpp}` mirror
`Application_Win32.{h,cpp}` on SDL3. A window opens, resizes, persists its layout and shuts down
cleanly. **Nothing derives from it yet**: `EngineApplication_Linux` is P6.7.

**Verified by running it, not only by building it.** A scratch subclass linked against
`libEsoterica.Base.so` opened a window, took a resize, wrote and re-read its layout file, ran the
borderless hit test and exited 0. Details below. The program is not committed.

#### What carried over unchanged

`Initialize`, `Shutdown`, `ApplicationLoop`, `ResizeMainWindow`, `OnUserExitRequest`,
`FatalError`, `OnFirstShowMainWindow`, `ProcessWindowDestructionMessage`, `ReadWindowSettings`,
`WriteWindowSettings`, `RequestApplicationExit`, `GetBorderlessTitleBarInfo`, `Run`,
`WasInitialized`, and both `InitOptions` flags. A subclass reads the same on both platforms.

#### What changed, and why

| Win32 | Linux |
|---|---|
| `Win32Application( HINSTANCE, name, iconResourceID, splashResourceID, options )` | `LinuxApplication( name, iconFilePath, splashScreenFilePath, options )`. Both paths may be null, and both are null today. |
| `WindowMessageProcessor( HWND, UINT, WPARAM, LPARAM )` | `bool ProcessEvent( SDL_Event const& )` |
| `ProcessInputMessage( UINT, WPARAM, LPARAM )` | `ProcessInputEvent( SDL_Event const& )` |
| `HICON GetIcon()` | `SDL_Surface* GetIcon()` |
| `BorderlessWindowHitTest( POINT )` returning an `HT*` code | Same shape, returning an `SDL_HitTestResult` through `SDL_SetWindowHitTest` |
| `RECT m_windowRect` | `Int2 m_windowPosition` and `Int2 m_windowSize` |
| `WM_GETMINMAXINFO` clamps to 320x240 | `SDL_SetWindowMinimumSize` |
| Live++ agent, hooks and `#if EE_ENABLE_LPP` blocks | Absent. There is no Live++ on Linux. |

**The header forward declares `SDL_Window`, `SDL_Surface` and `SDL_Event` and includes no SDL
header.** `ImguiPlatform_Win32.h` forward declares `HWND__` for the same reason. This keeps SDL3
off the include path of everything that derives from `LinuxApplication`, so
`Esoterica.Applications.Engine` needs the SDL3 sheet only when P6.7 writes a `.cpp` that reads
event fields.

**`m_windowPosition` and `m_windowSize` are in different units, on purpose.** The position is
logical desktop coordinates, which is what `SDL_SetWindowPosition` takes. The size is pixels,
which is what the swapchain needs. They are the same numbers on a non-HiDPI display. The window is
created with `SDL_WINDOW_HIGH_PIXEL_DENSITY`, and `m_windowSize` is read back with
`SDL_GetWindowSizeInPixels` after creation and from `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` after
that. **`SDL_EVENT_WINDOW_RESIZED` is the wrong event here**: it reports logical coordinates.

**The layout file keeps the Win32 key names.** `Left`, `Right`, `Top`, `Bottom`, `WasMaximized`
under `[WindowSettings]`, so one `.layout.ini` is portable between the two builds.

**`WriteWindowSettings` is called from `Run` before `Shutdown`, not only from an event.** The
window is destroyed in the destructor, so `SDL_EVENT_WINDOW_DESTROYED` never arrives while the
loop is running, and `WM_DESTROY`'s job has to be done explicitly.

**Window events are filtered by window id.** imgui multi-viewport creates windows of its own, and
closing one of those is not a request to close the application.

**`SDL_Init( SDL_INIT_VIDEO )` happens in `Run`, and `SDL_Quit` in the destructor.**
`Win32Application` has no equivalent step. P6.5 adds `SDL_INIT_GAMEPAD`.

**`FatalError` logs as well as showing `SDL_ShowSimpleMessageBox`.** A dialog needs a desktop, and
this runs from a terminal often enough that the message has to survive without one.

**The exit code is always 0 on a clean run.** `Win32Application` takes its code from `WM_QUIT`'s
`wParam`; SDL has no such value.

#### Left for the next tasks, marked in the code

- **P6.3.** `ProcessEvent` has no imgui hook yet. `Win32Application` calls
  `ImGuiX::Platform::WindowMessageProcessor` first, before anything else; there is no
  `ImguiPlatform_Linux.h` to call into. A comment marks the spot.
- **P6.5.** `ProcessEvent` has no `SDL_EVENT_GAMEPAD_*` cases. XInput polls, so the Win32 sibling
  has nothing to copy, and `SDL_EVENT_GAMEPAD_ADDED` and `_REMOVED` have no polling equivalent.
  A comment marks the spot.

#### **Escalation: Phase 5 and Phase 6 disagree about `Platform::SetMainWindowHandle`**

**This blocks P6.6 and it is a human decision.** `LinuxApplication::TryCreateMainWindow` stores
the `SDL_Window*`, exactly as `Win32Application` stores the `HWND`. That is what the imgui and
input backends need. But:

- `EngineModule.cpp:135` does `m_renderWindow.SetNativeWindowHandle( Platform::GetMainWindowHandle() )`.
- `RHI_Vulkan.cpp:2216` casts that value straight to a `VkSurfaceKHR`, which is P5.3's recorded
  decision.
- **An `SDL_Window*` is not a `VkSurfaceKHR`.**

P5.3's answer was "the application creates the surface and hands it over", and **the application
has no place to do it.** `EngineModule::InitializeModule` calls `RenderSystem::Initialize`, which
creates the `VkInstance`, and then calls `SetNativeWindowHandle` three lines later. Nothing the
application owns runs in between. Before that call there is no instance, so no surface can exist.

**`Code/Engine/_Module/EngineModule.cpp` is not in [TouchedFiles.md](TouchedFiles.md)**, which is
the escalation trigger. Three candidates, best first:

1. **`RHI_Vulkan.cpp` creates the surface itself, through a Linux-only `Platform` function.**
   `Platform::CreateVulkanSurface( instance, pNativeWindowHandle )` lives in `Platform_Linux.cpp`,
   which may link SDL3 because `Esoterica.Base` already does. **No upstream file is edited**, and
   `Base/Render` still includes no window system header, which is what P5.3 actually required.
   It does revise P5.3's "the application owns it", so it is a decision, not a detail.
2. **A two-line `#elif defined( __linux__ )` in `EngineModule.cpp:135`.** The right shape under
   Conventions rule 2, but it puts SDL3 into `Esoterica.Engine.Runtime` and adds a file to the
   registry that a full survey did not predict.
3. **Store the surface in `Platform::SetMainWindowHandle`.** Does not work, for the ordering
   reason above.

**Answered the same day: candidate 1.** See the decision entry, "`RHI_Vulkan.cpp` creates the
Vulkan surface, through `Platform_Linux.cpp`". P6.2 itself does not change: it stores the
`SDL_Window*`, which is right for imgui and input either way. **P6.6 writes the surface function.**

#### X11 and Wayland findings so far

**The development session runs i3 on X11, and a tiling window manager makes two Phase 6
acceptance criteria untestable here.** i3 tiled the window to 958x1042 and ignored both
`SDL_SetWindowPosition` and `SDL_SetWindowSize`. That is correct behaviour, not a defect, and the
code handles it: the tiling arrives as `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` and
`ResizeMainWindow` gets the real size. **Criterion 7, DPI scaling, and the parts of criterion 4
that need a client-driven resize need a floating window manager or a second session.** Wayland is
not tested yet; P6.8 owns that.

#### Known issue for P6.7, not introduced here

**`External/` shared libraries are on a relative rpath, so a binary only resolves them from the
repository root.** `ldd Build/Linux_Release/libEsoterica.Base.so` from any other directory reports
`libSDL3.so.0`, `libdxcompiler.so` and `libGameNetworkingSockets.so` as not found. This predates
Phase 6: `Toolchain.py:503` emits `-Wl,-rpath,<repo-relative directory>`. The Phase 6 deliverable
is run from the repository root, so it works, but P6.7 should make that path absolute in one place
rather than working around it.

#### Verification

- Files added: `Code/Base/Application/Platform/Application_Linux.h`,
  `Code/Base/Application/Platform/Application_Linux.cpp`.
- Files edited: `Code/Scripts/NinjaGen/LinuxSources.txt` (adds the new `.cpp`, and drops the stale
  "still to come in Phase 1" note that listed it).
- **Upstream files edited: none.**
- Build: `Checks.py` passes. `ninja -k 0` fails on `Esoterica.Applications.Editor` and
  `Esoterica.Applications.ResourceServer` only, which is where it failed before. Both are Phase 7.
  `libEsoterica.Base.so` builds and links in Debug and Release.
- Run: a scratch subclass linked against `libEsoterica.Base.so`. `Initialize` saw 640x480 at
  100,100 on the first run; the window opened; `ResizeMainWindow` got 958x1042 from i3;
  `SDL_EVENT_WINDOW_CLOSE_REQUESTED` reached `OnUserExitRequest`; `Shutdown` ran and `Run`
  returned 0. The layout file was written with the real geometry, and a second run read it back
  and reported 958x1042 at 961,1. With `InitOptions::Borderless` set, the hit test returned
  `RESIZE_TOPLEFT` at 4,4, `DRAGGABLE` inside the reported title bar, and `NORMAL` in the client
  area, and input events reached `ProcessInputEvent`.
- Acceptance criteria: P6.2 has none of its own. Phase 6 criterion 1 is not met, and cannot be
  until P6.7. Criterion 10, the Windows build, is untouched: the two new files are guarded with
  `#ifdef __linux__` and no `.vcxproj` lists them.

### 2026-08-29 - P6.1 SDL3, and open question 4 is answered

**SDL3 is fetched, built and wired into the generator.** `./DownloadDependencies.sh sdl3` builds
`release-3.4.14` from source into `External/SDL3/`, and `Esoterica.Base` now links `-lSDL3`.
Nothing includes an SDL header yet: P6.2 onwards does that.

**Open question 4 is answered: no, and the port always builds SDL3 from source.** Ubuntu 24.04
LTS, the development target, packages no SDL3 at all. SDL3 first shipped in January 2025 and
reaches the archives from Ubuntu 25.04 onwards, so a distribution package cannot be relied on.
That also settles the version question: a source build pins one version for every distribution,
which matters because the vendored Dear ImGui is 1.92.9b and its `imgui_impl_sdl3.cpp` calls
recent SDL3 additions.

**The sheet points at `External/`, not at `pkg-config --libs sdl3`**, which is what
[Phase6-WindowingInput.md](Phases/Phase6-WindowingInput.md) planned. There is no `sdl3.pc` on the
system to find, and once `External/SDL3/` holds a build, a pkg-config lookup would need
`PKG_CONFIG_PATH` set to reach it. Every other `External/` dependency uses include and library
directories, so SDL3 does too. `Toolchain.py` adds `-Wl,-rpath` for every library directory
already, so the built binaries find `libSDL3.so.0` without a staging step.

**X11 and Wayland are both forced on in the CMake configure, rather than left to detection.**
SDL's CMake drops a video backend whose headers are missing and still configures successfully,
which produces a library that builds, links, and then finds no display at run time. That is the
silent failure this build guards against, so `requirements_sdl3()` checks all the X11, Wayland
and xkbcommon packages by `pkg-config` name and reports every missing one in one message.
`CMAKE_INSTALL_LIBDIR=lib` is set too, because `GNUInstallDirs` on Debian and Ubuntu would
install into `lib/x86_64-linux-gnu`.

**Checked at run time, once, with a scratch program.** A green build hides all of this. The
built library reports video drivers `wayland`, `x11`, `kmsdrm`, `offscreen`, `dummy` and
`evdev`; `SDL_CreateWindow` with `SDL_WINDOW_VULKAN` succeeds; and
`SDL_Vulkan_GetInstanceExtensions` returns `VK_KHR_surface` and `VK_KHR_xlib_surface`, both of
which `CreateContext` already enables. This session runs X11. The program is not committed:
the build is the test, and this was a one-off check of a run-time property.

- Files added: none.
- Files edited: `DownloadDependencies.sh` (the `sdl3` target),
  `Code/Scripts/NinjaGen/Toolchain.py` (the `SDL3` sheet, and `Esoterica.Base` in
  `LINUX_ONLY_SHEETS`). Both are new files this fork owns, and both are already registered.
- **Upstream files edited: none.**
- Build: `python3 Code/Scripts/NinjaGen/NinjaGen.py` and `ninja -f Build/Linux/Esoterica.ninja
  -k 0` fail on `Esoterica.Applications.Editor` and `Esoterica.Applications.ResourceServer`
  only, which is exactly where they failed before. `Checks.py` passes.
- Acceptance criteria: P6.1 has none of its own. Phase 6 criterion 10, the Windows build, is
  untouched: neither edited file is read by MSBuild.

**For P6.2.** `External/SDL3/lib/cmake` and `External/SDL3/lib/pkgconfig` are installed and
unused. Leave them; deleting them would only make a future `find_package` harder.

### 2026-08-29 - The Phase 5 stack is merged, and one question is left

**All seventeen Phase 5 branches are on `main`.** PRs #24 to #41 went in as merge commits, ending
with `p5.16-raytracing` at `117d45b`. Nothing is in flight.

**Nothing about the code changed on merge, so nothing above changes.** Every group is still
compile-verified and run-unverified, 3 `EE_UNIMPLEMENTED_FUNCTION` remain, and Phase 5 acceptance
criteria 5 to 10 still need a running engine.

**One item in Phase 5 is left, and it is a decision rather than code: open question 7.** The
engine's command signatures carry root data, no Vulkan indirect draw can bind it, and both
candidate answers change the shaders. It blocks a rendered frame and nothing else in Phase 5 moves
past it. Phase 6 can bring up the window, the input and the swapchain without it.

Docs brought in line with the merge: [README.md](README.md) status,
[Phase5-VulkanRHI.md](Phases/Phase5-VulkanRHI.md) (the indirect note, acceptance criterion 1, and
the P5.13 group), and [Phase6-WindowingInput.md](Phases/Phase6-WindowingInput.md) (its
prerequisites, and P6.6, which still planned to pass an `SDL_Window*` into the RHI).

**Upstream files edited: none.**

### 2026-08-29 - P5.16 Raytracing. The last group, and the least reachable

**`CreateAccelerationStructure`, `GetAccelerationStructureHandle`, `CmdBuildAccelerationStructure`,
`CmdDispatchRays`, the raytracing `CreatePipeline` overload and the indirect ray path are
implemented.** 3 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 9, and none of them is a whole
function. Nothing has run.

**All sixteen groups are now written.**

#### Unreachable three times over, and that is worth stating plainly

- **No caller.** Nothing in `Code/Engine` or `Code/Applications` creates an acceleration
  structure. `RHI.esh` defines `GetRaytracingAccelerationStructure` and no shader uses it.
- **No shader table, on either backend.** `RHI.h` declares no factory for a
  `RaytracingShaderTable`, and `RHI_Direct3D12.cpp` never constructs its own version either, so
  `CmdDispatchRays` is unreachable by construction on both sides.
- **No hardware here.** Only `llvmpipe` reports `VK_KHR_ray_tracing_pipeline` in this machine, the
  same story as mesh shaders.

It is written because the phase document asks for full parity and criterion 1 counts the markers.
It should be treated as unproven code until something calls it.

#### The heap question the binding model left open, answered without changing anything

P5.7 left a note asking whether `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` has to join the
heap's mutable type list, since `RHI.esh` reads an acceleration structure straight out of the heap.

**It does not, because no shader does.** `GetAccelerationStructureHandle` returns a buffer handle
on the top level structure buffer, which is exactly what `RHI_Direct3D12.cpp:4002` returns. The day
a shader actually writes `GetRaytracingAccelerationStructure`, the heap needs the descriptor type
and that is a Phase 4 binding model decision, not this file's.

#### Three places the reference is broken and this is not

| Reference | What happens |
|---|---|
| `RHI_Direct3D12.cpp:3981` | The line that fills in `m_instanceBuffer` is commented out, and `:3390` then dereferences it. The top level build would crash. `CreateAccelerationStructure` here records the buffer. |
| `RHI_Direct3D12.cpp:3978` | The top level structure buffer is created with `BufferFlags::NoDescriptors` and descriptor types `RWBuffer\|Raw`, and `GetAccelerationStructureHandle` then asks it for a `DescriptorTypeFlags::Buffer` handle it cannot have. Two asserts. This one gets the descriptor it is about to be asked for. |
| `RHI_Direct3D12.cpp:3969` | The scratch buffer is sized from the bottom level prebuild alone and then reused for the top level build at `:3392`, which overruns whenever the top level needs more. Here it is sized to the larger of the two. |

All three are recorded under "Upstream issues observed".

#### Four pipeline parameters have no Vulkan equivalent

`m_pEmptyRootSignature`, `m_pRayGenRootSignature`, `m_rayMissRootSignatures` and each hit group's
`m_pRootSignature` are Direct3D 12 **local** root signatures, which let each shader binding table
record carry its own bindings. **Vulkan has one pipeline layout for the whole raytracing pipeline
and nothing else.** Moving the per-record data into the table and reading it in the shader is a
shader change, so the local signatures are dropped and the empty case is asserted.

`m_payloadSize` and `m_attributeSize` are Direct3D's shader config; Vulkan reads both out of the
SPIR-V. `m_maxNumRays` has no counterpart at all.

#### The role of a raytracing shader comes from where it sits, not from the shader

`RHI.h` has one `ShaderStage::RayTracing` for all five roles, so `VulkanShaderStage` cannot tell a
miss shader from a closest hit one. The Vulkan stage is decided by which parameter field the
`Shader` arrived in.

The entry point names need a null terminator that `StringView` does not carry, so they are copied
into a vector **reserved to its exact maximum before the first one is added**. Every
`VkPipelineShaderStageCreateInfo::pName` points into it, and one reallocation part way through
would dangle every pointer taken so far.

#### The third and last file static

`CreateBuffer` has no `Context`, and a raytracing build reads its inputs and stores its result in
ordinary buffers, which need usage bits that only exist once `VK_KHR_acceleration_structure` is
enabled. So every buffer carries them when `g_raytracingEnabled` is true, rather than the RHI
growing a flag it does not have. Same shape as `g_meshShaderEnabled` and
`g_fragmentShadingRateEnabled`.

All three extensions go on together or not at all: `VK_KHR_acceleration_structure`,
`VK_KHR_ray_tracing_pipeline`, and `VK_KHR_deferred_host_operations`, which the first depends on
and which carries no feature bit of its own.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 111 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved. Unchanged on purpose: all
  nine raytracing entry points are extension functions looked up through `vkGetDeviceProcAddr`.
- The new code adds no compiler warning.
- The three remaining markers were read and attributed.

#### Not verified

None of it. No structure built, no ray traced, no raytracing pipeline compiled, and no hardware
here to try. **The first machine with the extensions should not assume any of this works.**

**Upstream files edited: none.**

### 2026-08-29 - P5.15 Variable rate shading. Written, and deliberately switched off

**`CmdSetShadingRate` is implemented and the reported capability stays `NotSupported`.** 9
`EE_UNIMPLEMENTED_FUNCTION` remain, down from 10. Nothing has run, and **nothing can reach this
code until a decision is taken on the Direct3D 12 side.**

#### Why it is off, which is the whole of the group

`RHI_Direct3D12.cpp:2177` sets `m_shadingRate` and `m_shadingRateCaps` to `NotSupported` with a
TODO, so `CmdSetShadingRate` is a no-op on the reference too. Nothing in the engine calls it either
way.

Reporting the real device capability here would be a divergence **we** introduced: the engine would
start shading at a reduced rate on Linux and not on Windows, and acceptance criterion 7 is a
screenshot comparison between the two. So the capability line matches the reference exactly, and
the code behind it is written and reachable the moment both backends change together.

#### Turning it on needs three things, not one

| What | Where |
|---|---|
| The capability line | `FillDeviceCapabilities` here and `RHI_Direct3D12.cpp:2177` there. Both, or the two backends diverge. |
| `VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR` on a rate image | `CreateTexture` does not set it, and a rate image requires it. |
| A view for the rate image | The per-tile path uses the render target view, which P5.6 only builds for a texture created with `DescriptorTypeFlags::RenderTarget`. A rate image wants a view of its own. |

The last two are guesswork until something creates a rate image, and both are small.

#### The structural difference: a command against an attachment

**Direct3D 12 binds the shading rate image with `RSSetShadingRateImage`, a command. Vulkan makes it
an attachment of the render pass.** So `CmdSetShadingRate` records the view on the command buffer
and `BeginRenderingIfPending` chains a `VkRenderingFragmentShadingRateAttachmentInfoKHR` onto the
`VkRenderingInfo`. A pass without one carries no `pNext` at all.

The per-draw rate maps directly: `vkCmdSetFragmentShadingRateKHR`, guarded on the same
`m_shadingRateCaps` the reference copies onto its command buffer at `RHI_Direct3D12.cpp:2862`.

#### Four combiners map and the fifth does not

`Passthrough`, `Override`, `Min` and `Max` are `KEEP`, `REPLACE`, `MIN` and `MAX`. **Direct3D's
`SUM` adds the two rates and Vulkan's nearest operation, `MUL`, multiplies them.** There is no
Vulkan combiner that sums, so `Sum` maps to `MUL` and the two backends would disagree on it.
Nothing calls `CmdSetShadingRate`, so nothing disagrees today.

#### A dynamic state that has to be set even though nothing uses it

Every graphics pipeline declares `VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR` when the extension is
enabled, because `vkCmdSetFragmentShadingRateKHR` needs it. **A declared dynamic state that is
never set leaves every draw undefined**, and nothing in the engine sets it, so `BeginCommandBuffer`
sets the full rate once per command buffer. That is what a pipeline without variable rate shading
does anyway.

The list is built conditionally, through a second file static, `g_fragmentShadingRateEnabled`,
for the same reason `g_meshShaderEnabled` exists: declaring a dynamic state from a disabled
extension is a validation error, and `CreateGraphicsOrMeshPipeline` has no `Context`.

`VK_KHR_fragment_shading_rate` is optional like `VK_EXT_mesh_shader`, and needs both
`pipelineFragmentShadingRate` and `attachmentFragmentShadingRate`.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 111 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved. Unchanged on purpose:
  `vkCmdSetFragmentShadingRateKHR` is an extension function looked up through
  `vkGetDeviceProcAddr`.
- The new code adds no compiler warning.

#### Not verified, and unverifiable as it stands

Nothing here runs while the capability says `NotSupported`. **The pipeline change is the one to
watch**: every graphics pipeline now declares one more dynamic state on a device that has the
extension, and a mistake there breaks every draw rather than only the shading rate. The default
rate set in `BeginCommandBuffer` is what stops that.

**Upstream files edited: none.**

### 2026-08-29 - P5.14 Mesh shaders. No GPU in this machine has them

**`CmdDispatchMesh`, the mesh `CreatePipeline` overload and the indirect mesh path are
implemented.** 10 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 13. Nothing has run.

#### The finding that matters more than the code

**Neither real GPU in this development machine supports `VK_EXT_mesh_shader`.** `vulkaninfo`
reports it on `llvmpipe` alone, the software rasteriser. The Intel UHD 620 and the NVIDIA MX250
both lack it.

That is not a port problem. Both are below the hardware bar for mesh shaders on Direct3D 12 too,
so the engine's debug draw would fail on Windows on this machine as well. It does mean **the debug
draw path cannot be verified here**, and whoever first runs a development build on this machine
will hit the assert in `CmdDispatchMesh`.

#### Optional, not required, and that is a decision

Every other capability the backend needs is in `g_requiredDeviceExtensions`, and a device missing
one is refused at `CreateContext` with the extension named. Mesh shaders are not, for one reason:
**the engine has no capability flag for them and no fallback path.** `RenderPass_DebugDraw` calls
`CmdDispatchMesh` outright, `RHI.h` has no mesh shader field in `DeviceCapabilities`, and nothing
anywhere asks whether they exist. Direct3D 12 simply assumes the hardware has them.

Requiring the extension would refuse a device the rest of the engine renders on perfectly well,
which is exactly the situation this machine is in. So the extension is asked for when present,
`CreateContext` logs a warning when it is missing, and every use asserts.

#### A mesh dispatch is a draw

`CmdDispatchMesh` calls `PrepareDraw`, not the flush-and-suspend pair `CmdDispatchCompute` uses.
`DispatchMesh` is Direct3D's name for it; it rasterises, it runs inside a render pass, and leaving
the pass for it would be wrong.

#### One pipeline body for two overloads

`MeshPipelineParameters` derives from `GraphicsPipelineParameters` and adds nothing, and Vulkan
builds both with `vkCreateGraphicsPipelines`. The whole delta is which shader stages are wanted
and whether there is an input assembler, so `CreateGraphicsOrMeshPipeline` takes a flag and both
overloads call it. A second copy of two hundred lines of blend, depth, raster and dynamic
rendering state would only be a place for the two to drift apart.

`pVertexInputState` and `pInputAssemblyState` are null on a mesh pipeline. There is no input
assembler in front of a mesh shader, and the spec says to leave both out.

#### A barrier correction that P5.9 flagged and could not make

`VulkanPipelineStage` mapped `PipelineStage::NonPixelShader` and `AllShader` onto every shader
stage **except task and mesh**, because naming a stage from a disabled extension is a validation
error. Now that the extension is conditionally enabled, both bits go in when it is.

**Without them a barrier before a mesh draw would not cover the stage that reads the result**,
which is a silent wrong-data bug rather than a validation one. `VulkanPipelineStage` is handed
flags and no `Context`, so the answer is a file static, `g_meshShaderEnabled`, set by
`CreateContext` and cleared by `DestroyContext`. One context at a time, which is what the engine
creates, and the same shape the leak counters already use.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 111 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved. The count is unchanged on
  purpose: all three mesh entry points are extension functions looked up through
  `vkGetDeviceProcAddr`.
- The new code adds no compiler warning.
- `vulkaninfo` was read for the mesh shader support on both GPUs in this machine, rather than
  assumed.

#### Not verified

No mesh shader compiled into a pipeline, no mesh draw issued, and **it cannot be verified on this
machine at all**. The first hardware with `VK_EXT_mesh_shader` should check the debug draw pass
before anything else, because it is the only consumer.

**Upstream files edited: none.**

### 2026-08-29 - P5.11 Query pools

**All seven query functions are implemented, and the `SetDebugName` overload P5.12 owed to this
group is filled in.** 13 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 21. Nothing has run.

#### A correction: nothing in the engine calls any of this

The P5.12 entry said P5.11 was needed for "the query pools a development build's profile scopes
use every frame". **That is wrong.** `EE_RHI_COMMAND_BUFFER_PROFILE_SCOPE` at `RHI.h:1705` expands
to a CPU profile scope and a `CommandBufferMarkerScope`, which is a debug marker. It records no
timestamp. There is no `CreateQueryPool` call anywhere in `Code/Engine` or `Code/Applications`.

The group is written for parity, not because the frame needs it.

#### The four mappings that are not one for one

| `RHI.h` | Vulkan |
|---|---|
| `CmdResetQueryPool` | **Direct3D 12 does nothing here and Vulkan requires it.** This is the one place in the backend where the asymmetry runs that way: a Vulkan query is undefined until it has been reset. It may not run inside a render pass, so it goes through `PrepareTransfer`. |
| `CmdBeginQuery` on a timestamp pool | Nothing. A timestamp is written at one point, not over a range, and `vkCmdBeginQuery` on a timestamp pool is a validation error. It matches what the reference achieves anyway; see "Upstream issues observed". |
| `CmdEndQuery` on a timestamp pool | `vkCmdWriteTimestamp2` at `BOTTOM_OF_PIPE`, because Direct3D's `EndQuery` timestamp is taken after the work the scope covers. **It is legal inside a render pass**, which matters: a profile scope around a pass must not tear it the way a reset would. |
| `GetQueryTimestampFrequency` | `1e9 / timestampPeriod`. Vulkan reports nanoseconds per tick and Direct3D 12 reports ticks per second, which is the inversion the phase document asks for. |

#### Two things Direct3D 12 has no equivalent of, and both are asserted

- **A queue family may report zero valid timestamp bits**, meaning it cannot write one at all.
  `VulkanQueue` now carries `m_timestampValidBits` and `m_timestampPeriod`, both read in
  `CreateQueue`, because `GetQueryTimestampFrequency` is handed a `Queue` and no `Context`.
- **`pipelineStatisticsQuery` is a device feature**, and a `PipelineStatistics` pool cannot be
  created without it. It is enabled **when the device has it and never required**, so a device
  missing it is not refused over a capability nothing uses. `CreateQueryPool` asserts on the flag.

#### The resolve writes eight bytes per query, which is only right for a timestamp

The destination offset is the reference's, `startQuery * 8` at `RHI_Direct3D12.cpp:3528`, and a
pipeline statistics query resolves to eleven counters rather than one. Both backends have to write
the same layout, so `CmdResolveQuery` asserts the pool is a timestamp pool rather than inventing a
second layout. Nothing creates a statistics pool.

`VK_QUERY_RESULT_WAIT_BIT` is set, because Direct3D's `ResolveQueryData` reads finished results.
Without it the copy could write nothing and report availability separately.

#### The eleven pipeline statistics all map

`D3D12_QUERY_DATA_PIPELINE_STATISTICS` has eleven counters and every one has a Vulkan equivalent,
which is unusual enough to be worth saying. They are listed in declaration order in
`CreateQueryPool`.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 111 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved. The seven this adds are
  `vkCreateQueryPool`, `vkDestroyQueryPool`, `vkCmdResetQueryPool`, `vkCmdBeginQuery`,
  `vkCmdEndQuery`, `vkCmdWriteTimestamp2` and `vkCmdCopyQueryPoolResults`.
- The new code adds no compiler warning.
- **All 13 remaining `EE_UNIMPLEMENTED_FUNCTION` were read and attributed**: six P5.16, three
  P5.14, one P5.15, one open question 7, two markers.

#### Not verified

No query written, no timestamp read. The first thing worth checking is the frequency inversion,
because a wrong one gives plausible-looking timings that are wrong by a constant factor, which is
the sort of thing nobody notices.

**Upstream files edited: none.**

### 2026-08-29 - P5.12 Debug names and markers

**Eight of the nine `SetDebugName` overloads, `CmdBeginDebugMarker`, `CmdEndDebugMarker` and
`CmdWriteDebugMarker` are implemented.** 21 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 32.
`BeginFrameCapture` and `EndFrameCapture` were already done in P5.1, so the group is complete
apart from the one overload P5.11 has to unblock. Nothing has run.

Most of this group was already paid for. `SetVulkanObjectName` has existed since P5.2 and every
`Create*` call has been naming its objects through it, so the nine overloads are a handle and an
object type each.

#### The three that needed thought

**`SetDebugName( QueryPool* )` halts**, because there is no `VulkanQueryPool` type to cast to
until P5.11 defines one. It is the only one of the nine that does.

**`SetDebugName( CommandSignature* )` does nothing, and that is the finished answer.** P5.13's
command signature is a record of one command's byte layout and creates no Vulkan object, so there
is no handle for a name to reach. Its parameters are unnamed to say so.

**`CmdWriteDebugMarker` is `vkCmdFillBuffer`, and it loses the ordering.** Direct3D 12 uses
`WriteBufferImmediate`, whose `MARKER_IN` and `MARKER_OUT` modes mean "before everything already
submitted" and "after". Vulkan spells that `VK_AMD_buffer_marker`, which is not enabled here and
would be a device requirement the Phase 4 list does not have.

The concrete loss is in the `InOut` case: **Direct3D writes the In value at the top of the pipe
and the Out value at the bottom, so a crash between the two leaves the In value in the buffer,
which is the entire point of a breadcrumb.** Two fills run in order and the second overwrites the
first. `DeviceCapabilities::m_breadcrumbs` is `false` on this backend and nothing in the engine
calls the function, so nothing loses anything today. Turning breadcrumbs on means enabling the
extension first.

#### Markers

`VK_EXT_debug_utils` labels, which RenderDoc and every Vulkan profiler read the way PIX reads a
Direct3D event. The extension is enabled whenever the loader has it, with or without the
validation layer, so markers are present in a Release build too.

The golden-ratio HSV colour walk is copied from `RHI_Direct3D12.cpp:3628` verbatim, starting from
the same `0.5F`, so a marker gets the same colour on both backends. `RHI.h` holds no such helper
and this file may not add one to it.

**`EndCommandBuffer` now asserts the marker scope counter is zero**, the same check
`RHI_Direct3D12.cpp:2947` makes. Vulkan is stricter than Direct3D here: an unbalanced label is a
validation error rather than a cosmetic problem. The counter is kept even when the extension is
missing, so the assert still catches an unmatched scope on a machine without it.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 104 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved. The count is unchanged on
  purpose: the two label entry points are looked up through `vkGetInstanceProcAddr` like every
  other `VK_EXT_debug_utils` function, and `vkCmdFillBuffer` was already linked by P5.10.
- The new code adds no compiler warning.

#### Not verified

No object named, no marker recorded. The first thing to check is that names appear in a RenderDoc
capture, which is also the cheapest thing to check, and it makes every remaining group easier.

**Upstream files edited: none.**

### 2026-08-29 - P5.13 Indirect draws. The engine's command signatures do not fit Vulkan

**`CreateCommandSignature` and `DestroyCommandSignature` are implemented. `CmdExecuteIndirect` is
implemented for a signature that carries only a draw or dispatch argument, and refuses the rest at
the line.** **P5.13 is not a real group**, because no engine call site takes the path that works.
Nothing has run.

The `EE_UNIMPLEMENTED_FUNCTION` count is unchanged at 32, and that is the honest number: two whole
functions became real and `CmdExecuteIndirect` gained two named refusals in place of one blanket
one.

#### Why it does not fit, in one picture

A Direct3D 12 command signature can set root constants and bind root descriptors per command.
**Vulkan's indirect draws read draw arguments and nothing else.** `EngineShader.cpp:108` walks the
root signature's descriptor reflections and emits one argument per root parameter ahead of the draw
argument, so one material command is laid out like this:

```
[ root constants   40 bytes ]   set 0 binding b0, a uniform buffer on Vulkan
[ root CBV address  8 bytes ]   set 0 binding b1, a uniform buffer on Vulkan
[ dispatch args    12 bytes ]   VkDispatchIndirectCommand
```

`vkCmdDrawIndirect` takes a stride, so it reads the last block out of the fat struct without help.
It cannot rebind the first two per command, and `BucketResolve.esf:36` writes a different value
into them for each command in the buffer.

**A compute pre-pass does not cover it**, which answers the question Phase 4 left open when it set
`m_indirectRootConstant` to `false`. A pre-pass can repack the draw arguments, and repacking is not
needed because of the stride. A pre-pass cannot bind a descriptor.

**Every engine signature carries root data.** `MaterialShader`, `SurfaceShader` and `ComputeShader`
all build theirs the same way, and the two argument-writing shaders, `BucketResolve.esf` and
`InstanceCulling.esf`, both fill in a per-command root CBV address. Nothing reads the
`m_indirectRootConstant` capability, so the engine does not offer a narrower path.

#### The decision taken

**Land the mechanical half and refuse the rest, by decision.** The alternative shapes both need a
change on the shader side, which is Phase 4's and not this file's:

| Shape | What it costs |
|---|---|
| The shader reads its own command's root data by indexing the argument buffer with `SV_DrawIndex`, which Vulkan has as core `gl_DrawID`. One indirect call then covers the whole buffer. | No extension. Changes `RHI.esh` and the renderer shaders, so all 46 stages recompile and revalidate, and Windows sees the change too. |
| Root constants become Vulkan push constants and root descriptors become buffer device addresses, driven by `VK_EXT_device_generated_commands`. | That extension sets push constants per command and still cannot bind a descriptor set, so it needs the same shader change **plus** a device requirement the Phase 4 list does not have. |

Recorded as open question 7.

#### What is real

- **`CreateCommandSignature`** records the byte layout of one command: the argument type, the
  stride, the offset of the draw argument inside the command, and whether root arguments are
  present. The arithmetic accumulates the same byte sizes `RHI_Direct3D12.cpp:3746` does, because
  both backends read one buffer a shader wrote and have to agree on where every field is. There is
  no Vulkan object to create.
- **`CmdExecuteIndirect`** maps `Draw` and `DrawIndexed` onto `vkCmdDrawIndirect` and
  `vkCmdDrawIndexedIndirect`, or their `Count` forms when a counter buffer is passed, reading the
  draw argument at its offset with the signature's stride. `DispatchCompute` maps onto
  `vkCmdDispatchIndirect`.

#### Three things the working path still cannot do, each asserted at the line

| Case | Why |
|---|---|
| A `DispatchCompute` signature with a counter buffer, or `maxNumCommands` above 1 | `vkCmdDispatchIndirect` runs exactly one dispatch and reads no count buffer, where Direct3D 12 runs `min( maxNumCommands, count )`. A caller would silently get one dispatch, so both are refused. |
| A `DispatchMesh` signature | `vkCmdDrawMeshTasksIndirectEXT`, which cannot be named until P5.14 enables `VK_EXT_mesh_shader`. |
| A `DispatchRays` signature | `vkCmdTraceRaysIndirect2KHR`, which is P5.16's. |

#### A correction to what P5.10 recorded as owed here

That entry said `CmdExecuteIndirect` should call `PrepareTransfer` and then
`BeginRenderingIfPending`, which would end the render pass and immediately restart it. **An
indirect draw wants exactly what `PrepareDraw` gives an ordinary one**, and only an indirect
dispatch has to leave the pass. It now branches on the signature's argument type.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 104 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved.
- Every engine `CmdExecuteIndirect` and `CreateCommandSignature` call site was read, along with the
  two shaders that write the argument buffers and the `DrawArgument` and `DebugDrawMeshArgument`
  layouts. That is what established that no call site takes the working path.

#### Not verified

Nothing indirect has been drawn. There is no point checking the working path against the engine
until open question 7 is answered, because no engine call site reaches it.

**Upstream files edited: none.**

### 2026-08-29 - P5.3 Swapchain. A Vulkan queue does not run its submits in order

**`CreateSwapchain`, `DestroySwapchain`, `AcquireNextImage`, `SetVSync` and `QueuePresent` are
implemented.** 32 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 37. Nothing has run.

#### The correction this group forced on P5.2, which matters more than the swapchain

**A Direct3D 12 queue executes its command lists in the order they were submitted. A Vulkan queue
does not.** Two `vkQueueSubmit2` calls on one `VkQueue` may overlap unless something orders them.

The engine relies on the Direct3D guarantee. `ForwardShadingRenderer::SubmitGraphicsCommandBuffer`
submits several graphics command buffers a frame with the barriers recorded across them, and
**`RHI.h` gives it no way to ask for the ordering**: `QueueDeviceWait` asserts that the two queues
differ, so a queue cannot be made to wait on itself.

`RecordQueueOrderingWait` now makes every submit wait on the value the previous submit on that
queue signalled. That is the Direct3D semantics exactly. It costs the overlap a Vulkan driver
might otherwise have found, and the alternative is a race that no validation layer reports.

It is also what makes the swapchain sound, which is how it was found. See below.

#### `m_pNativeWindowHandle` is a `VkSurfaceKHR`, and the application owns it

This is the first of the two answers Phase 5 owes Phase 6.

Direct3D 12 takes an `HWND` and asks DXGI for a swapchain. Vulkan needs a `VkSurfaceKHR`, and
creating one needs a window system library. **`Base/Render` depends on no such library and must
not start to**, so the application creates the surface from the instance and hands it over.
`SDL_Vulkan_CreateSurface` returns exactly that.

Two consequences:

- **`CreateContext` now enables the surface instance extensions**, even though no window exists
  yet. A surface may only be created from an instance that enabled its platform extension, and
  the instance is created once. `VK_KHR_surface` plus xlib, xcb and wayland, whichever the loader
  reports. They are named by string rather than by macro, because the macros only exist once
  `VK_USE_PLATFORM_*` is defined and that drags X11 headers into a file with no other use for
  them.
- **`DestroySwapchain` never destroys the surface.** `Window::ResizeSwapchain` destroys and
  recreates around an unchanged `m_pNativeWindowHandle`.

#### The application drives swapchain recreation, not the RHI

The second answer. `Engine.cpp:754` and `ImguiRenderer.cpp:91` both compare the window size
against `GetSwapchainSize()` and call `Window::ResizeSwapchain`, and each waits the graphics queue
idle first. So `AcquireNextImage` and `QueuePresent` accept `VK_SUBOPTIMAL_KHR` and
`VK_ERROR_OUT_OF_DATE_KHR` instead of recreating behind the engine's back.

`VK_ERROR_OUT_OF_DATE_KHR` from the acquire is the one case with teeth: **no image is acquired and
the semaphore is not signalled**, so recording a wait on it would hang the queue. That path returns
the image index it already held.

#### A null handle means headless, which is all of Phase 5

There is no window until Phase 6, so a null `m_pNativeWindowHandle` builds a swapchain with no
`VkSurfaceKHR` and no `VkSwapchainKHR`: a ring of ordinary offscreen render targets,
`AcquireNextImage` cycles the index, and `QueuePresent` signals its timeline value and presents
nothing. That is the phase document's own bring-up order, and it is what lets ladder steps 6 and
7 run as soon as there is an entry point to run them from.

#### Binary semaphores, which P5.2 left to this group

`VkPresentInfoKHR` has no timeline path, so:

| Semaphore | Count | Why |
|---|---|---|
| Acquire | One per image, used as a ring | `vkAcquireNextImageKHR` is told which semaphore to signal *before* it says which image it gave, so it cannot be indexed by image. The engine host-waits on the previous frame's timeline value before reusing a slot, so a ring of `MaxPendingFrames` is safe. |
| Present | One per image | An image is not presented again until it has been acquired again. |

The acquire wait goes onto the present queue's `m_pendingWaits`, which the next submit drains.
That is only sound because of the queue ordering above: the submit that writes the swapchain image
is not the first one after the acquire.

`QueuePresent` submits and then presents, where Direct3D 12 presents and then signals. It has to:
`vkQueuePresentKHR` waits on a semaphore only a submit can signal. The returned value still means
"the frame is done", which is all the engine reads it for.

#### Two smaller mappings

- **The swapchain image is created sRGB.** Direct3D 12 creates it `UNorm` and puts an sRGB render
  target view on it, which in Vulkan would need `VK_KHR_swapchain_mutable_format`. Creating the
  image in `m_renderTargetFormat` gives the same conversion on write and the same picture, with no
  extension. `m_colorFormat` is the fallback if the surface refuses it.
- **`SetVSync` picks a present mode and a later call needs a recreation to take effect**, because
  Vulkan fixes the mode when the swapchain is created. FIFO with vsync; MAILBOX or else IMMEDIATE
  without it. Nothing in the engine calls it outside `CreateSwapchain`, so the two backends behave
  identically today.

#### The one thing that can stop Phase 6 dead

**`minImageCount` is a minimum, so a driver may return more swapchain images than were asked for.**
`Swapchain::m_renderTargets` is a fixed `TArray` of `MaxPendingFrames`, which is 2, and several
Linux drivers want three or four. `CreateSwapchain` logs the two numbers and halts. The fix is
`MaxPendingFrames` in `RHI.h`, which is an upstream file and a human decision.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 99 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved.
- Every engine call site of `AcquireNextImage`, `QueuePresent`, `CreateSwapchain` and
  `QueueSubmit` was read, along with the frame order in `Engine.cpp`. That is what found the
  queue ordering problem.

#### Not verified

Nothing has been acquired or presented. The order to check things in:

1. **The image count.** It halts on a driver that wants more than two, and it is the first thing
   that will happen on real hardware.
2. **The queue ordering wait.** If it is wrong, passes read each other's half-written targets.
   Sync validation is what names it.
3. **The acquire wait reaching the right submit.** A validation error names it.

**Upstream files edited: none.**

### 2026-08-29 - P5.10 Copies and clears. A clear needs a barrier the engine does not record

**`CmdCopyBuffer`, both `CmdCopyTexture` overloads, `CmdClearTexture` and `CmdClearBuffer` are
implemented.** 37 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 42. Nothing has run.

The five commands are short. Three things around them are not, and each is a place where Direct3D
12 needs nothing and Vulkan needs something.

#### A Direct3D clear is a shader write, and a Vulkan clear is a transfer write

`ClearUnorderedAccessViewUint` writes through an unordered access view, so
`Renderer_ForwardShading.cpp:753` follows its clears with a barrier whose source is
`ResourceAccess::UnorderedAccess`. That is a shader storage write. `vkCmdFillBuffer` is a transfer
write, which that barrier does not cover at all, so **the culling counters would be read stale and
no validation layer would say a word**.

So every clear records the transfer half of its own visibility barrier, batched like the rest and
flushed with them at the next dispatch. `RecordClearVisibilityBarrier` is the one place it lives.
Its destination is `ALL_COMMANDS` and it is listed in the table above as a site to narrow.

#### A copy needs a layout the engine never asks for

`D3D12_BARRIER_LAYOUT_COMMON` is already a legal copy source, copy destination and unordered
access view clear target, so **the engine issues no layout barrier before a texture upload**. It
issues a global memory barrier and nothing else: `RenderSystem.cpp:489` and `:759` are the two
sites. Vulkan needs `GENERAL` or one of the `TRANSFER` layouts, and the image is still in the
`UNDEFINED` that `vkCreateImage` gave it, which is P5.6's recorded obligation.

`TransitionTextureForTransfer` records that barrier, once per texture, into `GENERAL` and not
`TRANSFER_DST_OPTIMAL`. `GENERAL` is what `TextureState::Common` maps to, so the engine's belief
about this texture stays true and the next barrier it records still passes `CmdBarrier`'s assert.
Every texture the engine copies into is created `Common`; `RenderSystem.cpp:650` asserts it.

#### The staging row stride, which P5.6 left to this task

`vkCmdCopyBufferToImage` takes its row length in texels and the engine lays out its staging rows
at the byte stride `GetTextureCopyRowStride` reports. `CopyRowLengthInTexels` converts the one
into the other, and **both** copy overloads use it, so the readback direction reads rows at the
stride the upload direction writes them. Direct3D 12 uses the destination buffer's own footprint
for the readback, which for a buffer resource is the whole buffer as a single row and says nothing
about texture rows.

#### Two clear-value divergences, and nothing calls either path today

- `CmdClearTexture` has **no caller anywhere in the engine**. `vkCmdClearColorImage` converts the
  clear value to the image format and `ClearUnorderedAccessViewUint` writes the raw bits, so the
  two agree on an integer format and disagree on a normalised one.
- `CmdClearBuffer` fills 32-bit words. Direct3D fills components of the view's format. Every
  buffer the engine clears is a counter or a 32-bit typed buffer, so the two agree; a 16-bit
  format would not.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 91 `vk*` symbols resolve against `libvulkan.so.1`, none unresolved. The five this task adds -
  `vkCmdCopyBuffer`, `vkCmdCopyBufferToImage`, `vkCmdCopyImageToBuffer`, `vkCmdFillBuffer` and
  `vkCmdClearColorImage` - are all among them.
- Every engine `CmdClearBuffer`, `CmdCopyBuffer` and `CmdCopyTexture` call site was read, along
  with the barriers around it, which is what found the clear visibility hole.

#### Not verified

No copy issued, no buffer filled. The order to check things in:

1. **The clear visibility barrier.** If it is wrong the culling counters are garbage and the frame
   is empty or wildly wrong. Sync validation is the tool that names it.
2. **The staging row stride.** A wrong stride skews every uploaded texture, which looks like a
   decode bug rather than a copy bug.
3. That `TransitionTextureForTransfer` leaves `m_currentLayout` agreeing with the engine's
   tracker. `CmdBarrier`'s assert is what says otherwise.

**Upstream files edited: none.**

### 2026-08-29 - P5.9 Barriers. The render pass now opens at the draw, not at the bind

**All three `CmdBarrier` overloads are implemented**, on synchronization2. 42
`EE_UNIMPLEMENTED_FUNCTION` remain, down from 45. Nothing has run.

Barriers are batched on the command buffer and flushed in one `vkCmdPipelineBarrier2` at every
draw, every dispatch and `EndCommandBuffer`, which are the points `RHI_Direct3D12.cpp:1586`
flushes at. The mapping work is the small half of this task. The large half is below.

#### The render pass had to become lazy, and this is the reason

**The engine records image layout barriers between `CmdSetRenderTargets` and the first draw.** It
is not an accident or one pass being odd; it is the shape of every pass in the renderer:

```
resourceStates.Writeable( target, ... )      // the target becomes a render target
resourceStates.FlushBarriers( cb )           //   -> RHI::CmdBarrier
RHI::CmdSetRenderTargets( cb, target, ... )
rootConstants.SetSourceTexture( resourceStates, ... )   // a source becomes shader-readable
resourceStates.FlushBarriers( cb )           //   -> RHI::CmdBarrier, *after* the bind
RHI::CmdDraw( cb, 3, 0 )
```

`RenderPass_SMAA.cpp:154`, `:190` and `:218`, `RenderPass_GTAO.cpp:435` and `:462` all do it. **A
barrier may not run inside dynamic rendering**, so a `CmdSetRenderTargets` that called
`vkCmdBeginRendering` would put every one of those barriers inside a pass.

So `CmdSetRenderTargets` records the attachment configuration and does not begin. The first draw
flushes the barriers and then begins, through `PrepareDraw`. Direct3D 12 has the same shape for
its own reason: `OMSetRenderTargets` is state, and its batched barriers flush at the draw.

Three consequences worth knowing before touching that code:

| Case | What happens |
|---|---|
| A pass with a clear and no draw | `FlushRendering` begins and immediately ends it, so the clear still happens. `CmdSetRenderTargets` and `EndCommandBuffer` both call it. |
| A barrier or dispatch between two draws of one pass | `SuspendRendering` ends the pass and forces every load op to `LOAD`, so the next draw resumes it without losing what the first half drew. No engine pass does this today. |
| The attachment's `imageLayout` | Read from `VulkanTexture::m_currentLayout`, not hard-coded. `RenderPass_DebugDraw.cpp:1342` binds a depth target it only reads, which is `DEPTH_STENCIL_READ_ONLY_OPTIMAL` and not the attachment layout. |

#### A correction to P5.8: load and store actions were discarding the frame

**`LoadAction` is zero initialised, and zero is `DontCare` for both actions.** Direct3D 12 has no
load or store actions at all - binding a render target preserves it, and the backend reads
`m_loadActionsColor` only to decide whether to call `ClearRenderTargetView` - so every action the
engine leaves alone arrives in the backend as `DontCare`.

- **No engine pass sets a store action at all.** Mapping `StoreActionType::DontCare` to
  `VK_ATTACHMENT_STORE_OP_DONT_CARE` discards the output of every render pass in the frame.
- **`RenderPass_DebugDraw.cpp:1316` builds a `LoadAction` that sets only the depth action**, then
  binds the frame's final colour target with it at `:1358`. `VK_ATTACHMENT_LOAD_OP_DONT_CARE`
  there discards the whole rendered frame.

Both `DontCare` values now preserve, which is what the reference backend does. `Clear`, `Load`
and `StoreActionType::None` are unchanged, so a caller that means "discard" still has
`StoreActionType::None` to say it with. **This is a deliberate divergence from the phase
document's literal mapping** and it is written up under "Upstream issues observed" below.

#### Queue ownership: `CONCURRENT`, which P5.5 left to this task

Direct3D 12 resources have no queue ownership. A buffer written on the compute queue is read on
the graphics queue with only a barrier between, and the engine's async compute path relies on it.
Vulkan's `EXCLUSIVE` sharing needs an ownership transfer for the same thing, and **nothing in
`RHI.h` says which queue last touched a resource**, so there is nothing to build a transfer from.

`SetSharingMode` gives every buffer and image `CONCURRENT` across the distinct queue families the
context uses, and leaves `EXCLUSIVE` when there is only one family. It costs some compression on
some hardware. It is the mapping that reproduces the Direct3D semantics exactly, and the
alternative is silent corruption across queues.

#### The mappings, and the two entries that are deliberately broad

`PipelineStage` to `VkPipelineStageFlags2`, `ResourceAccess` to `VkAccessFlags2`, `TextureState`
to `VkImageLayout`. Four of them do not line up one for one:

| `RHI.h` | Vulkan |
|---|---|
| `PipelineStage::Draw` | `ALL_GRAPHICS`, because `D3D12_BARRIER_SYNC_DRAW` is every stage a draw runs through. It has to cover the depth test stages: the engine transitions a depth target with `Draw` and never with a depth stage of its own. |
| `PipelineStage::NonPixelShader` | Every shader stage except fragment, compute included, as Direct3D has it. The task and mesh stage bits belong here and are left out until P5.14 enables `VK_EXT_mesh_shader`, because naming a stage from a disabled extension is a validation error. |
| `PipelineStage::Copy` | `ALL_TRANSFER`, not `COPY`. Direct3D's `SYNC_COPY` sits next to `SYNC_CLEAR` and `SYNC_RESOLVE` and the RHI has no separate flag, so a clear arrives here as `Copy`. |
| `TextureState::ShaderResource` | `VulkanTexture::m_shaderReadLayout`, which P5.6 set. It is `GENERAL` when the texture is also an `RWTexture`, because that is the layout its heap descriptor was written with. |

`PipelineStage::All` and `ResourceAccess::Common` map to `ALL_COMMANDS` and
`MEMORY_READ|MEMORY_WRITE`. Both are in the `ALL_COMMANDS` table above with why, and both are the
meaning of the flag rather than laziness.

`PipelineStage::VideoProcess`, `ResourceAccess::VideoProcessRead` and `VideoProcessWrite` have no
Vulkan equivalent at all: Vulkan has no video processing queue. Nothing asks for them.

#### P5.6's three obligations are met

1. `CmdBarrier` takes `oldLayout` from `VulkanTexture::m_currentLayout`, never from the caller's
   `sourceState`, because a `VkImage` starts in `UNDEFINED` whatever `m_initialState` says. The
   caller's belief is asserted against the truth rather than used, so the two cannot drift.
2. `TextureState::ShaderResource` resolves through `m_shaderReadLayout`.
3. `UnorderedAccess` is `GENERAL`.

One layout is tracked per image, which is exact only while callers barrier the whole texture.
`DeviceResourceStates::FlushBarriers` passes an empty `TextureBarrierRegion`, so every engine
barrier does.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged.
- 87 `vk*` symbols resolve against `libvulkan.so.1`, up from 86. None unresolved.
- Every engine `CmdSetRenderTargets` call site was read to confirm the deferred begin is needed
  and sufficient, rather than assumed from one pass.

#### Not verified

No barrier issued, no layout moved. The order to check things in:

1. **The load and store action correction.** If it is wrong, the frame is blank or garbage, and
   that is the loudest failure here rather than the quietest.
2. That the deferred begin puts every barrier outside its pass. A validation error names the
   exact draw if not.
3. That `m_currentLayout` agrees with the engine's tracker after a frame. The assert in
   `CmdBarrier` is what says otherwise.

**Upstream files edited: none.**

### 2026-08-29 - P5.6 Textures and samplers. The format mapping is complete

**All seven functions are implemented, and the `DataFormat` to `VkFormat` mapping is finished.**
45 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 52. Nothing has run.

**All 99 `DataFormat` members map, and there is exactly one mapping.** The phase document warns
that two mappings which disagree corrupt textures in a way that looks like a bug somewhere else,
so `VulkanFormat` is the only one: image creation, buffer views, pipeline attachment formats and
the device capability query all come through it.

#### Two things in the format mapping that are easy to get backwards

**Vulkan names a packed format most significant component first, and DXGI names it least
significant first.** So `DXGI_FORMAT_B5G6R5_UNORM` is `VK_FORMAT_R5G6B5_UNORM_PACK16`, not
`VK_FORMAT_B5G6R5_UNORM_PACK16`. The same reversal applies to every packed entry:
`B5G5R5A1` becomes `A1R5G5B5`, `B4G4R4A4` becomes `A4R4G4B4`, `R10G10B10A2` becomes
`A2B10G10R10`, `R11G11B10` becomes `B10G11R11`, `R9G9B9E5` becomes `E5B9G9R9`. Getting one
backwards swaps red and blue on that format alone, which is exactly the kind of failure that
looks like a bug in the asset.

**`RGB565_UNorm` and `BGR565_UNorm` deliberately map to the same `VkFormat`.** Vulkan can tell
them apart and Direct3D cannot, so mapping them faithfully would make the two backends draw the
same asset differently. See the upstream note below. Nothing in the engine uses either.

Two places where Vulkan is the more exact of the two, and the mapping says so rather than
copying Direct3D:

| `DataFormat` | Direct3D 12 | Vulkan |
|---|---|---|
| `DXBC1_RGB_*` against `DXBC1_RGBA_*` | Both are `DXGI_FORMAT_BC1_UNORM` | `VK_FORMAT_BC1_RGB_*` and `VK_FORMAT_BC1_RGBA_*`, which is the distinction the `DataFormat` enum already makes |
| The 28 ASTC formats | `DXGI_FORMAT_UNKNOWN`, no ASTC in Direct3D | All present, gated on `textureCompressionASTC_LDR`, which nothing enables. `FillDeviceCapabilities` reports each one honestly |

`R1_UNorm` has no Vulkan equivalent at all and returns `VK_FORMAT_UNDEFINED` without asserting,
which mirrors what the Direct3D 12 backend does with the ASTC formats it cannot express.

#### Views, because Vulkan puts the subresource in the view

Direct3D 12 selects a subresource in the descriptor. Vulkan selects it in the `VkImageView`, so
`CreateTexture` builds every view the engine can ask for, up front:

| View | How many | Why |
|---|---|---|
| Sampled | one | Covers every mip and layer. A depth-stencil image must name one aspect, and depth is the one the engine reads, matching `RHI_Direct3D12.cpp:4597`. |
| Storage | one per mip level | An `RWTexture` handle names a mip. `RenderPass_GTAO.cpp:263` asks for five of them on one texture. |
| Attachment | one per mip level per array layer | `RenderPass_GlobalEnvironmentMap.cpp:304` renders to one face of a cube at one mip, and `RenderPass_CascadedShadow.cpp` to one of four array slices. |

**That closes the assert P5.8 left.** `CmdSetRenderTargets` now honours `colorArraySlices`,
`colorMipSlices`, `depthArraySlice` and `depthMipSlice`, and the render area is the mip's extent
rather than the whole texture's.

An attachment view takes every aspect the image has and the sampled view takes one, which is not
a detail either API makes obvious.

#### Three things P5.9 has to know, and none of them are guessable from `RHI.h`

These are the reason the "owed by later groups" table above now has a long P5.9 row.

1. **A `VkImage` is always created in `VK_IMAGE_LAYOUT_UNDEFINED`.** `vkCreateImage` accepts that
   or `PREINITIALIZED`, and the second is for linear tiling. Direct3D 12 takes the initial layout
   directly, so the engine believes a fresh texture is already in `m_initialState` and the image
   is not. `VulkanTexture::m_currentLayout` records the truth, and the first barrier has to read
   it rather than the state the caller passes.
2. **A texture that is both `Texture` and `RWTexture` sits in `VK_IMAGE_LAYOUT_GENERAL`**, not in
   `SHADER_READ_ONLY_OPTIMAL`. A storage image descriptor may name no other layout and one image
   cannot be in two layouts at once. `VulkanTexture::m_shaderReadLayout` carries the layout the
   descriptor was actually written with. `RenderPass_GTAO.cpp:111` creates such a texture.
3. **`GetTextureCopyRowStride` is P5.10's `bufferRowLength`.** Direct3D 12 reads its answer out of
   `GetCopyableFootprints`; Vulkan has no such call because the staging layout is the caller's to
   choose, so the row stride is `ComputeFormatRowStride` rounded up to the device's
   `optimalBufferCopyRowPitchAlignment`. The engine writes its rows at that stride and
   `vkCmdCopyBufferToImage` has to read them at it.

#### Samplers

Six samplers, created once in `RenderSystem::Initialize`, and two of them need something Vulkan
puts outside `VkSamplerCreateInfo`:

- **`FilterMode::Min` and `FilterMode::Max` are a reduction mode**, not a filter.
  `COMMON_SAMPLER_LINEAR_CLAMP_MAX` is one of the six, so `samplerFilterMinmax` is now a device
  requirement. It is core in Vulkan 1.2.
- **The border colour is one of six fixed values.** Direct3D 12 takes any colour;
  `VK_EXT_custom_border_color` is the only way to that and it is not enabled. Transparent black,
  opaque black and opaque white map; anything else halts and names the sampler. Every sampler the
  engine creates leaves the default of transparent black, and only a `ClampToBorder` address mode
  reads it at all.

#### One correction to P5.5

**`descriptorBindingStorageTexelBufferUpdateAfterBind` was missing.** Every descriptor type in
the heap's mutable list has to support update-after-bind, and the list holds a storage texel
buffer for `RWBuffer<T>`. The other five types were asked for and this one was not, so creating
the set layout would have failed validation on the first run. It is one line, next to its five
siblings, and it changes nothing else.

#### Verified, in the only sense available

- All eight Linux targets that are supposed to build still compile and link. `Checks.py` passes.
  `Applications/Editor` and `Applications/ResourceServer` still fail on the same five Phase 7
  errors, unchanged; confirmed by building them without this change.
- 86 `vk*` symbols resolve against `libvulkan.so.1`, up from 81. None unresolved.
- Every one of the 99 `DataFormat` members has a case in `VulkanFormat`, checked by script
  against the enum in `RHI.h` rather than by reading.

#### Not verified

No image created, no view bound, no texture sampled. The order to check things in when Phase 6
makes that possible:

1. **The packed formats.** Nothing in the engine uses one today, so a mistake there will surface
   the first time an asset does, long after this.
2. That a cube face at a given mip is the subresource the attachment view actually names. Six
   faces and nine mips give 54 views and only one right answer per draw.
3. That the sampled and storage descriptors of one texture agree with the layout P5.9 puts it in.

**Upstream files edited: none.**

### 2026-08-28 - P5.8 Render pass and draw commands. Both Phase 4 decisions land

**All thirteen functions are implemented.** 52 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 65.
Nothing has run.

**This is where both Phase 4 decisions become code**, and each is in exactly one place:

- **Clip-space Y is inverted in `CmdSetViewport`**, by moving the origin to the bottom of the
  rectangle and making the height negative. The shader compiler does not invert, and
  `-fvk-invert-y` must never be added. The comment at that line says so and names the other half
  of the decision, so that nobody adds a second flip.
- **Heap set 1 is bound in `CmdSetPipeline`**, not in `BeginCommandBuffer` where Direct3D 12 calls
  `SetDescriptorHeaps` (`:2917`). Vulkan cannot do it once per command buffer: binding a pipeline
  whose layout differs from set N onwards disturbs every set from N up, and set 0 varies per
  shader. The binding model accepted the redundant rebind deliberately.

#### Three places where Vulkan needs something Direct3D 12 does not

**Dynamic rendering has a begin and an end; `OMSetRenderTargets` has neither.** So
`VulkanCommandBuffer` carries an `m_isRendering` flag, `CmdSetRenderTargets` closes the previous
pass before opening the next, and `EndCommandBuffer` closes the last one. **Anything that may not
run inside a render pass has to close it too**: `CmdDispatchCompute` does, and P5.9's barriers and
P5.10's copies will have to. `EndRenderingIfActive` is the one call, and the requirement is
written into the "in flight" table above so it is not discovered by a validation error.

**Clears become load ops.** Direct3D 12 binds targets and then clears with a separate
`ClearRenderTargetView`; here `LoadActionType::Clear` is `VK_ATTACHMENT_LOAD_OP_CLEAR` on the
attachment. That is what the phase document's mapping asks for and what a tiling GPU needs.
`StoreActionType::None` maps to `VK_ATTACHMENT_STORE_OP_NONE`, core in 1.3.

**Dynamic rendering needs a render area and Direct3D 12 has none.** The full extent of the
attachments is used, which is the same thing, and the viewport still restricts what is drawn.

#### Root constants are a ring buffer, as the binding model said they would be

`RHI.esh` declares the block through `EE_DECLARE_ROOT_CONSTANTS` as a `ConstantBuffer`, so DXC
emits a uniform buffer and Vulkan push constants are not available without `[[vk::push_constant]]`
in `RHI.esh`, which Phase 4 rule 4 forbids. So `CmdSetRootConstants` copies into a ring and pushes
a descriptor at the copy.

**One ring per command buffer, 64 KB, reset in `BeginCommandBuffer`.** That is safe without any
frame tracking, because Vulkan already requires a command buffer's previous submission to have
completed before it can be re-recorded. **The ring asserts rather than wraps**: wrapping would
overwrite constants the GPU is still reading, and the failure would look like a shader reading
wrong values, which is a miserable thing to chase. 64 KB against a handful of `uint32`s per set is
generous by a wide margin.

`CmdSetRootParameter` is the same push with the caller's buffer and offset and `VK_WHOLE_SIZE` for
the range, which is what a Direct3D 12 root descriptor is: an address with no size. The binding
model says so explicitly.

Both use `m_shaderResources[index].m_registerIndex`, which P5.7 recorded as holding the **Vulkan
binding** rather than the HLSL register. That is why no shift is applied here.

#### A file reorganisation, because the declaration order forced it

`RHI.h` declares the draw commands before the buffers, textures and pipelines they act on, so a
`VulkanBuffer` defined next to `CreateBuffer` comes too late for `CmdSetRootConstants`. Every
`VulkanXxx` type now sits in one "Resource types" block near the top. **The functions keep
`RHI.h`'s section order**, which is what Conventions rule and the phase document ask for; only the
type definitions moved.

#### Verified, in the only sense available

- All eight Linux targets compile and link. `ninja` exits 0, `Checks.py` passes.
- 81 `vk*` symbols resolve against `libvulkan.so.1`, up from 70.
- Every one of the thirteen functions plus `CmdSetShadingRate` is present exactly once. That was
  checked by name, because the splice that replaced the stubs spanned `CmdSetShadingRate`, which
  belongs to P5.15 and had to be put back.

#### Not verified

No viewport set, no pass begun, no descriptor pushed. The two decisions this group implements are
the ones Phase 4 spent the most effort on, and neither has run. The order to check them in:

1. **The Y flip and the front face together.** Getting both wrong looks correct. Render something
   with a known handedness before trusting either.
2. That a push descriptor lands on set 0 while set 1 stays bound across a pipeline change.
3. That the root constant ring holds a value long enough for the GPU to read it.

**Upstream files edited: none.**

### 2026-08-28 - P5.7 Shaders, root signatures and pipelines. Written, never run

**Graphics and compute pipelines are implemented, and SPIRV-Reflect finally has a caller.** 65
`EE_UNIMPLEMENTED_FUNCTION` remain, down from 74. Four of the 65 are markers rather than
unimplemented RHI functions: the `VulkanFormat` default, the static-sampler path, and the mesh and
raytracing `CreatePipeline` overloads. Nothing has run.

**This closes the set of five groups `RenderSystem::Initialize` needs.** P5.1, P5.2, P5.4, P5.5
and P5.7 are all written. Whether they work is a different question that nothing can answer before
Phase 6.

#### Reflection

`ExtractReflection` mirrors `RHI_Direct3D12.cpp:1003` and produces the same `ShaderReflection` the
engine already reads. The interesting mappings:

| Direct3D 12 | Vulkan |
|---|---|
| `D3D12_SHADER_INPUT_BIND_DESC::Type` | `SpvReflectDescriptorBinding::descriptor_type` |
| `D3D_SIT_STRUCTURED` against `D3D_SIT_UAV_RWSTRUCTURED` | `resource_type & SPV_REFLECT_RESOURCE_FLAG_UAV`. A SPIR-V storage buffer is both read and read-write, so the resource flag is the discriminator rather than the type. |
| `GetThreadGroupSize` | `entry_points[0].local_size` |
| `ID3D12ShaderReflectionConstantBuffer` members | `SpvReflectDescriptorBinding::block.members` |

**`m_registerIndex` holds the Vulkan binding, not the HLSL register.** The binding model shifts
`b`/`t`/`u`/`s` to 0/8/16/24, and un-shifting in reflection only to re-shift in
`CreateRootSignature` is two chances to get it wrong instead of none. Nothing outside the backend
reads it: `EngineShader.cpp` reads only `m_descriptorTypeFlags` and `m_numConstants` from a
`DescriptorReflection`, and uses position in the vector as the parameter index. This is written
down because the field means something different on the two backends, which is exactly the sort of
thing a later reader assumes rather than checks.

**Set 1 bindings are skipped during reflection.** They are the bindless heap, whose layout is
fixed and shared, so they are not root parameters and must not become them.

#### Root signatures

`RootSignature` becomes a `VkPipelineLayout` over two set layouts: set 0, built from reflection
with `VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR`, and set 1, the shared heap layout
from `CreateContext`. The merge of per-stage resources, first seen wins, is copied from
`RHI_Direct3D12.cpp:4946` including its cross-stage asserts, because the order decides
`m_parameterIndex` and both `CmdSetRootParameter` and `EngineShader.cpp` index by position.

**Root constants are a uniform buffer, not Vulkan push constants.** `RHI.esh` declares the block
through `EE_DECLARE_ROOT_CONSTANTS` as a `ConstantBuffer`, so DXC emits a uniform buffer, and
making it a push constant block needs `[[vk::push_constant]]` in `RHI.esh`, which Phase 4 rule 4
forbids. The binding model already recorded the answer: `CmdSetRootConstants` copies into a
per-frame upload ring and pushes a descriptor at it. P5.8 writes that.

**The static sampler path halts rather than warns.** The binding model found no shader using one,
and `CreateRootSignature` keeps the Direct3D 12 warning for a name that matches nothing. If a name
*does* match, that is a new shader using a static sampler and the halt says so at the point it
happens.

#### Pipelines

Graphics uses dynamic rendering, so there is no `VkRenderPass` and no framebuffer:
`VkPipelineRenderingCreateInfo` carries the formats instead. Direct3D 12's single `DSVFormat`
splits into `depthAttachmentFormat` and `stencilAttachmentFormat`.

Viewport, scissor and stencil reference are dynamic state, because `CmdSetViewport`,
`CmdSetScissor` and `CmdSetStencilReference` exist.

**No vertex input state, and that is correct.** `GraphicsPipelineParameters` carries no input
layout at all: the engine pulls vertices out of buffers in the shader. An empty
`VkPipelineVertexInputStateCreateInfo` says exactly that.

**The winding line is the one most likely to be wrong.** The Direct3D 12 backend sets
`FrontCounterClockwise = ( m_frontFace == ClockWise )`, already an inversion of the name. The
Vulkan viewport inverts Y with a negative height, which P5.8 applies, and that reverses winding in
framebuffer space. Inverting the inversion lands back on the name, so `ClockWise` maps to
`VK_FRONT_FACE_CLOCKWISE`. **This is reasoning, not a measurement.** If faces come out inside
out, this line and the sign of the viewport height in `CmdSetViewport` are the only two places
that can be responsible, and getting both wrong looks correct.

Three smaller mismatches, all recorded at the line rather than silently absorbed:

| `RHI.h` | Vulkan |
|---|---|
| `m_depthClip` | `depthClampEnable = !m_depthClip`. Clipping discards the primitive and clamping keeps it at the plane, so the two are not identical. `VK_EXT_depth_clip_enable` is the exact control and is not enabled. Nothing sets `m_depthClip` today. |
| `m_sampleQuality` | No equivalent. It is a Direct3D quality level for a sample count and Vulkan exposes only the count. |
| `m_independentBlend` | No switch. Vulkan is per attachment when the feature is supported and uniform otherwise, and the engine fills every attachment either way. |

A render target outside `m_renderTargetMask` gets a fully default attachment: blending off, no
colour written. That is what Direct3D 12 leaves behind for an unmasked target, and defaulting to a
full write mask instead would silently change what is drawn.

**The pipeline cache is implemented, which the reference is not.** `RHI_Direct3D12.cpp:5232`
asserts `pPipelineCache == nullptr` with "Not implemented yet". `VkPipelineCache` is a handful of
lines, so it is real here; `GetPipelineCacheData` keeps the bytes on the cache object because it
hands back a view.

#### Verified, in the only sense available

- All eight Linux targets compile and link. `ninja` exits 0, `Checks.py` passes.
- **SPIRV-Reflect is genuinely linked**: 45 `spvReflect*` symbols are pulled out of
  `libspirv-reflect.a` and into `libEsoterica.Base.so`. Until this group it was an archive nothing
  referenced.
- 70 `vk*` symbols resolve against `libvulkan.so.1`, up from 60.
- `VulkanFormat` gained the ten render target and depth formats the engine uses, measured from
  every `DataFormat` a texture or pipeline is created with in `Code/Engine`.

#### Not verified

No shader module has been created, no SPIR-V reflected, no pipeline compiled. The specific things
to check first, in order:

1. That `ExtractReflection` produces the same resource list Direct3D 12 does for the same shader.
   A difference here silently changes the root parameter order.
2. The winding, above.
3. That a push descriptor set layout with the reflected bindings is accepted alongside the
   update-after-bind heap set in one pipeline layout.

**Upstream files edited: none.**

### 2026-08-28 - P5.5 Buffers, and the bindless heap becomes code. Written, never run

**All seven functions are implemented, and `CreateContext` now builds the bindless descriptor
heap.** 74 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 80. One of those 74 is not an RHI
function: it is the default case of `VulkanFormat`, which P5.6 fills in. Nothing has run.

This is the group the phase document calls the point where the Phase 4 binding model stops being
a document, so this entry is long.

#### A correction to the binding model, on one flag

The binding model entry says both heap bindings take `PARTIALLY_BOUND` and
`VARIABLE_DESCRIPTOR_COUNT`. **Vulkan does not allow that.** Only the highest-numbered binding in
a set may carry `VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT`
(`VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-pBindingFlags-03004`), so binding 0 cannot have
it while binding 1 exists.

**The flag is also not needed.** It exists to let a set be allocated smaller than its layout
declares, and both heaps are allocated at their full declared size: 65472 and 2048. So it is
dropped from both bindings rather than kept on one.

`PARTIALLY_BOUND` and `UPDATE_AFTER_BIND` stay, and the layout keeps
`UPDATE_AFTER_BIND_POOL`. **Nothing a shader can observe changes**: the set, the bindings, the
descriptor types, the counts and the mutable type list are all exactly as recorded. The flag never
reaches the SPIR-V. This is written up here rather than escalated as a re-decision because it
corrects a detail of how the recorded model is built, not the model itself. **Say so if you
disagree** - the entry it corrects is the hard prerequisite for the whole phase.

#### What the heap looks like in code

Built in `CreateContext`, once, and torn down in `DestroyContext`:

| Binding | Type | Count | Flags |
|---|---|---|---|
| 0 | `VK_DESCRIPTOR_TYPE_MUTABLE_EXT` over six types | 65472 | `PARTIALLY_BOUND`, `UPDATE_AFTER_BIND` |
| 1 | `VK_DESCRIPTOR_TYPE_SAMPLER` | 2048 | `PARTIALLY_BOUND`, `UPDATE_AFTER_BIND` |

The counts are Direct3D 12's, from `RHI_Direct3D12.cpp:2229` and `:2236`. Keeping them identical
is what makes a handle mean the same thing on both backends.

**Index allocation reuses `HandleAllocator<GenericResourceHandle>`**, the platform-neutral
allocator in `Code/Base/Render/HandleAllocator.h` that the Direct3D 12 backend already uses for
its own descriptor heaps. So a handle is allocated the same way on both sides, and nothing new was
written to do it.

**Writing a mutable descriptor uses the actual type, never `VK_DESCRIPTOR_TYPE_MUTABLE_EXT`.**
That constant only ever appears in the layout and the pool size. `WriteResourceHeapSlot` is the
one place a heap slot is written.

#### Buffers

`GetBufferHandle` lays out its descriptors in the same order Direct3D 12 does - constant buffer
first if present, then the read view, then the read-write view - because the handle arithmetic has
to agree.

Vulkan wants buffer usage up front where Direct3D 12 derives it from the view, so the usage flags
come from the requested descriptor types. The awkward part is that a *typed* buffer is a different
descriptor type from a structured one:

| HLSL | `m_format` | Vulkan descriptor | Needs |
|---|---|---|---|
| `ConstantBuffer<T>` | undefined | `UNIFORM_BUFFER` | range |
| `StructuredBuffer<T>`, `ByteAddressBuffer` | undefined | `STORAGE_BUFFER` | range |
| `RWStructuredBuffer<T>` | undefined | `STORAGE_BUFFER` | range |
| `Buffer<T>` | set | `UNIFORM_TEXEL_BUFFER` | a `VkBufferView` |
| `RWBuffer<T>` | set | `STORAGE_TEXEL_BUFFER` | a `VkBufferView` |

**A `VkBufferView` needs a `VkFormat`, which is P5.6's mapping.** Rather than write a second
mapping, `VulkanFormat` is created here with the entries buffers actually need and asserts on
everything else. **P5.6 completes this function; it must not write another one.** That is exactly
the failure the phase document warns about, where two mappings disagree and textures corrupt in a
way that looks like a bug somewhere else.

Which entries were needed was **measured, not guessed**: every `BufferParameters::m_format`
assignment in `Code/Engine` uses one of `R32_UInt`, `RG32_UInt` or `R32_SFloat`, across
`SpatialHash.cpp`, `DeviceRenderView.cpp`, `Renderer_ForwardShading.cpp`, `DeviceAppendBuffer.cpp`,
`DeviceRenderWorld.cpp` and `ImguiRenderer.cpp`. The signed and float siblings of those three are
filled in too, because they cost a line each.

**`BufferParameters::m_pCounterBuffer` has nowhere to go, and needs nowhere.** Direct3D 12 hands a
counter resource to `CreateUnorderedAccessView`; Vulkan has no equivalent. `CreateBuffer` asserts
it is null. The binding model entry reaches the same conclusion from the shader side: no `.esh` or
`.esf` in the repository uses `IncrementCounter`, `DecrementCounter`, `AppendStructuredBuffer` or
`ConsumeStructuredBuffer`, and `AppendBuffer.esh` carries its own explicit `RWBuffer<uint>` counter
and does its own `InterlockedAdd`.

**Suballocation is a `VmaVirtualBlock`**, the direct equivalent of the `D3D12MA` virtual block the
reference uses. `BufferSubAllocation::m_internal` holds the `VmaVirtualAllocation`, with a
`static_assert` on the size, the same guard the Direct3D 12 side has.

**Freed heap slots are not cleared.** `PARTIALLY_BOUND` means a stale descriptor only matters if a
shader reads it, and a shader reading a freed handle is a bug either way. Direct3D 12 frees its
descriptors the same way.

**`VK_SHARING_MODE_EXCLUSIVE`, not `CONCURRENT`.** Direct3D 12 buffers have no queue ownership at
all, and `CONCURRENT` would reproduce that at a cost on some hardware. P5.9 owns barriers and has
to get the ownership transfers right regardless, so the stricter mode is the one to start from.

#### Verified, in the only sense available

- All eight Linux targets compile and link. `ninja` exits 0, `Checks.py` passes.
- 60 `vk*` symbols now resolve against `libvulkan.so.1`, up from 51.
- The three buffer formats were read out of the engine's own call sites.
- Every Vulkan structure and flag was read out of `/usr/include/vulkan/vulkan_core.h`.

#### Not verified, and this is the group where that matters most

No descriptor set has been created, no descriptor written, no handle handed to a shader. The heap
is the single highest-risk thing in Phase 5 - the phase document calls bringing it up "the highest
risk step" and says not to go past it on an unverified assumption. **Every claim in this entry is
an unverified assumption.** The specific things to check first, with validation layers on:

1. That a mutable descriptor binding of 65472 with `UPDATE_AFTER_BIND` allocates at all.
2. That writing a `UNIFORM_TEXEL_BUFFER` into a mutable slot is accepted.
3. That the heap index a shader sees equals the handle `GetBufferHandle` returned.

**Upstream files edited: none.**

### 2026-08-28 - P5.4 Command pools and buffers. Written, never run

**All seven functions are implemented.** 80 `EE_UNIMPLEMENTED_FUNCTION` remain, down from 87.
Nothing has run.

The group is nearly a straight translation, so this entry is short. Four things are worth keeping.

**The pool takes `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`.** Direct3D 12 resets an
individual command list against its allocator, in `ID3D12GraphicsCommandList::Reset`, and the
engine calls `BeginCommandBuffer` per buffer rather than resetting the pool each time. Without the
bit, a second `vkBeginCommandBuffer` with no intervening `vkResetCommandPool` is invalid. Some
drivers give such a pool per-buffer allocators, which costs a little; correctness first, and the
flag is the direct equivalent of what the reference does.

**`ResetCommandPool` passes no flags**, so `VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT` is off.
`ID3D12CommandAllocator::Reset` keeps its memory for reuse, and this runs once per frame per pool,
so handing memory back to the driver every frame is the opposite of what the caller wants.

**`BeginCommandBuffer` does not set `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT`.** It would be
faster, and it is an assumption rather than a fact: Direct3D 12 allows a closed command list to be
submitted more than once without re-recording, and nothing here proves the engine never does. The
flag is a validation error the moment it is wrong. Set it once someone has checked, not before.

**`BeginCommandBuffer` does not bind any descriptor set**, and Direct3D 12's does the equivalent.
`RHI_Direct3D12.cpp:2917` calls `SetDescriptorHeaps` once per command buffer. The Phase 4 binding
model puts the matching `vkCmdBindDescriptorSets` in `CmdSetPipeline` instead, because set 1 is
disturbed whenever a pipeline with a different set 0 layout is bound. P5.8 writes it there. The
comment at the empty spot says so, so that the absence reads as a decision rather than an
oversight.

Two smaller notes. Vulkan command buffers start in the initial state, so there is nothing matching
Direct3D 12's trick of creating a list already recording and closing it immediately; the stage is
just set to `Closed`. And `VulkanCommandBuffer` gains the `Stage` enum its Direct3D 12 sibling
has: the validation layers track the same lifecycle, but an `EE_ASSERT` names the caller that got
it wrong instead of a layer message three frames later.

**Verified, in the only sense available:** all eight Linux targets compile and link, `ninja` exits
0, and 51 `vk*` symbols now resolve against `libvulkan.so.1`, up from 44.

**Not verified:** that a pool is ever created or a command buffer ever recorded.

**Upstream files edited: none.**

### 2026-08-28 - P5.2 Queues and synchronization. Written, never run

**Eight of the nine P5.2 functions are implemented. `QueuePresent` is not, and cannot be.** 87
`EE_UNIMPLEMENTED_FUNCTION` remain, down from 95. Nothing has run.

**The monotonic counter is a timeline semaphore, and the mapping is nearly free.** One
`VK_SEMAPHORE_TYPE_TIMELINE` semaphore per queue. `QueueGetCompletedSemaphore` is
`vkGetSemaphoreCounterValue`, `QueueHostWait` is `vkWaitSemaphores`, `WaitQueueIdle` is
`vkQueueWaitIdle`. The phase document calls this the one thing to get right first; it is right
because `RHI.h` was already written against a counter rather than a fence object.

**The counter starts at 1 and means "the next value to be signalled".** `QueueGetCurrentSemaphore`
therefore returns a value that has *not* happened yet. That looks wrong and is not: it is exactly
what `Direct3D12Queue::m_fenceValue` does, initialised to 1 at `RHI_Direct3D12.cpp:2554`, and the
engine is written against that meaning. The timeline's initial value is 0, which is why both
backends skip a wait on 0.

**`QueueDeviceWait` is the one real mismatch.** `ID3D12CommandQueue::Wait` is a standalone queue
operation that blocks everything submitted after it. Vulkan has no such thing: a wait only exists
attached to a submit. So the wait is recorded on the queue and the next `QueueSubmit` drains it.

The obvious alternative is wrong and worth writing down. Submitting an empty `vkQueueSubmit2`
carrying only the wait does **not** reproduce the semantics, because submissions on one queue may
overlap: a wait in submit N does not hold back submit N+1. The pending-wait list does.

A wait with no submit after it is dropped. That is a behaviour difference from Direct3D 12 with no
observable consequence, since a queue wait that nothing follows cannot be observed.

**P5.1 had to be amended, and the reason is a deadlock.** `CreateContext` previously asked for one
`VkQueue` per unique family. On a device with no dedicated async compute or transfer family, all
three RHI queues then share one `VkQueue`, and a `QueueDeviceWait` between two of them waits for a
timeline value that only a later submit on that same `VkQueue` can signal. That is a hang, not a
slowdown. `CreateContext` now asks for as many queues per family as the engine will take, clamped
to `VkQueueFamilyProperties::queueCount`, and `CreateQueue` hands out distinct queue indices.
Where the family really does expose one queue, the queues still share it, which is correct but
serialised.

**`QueuePresent` belongs to P5.3, not to P5.2.** `VkPresentInfoKHR` accepts binary semaphores
only; there is no timeline path. Presenting needs the swapchain to carry a binary semaphore per
image, the submit before the present to signal it alongside the timeline value, and the present
to wait on it. None of that can be written before `VulkanSwapchain` exists. The stub says so, at
the stub.

**Two things `QueueParameters` asks for that Vulkan will not give:**

| Parameter | Status |
|---|---|
| `QueuePriority` | **Not honoured.** Vulkan fixes queue priorities at `vkCreateDevice`, and `CreateQueue` runs long after. Honouring it would mean recreating the device. Nothing in the engine sets it: `RenderSystem::Initialize` leaves all three queues on `Normal`. `GlobalRealtime` would additionally need `VK_EXT_global_priority`, also at device creation. |
| `QueueFlags::DisableTimeout` | **No equivalent.** `D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT` has no Vulkan counterpart. Nothing sets it either. |

Both are recorded rather than silently ignored, so that the first caller to set one finds an
explanation instead of a mystery.

**Two structures were written here that other groups own.**

- `VulkanCommandBuffer`, with the single `VkCommandBuffer` member `QueueSubmit` reads. P5.4 owns
  command buffers and extends it. The type has to exist for `QueueSubmit` to compile at all.
- `SetVulkanObjectName`, the one `vkSetDebugUtilsObjectNameEXT` call underneath all nine
  `SetDebugName` overloads. P5.12 owns those and builds them on this. It is written now because
  `CreateQueue` names its queue, and because the phase document says to do debug utils early: a
  named object makes every later group easier to debug.

`Queue::m_unifiedMemory` comes from a new `VulkanContext::m_isUnifiedMemory`, computed in
`FillDeviceCapabilities` as "no memory type is device-local without also being host-visible".
Direct3D 12 reads the same flag from D3D12MA's `IsUMA()`.

**Verified, in the only sense available:**

- All eight Linux targets compile and link. `ninja` exits 0.
- 44 `vk*` symbols are now undefined in `libEsoterica.Base.so` and resolve against
  `libvulkan.so.1`, up from 37.
- `Checks.py` passes.
- Every Vulkan structure, enum and entry point was read out of
  `/usr/include/vulkan/vulkan_core.h` before use.

**Not verified:** that a queue is ever created, that a timeline semaphore ever signals, or that
the pending-wait scheme behaves as reasoned. The `QueueDeviceWait` argument above is the piece
most worth re-checking with validation layers on, because it is reasoning about Vulkan's execution
model rather than a mechanical translation.

**Upstream files edited: none.**

### 2026-08-28 - P5.1 Device, context and memory. Written, never run

**P5.1 is implemented and has never executed a single instruction.** `CreateContext`,
`DestroyContext`, `GetTotalAllocatedDeviceMemory`, `GetDetailedMemoryStatistics`,
`GetResourceAllocationStatistics` and `ReportDeviceMemoryLeaks` are real, and so are
`BeginFrameCapture` and `EndFrameCapture`, which are P5.12's but are four lines each once
`CreateContext` has the RenderDoc API pointer. 95 `EE_UNIMPLEMENTED_FUNCTION` remain, down from
103.

Read every claim below as "the compiler and linker agree", never as "this works".

**What `CreateContext` does.** Instance with `VK_LAYER_KHRONOS_validation` and
`VK_EXT_debug_utils` when validation is asked for; physical device selection; device with the
binding model's features; VMA. Device selection honours `DeviceSelectionPreference`:
`UseProvidedIndex` takes the index and refuses it if it does not qualify, and the other two score
discrete against integrated in opposite directions, which mirrors what `DXGI_GPU_PREFERENCE` asks
the factory for rather than inventing a different notion of "best".

**The device is refused rather than worked around.** `GetDeviceRejectionReason` checks Vulkan 1.3,
the three required extensions, and 20 feature bits, and returns the **name of the first missing
one** so the log says what is wrong instead of "no suitable device". The list is the binding
model's, not a guess: `mutableDescriptorType`, `VK_KHR_push_descriptor`, `scalarBlockLayout`, the
descriptor indexing bits, the four `*UpdateAfterBind` bits and the four `*ArrayNonUniformIndexing`
bits, plus `timelineSemaphore`, `bufferDeviceAddress`, `drawIndirectCount`, `dynamicRendering` and
`synchronization2`. There is no fallback path, because the shaders are compiled for this model.

`VK_KHR_swapchain` is enabled here even though P5.3 owns the swapchain. A device is created once,
and adding the extension later would mean recreating it.

**Two symbols moved here that had no home on Linux.** `RHI_Direct3D12.cpp` defines both, and
exactly one backend compiles per platform:

- `Memory::Allocators::g_RHI`, named by seven `TVector` and `THashMap` members in `RHI.h`.
- `GenericResource::~GenericResource`, which `Context` derives from.

Neither was referenced before, because no Linux code instantiated the types that need them. The
link error for the second one is worth remembering: `undefined reference: vtable for
EE::Render::RHI::GenericResource`, which names the base class and not the derived one that caused
it.

**A validation error halts.** The debug messenger logs through `EE_LOG_FATAL_ERROR`, which
carries `EE_HALT()`. Phase 5 says to treat any validation error as a build break; this is what
makes that true rather than aspirational. The callback initialises the thread heap first, because
the layers call it from whichever thread tripped the check, exactly as the Direct3D 12 info queue
callback does.

**RenderDoc uses `RTLD_NOLOAD`.** `dlopen( "librenderdoc.so", RTLD_NOW | RTLD_NOLOAD )` attaches
to a RenderDoc that already injected itself and never loads one that is not there, which is what
`GetModuleHandleA` does on Windows. It still takes a reference, so `DestroyContext` calls
`dlclose`. The Vulkan device pointer is `RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE`, the dispatch
table at the start of the instance, not the instance handle.

**`ReportDeviceMemoryLeaks` had to be inverted.** Direct3D 12 asks DXGI for live objects, and
Vulkan has no global registry. It is also called from `BaseModule::ShutdownModule`, *after* the
context is destroyed, so the allocator that knows the answer is already gone. `DestroyContext`
therefore runs `vmaCalculateStatistics` while the allocator still exists and records any surviving
allocations in two file statics, which `ReportDeviceMemoryLeaks` then reports. Leaked Vulkan
*handles*, as opposed to memory, are the validation layers' job.

**What is deliberately not filled in, and why.**

| Left alone | Reason |
|---|---|
| `m_canShaderReadFrom`, `m_canShaderWriteTo`, `m_canRenderTargetWriteTo` | All three need the complete `DataFormat` to `VkFormat` mapping, which is P5.6's largest piece. Two partial mappings that disagree is the failure the phase document says corrupts textures in a way that looks like a bug somewhere else. |
| `m_shadingRate`, `m_shadingRateCaps` | `NotSupported`, matching the Direct3D 12 backend, which sets the same with a TODO. P5.15 owns them, and reporting a capability the backend cannot honour makes the engine issue calls that halt. |
| `m_indirectRootConstant` | `false`. Direct3D 12 command signatures set root constants per draw and Vulkan's indirect draws cannot. P5.13 decides whether a compute pre-pass covers it. |
| `m_breadcrumbs` | `false`. DRED's equivalents are `VK_AMD_buffer_marker` and `VK_NV_device_diagnostic_checkpoints`, neither wired up. |
| `m_rasterizerOrderViews` | `false`. `VK_EXT_fragment_shader_interlock` is the equivalent and nothing enables it. |
| `m_hdr` | `false`. It needs a swapchain colour space, which is P5.3. |
| `m_numRaytracingCores` | `0`. No Vulkan query exposes it, and nothing in the engine reads it. |

`m_optimalRootSignatureSizeInDWORDs` is copied from Direct3D 12 as 13 rather than derived. It is
an AMD packet-size heuristic with no Vulkan meaning, and it feeds the engine's own root signature
sizing, which has to agree across both backends.

**Queue families are chosen here, which looks like P5.2's work.** `vkCreateDevice` takes the queue
create infos, so it cannot be deferred. A device with no dedicated async compute or transfer
family falls back to the graphics family, which is correct rather than degraded: every
graphics-capable family also supports compute and transfer.

**`DeviceMode` is always `Single`.** Vulkan has no equivalent of a Direct3D 12 linked-node
adapter; multi-GPU is explicit device groups, which the engine never asks for. `m_numLinkedNodes`
is 1.

**Verified, in the only sense available:**

- All eight Linux targets compile and link: the five `.so` files, `Reflector`,
  `ResourceCompiler`, `Tester`. `ninja` exits 0 and a re-run reports "no work to do".
- `nm -DC libEsoterica.Base.so` shows `EE::Render::RHI::CreateContext` as a defined exported
  symbol and `EE::Memory::Allocators::g_RHI` in BSS. 37 `vk*` symbols are undefined and resolve
  against `libvulkan.so.1`.
- `Checks.py` passes.
- Every Vulkan extension macro, feature bit and struct name was read out of
  `/usr/include/vulkan/vulkan_core.h` before use, not recalled.

**Not verified, and not verifiable here:** that any of it produces a working device. No
instance has been created, no device enumerated, no capability read. The two GPUs in this machine
were surveyed during Phase 4 and both expose everything the binding model needs, so the
requirement list is not speculative, but the code that reads it has never executed.

**One `#pragma clang diagnostic` is used**, around the `vk_mem_alloc.h` include, to silence about
200 `-Wnullability-completeness` warnings. It is a new Linux-only file, so no upstream file is
touched, and the suppression is scoped to that include.

**Upstream files edited: none.** `RHI_Vulkan.cpp` is a file this fork added; it is listed in
`LinuxSources.txt` and does not appear in [TouchedFiles.md](TouchedFiles.md), which is correct.

### 2026-08-28 - P5.0 The Vulkan dependencies, and open question 3

Phase 5's dependency plumbing. **No RHI function is implemented.** `RHI_Vulkan.cpp` is untouched,
so all 16 groups are still Phase 1 stubs.

Three dependencies arrive, and one long-standing generator problem goes away:

| Dependency | Pin | Layout | Built? |
|---|---|---|---|
| VMA | `v3.4.0` | `External/VMA/include/vk_mem_alloc.h` | no, header only |
| SPIRV-Reflect | `vulkan-sdk-1.4.357.0` | `External/SPIRV-Reflect/`, plus `lib/libspirv-reflect.a` | one C file, `cc` and `ar` |
| RenderDoc header | `v1.45` | `External/RenderDoc/renderdoc_app.h` | no, one header |

**The Vulkan loader and headers are not fetched.** `libvulkan-dev` supplies both. The loader
carries an ICD layer that has to match the installed drivers, so a source build of it would fight
the distribution rather than help. `pkg-config vulkan` gives the include path and `-lvulkan`, the
same way `FreeType.props` already works.

**`External/RenderDoc` did not exist, and NinjaGen had reported that as a problem since Phase 0.**
`RenderDoc.props` maps to that include directory, `Esoterica.Base` imports the sheet, and nothing
had ever put the header there. `fetch_renderdoc` fixes it, so the generator now reports 17
problems instead of 18, and every one that remains is "no sources in <configuration>".

**Open question 3 is answered: the plain Vulkan loader, not `volk`.** The phase document's
default stands, because the question asks for a decision from measured dispatch overhead and
there is nothing to measure until the backend renders. Switching is one line in
`Toolchain.SHEETS` plus an include, so nothing is lost by waiting for a profile that argues for
it.

**Linux-only property sheets are a new concept in the generator, and they had to be.** The three
sheets have no `Code/PropertySheets/*.props` sibling and never will: the Windows build has no use
for Vulkan, VMA or SPIRV-Reflect. Every other sheet reaches a project through
`UpstreamProjects.txt`, which `SyncUpstream.py` regenerates from the `.vcxproj` files, so a
hand-written entry there would not survive the next sync - and `Checks.py` verifies that file
against upstream, so it would fail loudly first. `Toolchain.LINUX_ONLY_SHEETS` attaches a sheet
to a project **by name** instead, and `project_compile_flags` and `project_link_flags` fold it in
alongside the imported sheets. Only `Esoterica.Base` has an entry; every other project reaches
the RHI through it.

**One check was added**, and it is the kind `Checks.py` is for. A typo in either name of a
`LINUX_ONLY_SHEETS` entry silently contributes nothing. The build stays green until something
needs the symbols, and the link error then names `vkCreateInstance` rather than the misspelling.
The two new checks confirm that every key names a real project and every value is in `SHEETS`.

**Verified:**

- All eight Linux targets build and link: the five `.so` files, `Reflector`, `ResourceCompiler`
  and `Tester`. `ninja` exits 0.
- `ldd libEsoterica.Base.so` lists `libvulkan.so.1`. It is a real `DT_NEEDED`, not just a flag in
  the ninja file.
- `-lspirv-reflect` resolves at link time. No symbol is referenced yet, so the archive
  contributes nothing, but a missing archive would have failed the link.
- `vk_mem_alloc.h` with `VMA_IMPLEMENTATION`, `spirv_reflect.h` and `renderdoc_app.h` compile
  together in one translation unit under the project's own language flags: `-std=c++20
  -fno-exceptions -msse4.2 -mavx -mavx2`. That is the combination P5.1 will write, and it was
  checked rather than assumed.
- `Checks.py` passes, including the two new checks and the existing determinism and upstream-sync
  ones.
- Re-running the three fetchers reports "already present" and rebuilds nothing.
- `Editor` and `ResourceServer` still fail to compile, unchanged and for their existing Phase 7
  reasons: `shellapi.h`, `EE::Platform::Win32`, and an incomplete `EditorTool`.

**One thing for P5.1 to expect.** `vk_mem_alloc.h` emits about 200
`-Wnullability-completeness` warnings under `-Wall -Wextra`. They are noise, not errors - the
Linux build passes no `-Werror` - but the translation unit that defines `VMA_IMPLEMENTATION`
will want `-Wno-nullability-completeness` to keep the build log readable. That is a decision for
the task that writes the file, not for this one.

**Upstream files edited: `Code/Scripts/NinjaGen/NinjaGen.py` only**, which
[TouchedFiles.md](TouchedFiles.md) already registers as a large rewrite and a special case. Two
functions gain one line each. No new registry entry is needed, and no `.vcxproj` or `.slnx` file
is touched.

### 2026-08-28 - Criterion 3 is met: the shaders were valid, the validator was stale

**All 46 shader stages pass `spirv-val --target-env vulkan1.3`.** Acceptance criterion 3 is met
in full. Nothing about the shaders changed to achieve this.

**The `CullPrimitiveEXT` failure was never a defect in this project.** It was recorded twice, in
the P4.1 entry and again after the defect 2 fix, as pre-existing and unrelated, blocking
criterion 3 until "somebody works out which one is right". The answer is that the validator was
wrong:

| Validator | Version | Result on all 46 stages |
|---|---|---|
| `/usr/bin/spirv-val`, from Ubuntu 24.04 | SPIRV-Tools **v2025.1** | 6 mesh stages rejected |
| built from the SPIRV-Tools DXC vendors | SPIRV-Tools **v2026.3** (`1c336172`) | **0 rejected** |

The rejected variable is `%12 = OpVariable %_ptr_Output__arr_bool_uint_64 Output`, an array of 64
bools, and the v2025.1 diagnostic is "needs to be a boolean value array. ID <12> (OpVariable) is
not a bool scalar" - it asks for the thing it is looking at. The mesh built-in validation was
rewritten upstream since: the vendored `validate_builtins.cpp` has
`ValidateMeshBuiltinInterfaceRules`, `cull_primitive_entry_points_` and VUID 10591, none of which
exist in v2025.1. DXC's internal validation passed all along, and it passed because DXC validates
with the version it vendors.

**The lesson worth keeping: a distribution's `spirv-val` is not interchangeable with the one the
compiler validates against.** Two different judgements of the same module, and the older tool was
the one being believed.

**`spirv-val` is now installed next to `dxc`,** at
`External/DirectXShaderCompiler/bin/x64/spirv-val`, built by `DownloadDependencies.sh` from
`external/SPIRV-Tools` in the DXC clone. It costs 28 seconds against DXC's twenty minutes and
needs no extra download, because the source is already there. A criterion that can only be
checked by hand-building a tool is a criterion nobody re-checks.

`fetch_dxc`'s `git clean` now excludes `build-spirv-tools` as well as `build`, so a re-run does
not rebuild it. `git clean -e` takes a path, so it needed naming separately.

**Verified:** every one of the 46 stages compiled and validated under both validators in the same
sweep, which is how the 6-against-0 split above was measured rather than inferred.

**Use the installed one from now on.** `External/DirectXShaderCompiler/bin/x64/spirv-val`, not
whatever is on `PATH`.

### 2026-08-28 - P4.5 `CompileShaders.sh`, and `-hotreload` is not what the plan assumed

`CompileShaders.sh` exists and runs the Reflector's shader pass. Four modes, all exercised:

| Invocation | Reflector arguments | Result |
|---|---|---|
| `./CompileShaders.sh` | `-shaders` | exit 0, 34 generated files |
| `./CompileShaders.sh --hotreload` | `-shaders -hotreload` | exit 0 when the shader parameters have not changed |
| `./CompileShaders.sh --rebuild` | `-shaders -rebuild` | exit 0, reproduces the same 34 files |
| `./CompileShaders.sh --clean` | `-shaders -clean` | exit 0, removes them |

**`-hotreload` does not notify a running process.** The phase document guessed that it might, and
said to find out before including it. It does not. It is an in-process shortcut: a plain
`-shaders` run promotes itself to a full type reflection pass when the generated
`*.parameters.h` files change, and `-hotreload` refuses that promotion, exiting 1 with "Shader
Hot-Reload will result in changes to the engine runtime types requiring type reflection. Aborting
Hot-Reload!". See `Reflector.cpp:746` and `:1084`.

**Decision: `--hotreload` is not the default, though `CompileShaders.bat` passes `-hotreload`.**
This is the only place the two scripts differ. A first run always rewrites the parameters files,
so a literal mirror of the `.bat` gives a script that exits 1 on a fresh checkout. Both
behaviours were measured, not assumed: from a clean tree `-shaders -hotreload` exits 1, and on an
up-to-date tree it exits 0. The default is the run that always works, and the fast path is one
flag away. The script says why, at the top, so the divergence is not a mystery to whoever diffs
the two.

`--clean` and `--rebuild` pass straight through. `-shaders` scopes them to the shader output and
`CleanOutputs` cleans the right directories itself, so unlike `RunReflection.sh` this script
deletes nothing of its own. `RunReflection.sh` does its own deletion because `Reflect.nmake`
does; there is no nmake shader target to mirror here.

**Verified:** all four modes exit as expected; two consecutive default runs produce
**byte-identical** output; `--rebuild` from clean reproduces the incremental output byte for
byte; the documented `--hotreload` failure reproduces from a clean tree; and the engine still
compiles and links afterwards.

**A correction to the phase document's counts.** Criteria 2 and 3 are written in terms of "26
`.esh` files", one of them in `Base`. That conflates two file types:

- **`.esh` are headers**, 26 of them, and the Reflector never compiles one on its own.
  `Code/Base/Render/RHI.esh` is include-only, which is why `Base` reports "no shaders found" and
  its `_AutoGenerated/Shaders` directory stays empty. **No output is generated for `Base`, and
  none should be.**
- **`.esf` are the shaders**, 28 of them, all in `Esoterica.Engine.Runtime`, and they produce the
  46 stages that P4.3's defect 2 entry counts.

So criterion 2's "fills `_Module/_Autogenerated/Shaders/` for `Base` (`RHI.esh`) and `Engine`
(25 `.esh` files)" is met in substance and wrong in its numbers: 28 shaders, `Engine` only.

**Left in Phase 4:** P4.6, the structural diff against the Windows-generated output, which needs
a Windows machine. Criterion 9, clip-space Y. The constant buffer layout rule. Criterion 3's
`spirv-val` half, which is blocked on the pre-existing `CullPrimitiveEXT` failure.

### 2026-08-28 - P4.3 Defect 2 is fixed, and every shader compiles

**All 46 shader stages compile to SPIR-V, up from 41.** The Reflector's shader pass exits 0 and
fills `_AutoGenerated/Shaders/` with 34 files, the generated C++ compiles and links into
`libEsoterica.Engine.Runtime.so`, and the five `.material` resources Phase 3 could not compile
now compile. The fix is
`Code/Scripts/DXCPatches/0002-spirv-counter-for-a-heap-sourced-structured-buffer.patch`.

**What defect 2 actually was.** Assigning a counter-bearing resource out of a descriptor heap
failed, while initialising one from the same expression worked. The previous entry concluded the
destination was the hard case and that a correct fix meant implementing counters inside
structures in DXC, "a real project, not a patch". That was wrong. The destination already has a
counter; only the source failed to resolve, and it failed for **three** reasons, not two. All
three had to be fixed together, which is why the source-side half was written, looked correct,
and still did not compile a single shader:

1. `getFinalACSBufferCounter` tests AssocCounter#1 first, and `heap[i]` is a
   `CXXOperatorCallExpr`, so `getReferencedDef` resolves it to the subscript operator and the
   function returns that operator's empty pair before reaching the descriptor heap branch.
2. That branch tested `isResourceDescriptorHeap` against the expression's own type.
   `isDescriptorHeap` is the predicate that matches, as the previous entry worked out.
3. **The one that was missed.** The counter is resolved *before* the right-hand side is emitted.
   A descriptor heap access creates the heap's SPIR-V variable as a side effect of being
   emitted, and that is what puts the heap into `declRWSBuffers`, which is what lets a counter
   be created for it. Resolve first and there is nothing to find. `doBinaryOperator` now
   evaluates the rhs before calling `tryToAssignCounterVar`; its own comment already said "we
   need to evaluate rhs before lhs", and the counter call sat above the evaluation.

A fourth point was needed to avoid over-reporting, and it is not obvious: **one heap declaration
is shared by every resource type read out of it.** Registering the `RWStructuredBuffer` variant
puts that declaration into `declRWSBuffers`, so a later read of a `StructuredBuffer` or an
`RWBuffer` from the same heap reports the RW variant's counter and is then rejected against a
destination that rightly has none. `getDescriptorHeapResourceType` reads the resource type from
the implicit cast above the subscript, where `doCXXOperatorCallExpr` already reads it from, and
the heap branch returns no counter unless that type is counter-bearing. Without this, five
shaders compiled and two others that used to compile broke.

**Verified:**

- **The 41 stages that compiled before produce byte-identical SPIR-V.** Compared by hash against
  the previously installed compiler, over every `.esf` and every entry point. 41 identical, 0
  changed, 5 newly compiling.
- `spirv-val --target-env vulkan1.3` passes on the new shaders, except the two mesh shaders,
  which fail on `VUID-CullPrimitiveEXT-CullPrimitiveEXT-07036`. **That is the pre-existing and
  unrelated failure** already recorded for the material mesh shaders in the P4.1 entry.
- No shader emits a `counter_var`, so the counter heap reserved at set 1 binding 2 by the
  binding model stays unused, as that entry predicted.
- **DXC's own test suite: 1343 of 1357 CodeGenSPIRV tests pass, against 1345 before.** The two
  are `spirv.legal.sbuffer.struct.hlsl` and `spirv.legal.sbuffer.usage.hlsl`, and both fail only
  on `CHECK-NEXT` lines that assume the counter store is emitted before the right-hand side
  rather than after. The opcode histogram of each is **identical** to the unpatched compiler's,
  so this is a reordering and not a change of output. The tests are deliberately not edited, so
  that the fork's diff against the pinned tag stays confined to the fix. Upstream would want
  them updated in a submission.
- The patch applies to a pristine clone of the pinned tag after 0001, which is the path
  `DownloadDependencies.sh` takes. Checked by `git reset --hard`, `git clean`, applying both in
  name order, and rebuilding.
- The rebuilt `libdxcompiler.so` was installed and the Reflector relinked against it before the
  shader pass was run.

**Two things this unblocks, both checked:**

- **Acceptance criterion 5.** The generated `Shaders/*.cpp` compile and link. This needed
  `NinjaGen.py` to be re-run, because the ninja file was generated when the directory was empty.
  The case-insensitive match in `SourceLists.py` picks the files up correctly.
- **Phase 3's five `.material` resources.** All five compile, with
  `-compile data://... -force`. They were blocked on `DefaultPBRParameters`, which only the
  shader pass generates.

**How the test suite was run,** since the build has no test target. The DXC build here is
configured with `HLSL_INCLUDE_TESTS=OFF`, so there is no `FileCheck` and no `check-clang-spirv`.
The runner in the scratch directory parses each test's `// RUN:` line, substitutes `%dxc` and
`%s`, and pipes to the system `FileCheck-19`. 12 of the 1357 fail before any change of ours,
mostly cooperative-matrix and node tests; those are the baseline, not a regression.

**Still open in Phase 4:** P4.5, `CompileShaders.sh`, which does not exist yet. P4.6, the
structural diff of the generated output against Windows. Criterion 9, clip-space Y. The constant
buffer layout rule. Criterion 3 is met for compilation but not for `spirv-val`, because of the
pre-existing `CullPrimitiveEXT` failure on mesh shaders.

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

**`spirv-val` note. Resolved, see the P4.6 entry above: the shaders were valid all along and the
system `spirv-val` was wrong.** What follows is what was known at the time.

The compiled material shaders fail `spirv-val --target-env vulkan1.3` on
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

### 2026-08-30 - `RHI_Vulkan.cpp` creates the Vulkan surface, through `Platform_Linux.cpp`

**Context:** P6.2 raised it. `Platform::SetMainWindowHandle` holds an `SDL_Window*`, because that
is what the imgui and input backends need and what `Win32Application` stores. But
`EngineModule.cpp:135` hands the same value to `RenderWindow::SetNativeWindowHandle`, and
`RHI_Vulkan.cpp:2216` casts it to a `VkSurfaceKHR`, which is what P5.3 decided. The two do not
agree.

P5.3's answer was "the application creates the surface and hands it over", and **the application
has no place to do it**. `EngineModule::InitializeModule` calls `RenderSystem::Initialize`, which
creates the `VkInstance`, and calls `SetNativeWindowHandle` three lines later. Nothing the
application owns runs in between, and before that call there is no instance for a surface to come
from.

**Decision:** the RHI creates the surface. `m_pNativeWindowHandle` stays an `SDL_Window*` on
Linux, and `CreateSwapchain` calls a new Linux-only `Platform` function to turn it into a
`VkSurfaceKHR`:

```cpp
// Platform_Linux.h, in namespace EE::Platform
EE_BASE_API void* CreateVulkanSurface( void* pVulkanInstance, void* pNativeWindowHandle );
EE_BASE_API void DestroyVulkanSurface( void* pVulkanInstance, void* pSurface );
```

`Platform_Linux.cpp` is the only file that includes both SDL3 and Vulkan. `RHI_Vulkan.cpp` calls
through `void*` and never sees an SDL header.

**Rationale:** it edits no upstream file, which is the prime directive. It also keeps what P5.3
actually required, which was that **`Base/Render` depends on no window system library** - the
dependency lands in `Base/Platform`, and `Esoterica.Base` already links SDL3, so no project gains
a dependency it did not have. What it revises is only the second half of P5.3's sentence, "and the
application owns it", which turned out not to be reachable.

Surface ownership moves with it: `DestroySwapchain` still never destroys the surface, because
`ResizeSwapchain` recreates around an unchanged handle, but the RHI now destroys it when the
swapchain is destroyed for good rather than resized. **P6.6 owns writing this**, including where
the destroy belongs, and P5.3's entry should be read together with this one.

**Alternatives rejected:**

- **A two-line `#elif defined( __linux__ )` in `EngineModule.cpp:135`.** The right shape under
  Conventions rule 2, but `Code/Engine/_Module/EngineModule.cpp` is not in
  [TouchedFiles.md](TouchedFiles.md), and it would put SDL3 into `Esoterica.Engine.Runtime`, which
  has no other use for it.
- **Storing the `VkSurfaceKHR` in `Platform::SetMainWindowHandle`.** Does not work: there is no
  instance when the window is created, and the imgui and input backends need the `SDL_Window*`
  from that same accessor.

### 2026-08-29 - Open question 7: the shader reads its own indirect arguments

**Context:** Every engine render pass draws through `CmdExecuteIndirect`, and every engine command
signature sets root constants and binds a root CBV per command. **No Vulkan indirect draw rebinds
anything per command.** P5.13 landed the mechanical half and refused the rest at the line, which
left the frame unable to draw.

**Decision:** **Turn the push into a pull.** The shader reads its own command's root data out of
the argument buffer, indexing with the `DrawIndex` builtin, which is core Vulkan 1.1 through
`VK_KHR_shader_draw_parameters`. The change hides in the `RHI.esh` macros and is guarded on
`__spirv__`, so shader bodies and the whole Direct3D 12 path stay as they are. Scheduled as
**[P5.17](Phases/Phase5-VulkanRHI.md#p517---the-indirect-draw-shader-change---scheduled-not-started)**,
**after Phase 6 bring-up**, because nothing here can be tested until the engine runs and this
touches shaders Windows also compiles.

**Rationale:** It needs no extension and no new device requirement, and it is how GPU-driven
renderers normally solve this. Two things were checked before choosing it, because every engine
indirect draw is a mesh dispatch rather than a vertex draw: the bundled SPIR-V validator allows
`DrawIndex` in `MeshEXT` and `TaskEXT` (`validate_builtins.cpp:4009`), and DXC accepts
`[[vk::builtin( "DrawIndex" )]]` on mesh and amplification inputs
(`DeclResultIdMapper.cpp:3508`). The Vulkan backend uses no push constants today, so the range
that carries the argument buffer address is free.

**Alternatives rejected:**

- **`VK_EXT_device_generated_commands`.** It sets push constants per command and binds index and
  vertex buffers per command, but it **still cannot bind a descriptor set** per command. It
  therefore needs the same shader change *plus* a device extension the Phase 4 requirement list
  does not have.
- **A compute pre-pass.** This is the usual answer when an argument buffer is the wrong shape, and
  it does not apply. Repacking is not needed - `vkCmdDrawMeshTasksIndirectEXT` takes a stride and
  already reads the draw block correctly. What is needed is a descriptor binding, and a compute
  shader cannot bind a descriptor for a later draw either. This also answers the question Phase 4
  left open when it set `m_indirectRootConstant` to `false`.
- **A CPU loop, one indirect call per command.** The command count is produced on the GPU, so the
  CPU does not know how many commands there are and cannot read the GPU-written root data to bind
  it. It survives only as a fallback for the indirect *compute* case, where the index is known
  because the CPU wrote the loop.

**Consequences worth stating:** This edits upstream shader files, which the escalation list
normally forbids. It was escalated and approved. The five files are on
[TouchedFiles.md](TouchedFiles.md) as `planned`, each edit inside an `#ifdef __spirv__`.
**Verifying it needs mesh shader hardware**, which this development machine does not have.

### 2026-08-28 - Phase 5 is written blind, because nothing on Linux can run it

**Decision: implement the Vulkan backend verified by compile and link only, and first execute it
when Phase 6 lands.** Recorded because it makes every Phase 5 entry weaker than it looks, and a
later session reading "P5.1 done" needs to know what "done" meant.

**The blocker, verified rather than assumed.** Nothing on Linux can reach `RHI::CreateContext`:

- Of the seven applications, `BuildGenerator` is excluded permanently, `Editor` and
  `ResourceServer` do not compile and are Phase 7, `Reflector` and `ResourceCompiler` are offline
  tools that never touch the RHI, and `Tester` is an upstream scratchpad.
- `Esoterica.Applications.Engine` has exactly one source file, `Win32/EngineApplication_Win32.cpp`,
  which the `**/Win32/**` exclusion glob drops. There is no Linux engine binary.
- Writing one does not help on its own. `BaseModule::InitializeModule` calls
  `m_inputSystem.Initialize()` at `:151`, which calls `Initialize()` on a `KeyboardMouseDevice`
  and four `XBoxControllerInputDevice`s, and `m_imguiSystem.Initialize()` at `:157`, which calls
  `InitializePlatform()`. All three are Phase 6 stubs that halt. `RHI::CreateContext` lives at
  `RenderSystem.cpp:45`, inside `EngineModule::InitializeModule`, which runs after both.

So running the engine needs P6.1 through P6.5 real first - SDL3, `LinuxApplication`, the imgui
SDL3 backend, keyboard and mouse, gamepad. That is essentially all of Phase 6, whose own estimate
is 3-4 weeks, before a line of Vulkan is written. Phase 6's stated prerequisite is "Phase 5
through bring-up step 8", so the two phases are circular.

**Rejected: the `Tester` harness the phase document names.** `Code/Applications/Tester/Main.cpp`
is 114 lines, roughly 90 of them commented-out experiments, and the live code loads and saves an
ini file from a hardcoded `D:\Esoterica\...` path. `int numTestFailures` is returned and never
incremented. It is a scratchpad, not a test framework, and it is an upstream file that
[TouchedFiles.md](TouchedFiles.md) does not list. **Acceptance criterion 4 cannot be met as
written**, and the bring-up steps 1 to 8 it asks for have no home.

**Rejected: a small Linux-only bring-up binary.** It would have verified P5.1 today, at the cost
of a target with no Windows counterpart.

**Rejected: the `EE_SHIPPING` loophole.** `EE_UNIMPLEMENTED_FUNCTION` compiles to nothing when
`EE_DEVELOPMENT_TOOLS` is off, so a Shipping build walks past all three Phase 6 stubs silently.
It also disables every assert and the validation layers, which Phase 5's "do not" list rules out
explicitly.

**What this costs, stated plainly.** Device feature checks, capability reporting, memory
statistics and the whole binding model are unexercised. A wrong assumption in any of them
surfaces at Phase 6 on top of several finished task groups rather than immediately. The
mitigation available is that every extension name, feature bit and struct name is read out of the
system Vulkan headers rather than recalled, and the two GPUs in this machine were surveyed during
Phase 4 against the binding model's requirement list.

**One thing that makes this less bad than it looks.** The engine's own initialisation order is
already headless-first: `EngineModule::InitializeModule` calls `m_renderSystem.Initialize()`
*before* `m_renderWindow.SetNativeWindowHandle( Platform::GetMainWindowHandle() )` at `:135`, and
`Platform::GetMainWindowHandle` is a plain `void*` global with no Win32 types in it. When Phase 6
lands, bring-up steps 1 to 8 map onto the real engine with no window needed, and step 9 is where
the swapchain arrives. The ladder the phase document wanted still exists; it just runs later.

### 2026-08-28 - P4.6 is deferred to a Windows machine, with the Linux half done in advance

**Phase 4 is done on Linux.** What is left of P4.6 is one thing: the structural diff of the
generated shader code against the Windows-generated output, and it cannot be done without a
Windows machine. Criterion 10, that the Windows MSBuild build still succeeds, is in the same
position.

**P4.6 is four checks and three of them were already done:**

| Check | State |
|---|---|
| 1. The generated C++ compiles | done, and it links into `libEsoterica.Engine.Runtime.so` |
| 2. Structural diff against Windows, bytecode ignored | **needs Windows** |
| 3. Binding indices agree with what Phase 5 expects | not applicable, see below |
| 4. The Phase 0 generator's glob picks the files up, which is P0.4 | done |

**Check 3 rests on a wrong picture of the generated code, and is worth correcting.** A generated
`.cpp` is a base85 LZAV-compressed bytecode blob, three size constants, and a `Register_<Name>`
function that emplaces a `ShaderByteCode` into one of three vectors. **There are no binding tables
and no binding indices in it at all.** Binding information lives in the SPIR-V and is reflected at
runtime by the backend, which is the same thing P4.2 and P4.4 found when they established that the
Reflector never reflects bytecode. So check 3 was settled by the P4.3 binding model decision, and
check 2's "the same binding tables" has nothing to compare.

**Deferring blocks nothing.** `P4.6` is referenced nowhere outside its own task heading. Phase 5's
prerequisite names what actually matters - "Phase 4's binding-model decisions are hard
prerequisites" - and those are made and recorded. Every phase from 0 to 7 carries a "the Windows
MSBuild build still succeeds" criterion, so Windows work is owed across the whole plan rather than
here in particular, and Phases 2 and 3 are already marked done on Linux with Windows criteria
outstanding. This is the same call, made the same way.

**The Linux half is done in advance, so that P4.6 is a one-command diff later rather than a task
somebody has to re-derive.** `Code/Scripts/ShaderStructure/ExtractShaderStructure.py` emits a
normalised structural summary of a generated shaders directory, and
`Code/Scripts/ShaderStructure/Linux.structure.txt` is the Linux side of the comparison, committed.
Whoever has a Windows machine runs the same script there and diffs the two files.

The script removes only what is bytecode-dependent: the base85 payload lines, the three
`g_byteCode_*_{decompressed,decoded,compressed}Size` constants, the array bound in
`static char const g_byteCode_X[9501]`, and the `Generated For:` line, which is an absolute path.
Everything else is kept **verbatim**, with whitespace collapsed. Keeping the rest verbatim rather
than parsing out the interesting parts is deliberate: a parser only compares what it was taught to
look for, and this check exists to catch what nobody predicted.

What survives is exactly the structure criterion 6 asks about - here is a whole entry:

```
===== WorldUpdate.cpp =====
...
void Register_WorldUpdate( RHI::Context* pContextRHI, TVector<MaterialShader>& materialShaders, ... )
{
ByteCodeList shaderByteCodeParameters;
shaderByteCodeParameters.clear();
shaderByteCodeParameters.emplace_back( RHI::ShaderByteCode( RHI::ShaderStage::Compute, StringID( "CS" ), g_byteCode_WorldUpdate_CS, ... ) );
computeShaders.emplace_back( ComputeShader( pContextRHI, StringID( "WorldUpdate" ), shaderByteCodeParameters ) );
}
```

The shader set, each shader's stages, its `StringID` names, which of the three vectors it
registers into, the `Initialize` call order, and the `.parameters.h` structs with their members and
defaults. 34 files, 1010 lines.

**Verified:** the snapshot contains no base85 payload line and no size constant definition, checked
by grep; and it is **byte-identical after a full `./CompileShaders.sh --rebuild`**, so a future
diff against Windows will show real divergence rather than regeneration noise.

**Internal consistency already holds on Linux**, which is the part of criterion 6 that can be
checked without Windows: 28 `.esf` files produce 28 generated `.cpp`, 28 `Register_` declarations
and 28 `Initialize` calls, with no gaps in either direction.

**What a Windows machine still owes Phase 4:** criterion 6, the structural diff, now one command;
and criterion 10, that the Windows Reflector's DXIL output is unchanged. Criterion 10 is the more
valuable of the two. `ShaderReflection_ShaderCompiler.cpp` has been edited three times in this
phase - the `-spirv` switch, `DXC_ARG_BINDING_MODEL` and `DXC_ARG_MEMORY_LAYOUT` - and every one
expands to nothing under `_WIN32`, so Windows is unchanged **by construction**. That is an
argument, not a test.

### 2026-08-28 - Defect 3 is fixed: bitfields that differ only in signedness now merge

**Fixed**, in `Code/Scripts/DXCPatches/0003-spirv-merge-bitfields-that-differ-only-in-signedness.patch`.
Every structured buffer stride now equals the size the engine's own
`HLSL_STATIC_ASSERT`s demand, `MeshCluster` included.

**It was worse than the previous entry recorded.** That entry named `MeshCluster` as the only
affected type, on the grounds that it is the only struct mixing signed and unsigned bitfields.
That was the right observation and the wrong conclusion: the refusal is on the *exact* base type,
not on signedness, so **two distinct enum types are also refused**. `RenderView` has
`RenderViewFlags : 2`, `RenderViewLayerFlags : 8` and `uint : 22` adjacent, gets three words
instead of one, and came out at 360 bytes where C++ and DXIL both say 352. It was silently wrong
too, and nothing in the earlier survey would have caught it, because `RenderView` carries no
`HLSL_STATIC_ASSERT`. The lesson is that the asserts are a partial oracle: they cover 13 types
out of everything shared with C++.

| Type | C++ / DXIL | Before | After |
|---|---|---|---|
| `MeshCluster` | 32 | 40 | 32 |
| `RenderView` | 352 | 360 | 352 |

Both confirmed against C++ by compiling the declarations, and against Direct3D by compiling the
engine's asserts on the DXIL back end, which implements `_Static_assert`.

**Why it was not a one-line change.** The comment DXC left behind is true:

> Bitfields can only be merged if they have the exact base type.
> (SPIR-V cannot handle mixed-types bitfields).

A SPIR-V struct member cannot be signed and unsigned at once, and both extract opcodes require
Base and Result Type to be **the same type**, not merely the same width. `spirv-val` rejects
`OpBitFieldUExtract %uint %int_base` with "Expected Base Type to be equal to Result Type", which
was checked directly with a hand-assembled module rather than assumed from the spec text. So
merging is only half the job; the accesses have to be fixed too, and that is most of the patch.

The merged member takes the first field's type. Each field is reached by reinterpreting the
member into the field's own type and extracting in that type, so Base and Result Type agree and
the signed-or-unsigned choice still follows the field:

```
%35 = OpBitcast %int %34
%36 = OpBitFieldSExtract %int %35 %uint_0 %uint_24     ; int sgn : 24  -> sign-extends
%40 = OpBitcast %uint %39
%41 = OpBitFieldUExtract %uint %40 %uint_24 %uint_8    ; uint uns : 8  -> zero-extends
```

Writes mirror it: the value is reinterpreted into the member's type before `OpBitFieldInsert`,
which has the same requirement.

**Four places changed, and the second one is the trap.** `AlignmentSizeCalculator` computes sizes
and offsets from the AST with its own copy of the merge rule, entirely separately from
`LowerTypeVisitor`, which decides which fields share a member. Fixing only the lowering produces
a struct whose members are merged and whose stride says otherwise - which is what the first
attempt here did, and it is worse than the original bug. The two have to agree.

**Verified:**

- All 46 stages compile and pass `spirv-val --target-env vulkan1.3 --scalar-block-layout`.
- **Every** stride now matches the asserted size. Before this patch `MeshCluster` did not.
- **32 of the 46 stages are byte-identical to the previous compiler.** The 14 that change are
  exactly those reading `MeshCluster` or `RenderView`, and both move to the Direct3D layout.
- Sign extension checked instruction by instruction on a shader that reads and writes both a
  signed and an unsigned field of one merged word, in both directions.
- `./CompileShaders.sh --rebuild` exits 0, and the engine compiles and links against the result.
- The patch applies to a pristine clone of the pinned tag after 0001 and 0002, which is the path
  `DownloadDependencies.sh` takes.

**DXC's own tests: 1340 of 1357 pass, against a 1345 baseline.** Five fail. Two are the
instruction reordering from patch 0002, already recorded. The other three -
`vk.layout.struct.bitfield.hlsl`, `vk.layout.struct.bitfield.assignment.hlsl` and
`op.structured-buffer.access.bitfield.hlsl` - encode the behaviour this patch changes rather than
catching a fault in it. The first expects `%S6 = OpTypeStruct %uint %int` for
`struct S6 { uint f2 : 1; int f1 : 1; }`; compiling that struct with
`_Static_assert( sizeof( S6 ) == 4 )` on the DXIL back end passes, so one member is the Direct3D
answer and the test encodes the divergence. Upstream would want all three updated in a
submission; they are deliberately not touched, so the fork's diff stays confined to the fix.

**Three patches now, all in the SPIR-V back end.** The DXC decision entry says to move to a fork
"if the patching becomes substantial". Three patches across six files is the point at which that
is worth asking rather than assuming. All three are worth submitting upstream.

### 2026-08-28 - P4.3 Memory layout: `-fvk-use-dx-layout`, and the defect it exposed

**Decision: the Reflector passes `-fvk-use-dx-layout` on Linux.** It is not optional. Without it
seven shared struct types are laid out wrong, and nothing anywhere reports it.

**Why it is forced.** The `.esh` files compile as both C++ and HLSL, so the same struct
declaration describes the bytes the engine writes and the bytes the shader reads. DXC's default
is Vulkan's standard buffer layout, which rounds a struct's size up to its largest member's
alignment; Direct3D packs tightly. Measured against the sizes the engine's own
`HLSL_STATIC_ASSERT`s demand, over every `.esf` and entry point:

| Type | Asserted | Default rules | `-fvk-use-dx-layout` |
|---|---|---|---|
| `ImguiVertex` | 20 | **24** | 20 |
| `MeshInstance` | 64 | **80** | 64 |
| `MeshInstanceTransformUpdateCommand` | 64 | **96** | 64 |
| `PointLightUpdateCommand` | 64 | **80** | 64 |
| `SpotLightUpdateCommand` | 64 | **80** | 64 |
| `DirectionalLightUpdateCommand` | 32 | **48** | 32 |
| `MeshCluster` | 32 | **40** | **40** |
| the other six | - | correct | correct |

Seven wrong by default, one wrong with the flag. `ImguiVertex` is the clearest: 20 bytes in C++,
confirmed by compiling the declaration, and a 24-byte array stride under the default rules, so
every vertex after the first would be read from the wrong offset. No validation error, no crash,
just wrong geometry - exactly the failure the phase document says to prevent now rather than
debug in Phase 5.

**Two consequences that must not be missed.**

1. **The Vulkan device must enable `scalarBlockLayout`.** DXC's documentation says
   `-fvk-use-dx-layout` requires `VK_EXT_scalar_block_layout`, and it is not advisory: without it
   the modules are invalid. `spirv-val` rejects 18 of the 46 stages with "Matrix with a stride 12
   not satisfying alignment to 16 ... may be allowed if you enable the scalarBlockLayout
   feature". The feature is core in Vulkan 1.2 and the baseline is 1.3, so it is a feature bit,
   not an extension. Both GPUs in this machine report `scalarBlockLayout = true`. **P5.1 must
   enable it, and refuse a device that lacks it**, alongside the binding model's requirements.
2. **Validate with `--scalar-block-layout` from now on.** All 46 stages pass with it and 18 fail
   without it, and those 18 failures are the validator being told the wrong rules rather than bad
   modules. The criterion 3 check recorded earlier needs the flag.

**The defect this exposed. Defect 3: DXC's SPIR-V back end does not merge bitfields of different
signedness.**

`MeshCluster` is the one type the layout flag does not fix, and it is not a layout-rule problem.
Reduced to a minimal shader, with `-fvk-use-dx-layout` in force:

| Struct | Stride |
|---|---|
| `struct { int a : 24; uint b : 8; }` | **8** |
| `struct { int a : 24; int b : 8; }` | 4 |
| `struct { uint a : 24; uint b : 8; }` | 4 |

Same-signedness pairs merge into one 32-bit word. A mixed pair does not, and takes two.
`MeshCluster` has two mixed pairs - `int32_t m_anchorX : 24` with `uint32_t m_numVertices : 8`,
and the same for Y - so it comes out at 32 + 8 = 40.

**Direct3D does not do this.** The DXIL back end implements `_Static_assert`, so the engine's own
asserts are live there, and a DXIL compile of `ClusterCulling.esf` accepts
`sizeof( MeshCluster ) == 32`. A minimal `_Static_assert( sizeof( MixedSign ) == 4 )` also passes
on DXIL. C++ agrees: `sizeof` is 32, confirmed by compiling the declaration. So SPIR-V is the
odd one out, and `MeshCluster` reads are silently wrong on Linux today.

`MeshCluster` is the only affected type. Every other bitfield struct in the engine uses `uint` or
an unsigned enum throughout, which is why the layout table above has exactly one residual row.

**It is deliberate in DXC, and it is not a one-line fix.** `LowerTypeVisitor.cpp:1376`:

```cpp
    // Bitfields can only be merged if they have the exact base type.
    // (SPIR-V cannot handle mixed-types bitfields).
    if (previousField->type != loweredField.type)
      return loweredField;
```

That is true as far as it goes - a SPIR-V struct member cannot be both signed and unsigned - but
it is soluble: pick one base type for the merged word and choose `OpBitFieldSExtract` or
`OpBitFieldUExtract` per field at each access. Patch 0001 already added
`createBitFieldExtract` calls for exactly this shape of problem. The work is in type lowering and
at every read and write of a merged field, so it is closer to defect 2 in size than to a
one-liner. **Not attempted here.** Recorded so that whoever picks it up does not have to
re-derive it.

**Impact until it is fixed.** `MeshCluster` is read as a `StructuredBuffer` by the cluster
culling and mesh shading path. Reads past the first element take data from the wrong offset. This
will not show up until Phase 5 renders, and it will look like corrupt geometry rather than like a
layout bug, so it is written here in advance.

**Verified:** all 46 stages compile and pass `spirv-val --target-env vulkan1.3
--scalar-block-layout`; `./CompileShaders.sh --rebuild` exits 0 and regenerates all 34 files; the
engine compiles and links against the regenerated code.

### 2026-08-28 - Clip-space Y is inverted in the Vulkan viewport, not in the shader compiler

Acceptance criterion 9. It names one layer, as the criterion demands.

**Context.** The engine builds its projection matrices from DirectXMath, and says so:
`Math::CreatePerspectiveProjectionMatrix` is commented "Taken from DirectXMath:
XMMatrixPerspectiveFovRH", and the orthographic pair likewise (`ViewVolume.cpp:9` and `:29`).
Those are right-handed with a **Y-up NDC and a 0..1 depth range**. Vulkan agrees on the depth
range, which is why nothing here needs a depth fix, and disagrees on Y: Vulkan's NDC has +Y
pointing down. Uncorrected, every rendered image is vertically mirrored.

`RHI_Direct3D12.cpp:3049` sets the viewport straight through, with no flip of its own, so the
Direct3D path is the plain DirectXMath convention end to end.

**Decision: `RHI_Vulkan.cpp`'s `CmdSetViewport` performs the inversion, with a negative viewport
height. The shader compiler does not. `-fvk-invert-y` is not passed, and must not be added.**

```
vkViewport.y      = y + height;
vkViewport.height = -height;
```

`x`, `width`, `minDepth` and `maxDepth` pass through unchanged. A negative viewport height needs
no extension: it is core Vulkan from 1.1, and the baseline is 1.3.

The decision is written into `RHI_Vulkan.cpp` at the `CmdSetViewport` stub, not only here,
because the failure this criterion guards against is doing the flip twice, and the second flip is
silent. A comment in the function Phase 5 is about to fill in is the cheapest place to stop that.

**Why not `-fvk-invert-y`, which is what the phase document suggested first.**

- **It is a hard error on pixel and compute shaders.** Measured, not assumed:
  `error: -fvk-invert-y can only be used in VS/DS/GS/MS/Lib`. It compiles fine on `vs_6_6` and
  `ms_6_6`, and emits exactly what it says - an `OpFNegate` on component 1 before the
  `OpStore` to `gl_Position` - but the Reflector builds one `COMMON_DXC_ARGUMENTS` list and
  hands it to every stage. Adopting the flag means splitting that list per stage inside
  `ShaderReflection_ShaderCompiler.cpp`, a file the Windows build shares, for no capability the
  viewport does not already give.
- **A flip baked into bytecode is invisible at the API level.** Nothing in the backend would show
  it. The viewport version sits in the function anyone debugging an upside-down frame opens
  first.
- **It keeps the SPIR-V a faithful translation of the HLSL**, which matters for P4.6: a
  compiler-inserted negate in every vertex and mesh shader is noise in a structural comparison
  against the DXIL output.
- It is also the mapping every Direct3D-to-Vulkan translation layer uses, vkd3d-proton included.

**Why not flip the projection matrices.** They are engine code, shared with the Direct3D path,
and shaders do their own clip-space arithmetic against them. `RendererTypes.esh:92` converts a
projected bounding box from clip space to UV space with
`aabb.xwzy * float4( 0.5, -0.5, 0.5, -0.5 ) + 0.5`, where the negative Y terms encode the
Direct3D convention, and then samples a depth pyramid with the result. Flipping the matrix breaks
that; flipping the viewport does not, because the pyramid is rasterised through the same flipped
viewport and lands in memory the same way it does on Direct3D. This is the whole reason the flip
belongs in the rasteriser and not in the maths.

**The consequence that is easy to miss: front-face winding.**

Mirroring the viewport inverts triangle winding in framebuffer space, so the front-face mapping
has to absorb it. Two inversions apply on the way to Vulkan and they cancel:

1. `RHI_Direct3D12.cpp:5287` sets
   `FrontCounterClockwise = ( m_frontFace == FrontFace::ClockWise )`. That reads backwards -
   `FrontFace::CounterClockWise`, the RHI default, means front faces are **clockwise** in screen
   space on Direct3D. It is upstream's code and Conventions rule 3 says to leave it alone. To
   match Direct3D with no Y flip, Vulkan would have to invert the enum the same way.
2. The negative viewport height inverts winding again.

**So `RHI_Vulkan.cpp` maps `FrontFace` literally** - `ClockWise` to `VK_FRONT_FACE_CLOCKWISE` -
and it is only correct *because* of the viewport flip. Remove the flip and this has to be swapped
in the same commit. That derivation is recorded at the `CreatePipeline` stub too.

**What needs nothing, checked rather than assumed:**

- **`-fvk-use-dx-position-w` is not needed.** In Vulkan the `w` of `SV_Position` read by a pixel
  shader holds `1/w` where Direct3D holds `w`. **No pixel shader in this engine reads
  `SV_Position.w`.** Two read `SV_Position.xy`, `BilateralUpsample.esf:66` and
  `OITResolve.esf:32`, both as integer pixel coordinates for a `Load`. `FragCoord.xy` is
  framebuffer space with an upper-left origin in Vulkan exactly as `SV_Position.xy` is in
  Direct3D, so both are already right. The `.w` uses elsewhere are on clip positions the shaders
  compute themselves, not on the pixel shader input.
- **Depth.** DirectXMath's projections already produce 0..1, which is Vulkan's range. Nothing to
  do, and no `depthClipControl`.
- **The fullscreen triangle.** `FullscreenTriangle.esh` writes clip-space positions directly,
  bypassing the projection matrix, and encodes the same Direct3D convention: uv `(0,0)` maps to
  clip `(-1, 1)`, the top of the screen with Y up. It goes through the viewport like everything
  else, so the one flip covers it.

**Not verified, and cannot be until Phase 5 renders.** Everything above is derived from the
source and from measured compiler behaviour. The first real check is bring-up step 6, the
offscreen clear, and step 7, the offscreen triangle: if the image is upside down, the flip is
being applied twice or not at all. Check that before believing anything downstream of it.

### 2026-08-28 - P4.3 The bindless binding model

This is acceptance criterion 8, and Phase 5's hard prerequisite. It is written to be implemented
against, so it names every set, binding, descriptor type and feature bit.

**Context.** Direct3D 12 gives the engine two shader-visible descriptor heaps and a root
signature that holds nothing else. `CreateRootSignature` (`RHI_Direct3D12.cpp:4938`) builds only
root constants and root descriptors, never a descriptor table, and sets
`CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED`. `SetDescriptorHeaps` is
called once per command buffer (`:2917`). `GetBufferHandle`, `GetTextureHandle` and
`GetSamplerStateHandle` return a `uint16_t` offset into one of those two heaps, and the shaders
index them through the `GetBuffer` / `GetTexture` / `GetSamplerState` macros in `RHI.esh`, which
are `ResourceDescriptorHeap[i]` and `SamplerDescriptorHeap[i]`.

Vulkan has to reproduce that exactly, because the handle is the shared currency: the Reflector's
generated resource tables pack these same `uint16_t` values (see
`_AutoGenerated/ShaderReflection/WorldUpdate.esh`), and `InvalidResourceHandle` is `UINT16_MAX`.

#### How DXC maps SM6.6 heaps onto Vulkan

Established by compiling reduced shaders and by reading
`tools/clang/lib/SPIRV/DeclResultIdMapper.cpp` and `docs/SPIR-V.rst`, not from memory:

- The default is the **emulated** heap (`-fspv-use-emulated-heap`). Every distinct HLSL resource
  type read out of `ResourceDescriptorHeap` becomes its own SPIR-V `OpTypeRuntimeArray`
  variable, and **all of them are decorated with the same set and binding**. The heap index is
  the array index, unchanged. `SamplerDescriptorHeap` gets a second binding.
- Aliasing several descriptor types on one binding is what **`VK_EXT_mutable_descriptor_type`**
  exists for. DXC's own documentation names the extension as a requirement.
- The alternative native path, `-fspv-use-descriptor-heap`, emits `SPV_EXT_descriptor_heap`.
  DXC warns "SPV_EXT_descriptor_heap support is incomplete" on this engine's shaders, and the
  build treats warnings as errors. **Ruled out.**
- Heap bindings are otherwise allocated **lazily, from the first free binding numbers in set 0**,
  after every other resource is placed. That makes them a function of which registers a given
  shader happens to use, which is not something a backend can bind against. They must be pinned.

**Decision.** Two descriptor sets.

**Set 1 - the heaps.** One layout, identical for every pipeline in the engine, allocated once
and bound once.

| Binding | Vulkan descriptor type | Count | Source |
|---|---|---|---|
| 0 | `VK_DESCRIPTOR_TYPE_MUTABLE_EXT` | 65472 | `ResourceDescriptorHeap` |
| 1 | `VK_DESCRIPTOR_TYPE_SAMPLER` | 2048 | `SamplerDescriptorHeap` |
| 2 | reserved for the counter heap; do not create it | - | see below |

The counts are D3D12's, from `CreateContext`: `64 * 1023` for the CBV/SRV/UAV heap and `2048`
for the sampler heap (`RHI_Direct3D12.cpp:2229` and `:2236`). Keeping them identical is what
makes a handle mean the same thing on both backends. Both fit under `UINT16_MAX`, so
`InvalidResourceHandle` stays outside either heap.

The mutable type list for binding 0, taken from every `OpTypeImage` and buffer type that the
engine's shaders actually pull out of the heap:

| Vulkan descriptor type | HLSL that produces it |
|---|---|
| `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` | `Texture2D`, `Texture2DArray`, `Texture3D`, `TextureCube` |
| `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` | `RWTexture2D` |
| `VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER` | `Buffer<T>` |
| `VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER` | `RWBuffer<T>` |
| `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | `StructuredBuffer<T>`, `RWStructuredBuffer<T>`, `ByteAddressBuffer` |
| `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | no shader today, but `GetBufferHandle` accepts `DescriptorTypeFlags::ConstantBuffer` and returns a heap offset, so the heap must be able to hold one |

Binding 0 and binding 1 both take `PARTIALLY_BOUND` and `VARIABLE_DESCRIPTOR_COUNT`, and the
layout takes `UPDATE_AFTER_BIND_POOL`, because the engine writes handles into the heap while
command buffers that reference the heap are recording, exactly as
`CopyDescriptorsSimple` does on Direct3D 12.

Sampler heap slots 0 to 5 are the engine's common samplers, at the fixed indices in
`CommonSamplers.esh`. There are no Vulkan immutable samplers in this model, and no static
samplers in the pipeline layout: `CreateRootSignature`'s static sampler path finds no matching
shader resource in any current shader.

**Set 0 - the root parameters.** Per-pipeline layout, derived from reflection, created with
`VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT`.

| HLSL register | Set 0 binding | Vulkan descriptor type |
|---|---|---|
| `b<N>` | `N` | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` |
| `t<N>` | `8 + N` | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` |
| `u<N>` | `16 + N` | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` or `STORAGE_IMAGE` |
| `s<N>` | `24 + N` | `VK_DESCRIPTOR_TYPE_SAMPLER` |

The shifts exist because DXC ignores the register **type** when it derives a binding from
`register( xN, spaceY )`, so `b2` and `t2` would otherwise both be binding 2. Nothing in the
engine trips that today, and DXC does not diagnose the overlap when it happens, so the shifts
are there to make a future collision impossible rather than to fix a present one.

`CmdSetRootConstants` cannot become Vulkan push constants: the shader declares the block as
`ConstantBuffer<T> RootConstants : register( b0 )` through `EE_DECLARE_ROOT_CONSTANTS`, so DXC
emits a uniform buffer, and turning it into a push constant block needs `[[vk::push_constant]]`
in `RHI.esh`, which Phase 4 rule 4 forbids. Implement it instead as a copy into a per-frame
upload ring buffer followed by `vkCmdPushDescriptorSetKHR`. `CmdSetRootParameter` is the same
call with the caller's buffer and offset, and `VK_WHOLE_SIZE` for the range, which matches a
Direct3D 12 root descriptor: an address, with no size.

**The DXC flags.** Added to `DXC_ARG_BINDING_MODEL` in `ShaderReflection_ShaderCompiler.cpp`,
Linux only:

```
-fvk-auto-shift-bindings
-fvk-b-shift 0 0    -fvk-t-shift 8 0    -fvk-u-shift 16 0    -fvk-s-shift 24 0
-fvk-bind-resource-heap 0 1
-fvk-bind-sampler-heap  1 1
-fvk-bind-counter-heap  2 1
```

`-fvk-auto-shift-bindings` applies the same shifts to any resource that carries no `register()`,
which no shader has today; it is insurance against one arriving from upstream.

**There is no counter heap, and the engine needs none.** This corrects the previous entry.
`IncrementCounter`, `DecrementCounter`, `AppendStructuredBuffer` and `ConsumeStructuredBuffer`
appear in **no** `.esh` or `.esf` file in the repository. `AppendBuffer<T>` in `AppendBuffer.esh`
carries its own explicit `RWBuffer<uint> m_counterBuffer` and does its own `InterlockedAdd`, so
the hidden Direct3D counter is dead weight. DXC only emits the counter array when a counter is
actually used - `docs/SPIR-V.rst` says so, and compiling all 41 shader stages that build today
produces **zero** `counter_var` variables. Binding 2 of set 1 is therefore reserved and never
created; the flag is passed only so that a counter, if one ever appears, lands somewhere known
instead of displacing the sampler heap.

**Rationale for set 1 holding the heaps, rather than set 0.**

The Vulkan-idiomatic ordering is the opposite: the most stable set should be set 0, because
binding a pipeline whose layout differs from set N onwards disturbs every set from N up. Here
set 0 varies per shader and set 1 does not, so the heap set is disturbed whenever a pipeline
with a different root-parameter layout is bound, and the backend must re-bind set 1 on a
pipeline-layout change. **`CmdSetPipeline` is where that belongs**, next to the existing
`ResetRootSignature` call. One `vkCmdBindDescriptorSets` per pipeline change is a rounding error.

The reason for accepting that is the alternative. Putting the heaps in set 0 needs the root
parameters moved to set 1, and the only way to move a *set* without editing a `.esh` file is
`-fvk-bind-register`, which "requires all source code resources have `:register()` attribute and
all registers have corresponding Vulkan descriptors specified using this option". That makes the
Linux build fail the day upstream adds a global resource without a register, or with a register
outside our table, and upstream is active. The prime directive is cheap upstream merges. A
redundant descriptor-set bind is a much smaller price than a shader-compiler flag list that has
to track every register upstream ever writes.

**Verified.** Not asserted:

- All four heap and shift schemes were compared by compiling **every `.esf` in the repository**
  across `cs/ps/vs/ms/as_6_6` entry points. 41 stages compile; the flags introduce **no new
  failure**. The 5 that fail are exactly the known defect-2 shaders: `DebugDraw` MS,
  `DebugDrawMesh` MS, `DebugDrawResolve` CS, `InstancePickingResolve` CS, `WorldUpdate` CS.
- The set and binding decorations of every compiled stage were dumped and grouped. The result is
  the table above: set 0 binding 0 and 1 hold only uniform buffers, set 1 binding 0 holds the
  five aliased array types, set 1 binding 1 holds the sampler array.
- A real shader (`DefaultPBR` MS) shows nine aliased runtime arrays on set 1 binding 0.
- `spirv-val --target-env vulkan1.3` passes on the reduced heap shaders.
- The Reflector was rebuilt with the flags and run. `strings -eL` confirms the flags reach the
  binary, and the shader pass produces the same five failures and no others.
- Both GPUs in this machine (Intel UHD 620 on Mesa 25.2.8, NVIDIA driver 580.173) expose
  `VK_EXT_mutable_descriptor_type` with `mutableDescriptorType = true`, `VK_KHR_push_descriptor`
  with `maxPushDescriptors = 32`, and `runtimeDescriptorArray`,
  `descriptorBindingPartiallyBound`, `descriptorBindingVariableDescriptorCount`, the
  `*UpdateAfterBind` bits and the `*ArrayNonUniformIndexing` bits for sampled image, storage
  image, storage buffer and uniform texel buffer. The update-after-bind limits are six orders of
  magnitude above 65472.

**Required of the Vulkan device, so P5.1 can check it up front:** Vulkan 1.3, plus
`VK_EXT_mutable_descriptor_type` and `VK_KHR_push_descriptor` (core in 1.4), plus the descriptor
indexing feature bits listed above. Refuse the device if any is missing; there is no fallback
path, because the shaders are compiled for this model.

**One thing left open, for P5.16.** `RHI.esh` maps
`GetRaytracingAccelerationStructure( index )` to `ResourceDescriptorHeap[index]`, and
`GetAccelerationStructureHandle` returns `GetBufferHandle( ..., DescriptorTypeFlags::Buffer )`,
so on Direct3D 12 an acceleration structure is just another heap slot. On Vulkan it needs
`VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`, and whether that type may appear in a mutable
descriptor type list has **not** been checked here. No current shader reads an acceleration
structure from the heap, so this blocks nothing before raytracing. Settle it with the validation
layers on, in the `Tester` harness, before writing P5.16.

**Alternatives rejected:**

- `-fspv-use-descriptor-heap`, the native `SPV_EXT_descriptor_heap` path. DXC itself reports its
  support as incomplete on these shaders.
- Leaving the heap bindings to DXC's lazy allocation. The heap binding then depends on how many
  registers the individual shader used, so no two shaders agree, and the backend has nothing
  fixed to bind against.
- `-fvk-bind-register` to put the heaps in set 0 and the root parameters in set 1. Correct set
  ordering, but it requires an exhaustive register table and fails on any unregistered resource.
  See the rationale above.
- One descriptor set for everything. A push descriptor set layout cannot also hold the
  unbounded, update-after-bind heap arrays, so the root parameters and the heaps have to be in
  different sets whichever way round they go.

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

### 2026-08-28 - P4.4 has nothing to replace, and P4.2 belongs to Phase 5

**Context:** P4.4 is written as the largest task in the phase: "replace `ID3D12ShaderReflection`
with SPIRV-Reflect", "this is the real work", "the one place in the port where a new abstraction
is the right call". It describes `ShaderReflection_ShaderCompiler.h` as exposing reflection
results in D3D terms and `ShaderReflection_ShaderInputReflector.cpp` as consuming them.

**None of that is true of this codebase.** Checked before starting the work:

- Every use of `ID3D12ShaderReflection` in the repository is in `RHI_Direct3D12.cpp`, lines 1007
  to 1096. That file is in `Exclusions.txt` and is Phase 5's business. The Reflector only
  *includes* `d3d12shader.h`, and never calls anything from it. Phase 3 noticed the same thing
  and left a comment saying so at `ShaderReflection_ShaderCompiler.h:10`.
- `ShaderCompiler` exposes no reflection results at all. Its members are a task system, two
  paths, a mutex and some strings.
- `ReflectedShader` is entirely platform-neutral: strings, paths, byte vectors, and a
  `ParameterInfo` of type, name, stride and offset.
- `m_parameters` and `m_resourceTable` are filled by `ShaderReflection_ShaderParser.cpp`, which
  **parses the `.esh` source text**. `ShaderInputReflector` reads only those and writes C++
  structs. No reflection API is involved on either side.

So the pipeline never reflects bytecode, and there is no D3D-shaped intermediate to replace. The
platform-neutral structure P4.4 asks for already exists, and is called `ReflectedShader`.

**P4.2 follows from this.** Nothing in Phase 4 would consume SPIRV-Reflect. The only code that
reflects compiled shader bytecode is the Direct3D 12 backend, building root signature and
descriptor information at runtime. Its Vulkan sibling is what needs SPIRV-Reflect, so the
dependency belongs to **Phase 5**, not here. Vendoring it now would pin a library with no caller.

**Decision:** P4.4 is closed as not applicable, and P4.2 is moved to Phase 5. Both are marked in
[Phase4-ShaderPipeline.md](Phases/Phase4-ShaderPipeline.md). This removes what the plan called
the bulk of the phase, so what is really left in Phase 4 is the five shaders, the two decisions,
and P4.5 and P4.6.

### 2026-08-28 - Defect 2 stops here, because the fix needs the binding model decision

**Superseded, same day. This entry is kept for the analysis, but its conclusions are wrong on
two counts and the defect is fixed.** See "P4.3 Defect 2 is fixed" above for what was actually
required.

- **The fix does not need the binding model decision.** No `.esh` or `.esf` file in this
  repository uses `IncrementCounter`, `DecrementCounter`, `AppendStructuredBuffer` or
  `ConsumeStructuredBuffer`; `AppendBuffer<T>` carries its own explicit `RWBuffer<uint>` counter.
  DXC emits the counter array only when a counter is used, so no shader here produces a
  `counter_var` at all, and there is no counter heap to bind or to number.
- **No destination-side work was needed, and counters-inside-structures did not have to be
  implemented.** The claim below that the destination "must have a counter to store into" and
  that the fix therefore needs the work `DeclResultIdMapper.cpp:1289` calls non-trivial is wrong.
  The destination already has a counter, created eagerly by `createFieldCounterVars`. Only the
  **source** failed to resolve, for three reasons rather than the two identified here, and the
  third one - that the counter is resolved before the right-hand side is emitted - is why the
  source-side half looked correct and still did not work.

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
2. ~~Have DXC skip the association when the source is a heap access with no counter, instead of
   erroring.~~ **Tried on 2026-08-28, and it is wrong. Do not repeat it.** See below.
3. Restructure `AppendBuffer.esh` to keep the `RWStructuredBuffer` out of the struct. Forbidden by
   Phase 4 rule 4, changes Windows, and **fixes only three of the five** anyway, because the
   other two come out of the Reflector's code generator. Escalate before anyone tries it.

**Why option 2 is struck, in detail.** Skipping the association compiles all five shaders and
produces a module that **passes `spirv-val`** and is silently wrong. The counter resolves to
`OpUndef`:

```
%21 = OpUndef %_ptr_StorageBuffer_type_ACSBuffer_counter
%23 = OpAccessChain %_ptr_StorageBuffer_int %21 %uint_0
%24 = OpAtomicIAdd %int %23 %uint_1 %uint_0 %int_1        <- atomic on an undefined pointer
```

Compare the initialization path, which is correct, and addresses the counter heap by the same
index as the resource:

```
%22 = OpAccessChain %_ptr_StorageBuffer_int %counter_var_ResourceDescriptorHeap %uint_1 %uint_0
```

So "make assignment behave like initialization" is the wrong description of the fix.
Initialization does not skip anything: it resolves the counter properly. A green `spirv-val` is
not evidence here, which is worth remembering for the rest of this phase.

**What a correct fix needs, and why it is not small.** Two halves:

- *Source side.* `getFinalACSBufferCounter` must recognise the heap, using `isDescriptorHeap` and
  before the `AssocCounter#1` early return, as described above. This half is written, correct,
  and about fifteen lines.
- *Destination side.* The destination must have a counter to store into. It does not: a local
  `RWStructuredBuffer`, or a field of a local struct, gets no counter alias. With only the source
  half applied, the mismatch check still fails. This half is the work.

And for this engine the destination is the hard case. `AppendBuffer<T>` is a **struct containing a
structured buffer**, which DXC calls out as unsupported in its own source, at
`DeclResultIdMapper.cpp:1289`:

> Any kind of structured buffer has associated counters. The current DXC code is not written in a
> way to place associated counters inside a structure. Changing this behavior is non-trivial.
> There's also significant work to be done both in DXC (to properly generate binding numbers for
> the resource and its associated counters at correct offsets) and in spirv-opt (to flatten such
> structures and modify the binding numbers accordingly).

So unblocking these five shaders means implementing counters-inside-structures in DXC, alongside
the binding model decision. That is a real project, not a patch, and it is the point at which
[AGENTS.md](../../AGENTS.md) says to move the DXC work to a fork.

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
| 3 | ~~Use `volk`, or the plain Vulkan loader?~~ | Phase 5 | **answered: the plain loader.** Nothing is profiled yet, and nothing can be until the backend renders |
| 4 | ~~Do the target distros package SDL3, or must we always build it?~~ | Phase 6 | **answered 2026-08-29: always build it.** Ubuntu 24.04 LTS has no SDL3 package; it arrives from 25.04. `DownloadDependencies.sh` builds `release-3.4.14` from source |
| 5 | ~~Does `GameNetworkingSockets` block the first `Base` link?~~ | Phase 1 | **answered: yes, and at compile time, not link** |
| 6 | ~~Does the `VirtualAlloc` region in `Memory.cpp` have a working non-Windows path?~~ | Phase 1 | **answered: no** |
| 7 | ~~How do the engine's indirect draws reach Vulkan?~~ | Phase 5, and the whole frame | **answered 2026-08-29: the shader reads its own command's root data out of the argument buffer, indexed by `DrawIndex`.** Scheduled as P5.17, after Phase 6 bring-up. See the decision entry |
| 8 | How does `Buffer<uint64_t>` reach Vulkan? | Phase 6, and the whole frame | **open, raised 2026-08-31.** DXC emits a 64-bit sampled image; the RHI creates the view as `RG32_UInt` because that is what Direct3D 12 wants, and Mesa refuses the type. Six shader sites, one of them in every material pixel shader. See the P6.8 entry |

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

### `Engine::Shutdown` crashes when `Engine::Initialize` failed

Found during the P6.7 bring-up, and true on both platforms by inspection. A failed `Initialize`
leaves `RenderSystem` unconstructed, and `Shutdown` calls `RenderSystem::WaitAllQueuesIdle`
anyway, which dereferences a null queue. It only shows on a failed start, so it hides the real
error behind a segfault. `Engine::m_initializationStageReached` already records how far the start
got, and `Shutdown` could tear down only what that stage covers.

### `RHI.h:1044` - `LoadAction` defaults to discarding every attachment

`LoadAction` is zero initialised, `LoadActionType::DontCare` is zero and `StoreActionType::DontCare`
is zero, so every action a caller does not set says "discard". That is harmless on Direct3D 12,
which has no load or store actions and preserves a bound render target either way, and it is not
harmless on any backend that honours them.

**No engine pass sets a store action at all**, and `RenderPass_DebugDraw.cpp:1316` builds a
`LoadAction` that sets only the depth action and then binds the frame's final colour target with
it at `:1358`. On a backend that honours the values, the first discards every render pass output
in the frame and the second discards the rendered frame.

The Vulkan backend maps both `DontCare` values to preserve, and leaves `Clear`, `Load` and
`StoreActionType::None` exact. A caller that really wants an attachment left alone still has
`StoreActionType::None`. Worth raising upstream: the fix there is either a non-discarding default
or explicit actions at each call site.

### `RHI_Direct3D12.cpp:3981`, `:3978` and `:3969` - three faults in the raytracing path

None has ever run: nothing in the engine creates an acceleration structure.

- **`:3981`** has the line that fills in `Direct3D12AccelerationStructure::m_instanceBuffer`
  commented out, and `:3390` dereferences it during the top level build. That is a null pointer.
- **`:3978`** creates the top level structure buffer with `BufferFlags::NoDescriptors` and
  descriptor types `RWBuffer|Raw`, and `GetAccelerationStructureHandle` at `:4002` then asks it for
  a `DescriptorTypeFlags::Buffer` handle. Two asserts fire: one for the missing descriptor type and
  one for the missing handle.
- **`:3969`** sizes the scratch buffer from the bottom level prebuild alone and then reuses it for
  the top level build at `:3392`. It overruns whenever the top level needs more scratch, which is
  common.

The Vulkan backend does not reproduce any of the three. Each is written up at the line in
`RHI_Vulkan.cpp`.

### `RHI_Direct3D12.cpp:3490` - `CmdBeginQuery` calls `BeginQuery` on a timestamp query

`ID3D12GraphicsCommandList::BeginQuery` does not support `D3D12_QUERY_TYPE_TIMESTAMP`; a timestamp
is written by `EndQuery` alone. The reference switches on exactly that type and calls `BeginQuery`
for it, which the debug layer rejects.

Nothing in the engine calls `CmdBeginQuery`, so it has never run. The Vulkan backend does nothing
for a timestamp begin, which is what the reference effectively achieves minus the complaint.

### `RHI_Direct3D12.cpp:3714` - `CmdWriteDebugMarker` packs its auto flags two different ways

The `InOut` branch builds its flag from the enum's ordinal, `UINT( MarkerTypeFlags::In ) << 30`,
which is `1 << 30`. The single-marker branch builds it from the bit field, `markerType << 30`,
and `TBitFlags` converts to `1 << flagIndex`, so the same `In` becomes `2 << 30`. One of the two
is wrong and they cannot both be right.

Nothing in the engine calls `CmdWriteDebugMarker` and `DeviceCapabilities::m_breadcrumbs` is
`false` on both backends, so it costs nothing today. The Vulkan backend reproduces both branches
exactly, so the two write identical bytes; fixing it belongs upstream, next to a decision about
which one was meant.

### `RHI_Direct3D12.cpp:284` - `RGB565_UNorm` and `BGR565_UNorm` map to the same DXGI format

Both return `DXGI_FORMAT_B5G6R5_UNORM`. Under DXGI's naming, which lists a packed format's
components least significant first, that is the `BGR565` one; `RGB565` has no DXGI format at all.
Vulkan has both, so the Vulkan backend could tell them apart and chooses not to: mapping them
faithfully would make the two backends draw the same asset differently. Nothing in the engine uses
either format, so this costs nothing today. Recorded because a future reader will see two
`DataFormat` members reaching one `VkFormat` and assume it is a copy-paste slip.

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
