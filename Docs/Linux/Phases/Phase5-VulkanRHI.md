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
| `CmdExecuteIndirect`, `CommandSignature` | `VK_KHR_draw_indirect_count` (core 1.2), plus a compute pre-pass for multi-argument signatures |
| `SetDebugName` (9 overloads), `Cmd*DebugMarker` | `VK_EXT_debug_utils` |
| `RootSignature`, `CmdSetRootConstants` | `VkPipelineLayout` and push constants |
| `WaveOpsSupportFlags` | subgroup queries (core 1.1 and 1.2) |
| `BeginFrameCapture` and `EndFrameCapture` | RenderDoc in-app API, through `dlopen( "librenderdoc.so" )` |
| `GetTotalAllocatedDeviceMemory`, `ResourceAllocationStatistic`, `ReportDeviceMemoryLeaks` | VMA statistics |

`CmdExecuteIndirect` is the awkward one. Direct3D 12 command signatures can bind vertex buffers
and set root constants per draw, and Vulkan's indirect draws cannot. Read how the engine actually
uses it (`grep -rn 'CmdExecuteIndirect' Code/Engine`) before you design the workaround. The
engine may use draw-argument signatures only, in which case the mapping is direct.

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

---

## Bring-up strategy

**Do not** try to bring the backend up against the full engine frame. The engine's renderer
(`Code/Engine/Render/`, 55 files that call `RHI::`) exercises everything at once, and a
first-light failure there is nearly impossible to diagnose.

Use `Esoterica.Applications.Tester` as a harness, and build up:

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
   remains** in `RHI_Vulkan.cpp`.
2. `RHI.h` is unmodified. `git diff upstream/main -- Code/Base/Render/RHI.h` is empty.
3. `RHI_Direct3D12.cpp` is unmodified.
4. Bring-up steps 1 to 8 all pass in the `Tester` harness, and the tests are committed.
5. The full engine frame produces no Vulkan validation errors and no warnings.
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
