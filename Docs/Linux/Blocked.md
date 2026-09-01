# Blocked Work

**Everything the port has written but not verified, and what would unblock it.**

The port is being built on machines that cannot exercise all of it. Work therefore piles up in
two queues: one that needs a GPU which can run the render path, and one that needs a Windows
machine. Neither queue is visible in [Progress.md](Progress.md), because its entries are dated
and the blocked items are scattered through thirty of them.

**This file is the index. [Progress.md](Progress.md) is still the detail.** Every row points at
the entry that explains it.

---

## How to use this

Sit down at a machine, read the section for that machine, and work down it. The rows are ordered
so the cheapest and most informative checks come first.

**A row leaves this file when it is verified, not when it is attempted.** Move it by deleting it
here and saying so in that session's Progress.md entry.

---

## Needs a GPU that can run the render path

**The reference machine is the RTX 3090** - NVIDIA GeForce RTX 3090, driver 580.173.02, Ubuntu
24.04.4, X11. It has `VK_KHR_fragment_shader_barycentric`, `VK_EXT_mesh_shader`,
`VK_EXT_mutable_descriptor_type` and `shaderSharedInt64Atomics`, which is four of the five gaps in
[What this machine still cannot do](Progress.md#what-the-first-machine-still-cannot-do).

**The other development machine cannot do any of this.** Its Intel UHD 620 has no
`VK_EXT_mesh_shader`, so every mesh draw is dropped, later passes read what the geometry path
never wrote, and the GPU is lost. In practice **the editor dies about three seconds after it
reaches its frame loop**, on `EE_ASSERT( result == VK_SUCCESS )` in `QueueHostWait`
(`RHI_Vulkan.cpp:2232`). The GPU reset also stalls the Resource Server for about seven seconds,
so a second process cannot be used to watch the first. Do not try to work around this.

### The editor, which has never stayed alive long enough to use

| # | What to check | Where | Detail in |
|---|---|---|---|
| 1 | **P7.6, the editor shakedown.** The whole task. Resource browser, resource importer, animation graph editor, ragdoll editor, map editor, property grids. None has ever run on Linux | `Code/EngineTools/` | [Phase7-EditorTools.md](Phases/Phase7-EditorTools.md#p76---editor-shakedown) |
| 2 | **P7.5's last link.** Open the map, edit a material, watch the viewport reload it. The four links below it are proved | - | Progress.md, P7.5 entry |
| 3 | **The "Open In Explorer" menu items.** Six in EngineTools, two in the Resource Server. The function they call is verified; the menu items are not | `EditorTool_ResourceBrowser.cpp`, `EditorTool_ResourceImporter.cpp`, `DataPathPicker.cpp`, `ResourcePickers.cpp`, `ResourceServerUI.cpp` | Progress.md, P7.4 entry |
| 4 | **The file dialogs opened from the editor's own menus.** The `FileDialog` entry points are verified directly; no editor menu has opened one | `Code/EngineTools/Core/SystemDialogs_Linux.cpp` | Progress.md, P7.2 entry |
| 5 | **The borderless title bar buttons.** Minimize, maximize and close are drawn and have never been clicked, in either the editor or the Resource Server | `Application_Linux.cpp`, `EditorApplication_Linux.cpp` | Progress.md, P7.1 and P7.3 entries |
| 6 | **The Resource Server's Test Compile panel** draws "Force Recompile" and "Request Compilation" on top of each other. Shared imgui code, so check it on Windows too | `ResourceServerUI.cpp` | Progress.md, P7.5 entry |

### Phase 5 groups the correct frame did not cover

**Most of Phase 5 is no longer blocked.** The 2026-09-01 run drew the pbrdemo map correctly on an
RTX 3090 with host validation on and zero validation messages, which exercised P5.1 to P5.10,
P5.13 and P5.17. **Their individual "Not verified" lists are historical.** A frame capture on the
same machine then closed P5.12, P5.15 and most of criterion 8; see the "GPU-blocked queue, part 1"
entry in Progress.md. What is left:

| # | Group | What to check first | Detail in |
|---|---|---|---|
| 7 | **P5.11 query pools** | The timestamp frequency inversion. A wrong one gives plausible timings that are wrong by a constant factor, which nobody notices. **Note before you start: `CreateQueryPool` and `GetQueryTimestampFrequency` have zero callers in `Code/`, so running the engine cannot exercise this.** It needs a scratch harness, the way P6.6 proved the swapchain | Progress.md, P5.11 entry |
| 8 | **P5.16 raytracing** | All of it. No structure built, no ray traced, no raytracing pipeline compiled. **Assume none of it works** | Progress.md, P5.16 entry |
| 9 | **That the debug draw pass produces something visible.** The pass records, its pipelines create and it issues 5 indirect mesh draws - that much is verified. The counts are GPU-written and pbrdemo submits no debug geometry, so nothing has seen a debug primitive | **The editor is where this settles**, since it draws gizmos and bounds. Fold it into P7.6 | Progress.md, "GPU-blocked queue, part 1" |
| 10 | **Criterion 8's last live row: mesh picking.** Gated on `IsPickingEnabled()` at `Renderer_ForwardShading.cpp:1022`, so it never runs in a standalone engine frame | **Also P7.6.** Forward shading, cascaded shadows, GTAO and SMAA are verified; OIT is dead code on both backends and cannot be verified at all | [Phase5-VulkanRHI.md](Phases/Phase5-VulkanRHI.md#acceptance-criteria) |

### One thing only a different driver will find

| # | What to check | Why | Detail in |
|---|---|---|---|
| 11 | **The query-as-enable-request pattern** is still in place for the shading rate, acceleration structure and ray tracing feature blocks. It was a real defect for mesh shaders. Confirmed still present by reading `RHI_Vulkan.cpp:1157-1232`; the mesh shader block is the only one that clears the bits it does not want | No cross-dependency VUID catches it today. Measured: RTX 3090, driver 580.173.02, host validation on, raytracing enabled - **no VUID fires** | Progress.md, 2026-08-31 NVIDIA entry |

---

## Needs a Windows machine

**This is the highest-risk queue in the port, and it is not about hardware.** The rows below
change code that Direct3D 12 also compiles. Every one could be silently wrong on Windows right
now, and nothing here can tell.

Needed: MSBuild, and a GPU for the visual checks. [TouchedFiles.md](TouchedFiles.md) carries the
authoritative status of each file; the rows below say what to do about it.

| # | What to check | Files | Detail in |
|---|---|---|---|
| 1 | **`main` still builds with MSBuild.** The invariant every phase's acceptance criteria ends with, and **it has never been run.** Every phase since Phase 3 records it as "not run" | all | [AGENTS.md](../../AGENTS.md#definition-of-done) |
| 2 | **Open question 8's `Buffer<uint2>` change, rendered on Direct3D 12.** Six shader files, no `__linux__` branch to hide behind. It reaches instance picking, instance culling, light culling and every material pixel shader | `RHI.esh`, `SpatialHash.esh`, `InstancePickingResolve.esf`, `InstanceCulling.esf`, `LightCulling_CullLights.esf`, `MaterialShaderPBR.esh` | [TouchedFiles.md](TouchedFiles.md#shader-edits) |
| 3 | **P5.17's indirect root arguments.** All inside `#ifdef __spirv__` with an `#else` falling back to the existing declarations, so Direct3D 12 *should* be untouched. "Should" is the word that needs the build | `RHI.esh`, `ClusterCulling.esf`, `DefaultMeshShader.esh`, `DebugDraw.esf`, `DebugDrawMesh.esf` | [TouchedFiles.md](TouchedFiles.md#shader-edits) |
| 4 | **`EE_INDIRECT_PIXEL_ENTRY_INIT`,** which is empty on Direct3D 12. Two pixel shaders call it as their first statement. Compiles, never looked at | `RHI.esh`, `MaterialShaderPBR.esh`, `DebugDrawMesh.esf` | [TouchedFiles.md](TouchedFiles.md#shader-edits) |
| 5 | **P5.20's `EE_INTERSTAGE_HANDLE` and `EE_PER_PRIMITIVE`.** Both `__spirv__`-gated and no-ops on Direct3D 12, and `RendererTypes.esh` is a comment only. Same "should" as row 3 | `RHI.esh`, `DebugDraw.esf`, `RendererTypes.esh` | Progress.md, P5.20 entry |
| 6 | **Resource compiler output byte-identical to Windows.** Phase 3 criterion 4. Debug and Release on Linux already agree byte for byte across all 38 files, which rules out the float-formatting and optimisation differences and leaves only genuinely platform-dependent ones | `Esoterica.Applications.ResourceCompiler` | Progress.md, Phase 3 entry |
| 7 | **The two Phase 7 `#elif` edits.** `ResourceServerUI.cpp` and `BaseModule.cpp`. Both are sibling branches beside an existing `#if _WIN32`, and no Windows build has seen either | `ResourceServerUI.cpp`, `Code/Base/_Module/BaseModule.cpp` | Progress.md, P7.3 entry |
| 8 | **`HLSL_STATIC_ASSERT` is compiled out on SPIR-V** (`RHI.esh:68`), so every shared-struct size check is absent on Linux and present on Windows. A Windows build is the only place those assertions fire | `RHI.esh` | [Rendering: where we are](Progress.md#what-works) |

---

## Needs neither, and is easy to mistake for a hardware wait

**Do not park these behind a machine. They are ordinary work.**

| # | What to do | Where | Detail in |
|---|---|---|---|
| 1 | **`PrimitiveOutput` cannot carry `PerPrimitiveEXT`, and should.** `DebugDrawPrimitiveOutput` was fixed by P5.20; `PrimitiveOutput` in `RendererTypes.esh` - the material shaders and `DebugDrawMesh.esf` - was not, because `MaterialShaderInput::New` copies the struct into a local and DXC then builds SPIR-V that spirv-val rejects. Two ways out, neither cheap: a packed `uint` with accessors instead of the nested bitfield struct, which reaches Direct3D 12; or a fourth `Code/Scripts/DXCPatches` entry. **Not urgent** - NVIDIA renders correctly without it - but another driver need not be so forgiving | `RendererTypes.esh` | [Rendering: where we are](Progress.md#still-open) |
| 2 | **The items in [Deferred on purpose](Progress.md#deferred-on-purpose).** Known shortcuts, chosen rather than missed. Each is correct-enough to keep going and wrong enough to sweep before the port is called done. Not duplicated here | various | [Progress.md](Progress.md#deferred-on-purpose) |
| 3 | **`requirements_gamenetworkingsockets` does not version-check `protoc`.** A stale one earlier on `PATH` is accepted and fails deep inside the build | `DownloadDependencies.sh` | [Rendering: where we are](Progress.md#still-open) |

---

## Keeping this current

**A task that leaves something unverified adds a row here, in the task's own PR.** That is the
same rule as [TouchedFiles.md](TouchedFiles.md) and [Progress.md](Progress.md), and it is what
stops this file going stale the way a hand-maintained list normally does.

Write the row so someone at the right machine can act on it without reading the entry first:
what to check, where the code is, and what it costs if it is wrong. Then link the entry for the
rest.

**A row is deleted only when the thing is verified.** "Attempted and inconclusive" is a note in
Progress.md, not a deletion here.
