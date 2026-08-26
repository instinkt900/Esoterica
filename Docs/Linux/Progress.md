# Progress Log

Running state of the Linux port. **Every task appends here before it counts as done**
(Conventions rule 9). Newest entries go at the top of each section.

This file keeps a chain of independent agent sessions coherent. When you start a session, read
"Current state" and "In flight" first.

---

## Current state

**Phase: 0 (not started).** No implementation work has begun. `Docs/Linux/` holds the plan only.

| Phase | Status |
|---|---|
| 0 - Build System | not started |
| 1 - Base Platform Layer | not started |
| 2 - Reflector | not started |
| 3 - Resource Compiler | not started |
| 4 - Shader Pipeline | not started |
| 5 - Vulkan RHI | not started |
| 6 - Windowing and Input | not started |
| 7 - Editor and Tools | not started |

Linux build status: **does not build.**
Windows build status: **unchanged from upstream** (no edits landed yet).

## In flight

*(nothing)*

---

## Completed work

<!--
Append one entry per completed task, newest first. Format:

### YYYY-MM-DD - P<phase>.<task> <short title>
- What you did, concretely.
- Files added: ...
- Upstream files edited: ... (must match TouchedFiles.md)
- Acceptance criteria met: which ones, and which not.
- Anything the next agent needs to know.
-->

*(none yet)*

---

## Decisions made during implementation

Record any decision that a future reader would otherwise have to work out from the code. Give
the reasoning, not just the outcome.

<!--
### YYYY-MM-DD - <decision>
**Context:** ...
**Decision:** ...
**Rationale:** ...
**Alternatives rejected:** ...
-->

*(none yet. [README.md](README.md#scope-decisions-already-made) holds the planning-time
decisions.)*

---

## Open questions

Carried from
[03-Dependencies.md](03-Dependencies.md#open-questions-to-resolve-during-implementation). Move a
question to "Decisions made" once you answer it.

| # | Question | Blocks | Status |
|---|---|---|---|
| 1 | Does `ctt` (texture compression) build on Linux? | Phase 3 | open |
| 2 | Which LLVM version does the Reflector need, and does `clangAST` compile against it on Linux? | Phase 2 | open |
| 3 | Use `volk`, or the plain Vulkan loader? | Phase 5 | open |
| 4 | Do the target distros package SDL3, or must we always build it? | Phase 6 | open |
| 5 | Does `GameNetworkingSockets` block the first `Base` link, or can we defer it? | Phase 1 | open |
| 6 | Does the `VirtualAlloc` region in `Memory.cpp` (`PageAllocator`, near line 234) have a working non-Windows path? | Phase 1 | open |

---

## Upstream issues observed

Bugs and oddities in upstream code. **Do not fix them here** (Conventions rule 3). Record them,
and file them upstream as issues if they matter.

<!-- ### <file>:<line> - <description> -->

Noted during the first survey, in `Code/Scripts/NinjaGen/NinjaGen.py`. This port rewrites that
stale build script, so these get fixed as a side effect rather than as upstream fixes:

- It parses `Esoterica.sln`, which the repo no longer contains. The project moved to
  `Esoterica.slnx`.
- `cpp_rule` calls `toolchain.compiler_c` instead of `compiler_cpp`.
- `-fsanitize-address` is not a valid flag. It should be `-fsanitize=address`.
- It declares `-std=c++17`, but the project needs C++20.

Also noted, and not fixed:

- `Code/Applications/BuildGenerator/` does not work. It emits rule references with no rule
  definitions, and it parses the legacy `.sln` GUID format. Left alone on purpose.
- `Esoterica.slnx` references `Docs/docs/CodingGuidelines.md`, which the repository does not
  contain.

---

## Merge notes

See [01-UpstreamMerges.md](01-UpstreamMerges.md#merge-notes) for the sync-point table. Record
hard conflict resolutions there, not here.
