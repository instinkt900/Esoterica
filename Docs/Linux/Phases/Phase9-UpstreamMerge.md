# Phase 9 - The First Upstream Merge

**Goal:** absorb `upstream/main` for the first time since the fork point, and find out what the
merge discipline actually bought.

**Deliverable:** `main` carries upstream commit `47e6293`, the Linux build compiles and runs, the
new culling pipeline has been seen on Vulkan, and [01-UpstreamMerges.md](../01-UpstreamMerges.md)
records the sync point and every non-trivial resolution.

**Prerequisites:** none that block starting. The trial merge below was run against
`linux/rhi-vulkan-comment-cleanup`, so the comment-cleanup PR should land first to avoid merging
the same conflicts twice. **P8.1 is not a prerequisite and this phase does not wait for it** - see
[What this phase does not settle](#what-this-phase-does-not-settle).

**Rough cost:** P9.1 is hours. P9.2 and P9.3 are a session each. P9.4 is unknown and is the whole
of the risk.

**Read first:** [01-UpstreamMerges.md](../01-UpstreamMerges.md) - it holds the **procedure**, and
this document does not repeat it. Then [00-Conventions.md](../00-Conventions.md) and
[TouchedFiles.md](../TouchedFiles.md).

---

> ## Start here
>
> | Task | State |
> |---|---|
> | P9.1 The mechanical merge | not started. Three conflicts, one of them trivial, plus the source-list resync |
> | P9.2 `ClusterCulling.esf` - reapply the indirect-dispatch shim | not started |
> | P9.3 The indirect argument buffers have no home | not started. **A wrong answer here hangs the GPU, it does not fail to compile** |
> | P9.4 The new culling pipeline, on Vulkan | not started. **The real work.** None of upstream's new culling code has ever run against this backend |
> | P9.5 Post-merge audit and provenance | not started. Do this last |
>
> **P9.1 first, and it unblocks everything else.** P9.2 and P9.3 can then go in either order.
> P9.4 needs the RTX 3090 machine and needs P9.1 to P9.3 done.
>
> **The text conflicts are not the job.** Three single-hunk conflicts is a morning. Upstream
> rewrote the culling pipeline wholesale and none of it has met this Vulkan backend, which is P9.4.

---

## Why this phase exists

Phase 8 ends with "when this phase closes, the port is done. There is no Phase 9." That was
written on 2026-09-02 and it was about *porting* work, which is finished. This phase is not
porting. It is the first test of the thing the whole port was arranged around.

The [prime directive](../README.md#prime-directive) is *keep `git merge upstream/main` cheap,
forever*. Every convention in this fork - add files rather than edit them, make each unavoidable
edit a two-line `#elif`, register it in [TouchedFiles.md](TouchedFiles.md) - is a cost paid up
front against a merge that had never happened. On 2026-09-03 upstream pushed a 281-file commit.
That is the bill arriving, and paying it is a phase.

**This phase does not add features and does not port anything new.** If upstream's new code needs
a Vulkan capability the backend does not have, that is a finding to record, not a Phase 5 group to
reopen inside this phase.

---

## What the trial merge measured

Run on 2026-09-03 on a scratch branch off `linux/rhi-vulkan-comment-cleanup`, then aborted. These
numbers are the starting position, not a result.

Upstream commit `47e6293` "Push Esoterica Main @800": **281 files, +5,668 / -36,670**. New
spatial-hash light culling, a new instance and cluster culling system with a compaction pass, FFX
parallel sort, animation graph changes, resource server rework, and a `FileRegistry` to
`DataFileSystem` rename.

| | count |
|---|---|
| Files the fork has touched since the fork point | 155 |
| Files upstream touched in this commit | 143 |
| Overlap | 17 |
| Files that auto-merged | 140 |
| **Conflicts** | **3**, one hunk each |

**Upstream touched zero files under `RHI/`, `Vulkan/` or any `Linux/` directory.** 52 of the
fork's 155 files live there. The largest single surface of this port has no contention with
upstream at all, which is the isolation strategy working exactly as designed.

Of the 14 overlapping files that auto-merged, the fork's deltas were 1 to 12 lines each against
upstream rewrites of 55 to 192 lines. The three load-bearing ones were checked by hand and all
three survived intact:

- `InstanceCulling.esf` - the `Buffer<uint2>` plus `PackUint64` shim from open question 8
- `DeviceRenderWorld.cpp` - the LP64 `Math::Max( size_t( 1 ), ... )` fix. Upstream independently
  wrote `Math::Max( m_clusterRecordOffsets.size(), size_t( 1 ) )` in its new code, so that hazard
  did not recur
- `RendererTypes.esh` - the `PrimitiveOutput` decoration note from P8.5

Upstream's additions were also scanned for the two patterns that have bitten this port before.
**No new `ULL`-literal `Math::Max` against a `.size()`, and no new `Buffer<uint64_t>` typed loads
in the shaders.** Upstream's new `uint64_t` use is scalars and groupshared arrays, which are fine.

The `FileRegistry` to `DataFileSystem` rename reaches 29 files, but only 5 that the fork has
anything in, and 4 of those merged cleanly because upstream did the rename in them itself.

---

## Tasks

### P9.1 - The mechanical merge

Follow the procedure in [01-UpstreamMerges.md](../01-UpstreamMerges.md#the-merge-procedure). It is
not repeated here. What follows is only what the trial merge found that the procedure does not
already cover.

**The one trivial conflict.** `Code/Applications/Editor/EditorUI.h`, a single hunk, entirely an
include collision: upstream renamed `FileRegistry.h` to `DataFileSystem.h` on the line above the
`EditorTool.h` include that P7.0 added. Keep both sides. That is the whole resolution.

**The source-list resync.** `SyncUpstream.py` already reports exactly two changes, and it should
report exactly these two and nothing else:

```
+ src Render/Device/DeviceRadixSort.cpp
- src FileSystem/FileRegistry.cpp
+ src FileSystem/DataFileSystem.cpp
```

Neither needs an `Exclusions.txt` entry on the face of it. Confirm rather than assume, and check
that no glob in `Exclusions.txt` existed only for `FileRegistry.cpp`.

**Nine new shader files** arrive with the merge, in `Code/Engine/Render/Shaders/FFX/` plus
`ClusterCompaction.esf`. **Run `CompileShaders.sh` and then `NinjaGen.py` again**, per the warning
in [Progress.md, Start here](../Progress.md#start-here): `ninja` does not know the `.esh` and
`.esf` sources, so the build stays green while the binary carries the old SPIR-V. A merge is
precisely the case that warning was written for.

**Done when** the three conflicts are resolved, `SyncUpstream.py` is clean, all 46-plus shader
stages compile to SPIR-V, and Debug and Release build the whole tree. Running is P9.4.

### P9.2 - `ClusterCulling.esf`, reapply the indirect-dispatch shim

The fork's version of this kernel carries `EE_INDIRECT_DISPATCH_ENTRY_ARGS` and
`EE_INDIRECT_DISPATCH_ENTRY_INIT` from P5.17, at 64 threads. Upstream rewrote the kernel: 128
threads, `Buffer<uint2> clusterCulling_ClusterBuffer` became `Buffer<uint4> clusterRecordBuffer`,
and it now uses 128-bit groupshared visibility masks.

**Do not merge the shim into the old body.** Take upstream's new kernel whole and reapply the
P5.17 intent on top of it - that is [01-UpstreamMerges.md](../01-UpstreamMerges.md#what-to-do-when-a-merge-conflict-is-not-trivial)
rule 3, and this conflict is the reason that rule is written down.

Check while you are in there whether upstream's new 128-thread group and its
`uint64_t2` groupshared masks need anything the SPIR-V path does not give. The wave-intrinsic
fixes in upstream's changelog are in this neighbourhood.

**Done when** the kernel compiles to valid SPIR-V with the shim in place, and the resolution is
recorded under "Merge notes" in [01-UpstreamMerges.md](../01-UpstreamMerges.md#merge-notes).

### P9.3 - The indirect argument buffers have no home

**This is the conflict that can hang a GPU, and it will not fail to compile.**

The fork added 8 lines to `Renderer_ForwardShading.cpp`, including the zeroing of
`m_ClusterCulling_ArgumentBuffer` and the comment that explains why:

> Vulkan has no indirect dispatch count, so `CmdExecuteIndirect` spends the count on the CPU and
> records one dispatch per possible command; a command past the GPU-written count reads its slot
> anyway. Zeroed, that slot is a `(0,0,0)` dispatch and a legal no-op - left stale, it is whatever
> the last frame wrote, and a garbage group count hangs the GPU.

Upstream deleted that entire clear block as part of "simplified buffer barrier handling" and put
`EE_ASSERT( !m_resourceStates.HasPendingBarriers() )` in its place. The requirement did not go
away; its home did.

Two things to establish, in order:

1. **Where upstream clears these buffers now**, if it does. It may have moved into the buffer
   abstraction, in which case the fork's need may already be met and the 8 lines are deleted with
   a recorded reason.
2. **Upstream added a second indirect argument buffer.** `m_ClusterCompaction_ArgumentBuffer` is
   new and takes the same treatment as `m_ClusterCulling_ArgumentBuffer`, for the same reason.
   Whatever the answer to 1 is, it has to cover both.

The Deferred-on-purpose row for the cluster culling argument buffer records that the editor passes
`maxNumCommands` = 146 where the engine passes 1. **The editor is where this shows up**, so a
frame in the engine proving nothing is expected, not reassuring.

**Done when** both argument buffers are known to start every frame zeroed, by whatever mechanism,
and the editor has been driven for long enough to trust it.

### P9.4 - The new culling pipeline, on Vulkan

**Needs the RTX 3090 machine. This is the task the other four exist to reach.**

Upstream replaced the culling system. Spatial-hash light culling, bitmask cluster visibility, a
compaction pass generating draw arguments, 64-bit light masks in the pixel shaders, and FFX
parallel sort arriving as nine new shader files. **None of it has ever been compiled for SPIR-V,
let alone executed against this backend.** Every Vulkan conformance finding from Phases 5 and 8 was
made against the culling pipeline this commit deletes.

Work it the way P6.8 and the 2026-08-31 NVIDIA session worked: host validation on, no message-ID
filters, and treat any filter you need as a finding rather than a setting.

Expect the failures to cluster where they always have:

- **Wave and subgroup intrinsics.** Upstream's changelog says it fixed several wave-intrinsic
  bugs. `SelectPosFromLSBRank` and `CountBits128` are new and are exactly this shape.
- **Indirect dispatch and indirect argument buffers.** P5.17's shim is the fork's only answer here
  and upstream just added a second consumer of it.
- **The feature blocks.** P8.5 fixed query-as-enable in three of them. New code asking for a new
  capability goes through the same path.
- **`HLSL_STATIC_ASSERT` is compiled out on SPIR-V.** Upstream added shared structs. Their size
  checks are absent on Linux and present on Windows, so a mismatch introduced by this merge is
  invisible here and fires on P8.1.

Record each defect as "Linux port bug" or "upstream bug" per Conventions rule 3. **An upstream bug
found here is reported, not fixed.**

**Done when** the engine renders pbrdemo through the new culling pipeline on the 3090 with host
validation on, and the editor opens the map, with every remaining validation message either fixed
or recorded with a reason.

### P9.5 - Post-merge audit and provenance

Run the audit in [01-UpstreamMerges.md](../01-UpstreamMerges.md#post-merge-audit) in full. It is
mechanical and it is written down; the point of this task is that it actually gets run rather than
skimmed, on the merge it was designed for.

Beyond the audit itself:

- **Append the sync point** to the table in
  [01-UpstreamMerges.md](../01-UpstreamMerges.md#recording-sync-points). Date, commit `47e6293`,
  and a one-line note. The table has had one row since 2026-08-13 and this is the second.
- **Every non-trivial resolution goes under "Merge notes"**, newest first. P9.2 and P9.3 both
  qualify. This is the section that answers "why does this file look like that" for the next
  person, and it is currently empty.
- **Verify [TouchedFiles.md](../TouchedFiles.md) against the post-merge diff.** Upstream deleted
  and renamed files; a registry row pointing at a file that no longer exists is a defect in the
  merge procedure, not a documentation nit.
- **Re-measure the four numbers** in [What the trial merge measured](#what-the-trial-merge-measured)
  against the merged tree, so P8.7's fork review has a real baseline rather than a pre-merge one.

**Done when** the audit has been run, the sync point and merge notes are recorded, and
`TouchedFiles.md` is verified against the diff rather than believed.

---

## What this phase does not settle

**P8.1, the Windows build, is still unrun and this phase does not change that.** It gets worse in
one specific way and better in another, and both are worth stating.

Worse: this merge adds upstream code that has only ever been built on Windows to a tree that has
only ever been built on Linux, so the two are now interleaved in files neither platform has
compiled in its merged form.

Better: every one of upstream's 281 files came from a tree that MSBuild builds. The merge does not
introduce new *fork* edits to shared files - P9.2 and P9.3 reapply existing ones onto new shapes.
The shader files remain the risk they already were.

**Do not treat P9.4 as a substitute for P8.1.** A correct frame on Linux says nothing about the
Direct3D 12 path, and `HLSL_STATIC_ASSERT` being compiled out on SPIR-V is the concrete reason.

---

## Acceptance criteria

1. **`main` carries upstream `47e6293`**, merged through a reviewed PR, not merged directly.
2. **All three conflicts are resolved by reapplying fork intent onto upstream's new shape**, with
   neither side deleted to make the conflict go away.
3. **`SyncUpstream.py` is clean** and the two source-list changes have been reviewed rather than
   accepted.
4. **All shader stages compile to SPIR-V**, including the nine new files, and `CompileShaders.sh`
   plus `NinjaGen.py` were re-run in that order.
5. **Debug and Release build the whole tree on Linux.**
6. **Both indirect argument buffers are known to start every frame zeroed**, and the editor has
   been driven long enough to trust it.
7. **The engine renders pbrdemo through the new culling pipeline** on the RTX 3090 with host
   validation on and no message-ID filters.
8. **The editor opens the map** and its tools open.
9. **The post-merge audit has been run in full**, and the sync point and merge notes are recorded
   in [01-UpstreamMerges.md](../01-UpstreamMerges.md).
10. **`TouchedFiles.md` is verified against the post-merge diff.**
11. **The Windows MSBuild build still succeeds.** Unmeasurable until P8.1, and it stays unmeasured
    rather than being quietly dropped.

## Do not

- **Do not resolve a conflict by deleting either side.**
  [01-UpstreamMerges.md](../01-UpstreamMerges.md#what-to-do-when-a-merge-conflict-is-not-trivial)
  rules 1 and 2. Both failure modes are silent.
- **Do not fix upstream bugs found in P9.4.** Record them. Conventions rule 3. This applies with
  more force than usual: upstream just rewrote this code and a bug here may already be known to
  them.
- **Do not port new Vulkan capability to make upstream's new code work.** If the new culling
  pipeline needs something the backend does not have, that is a finding and a decision to escalate,
  not a Phase 5 group reopened inside a merge.
- **Do not merge directly onto `main`**, however clean the branch looks. See
  [/AGENTS.md, Branching](../../AGENTS.md#branching).
- **Do not fold P8.7's fork review into this phase.** The review needs the merge done and measured;
  it does not need to happen at the same time.

## Notes for the next agent

**Land the comment-cleanup PR first.** The trial merge was run on top of it, and the three
conflicts reported above are the conflicts you will get. Merging from an older base means finding
them twice.

**The three-conflict number is the good news and it is also the trap.** It is easy to resolve three
hunks, watch the tree build, and call the merge done. The tree building proves almost nothing here:
upstream deleted the culling system this port was verified against, and the failure modes in that
neighbourhood are GPU hangs and silently wrong data, not compile errors. **P9.1 is the cheap part
and P9.4 is the phase.**
