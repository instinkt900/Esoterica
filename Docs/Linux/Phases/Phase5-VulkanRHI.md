# Phase 5 — Vulkan RHI

**Goal:** a full-parity Vulkan implementation of `RHI.h`.

**Deliverable:** `Code/Base/Render/RHI_Vulkan.cpp` implements all ~110 `RHI.h` functions for
real, replacing the Phase 1 stubs.

**Prerequisites:** Phases 1–4 complete. Phase 4's binding-model decisions are hard
prerequisites — read them in [Progress.md](../Progress.md) before writing any code.

**Rough cost:** 3–5 months. This is the bulk of the project.

**Read first:** [00-Conventions.md](../00-Conventions.md),
[02-Architecture.md § Renderer](../02-Architecture.md#renderer), and
`Code/Base/Render/RHI_Direct3D12.cpp` in full. All 6,084 lines of it. It is the specification.

---

## The one thing to get right first

`RHI.h` exposes a **monotonic counter** synchronisation model:

```
QueueGetCurrentSemaphore    QueueGetCompletedSemaphore
QueueHostWait               QueueDeviceWait
```

This maps onto **Vulkan timeline semaphores** (`VK_KHR_timeline_semaphore`, core in 1.2), not
onto binary semaphores. Getting this mapping right determines whether the rest of the backend is
clean or a permanent fight. Do this before anything else, and validate it in isolation.

The second such decision is the **bindless descriptor model**, which was fixed in Phase 4. The
backend must agree exactly with what the shaders were compiled to expect. If Phase 4's recorded
decision turns out to be unworkable, that is a joint re-decision — escalate; do not silently
diverge.

## Ground rules

- `RHI.h` is **not modified**. If you believe it must be, escalate. The survey found zero
  Direct3D types in it, so a genuine need indicates a concept that is D3D-shaped in a way the
  survey missed — that is worth a human decision.
- `RHI_Direct3D12.cpp` is **not modified**. It is the reference, and it must keep working.
- Full parity is the goal, including raytracing, mesh shaders, and variable rate shading.
- Baseline **Vulkan 1.3** — dynamic rendering, descriptor indexing, buffer device address,
  timeline semaphores, and synchronization2 are all core, which removes a lot of extension
  plumbing.

## Required Vulkan features

| `RHI.h` concept | Vulkan requirement |
|---|---|
| Bindless, `CmdSetRootParameter` | descriptor indexing + buffer device address (core 1.2) |
| `CmdSetRenderTargets` + load/store actions | dynamic rendering (core 1.3) |
| `QueueGet*Semaphore`, `Queue*Wait` | timeline semaphores (core 1.2) |
| `CmdBarrier` (3 overloads) | synchronization2 (core 1.3) |
| `CmdDispatchRays`, `CmdBuildAccelerationStructure`, `AccelerationStructure*` | `VK_KHR_ray_tracing_pipeline`, `VK_KHR_acceleration_structure`, `VK_KHR_deferred_host_operations` |
| `CmdDispatchMesh` | `VK_EXT_mesh_shader` |
| `CmdSetShadingRate`, `ShadingRateCombiner`, `ShadingRateCaps` | `VK_KHR_fragment_shading_rate` |
| `CmdExecuteIndirect`, `CommandSignature` | `VK_KHR_draw_indirect_count` (core 1.2), plus a compute pre-pass for multi-argument signatures |
| `SetDebugName` ×9, `Cmd*DebugMarker` | `VK_EXT_debug_utils` |
| `RootSignature`, `CmdSetRootConstants` | `VkPipelineLayout` + push constants |
| `WaveOpsSupportFlags` | subgroup queries (core 1.1/1.2) |
| `BeginFrameCapture` / `EndFrameCapture` | RenderDoc in-app API via `dlopen( "librenderdoc.so" )` |
| `GetTotalAllocatedDeviceMemory`, `ResourceAllocationStatistic`, `ReportDeviceMemoryLeaks` | VMA statistics |

`CmdExecuteIndirect` is the awkward one — Direct3D 12's command signatures can bind vertex
buffers and set root constants per-draw, which Vulkan's indirect draws cannot. Read how the
engine actually uses it (`grep -rn 'CmdExecuteIndirect' Code/Engine`) before designing the
workaround; the engine may only use draw-argument signatures, in which case the mapping is
direct.

---

## Task groups

The RHI is a flat free-function API in one namespace, which makes it unusually well suited to
parallel work: **each group below is an independently implementable slice** with a clear reference
implementation. Groups 1–3 must land first; after that, several can proceed concurrently.

Keep the section-comment structure of `RHI.h` in `RHI_Vulkan.cpp` so the file stays navigable at
6,000+ lines.

### P5.1 — Device, context, and memory (foundation)
`CreateContext`, `DestroyContext`, `GetTotalAllocatedDeviceMemory`,
`GetDetailedMemoryStatistics`, `GetResourceAllocationStatistics`, `ReportDeviceMemoryLeaks`

Instance and device creation, physical device selection honouring
`DeviceSelectionPreference` and `DeviceMode`, populating `DeviceCapabilities` and
`DeviceVendorInfo`, VMA initialisation, validation layers in non-Shipping builds. Decide
`volk`-vs-loader here ([open question 3](../Progress.md#open-questions)); default to the plain
loader.

### P5.2 — Queues and synchronisation (foundation)
`CreateQueue`, `DestroyQueue`, `QueueGetCurrentSemaphore`, `QueueGetCompletedSemaphore`,
`QueueHostWait`, `QueueDeviceWait`, `QueueSubmit`, `QueuePresent`, `WaitQueueIdle`

Timeline semaphores. See the note at the top. Map `QueueType`, `QueueFlags`, and
`QueuePriority` onto Vulkan queue families, handling the case where the device does not expose a
distinct async-compute or transfer family.

### P5.3 — Swapchain and presentation (foundation)
`CreateSwapchain`, `DestroySwapchain`, `AcquireNextImage`, `SetVSync`

`m_pNativeWindowHandle` is a `void*`. In Phase 5 you have no window yet — Phase 6 provides SDL3.
**Bring this up headless first** using an offscreen render target, then wire the real surface in
Phase 6. Do not block Phase 5 on Phase 6; the dependency runs the other way.

`SetVSync` maps to present mode (`FIFO` vs `MAILBOX`/`IMMEDIATE`) and requires swapchain
recreation.

### P5.4 — Command pools and buffers
`Create/Destroy/ResetCommandPool`, `Create/DestroyCommandBuffer`,
`Begin/EndCommandBuffer`

### P5.5 — Buffers
`CreateBuffer`, `DestroyBuffer`, `MapBuffer`, `UnmapBuffer`, `GetBufferHandle`,
`BufferSubAllocate`, `BufferSubDeallocate`

`GetBufferHandle` is the bindless handle — this is where the Phase 4 binding model becomes
concrete. `ResourceMemoryType` maps to VMA usage flags. `BufferSubAllocation` and the
`PageAllocator`/`HandleAllocator` helpers in `Code/Base/Render/` are platform-neutral and reused.

### P5.6 — Textures and samplers
`CreateTexture`, `DestroyTexture`, `GetTextureCopyRowStride`, `GetTextureHandle`,
`CreateSampler`, `DestroySampler`, `GetSamplerStateHandle`

The `DataFormat` enum (`RHI.h:75`, ~115 entries) needs a complete mapping to `VkFormat`. The
helpers `IsCompressedFormat`, `FormatBlockBitSize`, `FormatBlockWidth`, `FormatBlockHeight`,
`ComputeFormatRowStride`, `ComputeFormatNumRows`, `ComputeTextureMipLevels` are already
implemented platform-neutrally in `RHI.h` — reuse them, and make sure your `VkFormat` mapping
agrees with them. A disagreement here produces corrupt textures that look like a bug elsewhere.

`SamplerRange` and `SamplerModelConversion` imply YCbCr sampler support
(`VK_KHR_sampler_ycbcr_conversion`, core 1.1).

### P5.7 — Shaders, root signatures, pipelines
`CreateShader`, `DestroyShader`, `CreateRootSignature`, `DestroyRootSignature`,
`CreatePipeline` ×4 (graphics, compute, mesh, raytracing), `DestroyPipeline`,
`CreatePipelineCache`, `DestroyPipelineCache`, `GetPipelineCacheData`

`ShaderByteCode` now carries SPIR-V. `RootSignature` becomes `VkPipelineLayout` plus descriptor
set layouts. `PipelineCache` maps to `VkPipelineCache`; `PipelineCacheFlags` needs checking
against what Vulkan actually offers.

The four `CreatePipeline` overloads correspond to the four pipeline types — implement graphics and
compute first; mesh and raytracing can follow later without blocking Phase 6.

### P5.8 — Render pass and draw commands
`CmdSetRenderTargets`, `CmdSetViewport`, `CmdSetScissor`, `CmdSetStencilReference`,
`CmdSetPipeline`, `CmdSetRootConstants`, `CmdSetRootParameter`, `CmdSetIndexBuffer`,
`CmdDraw`, `CmdDrawInstanced`, `CmdDrawIndexed`, `CmdDrawIndexedInstanced`,
`CmdDispatchCompute`

`LoadActionType` / `StoreActionType` map onto dynamic rendering's
`VkRenderingAttachmentInfo::loadOp`/`storeOp`. Apply the clip-space Y convention exactly as
Phase 4 recorded — **once**, not twice.

### P5.9 — Barriers
`CmdBarrier` ×3

Use synchronization2. This is where a naive port loses most of its performance; read the
Direct3D resource-state transitions carefully and map states to
`VkPipelineStageFlags2`/`VkAccessFlags2` pairs rather than reaching for
`ALL_COMMANDS`/`MEMORY_READ|WRITE` everywhere. Getting this merely *correct* is fine initially;
note it for later tuning rather than over-engineering up front.

### P5.10 — Copies and clears
`CmdCopyBuffer`, `CmdCopyTexture` ×2, `CmdClearTexture`, `CmdClearBuffer`

### P5.11 — Queries
`CreateQueryPool`, `DestroyQueryPool`, `GetQueryTimestampFrequency`,
`CmdResetQueryPool`, `CmdBeginQuery`, `CmdEndQuery`, `CmdResolveQuery`

`QueryType` covers timestamps, occlusion, and pipeline statistics. Note Vulkan reports timestamp
period in nanoseconds per tick, whereas D3D12 reports a frequency — invert appropriately.

### P5.12 — Debug utilities
`SetDebugName` ×9, `CmdBeginDebugMarker`, `CmdEndDebugMarker`, `CmdWriteDebugMarker`,
`BeginFrameCapture`, `EndFrameCapture`, `CommandBufferMarkerScope`

`VK_EXT_debug_utils` for names and markers. RenderDoc via `dlopen( "librenderdoc.so" )` —
the in-application API header is the same one `RHI_Direct3D12.cpp` already uses.

Do this group **early**, not last. Named objects and markers make every subsequent group
dramatically easier to debug, and the group is cheap.

### P5.13 — Indirect draws and command signatures
`CreateCommandSignature`, `DestroyCommandSignature`, `CmdExecuteIndirect`

See the note above on `IndirectArgumentType`.

### P5.14 — Mesh shaders
`CmdDispatchMesh`, plus the mesh `CreatePipeline` overload

### P5.15 — Variable rate shading
`CmdSetShadingRate`, `ShadingRateCaps` reporting

### P5.16 — Raytracing
`CreateAccelerationStructure`, `GetAccelerationStructureHandle`,
`CmdBuildAccelerationStructure`, `CmdDispatchRays`, plus the raytracing `CreatePipeline`
overload

The largest optional-feature group. `AccelerationStructureBuildFlags`,
`AccelerationStructureGeometryFlags`, and `AccelerationStructureInstanceFlags` map closely onto
their `VK_KHR_acceleration_structure` equivalents, so this is more mechanical than it looks.

---

## Bring-up strategy

**Do not** try to bring the backend up against the full engine frame. The engine's renderer
(`Code/Engine/Render/`, 55 files calling `RHI::`) exercises everything at once, and a first-light
failure there is nearly undiagnosable.

Instead use `Esoterica.Applications.Tester` as a harness and escalate:

1. Create context, enumerate device, report capabilities. No rendering.
2. Timeline semaphore round-trip — submit an empty command buffer, wait on the host.
3. Buffer create / map / write / read back.
4. Texture create, clear, copy to a readback buffer, verify pixels on the CPU.
5. Compute dispatch writing a known pattern to a buffer; verify.
6. Offscreen graphics: clear a render target, verify.
7. Offscreen triangle with a real SPIR-V shader from Phase 4.
8. Bindless: a descriptor array indexed from a shader. **This is the highest-risk step** — it
   validates the Phase 4 binding model end to end. Do not proceed past it on an unverified
   assumption.
9. Swapchain present — needs Phase 6's window, so this is where the two phases meet.
10. The real engine frame.

Enable validation layers throughout and treat any validation error as a build break. They catch
the great majority of the bugs this phase can produce, far more cheaply than debugging visual
artefacts.

---

## Acceptance criteria

1. Every function in `RHI.h` has a real implementation; **no `EE_UNIMPLEMENTED_FUNCTION()`
   remains** in `RHI_Vulkan.cpp`.
2. `RHI.h` is unmodified: `git diff upstream/main -- Code/Base/Render/RHI.h` is empty.
3. `RHI_Direct3D12.cpp` is unmodified.
4. Bring-up steps 1–8 all pass in the `Tester` harness, with the tests committed.
5. Zero Vulkan validation errors or warnings across the full engine frame.
6. All 26 shaders from Phase 4 load and execute.
7. The full engine frame renders correctly — verified against Windows Direct3D 12 screenshots of
   the same scene. Enumerate any visual differences with an explanation.
8. Feature parity demonstrated: forward shading, cascaded shadows, GTAO, SMAA, OIT, mesh picking,
   debug draw. Each named, each verified.
9. `RenderDoc` capture works on Linux via `BeginFrameCapture`/`EndFrameCapture`.
10. `ReportDeviceMemoryLeaks` reports zero leaks after a clean shutdown.
11. **The Windows MSBuild build still succeeds** and the Direct3D 12 renderer is unchanged.

Criteria 7 and 8 are the ones that actually matter, and they cannot be checked mechanically. Be
honest in reporting them: "GTAO renders but with visible banding not present on Direct3D" is
useful; "feature parity achieved" when it is not is worse than useless.

## Do not

- Modify `RHI.h` or `RHI_Direct3D12.cpp`.
- Diverge from Phase 4's recorded binding model without escalating.
- Apply clip-space Y inversion if Phase 4 already did it in the shader compiler.
- Bring up against the full engine frame before the `Tester` harness passes step 8.
- Disable validation layers to make progress.
- Reach for `ALL_COMMANDS` barriers as a permanent solution — if you use them as a temporary
  measure, record every site in [Progress.md](../Progress.md).
- Mark this phase complete with stubs remaining. Report exactly which groups are done.

## Notes for the next agent

Phase 6 needs from this phase:
- The exact surface-creation requirement, so SDL3 creates a compatible window.
- Whether swapchain recreation on resize is driven by the RHI or the application.

Record group-by-group completion status in [Progress.md](../Progress.md) — this phase spans
months and many sessions, and "which of the 16 groups are real" is the single most important
piece of state.
