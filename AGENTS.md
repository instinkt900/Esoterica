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
3. **The phase document for your task**, in [Docs/Linux/Phases/](Docs/Linux/Phases/).

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

## Definition of done

A task is done only when all of these hold:

1. The task meets its **acceptance criteria in the phase doc**. The criteria are written to be
   checkable, so check them instead of assuming.
2. **The Linux build succeeds** (after Phase 0 lands):
   ```bash
   python3 Code/Scripts/NinjaGen/NinjaGen.py && ninja -f Build/Linux/Esoterica.ninja
   ```
3. **The Windows MSBuild build still succeeds**, unchanged. This is not optional, and it is not
   someone else's problem. A port that breaks Windows is worse than no port.
4. **[Docs/Linux/TouchedFiles.md](Docs/Linux/TouchedFiles.md)** lists every upstream file you
   edited, with its reason and status.
5. **[Docs/Linux/Progress.md](Docs/Linux/Progress.md)** records what you did and anything the next
   session needs to know.
6. **A PR is open** against `main`. Its description states which acceptance criteria are met and
   which are not.

Items 4 and 5 keep a chain of independent sessions coherent. Skipping them is the most expensive
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

Merge upstream **weekly, or before you start a new phase**, whichever comes first. Merge cost grows
faster than the drift does. A week is routine. Six months is a research project.

[01-UpstreamMerges.md](Docs/Linux/01-UpstreamMerges.md) holds the full procedure, including the
post-merge audit that catches new Windows-only code paths.

The merge goes on an `upstream-merge/<date>` branch and reaches `main` through a PR, like any other
code change. The post-merge audit and the two-platform rebuild are what review is for.

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
