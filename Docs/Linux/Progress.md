# Progress Log

Running state of the Linux port. **Every task appends here before it is considered done**
(Conventions rule 9). Newest entries at the top of each section.

This file is how a chain of independent agent sessions stays coherent. If you are starting a
session, read the "Current state" and "In flight" sections first.

---

## Current state

**Phase: 0 (not started).** No implementation work has begun. `Docs/Linux/` contains the plan
only.

| Phase | Status |
|---|---|
| 0 — Build System | not started |
| 1 — Base Platform Layer | not started |
| 2 — Reflector | not started |
| 3 — Resource Compiler | not started |
| 4 — Shader Pipeline | not started |
| 5 — Vulkan RHI | not started |
| 6 — Windowing & Input | not started |
| 7 — Editor & Tools | not started |

Linux build status: **does not build.**
Windows build status: **unchanged from upstream** (no edits landed yet).

## In flight

*(nothing)*

---

## Completed work

<!--
Append one entry per completed task, newest first. Format:

### YYYY-MM-DD — P<phase>.<task> <short title>
- What was done, concretely.
- Files added: ...
- Upstream files edited: ... (must match TouchedFiles.md)
- Acceptance criteria met: which ones, and which not.
- Anything the next agent needs to know.
-->

*(none yet)*

---

## Decisions made during implementation

Record any decision that a future reader would otherwise have to reverse-engineer. Include the
reasoning, not just the outcome.

<!--
### YYYY-MM-DD — <decision>
**Context:** ...
**Decision:** ...
**Rationale:** ...
**Alternatives rejected:** ...
-->

*(none yet — planning-time decisions are recorded in [README.md](README.md#scope-decisions-already-made))*

---

## Open questions

Carried from [03-Dependencies.md](03-Dependencies.md#open-questions-to-resolve-during-implementation).
Move to "Decisions made" once answered.

| # | Question | Blocks | Status |
|---|---|---|---|
| 1 | Does `ctt` (texture compression) build on Linux? | Phase 3 | open |
| 2 | Which exact LLVM version does the Reflector need, and does `clangAST` compile against it on Linux? | Phase 2 | open |
| 3 | `volk` vs the plain Vulkan loader? | Phase 5 | open |
| 4 | Is SDL3 packaged on the target distros, or must we always build it? | Phase 6 | open |
| 5 | Does `GameNetworkingSockets` block the first `Base` link, or can it be deferred? | Phase 1 | open |
| 6 | Does `Memory.cpp`'s `VirtualAlloc` region (`PageAllocator`, ~line 234) have a working non-Windows path? | Phase 1 | open |

---

## Upstream issues observed

Bugs or oddities noticed in upstream code. **Do not fix these here** (Conventions rule 3) —
record them, and file them upstream as issues if they matter.

<!-- ### <file>:<line> — <description> -->

Noted during the initial survey, in `Code/Scripts/NinjaGen/NinjaGen.py` (the stale build script
this port rewrites, so these get fixed as a side effect rather than as upstream fixes):

- Parses `Esoterica.sln`, which no longer exists in the repo (migrated to `Esoterica.slnx`).
- `cpp_rule` invokes `toolchain.compiler_c` rather than `compiler_cpp`.
- `-fsanitize-address` is not a valid flag; should be `-fsanitize=address`.
- Declares `-std=c++17` while the project requires C++20.

Also noted, not fixed:

- `Code/Applications/BuildGenerator/` is non-functional (emits rule references with no rule
  definitions, and parses the legacy `.sln` GUID format). Left alone deliberately.
- `Docs/docs/CodingGuidelines.md` is referenced by `Esoterica.slnx` but absent from the
  repository.

---

## Merge notes

See [01-UpstreamMerges.md](01-UpstreamMerges.md#merge-notes) for the sync-point table.
Record non-trivial conflict resolutions there, not here.
