# Phase 5 - Vulkan RHI

**Goal:** a Vulkan implementation of `RHI.h` at full parity.

**Deliverable:** `Code/Base/Render/RHI_Vulkan.cpp` implements all of the roughly 110 `RHI.h`
functions for real, and replaces the Phase 1 stubs.

**Prerequisites:** Phases 1 to 4 complete. Phase 4's binding-model decisions are hard
prerequisites. Read them in [Progress.md](../Progress.md) before you write any code.

**Rough cost:** 3-5 months. This is the bulk of the project.

**Read first:** [00-Conventions.md](../00-Conventions.md),
[02-Architecture.md, Renderer](../02-Architecture.md#renderer), and
`Code/Base/Render/RHI_Direct3D12.cpp` in full. All 6,084 lines of it. It is the specification.

> ## Status: all sixteen groups are written and merged, and the backend has now **run**
>
> Phase 6 provided the entry point, and P6.6 and P6.7 executed this backend for the first time.
> **A scratch application cleared and presented twelve frames with no validation errors**, which
> covers `CreateContext`, queues, command pools and buffers, the swapchain, `CmdBarrier`,
> `CmdSetRenderTargets`, `CmdSetViewport`, `QueueSubmit`, `AcquireNextImage` and `QueuePresent`.
>
> **Running it found four defects in this phase's own code**, all now fixed. They are the best
> evidence available for how much of the rest is still unverified:
>
> | Found | What it was |
> |---|---|
> | P6.6 | `CreateSwapchain` asked only for RGBA surface formats. Every surface on Linux offers BGRA. |
> | P6.6 | `DestroySwapchain` freed command buffers after their pool. P5.4 assumed the other order. |
> | P6.6 | `MaxPendingFrames` was 2 and Linux drivers report a `minImageCount` of 3. |
> | P6.7 | `CreateContext` never enabled the 16-bit shader feature bits. Direct3D 12 has nothing to mirror. |
> | P6.8 | `CreateContext` missed five more shader feature bits, and mesh modules were created on devices without `VK_EXT_mesh_shader`. |
> | OQ8 | `CreateContext` never enabled `depthClamp`, which every non-clipping pass turns on. |
> | P5.4 | `BufferFlags::NoDescriptors` stripped a buffer's usage flags along with its heap slot. |
> | P5.9 | Barriers named tessellation and geometry stages the device never enabled. |
>
> **The `VK_ERROR_UNKNOWN` from `vkCreateComputePipelines` was not P5.7's.** It was
> `Buffer<uint64_t>`, now [answered](../Progress.md#open-questions) as `Buffer<uint2>`.
> **The engine reaches `CmdExecuteIndirect` with host validation on and zero validation
> messages, so P5.17 is the only wall left before a drawn frame** - and it can be written
> against a live engine.
>
> **Criteria 5 to 10 are now checkable.** They were not before.

---

## The one thing to get right first

`RHI.h` exposes a **monotonic counter** synchronization model:

```
QueueGetCurrentSemaphore    QueueGetCompletedSemaphore
QueueHostWait               QueueDeviceWait
```

This maps onto **Vulkan timeline semaphores** (`VK_KHR_timeline_semaphore`, core in 1.2), not
onto binary semaphores. Getting this mapping right decides whether the rest of the backend stays
clean or becomes a permanent fight. Do it before anything else, and check it in isolation.

The second such decision is the **bindless descriptor model**, which Phase 4 fixed. The backend
must agree exactly with what the shaders were compiled to expect. If Phase 4's recorded decision
turns out to be unworkable, that is a joint re-decision. Escalate. Do not diverge quietly.

## Ground rules

- **Do not modify `RHI.h`.** If you believe you must, escalate. The survey found no Direct3D
  types in it, so a genuine need means a concept is D3D-shaped in a way the survey missed. That
  deserves a human decision.
- **Do not modify `RHI_Direct3D12.cpp`.** It is the reference, and it must keep working.
- Full parity is the goal, including raytracing, mesh shaders, and variable rate shading.
- The baseline is **Vulkan 1.3**. Dynamic rendering, descriptor indexing, buffer device address,
  timeline semaphores, and synchronization2 are all core, which removes a lot of extension
  plumbing.

## Required Vulkan features

| `RHI.h` concept | Vulkan requirement |
|---|---|
| Bindless, `CmdSetRootParameter` | descriptor indexing and buffer device address (core 1.2) |
| `CmdSetRenderTargets` with load and store actions | dynamic rendering (core 1.3) |
| `QueueGet*Semaphore`, `Queue*Wait` | timeline semaphores (core 1.2) |
| `CmdBarrier` (3 overloads) | synchronization2 (core 1.3) |
| `CmdDispatchRays`, `CmdBuildAccelerationStructure`, `AccelerationStructure*` | `VK_KHR_ray_tracing_pipeline`, `VK_KHR_acceleration_structure`, `VK_KHR_deferred_host_operations` |
| `CmdDispatchMesh` | `VK_EXT_mesh_shader` |
| `CmdSetShadingRate`, `ShadingRateCombiner`, `ShadingRateCaps` | `VK_KHR_fragment_shading_rate` |
| `CmdExecuteIndirect`, `CommandSignature` | `VK_KHR_draw_indirect_count` (core 1.2). A signature that carries root data needs a shader change, not a pre-pass. See [open question 7](../Progress.md#open-questions) |
| `SetDebugName` (9 overloads), `Cmd*DebugMarker` | `VK_EXT_debug_utils` |
| `RootSignature`, `CmdSetRootConstants` | `VkPipelineLayout` and push constants |
| 16-bit types in the engine's shaders | `shaderInt16`, `shaderFloat16` and the 16-bit storage bits. **Added in P6.7**, after `vkCreateShaderModule` rejected a module for declaring `Int16`. Direct3D 12 has no equivalent step, so nothing here pointed at it |
| `WaveOpsSupportFlags` | subgroup queries (core 1.1 and 1.2) |
| `BeginFrameCapture` and `EndFrameCapture` | RenderDoc in-app API, through `dlopen( "librenderdoc.so" )` |
| `GetTotalAllocatedDeviceMemory`, `ResourceAllocationStatistic`, `ReportDeviceMemoryLeaks` | VMA statistics |

`CmdExecuteIndirect` is the awkward one, and P5.13 answered it. Direct3D 12 command signatures
can set root constants and bind root descriptors per command, and Vulkan's indirect draws cannot.
**Every engine signature carries root data**, so no engine call site takes the path that maps
directly. A compute pre-pass does not help, because a pre-pass cannot bind a descriptor either.
Both remaining shapes change the shaders, which is Phase 4's. This is
[open question 7](../Progress.md#open-questions), it is still open, and it blocks the frame.

---

## Task groups

The RHI is a flat free-function API in one namespace, which suits parallel work well. **Each
group below is an independent slice** with a clear reference implementation. Groups 1 to 3 must
land first. After that, several can run at the same time.

Keep the section-comment structure of `RHI.h` in `RHI_Vulkan.cpp`, so that the file stays
navigable past 6,000 lines.

### P5.1 - Device, context, and memory (foundation)

`CreateContext`, `DestroyContext`, `GetTotalAllocatedDeviceMemory`,
`GetDetailedMemoryStatistics`, `GetResourceAllocationStatistics`, `ReportDeviceMemoryLeaks`

Instance and device creation. Physical device selection that honors `DeviceSelectionPreference`
and `DeviceMode`. Filling in `DeviceCapabilities` and `DeviceVendorInfo`. VMA initialization.
Validation layers in non-Shipping builds. Decide `volk` against the plain loader here, which
answers [open question 3](../Progress.md#open-questions). Default to the plain loader.

### P5.2 - Queues and synchronization (foundation)

`CreateQueue`, `DestroyQueue`, `QueueGetCurrentSemaphore`, `QueueGetCompletedSemaphore`,
`QueueHostWait`, `QueueDeviceWait`, `QueueSubmit`, `QueuePresent`, `WaitQueueIdle`

Timeline semaphores. See the note at the top. Map `QueueType`, `QueueFlags`, and `QueuePriority`
onto Vulkan queue families. Handle the case where the device exposes no separate async-compute or
transfer family.

### P5.3 - Swapchain and presentation (foundation)

`CreateSwapchain`, `DestroySwapchain`, `AcquireNextImage`, `SetVSync`

`m_pNativeWindowHandle` is a `void*`. In Phase 5 you have no window yet, because Phase 6 provides
SDL3. **Bring this up headless first**, using an offscreen render target, then wire the real
surface in Phase 6. Do not block Phase 5 on Phase 6. The dependency runs the other way.

`SetVSync` maps to the present mode, `FIFO` against `MAILBOX` or `IMMEDIATE`, and it needs
swapchain recreation.

### P5.4 - Command pools and buffers

`Create/Destroy/ResetCommandPool`, `Create/DestroyCommandBuffer`, `Begin/EndCommandBuffer`

### P5.5 - Buffers

`CreateBuffer`, `DestroyBuffer`, `MapBuffer`, `UnmapBuffer`, `GetBufferHandle`,
`BufferSubAllocate`, `BufferSubDeallocate`

`GetBufferHandle` is the bindless handle, so this is where the Phase 4 binding model becomes
concrete. `ResourceMemoryType` maps to VMA usage flags. The `BufferSubAllocation`,
`PageAllocator`, and `HandleAllocator` helpers in `Code/Base/Render/` are platform-neutral, so
reuse them.

### P5.6 - Textures and samplers

`CreateTexture`, `DestroyTexture`, `GetTextureCopyRowStride`, `GetTextureHandle`,
`CreateSampler`, `DestroySampler`, `GetSamplerStateHandle`

The `DataFormat` enum (`RHI.h:75`, about 115 entries) needs a complete mapping to `VkFormat`.
`RHI.h` already implements `IsCompressedFormat`, `FormatBlockBitSize`, `FormatBlockWidth`,
`FormatBlockHeight`, `ComputeFormatRowStride`, `ComputeFormatNumRows`, and
`ComputeTextureMipLevels` platform-neutrally. Reuse them, and make sure your `VkFormat` mapping
agrees with them. A disagreement here corrupts textures in a way that looks like a bug somewhere
else.

`SamplerRange` and `SamplerModelConversion` imply YCbCr sampler support
(`VK_KHR_sampler_ycbcr_conversion`, core 1.1).

### P5.7 - Shaders, root signatures, pipelines

`CreateShader`, `DestroyShader`, `CreateRootSignature`, `DestroyRootSignature`,
`CreatePipeline` (4 overloads: graphics, compute, mesh, raytracing), `DestroyPipeline`,
`CreatePipelineCache`, `DestroyPipelineCache`, `GetPipelineCacheData`

`ShaderByteCode` now carries SPIR-V. `RootSignature` becomes a `VkPipelineLayout` plus descriptor
set layouts. `PipelineCache` maps to `VkPipelineCache`. Check `PipelineCacheFlags` against what
Vulkan actually offers.

The four `CreatePipeline` overloads match the four pipeline types. Implement graphics and compute
first. Mesh and raytracing can follow later without blocking Phase 6.

### P5.8 - Render pass and draw commands

`CmdSetRenderTargets`, `CmdSetViewport`, `CmdSetScissor`, `CmdSetStencilReference`,
`CmdSetPipeline`, `CmdSetRootConstants`, `CmdSetRootParameter`, `CmdSetIndexBuffer`, `CmdDraw`,
`CmdDrawInstanced`, `CmdDrawIndexed`, `CmdDrawIndexedInstanced`, `CmdDispatchCompute`

`LoadActionType` and `StoreActionType` map onto dynamic rendering's
`VkRenderingAttachmentInfo::loadOp` and `storeOp`. Apply the clip-space Y convention exactly as
Phase 4 recorded it, **once**, not twice.

### P5.9 - Barriers

`CmdBarrier` (3 overloads)

Use synchronization2. This is where a naive port loses most of its performance. Read the
Direct3D resource-state transitions carefully, and map the states to `VkPipelineStageFlags2` and
`VkAccessFlags2` pairs. Do not reach for `ALL_COMMANDS` and `MEMORY_READ|WRITE` everywhere.
Merely *correct* is fine at first. Note it for later tuning rather than over-engineering it up
front.

### P5.10 - Copies and clears

`CmdCopyBuffer`, `CmdCopyTexture` (2 overloads), `CmdClearTexture`, `CmdClearBuffer`

### P5.11 - Queries

`CreateQueryPool`, `DestroyQueryPool`, `GetQueryTimestampFrequency`, `CmdResetQueryPool`,
`CmdBeginQuery`, `CmdEndQuery`, `CmdResolveQuery`

`QueryType` covers timestamps, occlusion, and pipeline statistics. Vulkan reports a timestamp
period in nanoseconds per tick, and D3D12 reports a frequency. Invert accordingly.

### P5.12 - Debug utilities

`SetDebugName` (9 overloads), `CmdBeginDebugMarker`, `CmdEndDebugMarker`, `CmdWriteDebugMarker`,
`BeginFrameCapture`, `EndFrameCapture`, `CommandBufferMarkerScope`

Use `VK_EXT_debug_utils` for names and markers. Reach RenderDoc through
`dlopen( "librenderdoc.so" )`. The in-application API header is the same one that
`RHI_Direct3D12.cpp` already uses.

Do this group **early**, not last. Named objects and markers make every later group far easier to
debug, and the group is cheap.

### P5.13 - Indirect draws and command signatures

`CreateCommandSignature`, `DestroyCommandSignature`, `CmdExecuteIndirect`

See the note above on `IndirectArgumentType`.

**Half written, by decision.** `CreateCommandSignature` and `DestroyCommandSignature` are real,
and `CmdExecuteIndirect` handles a signature that carries only a draw or dispatch argument and
refuses the rest at the line. See the P5.13 entry in [Progress.md](../Progress.md).

**Open question 7 is now answered, and [P5.17](#p517---the-indirect-draw-shader-change---scheduled-not-started)
finishes this group.** Do not try to complete P5.13 on its own; the missing half is a shader
change.

### P5.14 - Mesh shaders

`CmdDispatchMesh`, plus the mesh `CreatePipeline` overload

### P5.15 - Variable rate shading

`CmdSetShadingRate`, and `ShadingRateCaps` reporting

### P5.16 - Raytracing

`CreateAccelerationStructure`, `GetAccelerationStructureHandle`,
`CmdBuildAccelerationStructure`, `CmdDispatchRays`, plus the raytracing `CreatePipeline` overload

This is the largest optional-feature group. `AccelerationStructureBuildFlags`,
`AccelerationStructureGeometryFlags`, and `AccelerationStructureInstanceFlags` map closely onto
their `VK_KHR_acceleration_structure` equivalents, so it is more mechanical than it looks.

### P5.17 - The indirect draw shader change - **scheduled, not started**

**This finishes P5.13 and unblocks the frame. It is the answer to open question 7, and the
approach is decided: the shader reads its own command's root data out of the argument buffer.**
Read the decision entry in [Progress.md](../Progress.md) before starting; it records why the two
alternatives were rejected.

**Phase 6 bring-up has happened and the door is open.** The engine builds every shader and
every pipeline, enters its frame loop, and halts in `CmdExecuteIndirect` - this refusal. It gets
there with **host validation on and no validation messages**, so P5.17 can be written against a
live engine with validation watching. **It is the last thing between this port and a drawn
frame.**

**Do this after Phase 6 bring-up, not before.** Nothing here can be tested until the engine runs,
and this is a change to shaders that Windows also compiles. Bring the window, the input and the
swapchain up first, then take this on with a live engine and RenderDoc in front of you.

#### The problem, in one picture

A Direct3D 12 command signature sets shader bindings per command as the GPU walks the argument
buffer. One material command is laid out like this:

```
[ root constants   40 bytes ]  set 0 binding b0 - per-draw data, different for every command
[ root CBV address  8 bytes ]  set 0 binding b1 - a GPU address, different for every command
[ dispatch args    12 bytes ]  VkDrawMeshTasksIndirectCommandEXT
```

`vkCmdDrawMeshTasksIndirectEXT` takes a stride, so it reads the last block correctly and can do
nothing at all with the first two. **No Vulkan indirect draw rebinds a descriptor per command.**

#### The fix

**Turn the push into a pull.** Instead of the command processor writing the root data into the
shader, the shader reads its own command out of the argument buffer, using its command index.
Vulkan supplies that index as the `DrawIndex` builtin.

Two facts were checked before choosing this, because every engine indirect draw is a *mesh*
dispatch rather than a classic vertex draw:

- The bundled SPIR-V validator allows `DrawIndex` in `MeshEXT` and `TaskEXT`, not only `Vertex`
  (`External/DirectXShaderCompiler_src/external/SPIRV-Tools/source/val/validate_builtins.cpp:4009`).
- DXC accepts `[[vk::builtin( "DrawIndex" )]]` and special-cases mesh and amplification inputs to
  allow it (`.../tools/clang/lib/SPIRV/DeclResultIdMapper.cpp:3508`).

`DrawIndex` needs `VK_KHR_shader_draw_parameters`, which is core in Vulkan 1.1. **No new device
requirement, and no new extension.**

#### The shape that fits what is already there

This is a sketch, not a specification. Read the code and improve on it.

1. **The pipeline layout gains a push constant range.** `RHI_Vulkan.cpp` uses **no push constants
   at all** today - `CmdSetRootConstants` and `CmdSetRootParameter` are push-descriptor writes
   into set 0 - so the whole range is free. Put the argument buffer's device address, the
   signature's stride, and the byte offset of the root block in it.
2. **`CmdExecuteIndirect` pushes that block, then draws.** It already knows all three numbers:
   `CreateCommandSignature` records the stride and the offsets, and the argument buffer is a
   parameter. **No change to `CreateCommandSignature`, and no change to `EngineShader.cpp`** - the
   signature is still the description of the buffer layout, which is what both backends need.
3. **The shader reads its root data from `argumentBufferAddress + DrawIndex * stride +
   rootOffset`.** A buffer device address deref, which the bindless model already relies on.
4. **Hide all of it in the `RHI.esh` macros.** `EE_DECLARE_ROOT_CONSTANTS` and
   `EE_DECLARE_ROOT_CBV` (`RHI.esh:124` and `:125`) expand to `ConstantBuffer<T> RootConstants`
   and `ConstantBuffer<T> RootCBV`. Add indirect variants that expand to the buffer read on
   SPIR-V, keeping the names `RootConstants` and `RootCBV`, so **shader bodies do not change at
   all** - only the declaration line at the top of each file.

#### Guard it on `__spirv__`, so Windows is untouched

DXC defines `__spirv__` when it targets SPIR-V
(`.../tools/clang/lib/Frontend/InitPreprocessor.cpp:403`), and it is the only guard needed. The
`#else` branch is the declaration that is there today, so **the Direct3D 12 path keeps its command
signature and compiles to identical DXIL.** That matters twice: the Windows build must keep
working, and these are upstream files, so the smaller and more mechanical the diff, the cheaper
the next upstream merge.

`DrawIndex` has no Direct3D 12 equivalent for a mesh shader, which is exactly why the two backends
keep different paths here rather than converging on one.

#### The shaders that change

Only the ones reached by an indirect draw. Every other shader keeps `CmdSetRootConstants` and is
not touched:

| Shader | Reached by |
|---|---|
| `Renderer/DefaultMeshShader.esh` | The material path. `MaterialShader` builds a `DispatchMesh` signature at `EngineShader.cpp:139`, and forward shading and cascaded shadows execute it. |
| `Debug/DebugDrawMesh.esf`, `Debug/DebugDraw.esf` | `SurfaceShader`, which builds `Draw`, `DrawIndexed` and `DispatchMesh` signatures at `EngineShader.cpp:208` to `:216`. `RenderPass_DebugDraw` executes them six times. |
| `Renderer/ClusterCulling.esf` | The one indirect **compute** dispatch, at `Renderer_ForwardShading.cpp:794`. See below; it is the hard one. |

`BucketResolve.esf` and `InstanceCulling.esf` **write** argument buffers and are dispatched
directly. Their layouts (`DrawArgument`, `ClusterCullingArgument` in `RendererTypes.esh`) are the
contract this change reads, so read them, but they should not need to change.

#### Indirect compute is the part that does not fall out for free

**`DrawIndex` does not exist in a compute shader, and `vkCmdDispatchIndirect` runs exactly one
dispatch and reads no counter buffer.** `Renderer_ForwardShading.cpp:794` needs both: it passes
`maxNumCommands` of `clusterCapacity / 64` and a counter buffer, and `InstanceCulling.esf:210`
appends one command per thread group that found visible clusters, each with its own cluster range
in its root constants. `CmdExecuteIndirect` refuses this case at the line today, and it is the
only engine call site that hits that refusal.

Two candidate answers. **Decide between them with the engine running, not now:**

| Approach | What it costs |
|---|---|
| **Record `maxNumCommands` separate `vkCmdDispatchIndirect` calls**, each at its own offset, pushing the command index as the push constant that `DrawIndex` supplies for a draw. | Records a lot of commands that usually do nothing. Needs the argument buffer zeroed each frame so an unused slot dispatches `(0,0,0)`, which is a legal no-op - **check whether the engine already clears it, and escalate if it does not**, because that is an engine-side change. |
| **Flatten to one dispatch.** A pre-pass computes a prefix sum of the per-command group counts, one indirect dispatch covers the total, and each group works out which command it belongs to. | More shader work in `ClusterCulling.esf`, and a pre-pass to write. Repacking is something a compute pre-pass *can* do; binding a descriptor is not. Scales properly. |

Start with the first. It is a few lines and it makes the frame draw. Move to the second only if a
capture says the empty dispatches cost something.

#### This edits upstream files, which normally means escalate

**It was escalated, and the human approved it on 2026-08-29.** `RHI.esh` and the four shader files
above are upstream files, and Conventions rule 3 and the escalation list in
[/AGENTS.md](../../AGENTS.md) both say to stop before editing one that
[TouchedFiles.md](../TouchedFiles.md) does not list. They are on the registry now, marked
`planned`, with the guard shape that keeps each diff small. **Keep every edit inside an
`#ifdef __spirv__`.** A change that alters the Direct3D path has broken the prime directive, not
just the Windows build.

#### Done when

1. `CmdExecuteIndirect` has no `EE_UNIMPLEMENTED_FUNCTION` left, which takes
   `RHI_Vulkan.cpp` to 2 markers, both of them unreachable-caller markers.
2. `./CompileShaders.sh` exits 0, and all 46 stages pass `spirv-val --target-env vulkan1.3
   --scalar-block-layout` with the validator in `External/DirectXShaderCompiler/bin/x64/`.
3. The Windows MSBuild build still succeeds and the DXIL is unchanged. `git diff` on the shader
   files shows nothing outside an `#ifdef __spirv__`.
4. The engine frame draws geometry on Linux, with validation layers on and no errors.
5. A Windows screenshot and a Linux screenshot of the same scene are compared, and every
   difference is listed with an explanation. This is Phase 5 criteria 7 and 8, and this task is
   what makes them checkable.

**Mesh shader hardware is required to check items 4 and 5.** Neither GPU in the current
development machine has `VK_EXT_mesh_shader`, and the engine's whole geometry path is mesh
shaders, so this needs different hardware or a long wait on `llvmpipe`.

---

## Bring-up strategy

**Do not** try to bring the backend up against the full engine frame. The engine's renderer
(`Code/Engine/Render/`, 55 files that call `RHI::`) exercises everything at once, and a
first-light failure there is nearly impossible to diagnose.

**The `Tester` harness in this section does not exist.** `Code/Applications/Tester/Main.cpp` is
114 lines of upstream scratch code, and no Linux binary can reach `RHI::CreateContext` before
Phase 6. Read the ladder below as the order to *implement* in, not as a thing that runs today.
The engine's own initialisation order matches it: `RenderSystem::Initialize` runs before any
window handle is needed, so steps 1 to 8 map onto the real engine when Phase 6 arrives.

The ladder, in the order the phase document intended it to run:

1. Create the context, enumerate the device, report the capabilities. No rendering.
2. Timeline semaphore round-trip. Submit an empty command buffer, and wait on the host.
3. Buffer create, map, write, read back.
4. Texture create, clear, copy to a readback buffer, and check the pixels on the CPU.
5. Compute dispatch that writes a known pattern to a buffer. Check it.
6. Offscreen graphics. Clear a render target, and check it.
7. Offscreen triangle, with a real SPIR-V shader from Phase 4.
8. Bindless. A descriptor array indexed from a shader. **This is the highest-risk step.** It
   validates the Phase 4 binding model end to end. Do not go past it on an unverified assumption.
9. Swapchain present. This needs Phase 6's window, so this is where the two phases meet.
10. The real engine frame.

Keep validation layers on throughout, and treat any validation error as a build break. They catch
most of the bugs this phase can produce, and far more cheaply than debugging visual artifacts.

---

## Acceptance criteria

1. Every function in `RHI.h` has a real implementation. **No `EE_UNIMPLEMENTED_FUNCTION()`
   remains** in `RHI_Vulkan.cpp`. **Not met: 3 remain, down from 103, and none is a whole
   function.** One is the indirect refusal from open question 7. The other two are markers that
   name a caller if one ever appears: a sampler border colour that needs
   `VK_EXT_custom_border_color`, and the static-sampler path the binding model does not use.
2. ~~`RHI.h` is unmodified.~~ **No longer met, deliberately.** `git diff --stat upstream/main --
   Code/Base/Render/RHI.h` reports `4 ++++`: `MaxPendingFrames` is 3 on Linux, in a
   `#if defined( __linux__ )` branch that leaves the Windows value verbatim. Linux drivers report
   a swapchain `minImageCount` of 3 and the backend cannot absorb that. Escalated, approved and
   registered in [TouchedFiles.md](../TouchedFiles.md) during P6.6. **The spirit of the criterion
   still holds: no `RHI.h` concept changed, and Windows is bit for bit unchanged.**
3. `RHI_Direct3D12.cpp` is unmodified.
4. ~~Bring-up steps 1 to 8 all pass in the `Tester` harness, and the tests are committed.~~
   **Cannot be met as written**, and it is now partly met against the real engine instead.
   `Esoterica.Applications.Tester` is an upstream scratchpad, not a test framework. **P6.6 ran
   steps 1 to 7 for real**: a scratch application built a context, queues, command pools and
   buffers, a swapchain with a real surface, and cleared and presented twelve frames with no
   validation errors. See the 2026-08-28 decision entry and the P6.6 entry in
   [Progress.md](../Progress.md).
5. The full engine frame produces no Vulkan validation errors and no warnings. **Blocked on
   [open question 8](../Progress.md#open-questions)**, and on hardware; see the P6.8 entry. The
   Ubuntu 24.04 validation layers are usable after all, with
   `VK_KHRONOS_VALIDATION_DEBUG_DISABLE_SPIRV_VAL=true`.
6. All 26 shaders from Phase 4 load and execute.
7. The full engine frame renders correctly, checked against Windows Direct3D 12 screenshots of
   the same scene. List any visual difference, with an explanation.
8. Feature parity is demonstrated for forward shading, cascaded shadows, GTAO, SMAA, OIT, mesh
   picking, and debug draw. Name each one, and verify each one.
9. RenderDoc capture works on Linux, through `BeginFrameCapture` and `EndFrameCapture`.
10. `ReportDeviceMemoryLeaks` reports zero leaks after a clean shutdown.
11. **The Windows MSBuild build still succeeds**, and the Direct3D 12 renderer is unchanged.

Criteria 7 and 8 are the ones that matter, and no tool can check them. Report them honestly.
"GTAO renders, but with visible banding that Direct3D does not show" is useful. "Feature parity
achieved", when it is not, is worse than useless.

## Do not

- Modify `RHI.h` or `RHI_Direct3D12.cpp`.
- Diverge from Phase 4's recorded binding model without escalating.
- Apply clip-space Y inversion if Phase 4 already did it in the shader compiler.
- Bring up against the full engine frame before the `Tester` harness passes step 8.
- Turn off validation layers to make progress.
- Keep `ALL_COMMANDS` barriers as a permanent solution. If you use them as a temporary measure,
  record every site in [Progress.md](../Progress.md).
- Mark this phase complete with stubs left. Report exactly which groups are done.

## Notes for the next agent

Phase 6 needs two things from this phase:

- The exact surface-creation requirement, so that SDL3 creates a compatible window.
- Whether the RHI or the application drives swapchain recreation on resize.

Record group-by-group completion status in [Progress.md](../Progress.md). This phase spans months
and many sessions, and "which of the 16 groups are real" is the single most important piece of
state.
