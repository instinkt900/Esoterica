# AGENTS.md

Development workflow for this repository. This file covers **how** work flows. For **what** the
work builds towards, see [Docs/Linux/](Docs/Linux/README.md).

---

## What this repository is

A fork of [Esoterica](https://github.com/BobbyAnguelov/Esoterica), a prototype game engine. This
fork adds **Linux support**. Upstream is Windows-only, under active development, and rejects large
PRs.

**Prime directive: keep `git merge upstream/main` cheap, forever.** A Linux port that works but
cannot absorb upstream changes has failed. Every rule below serves that goal.

In practice: add files instead of editing them. When you must edit an upstream file, make the edit
a 2-3 line `#elif defined( __linux__ )` branch next to the existing `#if _WIN32`.

## At the start of every session

Read these, in order:

1. **[Docs/Linux/00-Conventions.md](Docs/Linux/00-Conventions.md)** - the porting rules and code
   style. These are not negotiable. A violation creates merge debt that someone else pays later.
2. **[Docs/Linux/Progress.md](Docs/Linux/Progress.md)** - what is done, what is in flight, and the
   open questions.
3. **[Docs/Linux/Blocked.md](Docs/Linux/Blocked.md)** - what is written but not verified, indexed
   by the machine that would unblock it. Check it before you conclude something is untested, and
   add a row to it when your task leaves something unverified.
4. **The phase document for your task**, in [Docs/Linux/Phases/](Docs/Linux/Phases/).

Then do the task you were given, and nothing else. Do not fix, tidy, reformat, or improve upstream
code that you read along the way. Conventions rule 3 lists the specific temptations to resist.

## Remotes

```
origin      github-personal:instinkt900/Esoterica.git      this fork
upstream    https://github.com/BobbyAnguelov/Esoterica.git  read-only reference
```

If `git remote -v` does not show `upstream`, add it:

```bash
git remote add upstream https://github.com/BobbyAnguelov/Esoterica.git
git fetch upstream
```

`upstream/main` is the clean reference. Never commit to it. Never merge into it. Use it freely for
comparison. `git diff upstream/main -- <path>` is the fastest way to see what this fork changed,
and several phase acceptance criteria are written in those terms.

## Branching

**`main` carries the Linux work.** This is a fork, so divergence from upstream is the point. There
is no long-lived `linux` branch, on purpose. `upstream/main` already gives a clean reference, so a
second long-lived branch would only add a second drift axis to manage.

**The invariant: `main` builds on both Windows and Linux.** That is what these rules protect. It is
why every phase ends its acceptance criteria with "the Windows MSBuild build still succeeds".

Do task work on a **short-lived branch off `main`**. Name the branch after its task ID from the
phase docs:

```
main
  ├── linux/p0.1-slnx-parsing      ← PR merged, branch deleted
  ├── linux/p1.3-threading         ← PR merged, branch deleted
  └── linux/p5.7-pipelines         ← in flight
```

```bash
git checkout main && git pull
git checkout -b linux/p1.3-threading
# ... work ...
git push -u origin linux/p1.3-threading
gh pr create --repo instinkt900/Esoterica --base main --fill
```

**Always pass `--repo instinkt900/Esoterica` to `gh pr create`.** When `origin` is a fork, `gh`
defaults the base repository to the **parent**, so a bare `gh pr create` opens the PR against
`BobbyAnguelov/Esoterica`. It asks first in a terminal and picks the parent silently when it is
not interactive. That has already happened twice.

This clone is pinned with `gh repo set-default instinkt900/Esoterica`, which writes
`remote.origin.gh-resolved = base` to `.git/config`. Check it with `gh repo set-default --view`.
The setting is local to the clone, so a fresh clone needs it again. Pass `--repo` anyway.

Never open a PR against `upstream`. It is a read-only reference.

### All code changes go through a pull request

**A reviewed PR merges every work branch. There are no exceptions, and the agent that wrote the
branch never merges it.**

- **Open a PR when the task is done.** Push the branch and open the PR. Do not merge it yourself.
- **Do not merge your own PR.** Review is a human step. An agent that merges its own work has
  removed the only checkpoint in the process.
- **Do not push to `main`** for anything that the "code" definition below covers.
- **One PR per task**, to match one branch per task. A phase runs for weeks. A task is the
  reviewable unit.
- **Open the PR early.** A task branch that lives more than a few days has become the long-lived
  branch that this strategy avoids.
- Merge with a merge commit, not a squash and not a rebase. Each task then stays one identifiable
  unit in `git log`. Delete the branch afterwards.
- The PR description states which acceptance criteria from the phase doc are met, and which are
  not. See [Definition of done](#definition-of-done).

This rule holds even with one person driving the work: **the agent writes, the human reviews.**
That is the whole value of the checkpoint. It disappears if the agent both writes and merges.

### What the PR description says

**Describe the change, not the work that produced it.**

Cover:

- What the change does, and how that addresses the problem it sets out to solve.
- How to verify it, where that is not obvious from the diff.
- How to use anything new, when the PR adds a feature, a macro or a flag that other code calls.
- Which acceptance criteria from the phase doc are met, and which are not.

Leave out:

- The order things were tried in.
- Wrong turns, dead ends, and hypotheses that did not survive.
- Narration of the investigation: what was suspected, what was ruled out, what turned out to be
  the case.

A reviewer is deciding whether to merge the diff in front of them. The route taken to it does not
help with that decision, and burying the change under a travelogue makes the decision harder.

None of that means throwing the investigation away. **It belongs in
[Docs/Linux/Progress.md](Docs/Linux/Progress.md)**, which exists to carry exactly that between
sessions - what was measured, what misled, and what the next session should not repeat. Two
documents, two audiences. Keep them apart.

### What may go directly to `main`

Documentation and bookkeeping only:

- [Docs/Linux/Progress.md](Docs/Linux/Progress.md) updates
- [Docs/Linux/TouchedFiles.md](Docs/Linux/TouchedFiles.md) registry updates
- Task, phase, and timeline revisions in [Docs/Linux/](Docs/Linux/)
- `AGENTS.md` and `README.md` edits
- Typos and wording fixes in any of the above

Everything else is code and needs a PR. This includes the build scripts (`NinjaGen.py`,
`DownloadDependencies.sh`, `RunReflection.sh`, `CompileShaders.sh`) and `.gitignore`. If a change
alters what gets built, or how, it is code, even when it is not C++.

An **upstream merge** also goes through a PR. It is not a doc change.
[01-UpstreamMerges.md](Docs/Linux/01-UpstreamMerges.md) requires a post-merge audit and a
two-platform rebuild, which is what a review checkpoint is for. Put the merge on an
`upstream-merge/<date>` branch.

If you ever run parallel agents on independent tasks, give each one its own git worktree so they
cannot collide on the filesystem.

## Commits

Match upstream style. Keep the subject short and in Title Case. Do not use conventional-commits
prefixes:

```
Render Stability Improvements
Light Culling + Improvements to Gizmo + Fixes
```

For Linux port work, **prefix the subject with `[Linux]`**:

```
[Linux] Threading platform layer
[Linux] Vulkan RHI - buffers and textures
[Linux] Build generator - slnx parsing
```

This keeps the port commits easy to separate from merged upstream history in `git log`, which
matters when you audit a fork.

Never mix an upstream merge and port work in one commit.

## Writing style

Write all prose in plain, simple, concise English. This covers code comments, PR descriptions,
Progress.md entries, commit messages, and these documents.

Agent writing runs wordy by default. Keep it easy to read instead:

- One idea per sentence. Keep sentences short.
- Active voice. "The generator skips the project", not "the project is skipped by the generator".
- Plain words. "use", not "leverage". "show", not "demonstrate". "fix", not "remediate".
- Cut filler: hedging, throat-clearing, and restatements of what the reader already knows.
- Be specific. Name the file, function, or flag instead of gesturing at it.

The test: a reviewer who skims a PR, or a future session that reads Progress.md, gets the point in
one pass. A paragraph that needs a second read is too long or too twisted. Rewrite it.

## Testing

**The Linux build is the test.** Getting `ninja` further through the tree is the goal, and a
compile error is louder, more specific and cheaper than an assertion that duplicates it.

Do not write a check for something a build would catch. No asserting that a rule passes
`-std=c++20`, that a target resolves to a `.so`, or that link order is right. Build it instead.

`Code/Scripts/NinjaGen/Checks.py` holds the exception: failures that leave a **green build**
behind and surface much later, usually on an upstream merge. Drift detection, determinism, stale
exclusion globs, an unmapped property sheet. Add to it only when the failure would be silent.
See the 2026-08-27 decision in [Docs/Linux/Progress.md](Docs/Linux/Progress.md).

## Definition of done

A task is done only when all of these hold:

1. The task meets its **acceptance criteria in the phase doc**. The criteria are written to be
   checkable, so check them instead of assuming.
2. **The Linux build succeeds**, or gets measurably further than it did before:
   ```bash
   python3 Code/Scripts/NinjaGen/NinjaGen.py && ninja -f Build/Linux/Esoterica.ninja
   ```
   **That builds `Linux_Debug` only.** The generated `default` rule lists the Debug outputs and
   nothing else, so a Release binary can sit a day stale behind a green build. Name the
   configuration when you mean it:
   ```bash
   ninja -f Build/Linux/Esoterica.ninja Build/Linux_Release/Esoterica.Applications.Editor
   ```
   Until Phase 1 finishes, "further" is the measure. Say how far it got, and what stopped it.
3. **The Windows MSBuild build still succeeds**, unchanged. This is not optional, and it is not
   someone else's problem. A port that breaks Windows is worse than no port.
4. **[Docs/Linux/TouchedFiles.md](Docs/Linux/TouchedFiles.md)** lists every upstream file you
   edited, with its reason and status.
5. **[Docs/Linux/Progress.md](Docs/Linux/Progress.md)** records what you did and anything the next
   session needs to know.
6. **[Docs/Linux/Blocked.md](Docs/Linux/Blocked.md)** has a row for anything your task wrote but
   could not verify, saying which machine would unblock it. Delete a row only when the thing is
   verified.
7. **A PR is open** against `main`. Its description states which acceptance criteria are met and
   which are not.

Items 4 to 6 keep a chain of independent sessions coherent. Skipping them is the most expensive
shortcut available. Include them **in the task's own PR**. The "directly to `main`" allowance
covers standalone doc updates, not the bookkeeping that belongs with a task.

**Your work ends at "PR open".** A human merges. If review asks for changes, they land as new
commits on the same branch.

## Reporting

Report outcomes honestly. If the task does not meet its acceptance criteria, say so, and say
**which** ones. Do not mark a task complete because most of it works. Do not describe a stub as an
implementation.

"Vulkan buffer creation done. Texture creation is stubbed and halts" helps the next session far
more than an optimistic summary. This matters most in Phase 5, which runs for months. There,
"which of the 16 groups are real" is the single most important piece of state.

## Merging upstream

**Never merge upstream on your own initiative. Only when asked to, explicitly.**

There is no schedule and no trigger. Not weekly, not before a phase, not because
`SyncUpstream.py` reports drift, and not because `upstream/main` has moved. If you notice new
upstream commits, say so and carry on with the task you were given.

The reason: the port is being built against **one fixed upstream commit** until it works
end-to-end. Upstream develops slowly, so the drift is cheap to absorb later, and absorbing it
early means debugging the port and the merge at the same time. That trade is bad.

When a merge is asked for, [01-UpstreamMerges.md](Docs/Linux/01-UpstreamMerges.md) holds the full
procedure, including the post-merge audit that catches new Windows-only code paths. It goes on an
`upstream-merge/<date>` branch and reaches `main` through a PR, like any other code change.

Do not merge onto a dirty tree. Land or shelve in-flight task branches first.

## Escalate instead of improvising

Stop and ask when:

- Your task needs an edit to an upstream file that
  [TouchedFiles.md](Docs/Linux/TouchedFiles.md) does not list. A full survey produced that
  registry. A file outside it means the survey missed something, or your approach has drifted.
- Your task needs a public signature change in a shared header.
- Your task needs to change anything under `Code/**/ThirdParty/`. Upstream owns those directories.
- Your task needs a change to `Data/`.
- A shared abstraction genuinely cannot express what Linux needs.
- Two phases appear to conflict.

Quiet scope expansion is worse than a pause. The blast radius of an edit is the thing these rules
manage.

## Cut before adding

Efficient, never careless. The best code is the code never written.

Read the code a change touches before writing it. Skip that only for a brand-new file with nothing to read.

Then stop at the first rung that holds and act on it. Do not check the rungs below it.

1. Not genuinely needed? Skip it. Say so in one line.
2. Already in this codebase? One search. Reuse a hit, or move on the moment it comes up empty.
3. Stdlib does it? Use the stdlib.
4. Native platform feature does it? Use the platform.
5. An already-installed dependency does it? Use it. Never add a new one for what a few lines cover. Writing `import`/`require` for a package that is not already in the manifest is adding a dependency. Even when the user names the library, check stdlib and platform first, and reach for it only if nothing covers it.
6. Fits in one line? One line.
7. Only then: the minimum code that works, in as few statements.

The ladder is a reflex. Pick the rung and act on it in this same response.

Never narrate or deliberate the rungs, in output or in thinking.

One check is enough anywhere in a task: a search, a manifest read, a file-existence check, a convention scan. If it came back empty, or a tool error already told you what to do, act on that. Do not re-verify or broaden it.

Rules: no abstractions nobody asked for. No scaffolding for later. Boring over clever. Shortest working diff, in the right place. Bug fixes hit the root cause — one fix in the shared function beats a guard in every caller.

Never cut: validation at trust boundaries, error handling that prevents data loss, security, accessibility, or anything explicitly requested. If the user insists on the full version, build it without re-arguing.

## Report once, at the end

This turn is silent until the final message. Everything you learn goes in the final message.

Your next output after reading a tool result is another tool call. Chain the calls back to back. The final message is the only place you explain anything.

That still holds after a compact, a resume, or a long tool chain.

When your own output is consumed by another agent as a tool result, and not read as chat — you are a subagent, a Task worker, or a background agent — return the findings themselves. Data, paths, identifiers, verbatim errors, in complete clauses. No preamble. No restating of your instructions. No offers of further help. Emit no text between tool calls there either. Nobody reads it, so a progress update has no audience.
