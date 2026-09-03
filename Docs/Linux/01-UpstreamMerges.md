# Upstream Merge Discipline

Upstream: `https://github.com/BobbyAnguelov/Esoterica` (branch `main`). This fork's `main`
carries the Linux work.

This document holds the **merge procedure**. For the branching strategy, and for how task work
reaches `main`, see [/AGENTS.md, Branching](../../AGENTS.md#branching). There is no long-lived
`linux` branch, on purpose. `upstream/main` already serves as the clean reference, so a second
long-lived branch would only add a second drift axis to manage.

Upstream has no releases and no version tags. It is a continuously developed `main`. There is
therefore no stable point to sync to. You sync to a commit, and you record which one.

## One-time setup

```bash
git remote add upstream https://github.com/BobbyAnguelov/Esoterica.git
git fetch upstream
```

## When to merge

**On request only. Never on your own initiative.**

There is no cadence. Not weekly, not before a phase, not when `SyncUpstream.py` reports drift,
not when `upstream/main` moves. If you notice new upstream commits, mention them and carry on
with the task you were given.

The port is being built against **one fixed upstream commit** until it works end-to-end.
Upstream develops slowly - 107 commits in total, and four in the last twelve months touched a
source list - so the drift stays cheap to absorb. Absorbing it early means debugging the port
and the merge at the same time, and that trade is a bad one.

This reverses the original plan, which called for weekly merges. The measurement in
[Progress.md](Progress.md) is what changed it: at this rate of upstream change, the drift is not
the risk. Half-finished work colliding with a merge is.

**A merge was asked for on 2026-09-03**, and it is
[Phase 9](Phases/Phase9-UpstreamMerge.md). That phase holds what the trial merge found - three
conflicts, and which ones are not trivial. This document still holds the procedure, and Phase 9
does not repeat it.

The rest of this document is the procedure for when a merge **is** asked for.

## The merge procedure

```bash
# 1. Land or shelve all in-flight Linux work first. Never merge onto a dirty tree.
git status --porcelain          # must be empty

# 2. Fetch and inspect what is coming.
git fetch upstream
git log --oneline HEAD..upstream/main

# 3. Check whether upstream touched anything in the registry. This is the whole game.
git diff --name-only HEAD..upstream/main > /tmp/upstream-changed.txt
#    Compare /tmp/upstream-changed.txt against TouchedFiles.md.

# 4. Merge onto a branch, not directly onto main.
git checkout main && git pull
git checkout -b upstream-merge/$( date +%Y-%m-%d )
git merge upstream/main

# 5. Rebuild both platforms before you call it done.
#    Windows: msbuild, per README.md
#    Linux:   python3 Code/Scripts/NinjaGen/NinjaGen.py && ninja -f Build/Linux/Esoterica.ninja

# 6. Run the post-merge audit below, then open a PR. Do not merge it yourself.
git push -u origin HEAD && gh pr create --fill
```

Step 4 uses a branch because an upstream merge can break either platform. The merge reaches
`main` through a reviewed PR, like any other code change. See
[/AGENTS.md, Branching](../../AGENTS.md#branching).

### Step 3 is the important one

The registry in [TouchedFiles.md](TouchedFiles.md) makes this check mechanical. Every Linux edit
to an upstream file is a 2-line `#elif` addition (Conventions rule 2). Conflicts are therefore
rare, and easy to resolve even when upstream does touch the same file. You keep both branches.

These cases carry real risk, and need attention on every merge:

| Upstream change | Consequence | What to do |
|---|---|---|
| A new `#if _WIN32` guard in a shared file | The Linux build breaks with a link error, or worse, behaves wrongly and says nothing | Grep for new `_WIN32` matches after each merge. See below. |
| A new function in `RHI.h` | `RHI_Vulkan.cpp` no longer implements the full interface, so the link fails | The link error catches it. Implement the new function. |
| A signature change in `RHI.h` | `RHI_Vulkan.cpp` breaks | The compile error catches it. |
| A new `.vcxproj` project in `.slnx` | The Linux build silently omits it | `SyncUpstream.py` fails until the project is synced into `UpstreamProjects.txt`. |
| A new source file, of any name | The build silently excludes it, or wrongly includes it | `SyncUpstream.py` fails until it is synced, and the sync diff names it. Decide then whether it needs an entry in `Exclusions.txt`. |
| Upstream deletes or renames a source you excluded | A stale glob in `Exclusions.txt` quietly stops doing its job | `SourceLists.py` reports any exclusion glob that matches nothing. |
| A new `External/` dependency | The Linux build cannot find it | Update [03-Dependencies.md](03-Dependencies.md). |
| Upstream renames a `Platform/` directory | Your `_Linux.cpp` is orphaned | The compile error catches it. |

### Post-merge audit

Run this after every merge. It catches the dangerous "new Win32 guard" case:

```bash
# New platform guards introduced by the merge, excluding thirdparty:
git diff HEAD@{1} --unified=0 -- 'Code/**' \
  | grep '^+' \
  | grep -E '_WIN32|_MSC_VER|windows\.h' \
  | grep -vi thirdparty
```

Any hit means a shared file grew a Windows-only path. Decide whether Linux needs an `#elif`
sibling, add it, and update the registry.

Then resync the source list. This is the step that decides what the Linux build compiles, so
read the diff rather than skimming it:

```bash
python3 Code/Scripts/NinjaGen/SyncUpstream.py --update
git diff Code/Scripts/NinjaGen/UpstreamProjects.txt
```

Every added `src` line is a new source that the Linux build will now compile. For each one,
decide whether it belongs in `Exclusions.txt`. Every removed line is a source upstream dropped;
check that no glob in `Exclusions.txt` existed only for it.

```bash
# Reports any exclusion glob that no longer matches anything.
python3 Code/Scripts/NinjaGen/SourceLists.py
```

Also re-check the invariant that makes the sync work:

```bash
# Should be 0. A non-zero count means upstream started to use per-config file exclusion,
# which the sync does not model.
grep -c 'ExcludedFromBuild' Code/*/*.vcxproj Code/*/*/*.vcxproj | grep -v ':0'
```

## Special case: `Code/Scripts/NinjaGen/NinjaGen.py`

This is the one upstream file that the port **rewrites** instead of editing minimally. That is
a deliberate exception:

- Upstream's copy is a stale, broken experiment. It parses the old `.sln` format that the repo
  no longer uses, and it emits no link rules and no library flags. It cannot run.
- It is not part of the shipped build. Nothing depends on it.
- Upstream is unlikely to develop it further, exactly because it is dead.
- Rewriting it costs far less than maintaining a parallel CMake tree, which would duplicate the
  `.vcxproj` file lists with nothing checking the two against each other.

Accept that this one file will conflict wholesale if upstream ever revives it. Resolving that
conflict then means reading upstream's version and making a decision. Everything else in the
port stays a minimal diff.

## What to do when a merge conflict is not trivial

1. **Do not resolve by deleting the Linux side.** That regresses the port silently.
2. **Do not resolve by deleting the upstream side.** That forks behavior silently, and the next
   merge is worse.
3. Work out what upstream changed and why. Then re-apply the Linux intent on top of the new
   upstream shape.
4. Record the resolution in [Progress.md](Progress.md) under "Merge notes". This matters most
   when upstream restructured something the plan assumed.

## Recording sync points

Append to the table below on every merge. This is the fork's provenance record. Without it, "why
does this file look like that" has no answer.

| Date | Upstream commit | Notes |
|---|---|---|
| 2026-08-13 | `6813cf9` | Fork point for the Linux port plan. The survey in [README.md](README.md) reflects this commit. |
| 2026-09-03 | `47e6293` | "Push Esoterica Main @800". The first real merge, and [Phase 9](Phases/Phase9-UpstreamMerge.md). 281 files, +5,668 / -36,670: new spatial-hash light culling, a new instance and cluster culling system with a compaction pass, FFX parallel sort, animation graph changes, resource server rework, `FileRegistry` to `DataFileSystem`. **Three conflicts, one hunk each**, and upstream touched no `RHI/`, `Vulkan/` or `Linux/` file. **The conflicts were not the cost** - see the merge notes below. |

## Merge notes

<!-- Append notable resolutions here. Newest first. -->

### 2026-09-03, `47e6293`

**Read this before the next merge.** [Progress.md](Progress.md)'s P9.1-P9.4 entry has the full
detail; this is the part that generalises.

**Three single-hunk conflicts, and none of the real work was in them.** The conflicts took a
morning. Six defects took the rest, and no conflict marker pointed at any of them. Budget the next
merge accordingly: the conflict count tells you almost nothing about the cost.

#### The three conflicts, and how they were resolved

| File | Resolution |
|---|---|
| `EditorUI.h` | Include collision only, from `FileRegistry.h` to `DataFileSystem.h`. Kept both sides. |
| `ClusterCulling.esf` | Upstream rewrote the kernel - 128 threads, `Buffer<uint4> clusterRecordBuffer`, 128-bit groupshared masks. **P5.17's shim was reapplied onto the new body rather than merged into the old one**, so the file now differs from upstream by exactly the two indirect declaration macros, the `__spirv__` rename block, and the two entry macros. |
| `Renderer_ForwardShading.cpp` | Upstream deleted the whole "Clear Buffers" block that held the Vulkan indirect-argument zeroing and moved clearing into a new `pCommandBuffer_GeometryCulling`. The zeroing was reapplied there. **Only `ClusterCulling_ArgumentBuffer` needs it**, and that was checked rather than carried over: `ClusterCompaction.esf` appends through an `InterlockedAdd` so its slots go stale, while `InstanceCulling.esf` writes slot `groupID` unconditionally for every page and passes no count buffer. |

#### The five things that cost more than the conflicts

1. **A new upstream `.esf` reached by `CmdExecuteIndirect` needs P5.17's shim, and nothing warns
   you.** `ClusterCompaction.esf` arrived without it, upstream passes `nullptr` for its root
   constants because they come per-command from the argument buffer, so on Vulkan `RootCBV` read as
   zero, every buffer handle was 0, and **the frame was black with no validation message** -
   reading descriptor 0 is legal. Not the compiler, not validation, not the build. **Next merge:
   diff the new `.esf` files against the `CmdExecuteIndirect` call sites.**
2. **`WaveMatch` has no core Vulkan lowering.** Core SM 6.5 on Direct3D 12, and DXC can only emit
   `OpGroupNonUniformPartitionNV`. Enabling `VK_NV_shader_subgroup_partitioned` made the Linux
   renderer NVIDIA-only; see the queue in [Blocked.md](Blocked.md).
3. **Dead upstream code can stop the whole build.** The FFX radix sort has no callers and does not
   compile on *any* platform, and because the Reflector runs its shader pass before type
   reflection, it generated zero typeinfo and nothing linked. Excluded in `Reflector.cpp` and
   `Exclusions.txt`.
4. **A stored pointer to caller-owned settings.** `RenderSystem::Initialize` began keeping
   `&settings`; both `ResourceServer` entry points pass a stack local. Fixed in this fork's Linux
   sibling; **the Win32 one still dangles.**
5. **An assert that never fired became one that always does.** `EE_ASSERT( "LOST CONNECTION!!" )`
   asserts on a string literal. Upstream made it `EE_TRACE_HALT`, which turned an invisible startup
   race into a guaranteed abort. Fixed by tolerating `IsConnecting()`.

#### Procedural corrections

- **Step 3's command is wrong for this purpose.** `git diff --name-only HEAD..upstream/main` is the
  whole fork divergence - 282 files here - not what the merge brings. Use the merge base *before*
  merging, and the merge commit's first parent *after*:
  ```bash
  git diff --name-only $( git merge-base HEAD upstream/main ) upstream/main   # before: 143 files
  git diff --stat <merge-commit>^1 HEAD -- <path>                            # after
  ```
  **After the merge is committed, `git merge-base HEAD upstream/main` IS `upstream/main`**, so any
  diff against it comes back empty and reads as "upstream did not touch this file". That produced
  two wrong conclusions in this merge before it was noticed.
- **Check the registry by filename, not by grepping paths out of the document.** The path spellings
  in [TouchedFiles.md](TouchedFiles.md) are not uniform; a path-based sweep reported 11 gaps of
  which 7 were false. Five real gaps were found and registered.
- **Run Debug, not just Release.** Release compiles its asserts out. Four of the six defects above
  were found in Release, and the dangling settings pointer only surfaced when the developer ran the
  Debug editor - Release had been reading freed stack and surviving.
