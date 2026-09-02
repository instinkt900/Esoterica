# Blocked Work

**Everything the port has written but not verified, and what would unblock it.**

The port is being built on machines that cannot exercise all of it. Work therefore piles up in
two queues: one that needs a GPU which can run the render path, and one that needs a Windows
machine. Neither queue is visible in [Progress.md](Progress.md), because its entries are dated
and the blocked items are scattered through thirty of them.

**This file is the index. [Progress.md](Progress.md) is still the detail.** Every row points at
the entry that explains it.

---

## This is not the remaining-work list

**It is the subset of it that is blocked on hardware.** Everything left in the port is in
[Phase 8](Phases/Phase8-Completion.md), which is ordered by size and includes plenty that is
blocked on nothing at all - the runtime shakedown, the sanitizer builds, the RHI debt sweep, the
fork review.

The two files overlap on purpose and neither subsumes the other. This one is *"which desk do I
need"*; Phase 8 is *"what is left, and how big"*. The Windows queue below is
[P8.1](Phases/Phase8-Completion.md#p81---the-windows-build) seen from the desk.

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

### The editor, which has now been shaken down

**P7.6's second pass ran on 2026-09-02** and retired seven of the eight rows that used to be here.
Every tool opens, multi-viewport works, hot reload closes end to end, and `OpenInExplorer` opens a
file manager. Read the 2026-09-02 entry in [Progress.md](Progress.md) before starting - two traps
there are about driving imgui with `xdotool` rather than about the port, and each cost an hour.

**Everything here needs the desktop awake and unlocked.** i3 runs without a compositor, so a locked
screen makes every screenshot return the lock screen rather than the editor, and DPMS throttles the
editor to a fraction of its normal work.

| # | What to check | Where | Detail in |
|---|---|---|---|
| 1 | **The title bar minimize and maximize buttons.** **Close is verified** - it exits cleanly with no leaks. The other two cannot be tested under i3, which implements neither; `xdotool windowminimize` is equally a no-op. Needs a WM that iconifies, and installing one was declined on 2026-09-02 | `ImguiX_Linux.cpp`, `Application_Linux.cpp` | Progress.md, P7.6 entries |
| 2 | **The Resource Server's Test Compile panel** draws "Force Recompile" and "Request Compilation" on top of each other once the panel is narrow. **Reproduced and explained on 2026-09-02** - `SameLine( GetContentRegionAvail().x - 200 )` at `ResourceServerUI.cpp:881` goes left of the checkbox on a narrow panel. Upstream and platform-neutral, so it is left for an upstream report and a Windows check, not fixed here | `ResourceServerUI.cpp:881` | Progress.md, 2026-09-02 entry |
| 3 | **The seven `OpenInExplorer` call sites other than the resource browser's.** The implementation is proved - nautilus opened the folder and selected the item - so what is left is only whether each site builds the right path | `EditorTool_ResourceImporter.cpp`, `DataPathPicker.cpp`, `ResourcePickers.cpp`, `ResourceServerUI.cpp` | Progress.md, P7.4 and 2026-09-02 entries |

**The ragdoll editor row is gone.** P8.2 imported a skeletal asset with
[`Docs/Linux/Scripts/FetchTestAssets.sh`](Scripts/FetchTestAssets.sh) and the ragdoll editor opens
with it, renders its skinned preview mesh, and shows its bone hierarchy, preview controls and self
collision table.

### Needs a gamepad plugged into this machine

| # | What to check | Where | Detail in |
|---|---|---|---|
| 4 | **Camera control from a gamepad in a running engine.** P8.2 closed the keyboard and mouse halves of Phase 6 criterion 3 - hold right mouse in a viewport, then `W`/`A`/`S`/`D` move and mouse delta looks. The gamepad half is untested because **no controller is attached**: there is no `/dev/input/js*`, and the only `*-event-joystick` node belongs to the Keychron keyboard. Phase 6 tested the gamepad at the device level on other hardware, so this is the "works for camera control" half only | `Component_ToolsCamera.cpp` | Progress.md, P8.2 entry |

### Phase 5 groups the correct frame did not cover

**Most of Phase 5 is no longer blocked.** The 2026-09-01 run drew the pbrdemo map correctly on an
RTX 3090 with host validation on and zero validation messages, which exercised P5.1 to P5.10,
P5.13 and P5.17. **Their individual "Not verified" lists are historical.** A frame capture on the
same machine then closed P5.12, P5.15 and most of criterion 8, and a temporary in-engine harness
closed P5.11. See the "GPU-blocked queue, part 1" and P5.11 entries in Progress.md. What is left:

| # | Group | What to check first | Detail in |
|---|---|---|---|
| 6 | **Mesh picking.** The last unverified item in criterion 8. Gated on `IsPickingEnabled()` inside `#if EE_DEVELOPMENT_TOOLS`, so it needs an editor viewport. Click an entity in the map editor and confirm the picked entity is the one under the cursor | Phase 5 criterion 8, and [P8.4](Phases/Phase8-Completion.md#p84---rhi-debt-sweep) |

**P5.16 raytracing is no longer a row here.** It is not blocked on hardware: the 3090 has the
extensions and the code is written. It is blocked on there being **nothing that calls it** - zero
callers across `Code/Engine`, `Code/EngineTools` and `Code/Game`, and no raytracing shaders, on
either backend. That makes it a decision rather than a measurement, and it moved to
[P8.3](Phases/Phase8-Completion.md#p83---raytracing-or-the-decision-not-to).

### One thing only a different driver will find

| # | What to check | Why | Detail in |
|---|---|---|---|
| 7 | **The query-as-enable-request pattern** is still in place for the shading rate, acceleration structure and ray tracing feature blocks. It was a real defect for mesh shaders. Confirmed still present by reading `RHI_Vulkan.cpp:1157-1232`; the mesh shader block is the only one that clears the bits it does not want | No cross-dependency VUID catches it today. Measured: RTX 3090, driver 580.173.02, host validation on, raytracing enabled - **no VUID fires** | Progress.md, 2026-08-31 NVIDIA entry |

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

**Do not park these behind a machine. They are ordinary work**, and they are the reason this file
is not the remaining-work list. Each has a task in
[Phase 8](Phases/Phase8-Completion.md); the rows here exist so that nobody sitting at the wrong
machine assumes they are waiting on hardware.

| # | What to do | Task | Detail in |
|---|---|---|---|
| 1 | **An animation graph has still never been evaluated.** P8.2 closed the rest of this row - game preview starts and stops, three physics bodies fall and settle, and a skeletal asset opens in the skeleton, animation graph and ragdoll editors. What is left is running a graph: give the graph a skeleton in the Variation Editor, put the clip in it, and press "Preview Graph". Needs nothing but time; `Docs/Linux/Scripts/FetchTestAssets.sh` already provides the skeleton and clip | [P8.2](Phases/Phase8-Completion.md#p82---runtime-shakedown) | Progress.md, P8.2 entry |
| 2 | **Raytracing has no callers on either backend.** A scratch harness, or a recorded decision that it is unreachable. Not a hardware wait | [P8.3](Phases/Phase8-Completion.md#p83---raytracing-or-the-decision-not-to) | Progress.md, 2026-09-02 docs entry |
| 3 | **`PrimitiveOutput` cannot carry `PerPrimitiveEXT`, and should.** `DebugDrawPrimitiveOutput` was fixed by P5.20; `PrimitiveOutput` in `RendererTypes.esh` was not, because `MaterialShaderInput::New` copies the struct into a local and DXC then builds SPIR-V that spirv-val rejects. Two ways out, neither cheap: a packed `uint` with accessors, which reaches Direct3D 12; or a fourth `Code/Scripts/DXCPatches` entry. **Not urgent** - NVIDIA renders correctly without it - but another driver need not be so forgiving | [P8.5](Phases/Phase8-Completion.md#p85---shader-conformance) | [Rendering: where we are](Progress.md#still-open) |
| 4 | **The six sanitizer configurations have never been built.** They all generate; no output directory has ever existed. TSan is the interesting one on an engine with a task system | [P8.6](Phases/Phase8-Completion.md#p86---sanitizers-and-build-coverage) | Progress.md, 2026-09-02 docs entry |
| 5 | **The items in [Deferred on purpose](Progress.md#deferred-on-purpose).** Known shortcuts, chosen rather than missed. Each is correct-enough to keep going and wrong enough to sweep before the port is called done. Not duplicated here | [P8.4](Phases/Phase8-Completion.md#p84---rhi-debt-sweep) | [Progress.md](Progress.md#deferred-on-purpose) |

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
