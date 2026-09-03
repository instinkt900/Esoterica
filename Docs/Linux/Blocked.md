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
closed P5.11. See the "GPU-blocked queue, part 1" and P5.11 entries in Progress.md.

**Nothing is left in this group.** **P8.4 verified mesh picking on 2026-09-03** - the last
unverified item in Phase 5 criterion 8 - by clicking three entities in the map editor viewport and
confirming each time that the selected entity was the one under the cursor, including a floor that
is edge-on and about two pixels tall on screen.

**P5.16 raytracing is no longer a row here.** It is not blocked on hardware: the 3090 has the
extensions and the code is written. It is blocked on there being **nothing that calls it** - zero
callers across `Code/Engine`, `Code/EngineTools` and `Code/Game`, and no raytracing shaders, on
either backend. That makes it a decision rather than a measurement, and it moved to
[P8.3](Phases/Phase8-Completion.md#p83---raytracing-or-the-decision-not-to).

### One thing only a different driver would have found

**Nothing is left here.** **P8.5 fixed the query-as-enable-request pattern on 2026-09-03** rather
than waiting for a driver to catch it. All three remaining feature blocks now clear the bits the
backend does not use, the way the mesh shader block always did, so there is nothing left for a
stricter driver to reject. It was never observable on this one.

### Needs a non-NVIDIA GPU, which is a queue of its own

**The Linux renderer is NVIDIA-only as of the 47e6293 merge, and nothing here can see it.** Both
development machines are NVIDIA, so this queue cannot be worked on either of them. It is not a
driver-strictness wait like the row above - the code simply will not run elsewhere.

| # | What to check | Files | Detail in |
|---|---|---|---|
| 1 | **`VK_NV_shader_subgroup_partitioned` is now a hard requirement.** Upstream's `ClusterCompaction.esf` calls `WaveMatch`, which is core SM 6.5 on Direct3D 12 and which DXC can only lower to `OpGroupNonUniformPartitionNV`. There is no core Vulkan 1.3 equivalent. Compaction writes the draw arguments, so **an AMD or Intel GPU renders no geometry at all**, not one missing effect. The backend logs a warning naming the consequence and carries on | `RHI_Vulkan.cpp`, `ClusterCompaction.esf` | Progress.md, P9.4 entry |

This contradicts two things that were decided earlier and should be revisited together rather than
one at a time: Phase 5's **"full feature parity with the Direct3D 12 backend"**, and the stated
target of **Vulkan 1.3** rather than Vulkan-1.3-plus-a-vendor-extension. The alternative was
rewriting `WaveMatch` as a portable subgroup ballot loop in a shared shader, which reaches
Direct3D 12 and is therefore an escalation of its own. Escalated and accepted on 2026-09-03; the
decision was to unblock P9.4 and record the cost here.

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
| 3 | **P5.17's indirect root arguments.** All inside `#ifdef __spirv__` with an `#else` falling back to the existing declarations, so Direct3D 12 *should* be untouched. "Should" is the word that needs the build | `RHI.esh`, `ClusterCulling.esf`, `ClusterCompaction.esf`, `DefaultMeshShader.esh`, `DebugDraw.esf`, `DebugDrawMesh.esf` | [TouchedFiles.md](TouchedFiles.md#shader-edits) |
| 4 | **`EE_INDIRECT_PIXEL_ENTRY_INIT`,** which is empty on Direct3D 12. Two pixel shaders call it as their first statement. Compiles, never looked at | `RHI.esh`, `MaterialShaderPBR.esh`, `DebugDrawMesh.esf` | [TouchedFiles.md](TouchedFiles.md#shader-edits) |
| 5 | **P5.20's `EE_INTERSTAGE_HANDLE` and `EE_PER_PRIMITIVE`.** Both `__spirv__`-gated and no-ops on Direct3D 12, and `RendererTypes.esh` is a comment only. Same "should" as row 3 | `RHI.esh`, `DebugDraw.esf`, `RendererTypes.esh` | Progress.md, P5.20 entry |
| 6 | **Resource compiler output byte-identical to Windows.** Phase 3 criterion 4. Debug and Release on Linux already agree byte for byte across all 38 files, which rules out the float-formatting and optimisation differences and leaves only genuinely platform-dependent ones | `Esoterica.Applications.ResourceCompiler` | Progress.md, Phase 3 entry |
| 7 | **The two Phase 7 `#elif` edits.** `ResourceServerUI.cpp` and `BaseModule.cpp`. Both are sibling branches beside an existing `#if _WIN32`, and no Windows build has seen either | `ResourceServerUI.cpp`, `Code/Base/_Module/BaseModule.cpp` | Progress.md, P7.3 entry |
| 8 | **`HLSL_STATIC_ASSERT` is compiled out on SPIR-V** (`RHI.esh:68`), so every shared-struct size check is absent on Linux and present on Windows. A Windows build is the only place those assertions fire | `RHI.esh` | [Rendering: where we are](Progress.md#what-works) |

---

### The Debug configuration, blocked by the Resource Server

**Release runs; Debug does not.** Found 2026-09-03, immediately after the `47e6293` merge, by
running the Debug editor. Not a hardware wait and not a Windows wait - it needs a session.

| # | What to do | Files | Detail in |
|---|---|---|---|
| 1 | **The Debug `ResourceServer` asserts seconds after start**, `EndCommandBuffer` on a buffer that was never begun (`RHI_Vulkan.cpp:2771`). The Debug editor then dies with `LOST CONNECTION!!`, which is the symptom. Release only survives because its asserts are compiled out - **the mistake is present there too**. A specific, unconfirmed hypothesis about the merge's new compute command buffers is in the P9.1-P9.4 Progress.md entry; verify it before acting on it | `RHI_Vulkan.cpp`, `RenderSystem.cpp` | Progress.md, P9.1-P9.4 entry |
| 2 | **The engine aborts if the Resource Server is not already listening.** The merge turned `EE_ASSERT( "LOST CONNECTION!!" )` - always truthy, never fired - into a real `EE_TRACE_HALT`, so a startup race that had been invisible since P7.3 is now a hard abort. Upstream's code and platform-neutral, so **Windows will hit it too**. Report it; the workaround is to start the server first | `ResourceProvider_Network.cpp:97` | Progress.md, P9.1-P9.4 entry |

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
| 3 | **`PrimitiveOutput` cannot carry `PerPrimitiveEXT`.** **P8.5 measured it on 2026-09-03 and made it permanent**: there are three blockers, not one, and the third - `SV_CullPrimitive` being declared as a pixel shader Input once the struct is decorated - needs a separate pixel-shader input struct, which reaches Direct3D 12 and is therefore an escalation. **The obvious workaround compiles, passes spirv-val and renders a black frame**, so do not trust a clean compile here | escalation, then [P8.5](Phases/Phase8-Completion.md#p85---shader-conformance) | [Progress.md](Progress.md), P8.5 entry |
| 4 | ~~**The six sanitizer configurations have never been built.**~~ **Done by P8.6 on 2026-09-03**, for the three Release ones. What is left is not a build: **TSan cannot run against the NVIDIA driver**, failing inside its own interceptors right after `vkCreateDevice`, and no `TSAN_OPTIONS` setting avoids it. The engine's threading under a real frame is therefore still uncovered. Mesa's lavapipe ICD runs, but has no `VK_EXT_mesh_shader` so the frame halts early. **Needs a machine with a different GPU vendor** | [P8.6](Phases/Phase8-Completion.md#p86---sanitizers-and-build-coverage) | [Progress.md](Progress.md), P8.6 entry |
| 5 | ~~**The items in [Deferred on purpose](Progress.md#deferred-on-purpose).**~~ **Swept by P8.4 on 2026-09-03.** All seven `ALL_COMMANDS` barrier sites and both `EE_UNIMPLEMENTED_FUNCTION` markers are now permanent with a recorded reason, and the RenderDoc trigger is deliberately not wired up. The reasoning is in the [`ALL_COMMANDS` sites](Progress.md#all_commands-sites) table and the P8.4 entry | [P8.4](Phases/Phase8-Completion.md#p84---rhi-debt-sweep) | [Progress.md](Progress.md), P8.4 entry |

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
