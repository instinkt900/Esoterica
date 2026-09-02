# Phase 8 - Completion and Fork Review

**Goal:** finish the port, and decide what the fork is.

**Deliverable:** the Windows build runs, the engine has been seen to *simulate* rather than only
render, the remaining Vulkan and shader debt is either swept or recorded as permanent, and
`Docs/Linux/ForkReview.md` states how far this fork has diverged and whether any of it
could ever go upstream.

**Prerequisites:** Phase 7. The editor works; see its
[acceptance criteria](Phase7-EditorTools.md#acceptance-criteria).

**Rough cost:** unknown, and P8.1 is why. Everything else in this phase is days.

**Read first:** [00-Conventions.md](../00-Conventions.md), [Blocked.md](../Blocked.md),
[TouchedFiles.md](../TouchedFiles.md), [01-UpstreamMerges.md](../01-UpstreamMerges.md).

---

> ## Start here
>
> | Task | State |
> |---|---|
> | P8.1 The Windows build | not started. **The largest unmeasured risk in the port** |
> | P8.2 Runtime shakedown | **mostly done**, 2026-09-02. Game preview runs, physics simulates, a skeletal asset opens in all three animation editors. Left: a gamepad, and evaluating a graph |
> | P8.3 Raytracing, or the decision not to | **deferred**, 2026-09-03, by the developer. Not started, and nothing else in the phase waits on it |
> | P8.4 RHI debt sweep | **done**, 2026-09-03. Mesh picking verified; the barrier debt and both `EE_UNIMPLEMENTED_FUNCTION` markers made permanent; no RenderDoc trigger. Phase 5 criteria 1, 8 and 9 closed |
> | P8.5 Shader conformance | not started |
> | P8.6 Sanitizers and build coverage | not started |
> | P8.7 Fork review | not started. Do this last |
>
> **These are ordered by size and risk, not by dependency.** Only P8.7 has to come last. P8.1 is
> first because it is the only item that could turn out to be large, and everything else is
> cheaper to plan once its answer is known.
>
> **[Progress.md](../Progress.md) is the authority on what each task actually did.** The
> descriptions below are the plan.

> ## What Phase 7 hands you
>
> ### What genuinely works
>
> The engine renders the pbrdemo map correctly on an RTX 3090 with host validation on, zero
> validation messages and no message-ID filters. The editor opens the map, every tool opens,
> multi-viewport docking works, resource hot reload works end to end, and both applications shut
> down clean with no leaks. Debug and Release build the whole tree.
>
> ### What had never been exercised when this phase opened
>
> **This block is what Phase 7 handed over, and P8.2 has since answered most of it.** Kept as the
> starting position; the table above and [Progress.md](../Progress.md) are current.
>
> **The engine has only ever drawn a static scene for about thirty seconds.** Nothing in this port
> has been observed *simulating*: no physics body has been seen to move, no animation graph has
> been evaluated, and **"Play Map" has never been pressed**. That is P8.2, and it is the largest
> functional unknown after Windows.
>
> **No Windows build has been run at any point in this port.** Not once, in any phase. That is
> P8.1.
>
> ### Two things that are dead code on *both* backends
>
> Do not budget time to "port" either of these. Neither runs on Direct3D 12 either.
>
> - **Raytracing.** `RHI_Vulkan.cpp` implements acceleration structures, raytracing pipelines,
>   shader binding tables and `vkCmdTraceRays`. **`CreateAccelerationStructure`, `CmdDispatchRays`
>   and `RaytracingShaderTable` have zero callers** across `Code/Engine`, `Code/EngineTools` and
>   `Code/Game`, and the tree contains no raytracing shaders. See P8.3.
> - **OIT.** `OITResolve.esf` compiles and nothing looks it up; `OIT.esh` has no consumers at all.
>   Phase 5 already records this as a criterion that cannot be met.

---

## Why this phase exists

Phase 7 was the last phase in the original plan, and the plan was right that the port would work
by the end of it. What the plan did not carry was a place to put the work that is **left over
after the thing works**: the verification that was deferred because no machine could do it, the
shortcuts that were taken deliberately, and the question of what happens to this fork long-term.

Three kinds of work end up here.

1. **Verification that was never possible before.** The Windows build, and anything that needs the
   engine to run rather than compile.
2. **Debt that was chosen, not missed.** Every item is already recorded in
   [Deferred on purpose](../Progress.md#deferred-on-purpose) or
   [Still open](../Progress.md#still-open). This phase is where it is swept or promoted to
   permanent.
3. **The fork question**, which nothing before this phase was in a position to answer.

**This phase does not add features.** If a task here turns into feature work, stop and escalate.

---

## Tasks

### P8.1 - The Windows build

**Needs a Windows machine with MSBuild and a GPU. Nothing else in this phase does.**

"`main` builds on both Windows and Linux" is the invariant every phase's acceptance criteria ends
with, and **it has never been run**. Every phase since Phase 3 records it as "not run". This is not
a formality: the port edits ten shader files that compile for *both* platforms.

What makes this risky rather than routine:

- **Open question 8's `Buffer<uint2>` change has no `__linux__` branch to hide behind.** Six
  shader files, and it reaches instance picking, instance culling, light culling and every
  material pixel shader.
- **`HLSL_STATIC_ASSERT` is compiled out on SPIR-V** (`RHI.esh:68`). Every shared-struct size
  check is absent on Linux and present on Windows. A Windows build is the only place they fire,
  and a size mismatch introduced on Linux would be invisible until now.
- **P5.17's indirect root arguments and P5.20's `EE_INTERSTAGE_HANDLE` are `__spirv__`-gated**,
  with an `#else` falling back to the existing declarations, so Direct3D 12 *should* be untouched.
  "Should" is the word that needs the build.
- **`EE_INDIRECT_PIXEL_ENTRY_INIT` is empty on Direct3D 12** and two pixel shaders call it as
  their first statement. It compiles; nobody has looked at it.
- **Two Phase 7 `#elif` edits** in `ResourceServerUI.cpp` and `BaseModule.cpp` have never been
  seen by a Windows compiler.

Work down the Windows queue in [Blocked.md](../Blocked.md) in the order it gives. Build first,
then run, then compare.

**Done when** MSBuild succeeds for Debug, Release and Shipping; both Windows applications run and
render pbrdemo; the frame is compared against the Linux frame with any difference explained
(Phase 5 criterion 7); and the resource compiler's output is confirmed byte-identical across the
two platforms (Phase 3 criterion 4).

### P8.2 - Runtime shakedown

**The engine has rendered. It has never simulated.** Everything below is platform-neutral code that
has compiled since Phase 1 and has never executed on Linux.

- **Press "Play Map"** in the map editor (`MapEditor.cpp:353`, `m_requestStartGamePreview`). Start
  and stop game preview repeatedly. This is the editor's whole purpose and it is untouched.
- **Physics.** The engine vendors box3d (`Code/Engine/ThirdParty/box3d`). Confirm a body actually
  moves. pbrdemo has a `floor.physmesh`, so the collision data exists.
- **Camera control.** Phase 6 criterion 3 is half met: keyboard, mouse and gamepad are each tested
  at the *device* level, and "works for camera control" in a running engine has never been checked.
- **Animation.** Blocked by data before it is blocked by code: `Data/` contains **no `.skel`, no
  `.anim` and no `.ag`**, and the only FBXs are non-skeletal. Import a skeletal asset first.

**Importing that skeletal asset also unblocks the ragdoll editor**, which is a row in
[Blocked.md](../Blocked.md) for exactly this reason. Do it once and close both.

Expect defects here that a static frame cannot produce: threading, fixed-step update, and anything
that assumed a Windows timer. Record each as "Linux port bug" or "upstream bug", per Conventions
rule 3.

**Done when** game preview starts and stops cleanly, a physics body is seen to move, camera control
works from all three input devices, and a skeletal asset has been imported and opened in the
animation graph editor.

### P8.3 - Raytracing, or the decision not to

> **Deferred on 2026-09-03, by the developer: "raytracing can come later and will not affect this
> current line of work."** That is a scheduling decision, not one of the two outcomes below, so
> **this task is still open** and Phase 5's raytracing criterion is still unclosed. Nothing else in
> Phase 8 depends on it. Pick it up before P8.7, which needs every other answer.

**Read the block at the top of this document before starting.** P5.16 is written and has **no
callers and no shaders**. It cannot be verified by running the engine, because the engine never
asks for it.

Two honest outcomes, and either one closes the task:

1. **A scratch harness builds an acceleration structure and traces a ray**, the way P6.6's scratch
   application exercised the swapchain before the engine could. That proves the code and nothing
   more.
2. **The port records that the feature is unreachable on both backends** and closes Phase 5's
   raytracing criterion the way OIT's was closed - "cannot be met as written", with the reason.

**Do not add a raytracing renderer to the engine to give the code a caller.** That is feature work
on upstream's side of the line, and Phase 5's scope decision - full parity with the Direct3D 12
backend - is satisfied by matching a backend that also does not run it.

**Done when** one of the two outcomes above is recorded in [Progress.md](../Progress.md) and the
Phase 5 criterion is updated to match.

### P8.4 - RHI debt sweep

Each of these is already recorded. This task decides, one at a time, whether it is fixed or made
permanent with a reason.

- **Six `ALL_COMMANDS` barrier sites** (`RHI_Vulkan.cpp`). Correct, slow. The table in
  [Progress.md](../Progress.md#all_commands-sites) says for each one what narrowing it would need;
  three of them are the semantic and should simply be marked permanent.
- **Attachment transitions use all-access masks** in `TransitionAttachmentIfNeeded`. Same shape:
  nothing at the call site says what last touched the image.
- **Two `EE_UNIMPLEMENTED_FUNCTION` markers**, down from 103. A sampler border colour needing
  `VK_EXT_custom_border_color`, and a static-sampler path the binding model does not use. Neither
  is a whole function. Decide whether Phase 5 criterion 1 closes with them present.
- **Mesh picking has never been verified.** Phase 5 criterion 8 defers it to an editor viewport,
  and P7.6 did not cover it. Click an entity in the map editor viewport and confirm the picked
  entity is the one under the cursor.
- **RenderDoc has no in-engine trigger.** `BeginFrameCapture` and `EndFrameCapture` have zero
  callers in `Code/`, so a capture needs the capture key. Host validation must be off or the engine
  segfaults inside `librenderdoc.so`. Decide whether to wire the trigger up or record the
  constraint permanently.

**Done when** every item above is either fixed or has a recorded decision, and Phase 5's criteria
1, 8 and 9 are updated to match.

### P8.5 - Shader conformance

The frame is correct on NVIDIA. These are the places where it is correct *because the driver is
forgiving*, which is not the same thing.

- **`PrimitiveOutput` cannot carry `PerPrimitiveEXT`, and should.** `DebugDrawPrimitiveOutput` was
  fixed by P5.20; `PrimitiveOutput` in `RendererTypes.esh` was not, because `MaterialShaderInput::New`
  copies the struct into a local and DXC then builds SPIR-V that spirv-val rejects. Two ways out,
  neither cheap: a packed `uint` with accessors instead of the nested bitfield struct, which
  reaches Direct3D 12 and therefore needs escalation; or a fourth `Code/Scripts/DXCPatches` entry.
- **The query-as-enable-request pattern** is still in place for the shading rate, acceleration
  structure and ray tracing feature blocks (`RHI_Vulkan.cpp:1157-1232`). **It was a real defect for
  mesh shaders.** The mesh shader block is the only one that clears the bits it does not want. No
  VUID fires for it on the RTX 3090 with driver 580.173.02, so **finding it needs a different
  driver** - it is a row in the Blocked.md driver queue, not something this machine can settle.
- **`EE_INDIRECT_PIXEL_ENTRY_INIT` reads command 0's root constants.** Exact today, because
  `BucketResolve.esf:41` writes the same address into every command. **A pixel shader that starts
  reading a per-command root constant gets silently wrong data past the first command.**
- **An indirect `RootSRV` cannot be read.** `DebugDrawMesh.esf` declares one, so a signature can
  carry one; nothing indexes it yet.

**Done when** each item is fixed, or promoted from
[Deferred on purpose](../Progress.md#deferred-on-purpose) to a permanent limitation with the
consequence written down.

### P8.6 - Sanitizers and build coverage

Cheap, and likely to be productive on a threaded engine.

- **The six sanitizer configurations have never been built.** `Linux_{Debug,Release}_{ASan,TSan,UBSan}`
  all generate from `Toolchain.py`; no output directory has ever existed. **ASan and TSan on a run
  of the engine are the highest value here**, and TSan especially, given the task system.
- **Debug has never been built on the RTX 3090 machine.** Only Release and a partial Shipping.
- **Shipping does not link on that machine** - `External/LLVM/lib/LLVMgold.so` does not exist, so
  its LTO has no plugin. All 607 compile steps pass. This is a dependency gap between the two
  development machines, not a code defect.
- **There is no CI.** Nine configurations that nothing builds automatically will rot. Decide
  whether that is acceptable for a fork of this size, and record the decision either way.

**Done when** ASan and TSan builds of the engine have been run against pbrdemo with their findings
recorded, Shipping links on both machines, and the CI decision is written down.

### P8.7 - Fork review

**Do this last.** It is the only task here that needs every other answer.

Three questions, in order. They are not the same question and the third does not follow from the
second.

1. **How far has this fork diverged, and how destructive is it?**
2. **How expensive is `git merge upstream/main`, now and as a standing cost?**
3. **Could any of this ever go upstream safely?**

#### Method

Do not answer from memory or from `TouchedFiles.md` alone. **Measure it, then check the registry
against the measurement** - the registry being right is itself one of the findings.

```bash
git fetch upstream

# 1. The whole divergence, split into added files and edited files.
git diff --stat upstream/main -- Code/
git diff --name-status upstream/main -- Code/ | sort | uniq -c -w1

# 2. Every upstream file this fork edits, with its size of edit.
git diff --numstat upstream/main -- Code/ | sort -rn

# 3. Does upstream touch the files we touch? This is the standing cost.
#    Run per touched file, over the last 24 months.
git log upstream/main --oneline --since='24 months ago' -- <path>

# 4. Simulate the merge. Scratch branch, never on main.
git checkout -b scratch/merge-sim && git merge --no-commit --no-ff upstream/main
git diff --name-only --diff-filter=U
```

#### What the review has to establish

- **Every upstream file in the diff is classified** by edit shape: `#elif` addition, one-line
  change, real edit, or shader edit that compiles for both platforms. The last class is the one
  that matters; it is the only one that can break Windows.
- **`TouchedFiles.md` is verified complete.** No file in the diff missing from it, and no row with
  a stale status. The registry is what makes the post-merge audit in
  [01-UpstreamMerges.md](../01-UpstreamMerges.md) mechanical, so a wrong registry is a defect in
  the merge procedure, not a documentation nit.
- **A merge simulation against current `upstream/main`**, with the conflict count and the file
  list. Delete the scratch branch afterwards.
- **Upstream's churn rate on the touched set specifically.** Upstream changes about five source
  list entries a year overall, but the number that matters is how often it edits the ~14 files
  this fork edits. A file upstream never touches is free forever.
- **A verdict on each of the three questions**, with the reasoning, not just the conclusion.

#### The third question deserves care

"Could this be merged upstream" is not one decision. Split the diff by what it would do to a
Windows user:

- **Inert on Windows** - new `_Linux` files, and `#elif defined( __linux__ )` branches. About 40
  added files and 8 sibling branches. These cost upstream nothing and are the plausible offer.
- **Changes Windows behaviour** - anything that is not guarded. `RHI.h`'s `MaxPendingFrames`
  is guarded; the ten shared shader edits are not, and `HLSL_STATIC_ASSERT` being absent on SPIR-V
  means Linux has been building without checks Windows enforces.
- **Depends on the build system this fork replaced.** `NinjaGen.py` is a rewrite of a stale
  upstream script. Upstream builds with MSBuild and would gain a second build system to maintain.

**Upstream rejects large PRs** - that is the premise the whole port is built on
([README.md](../README.md#prime-directive)). So the useful output is not "yes" or "no" but *which
slice, in what order, at what size*, or a clear statement that no slice is worth offering and the
fork stands alone. Either answer is acceptable. An unsupported one is not.

**Done when** `Docs/Linux/ForkReview.md` exists with all five bullets above answered, its
verdict is summarised in [README.md](../README.md), and a Progress.md entry records the numbers so
the next review has a baseline to compare against.

---

## Acceptance criteria

1. **MSBuild builds `main` on Windows**, in Debug, Release and Shipping, and both Windows
   applications run. The first time in the project's history.
2. **The Windows frame is compared against the Linux frame**, with every difference explained.
   Phase 5 criterion 7.
3. **Resource compiler output is byte-identical across the two platforms.** Phase 3 criterion 4.
4. **Game preview runs.** "Play Map" starts and stops cleanly, and a physics body moves.
5. **A skeletal asset is imported**, opened in the animation graph editor, and the ragdoll editor
   opens with it.
6. **Camera control works** from keyboard, mouse and gamepad in a running engine. Closes Phase 6
   criterion 3.
7. **Mesh picking is verified** from an editor viewport. Closes the last of Phase 5 criterion 8.
8. **Raytracing is either exercised by a harness or formally closed as unreachable**, and Phase 5
   says which.
9. **Every item in [Deferred on purpose](../Progress.md#deferred-on-purpose) and
   [Still open](../Progress.md#still-open) is fixed or promoted to a permanent, explained
   limitation.** The two lists end this phase empty or entirely deliberate.
10. **ASan and TSan builds of the engine have been run**, and their findings recorded.
11. **`Docs/Linux/ForkReview.md` exists**, and answers all three of P8.7's questions with
    measurements rather than assertions.
12. **`TouchedFiles.md` is verified complete against the real diff**, not just believed to be.

## Do not

- **Do not add features to give dead code a caller.** That applies to raytracing and to OIT. Both
  are dead on Direct3D 12 too, and matching that is parity.
- **Do not fix platform-neutral upstream bugs found in this phase.** Record them. Conventions
  rule 3. The Test Compile panel overlap in `ResourceServerUI.cpp:881` is the current example.
- **Do not merge `upstream/main` as part of P8.7.** The review simulates a merge on a scratch
  branch and deletes it. Merging is on request only; see
  [01-UpstreamMerges.md](../01-UpstreamMerges.md#when-to-merge).
- **Do not open a PR against upstream**, whatever P8.7 concludes. The review produces a
  recommendation, not an approach to upstream.
- **Do not treat the sanitizer findings as this phase's problem.** Record them; fix the ones that
  are the port's, and file the rest.

## Notes for the next agent

**Start with P8.1 if you have a Windows machine, and P8.2 if you do not.** Those two are the whole
of the risk in this phase; P8.3 to P8.6 are known, scoped and recorded, and P8.7 needs them done.

**The out-of-scope list is in Phase 7**, under
[Beyond this phase](Phase7-EditorTools.md#beyond-this-phase): profiling and Tracy, C++ hot reload,
navmesh generation, packaging, a Vulkan backend on Windows, and ARM64. None of them belong here,
and P8.7 should say so explicitly rather than leave a reader wondering whether they were forgotten.

**When this phase closes, the port is done.** There is no Phase 9. If the work does not fit one of
the seven tasks above, it is either out of scope or a new phase, and either one is a decision to
record rather than to make quietly.
