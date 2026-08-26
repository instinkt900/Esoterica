# AGENTS.md

Development workflow for this repository. **How** work flows. For **what** the work is building
towards, see [Docs/Linux/](Docs/Linux/README.md).

---

## What this repository is

A fork of [Esoterica](https://github.com/BobbyAnguelov/Esoterica), a prototype game engine, being
extended with **Linux support**. Upstream is Windows-only, actively developed, and explicitly
rejects large PRs.

**Prime directive: keep `git merge upstream/main` painless, forever.** A Linux port that works but
cannot absorb upstream changes is a failure. Every workflow rule below exists to serve that.

In practice: add files rather than editing them, and when you must edit an upstream file, make it a
2–3 line `#elif defined( __linux__ )` branch beside the existing `#if _WIN32`.

## At the start of every session

Read, in order:

1. **[Docs/Linux/00-Conventions.md](Docs/Linux/00-Conventions.md)** — the porting rules and code
   style. Non-negotiable. Violating these creates merge debt someone pays later.
2. **[Docs/Linux/Progress.md](Docs/Linux/Progress.md)** — what is done, what is in flight, open
   questions.
3. **The phase document for your task**, in [Docs/Linux/Phases/](Docs/Linux/Phases/).

Then do exactly the task you were given. Do not opportunistically fix, tidy, reformat, or improve
upstream code you happen to read along the way — see Conventions rule 3, which lists the specific
temptations to resist.

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

`upstream/main` is the pristine reference. Never commit to it, never merge into it. Use it freely
for comparison — `git diff upstream/main -- <path>` is the fastest way to see exactly what this
fork has changed, and several phase acceptance criteria are stated in those terms.

## Branching

**`main` carries the Linux work.** This is a fork; diverging from upstream is the point. There is
deliberately no long-lived `linux` branch — `upstream/main` already provides a clean reference, so
a second long-lived branch would only add a second drift axis to manage.

**The invariant: `main` builds on both Windows and Linux.** That is the thing being protected. It
is why every phase's acceptance criteria end with "the Windows MSBuild build still succeeds."

Do task work on a **short-lived branch off `main`**, named for its task ID from the phase docs:

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
gh pr create --fill
```

### All code changes go through a pull request

**Every work branch is merged via a reviewed PR. No exceptions, and never by the agent that wrote
it.**

- **Open a PR when the task is done.** Push the branch and open it; do not merge it yourself.
- **Do not merge your own PR.** Review is a human step. An agent that merges its own work has
  removed the only checkpoint in the process.
- **Do not push to `main`** for anything covered by the "code" definition below.
- **One PR per task**, matching one branch per task. Phases run for weeks; tasks are the
  reviewable unit.
- **Open the PR promptly.** A task branch that lives more than a few days has become the
  long-lived branch this strategy exists to avoid.
- Merge with a merge commit (not squash, not rebase) so each task stays one identifiable unit in
  `git log`, and delete the branch afterwards.
- The PR description should state which acceptance criteria from the phase doc are met, and which
  are not. See [Definition of done](#definition-of-done).

The reason this holds even with one person driving the work: **the agent writes, the human
reviews.** That is the whole value of the checkpoint, and it disappears entirely if the agent both
writes and merges.

### What may go directly to `main`

Documentation and bookkeeping only:

- [Docs/Linux/Progress.md](Docs/Linux/Progress.md) updates
- [Docs/Linux/TouchedFiles.md](Docs/Linux/TouchedFiles.md) registry updates
- Task, phase, and timeline revisions in [Docs/Linux/](Docs/Linux/)
- `AGENTS.md` and `README.md` edits
- Typos and wording fixes in any of the above

Everything else is code and needs a PR — including build scripts (`NinjaGen.py`,
`DownloadDependencies.sh`, `RunReflection.sh`, `CompileShaders.sh`) and `.gitignore`. If a change
alters what gets built or how, it is code, even if it isn't C++.

An **upstream merge** also goes through a PR. It is not a doc change, and
[01-UpstreamMerges.md](Docs/Linux/01-UpstreamMerges.md) requires a post-merge audit plus a
two-platform rebuild — exactly the thing a review checkpoint is for. Put it on an
`upstream-merge/<date>` branch.

If parallel agents are ever run on independent tasks, give each its own git worktree so they cannot
collide on the filesystem.

## Commits

Match upstream's style — short, Title Case, no conventional-commits prefixes:

```
Render Stability Improvements
Light Culling + Improvements to Gizmo + Fixes
```

For Linux port work, **prefix with `[Linux]`**:

```
[Linux] Threading platform layer
[Linux] Vulkan RHI - buffers and textures
[Linux] Build generator - slnx parsing
```

This makes the port's commits trivially separable from merged-in upstream history in `git log`,
which matters a lot when auditing a fork.

Never mix an upstream merge with port work in one commit.

## Writing style

All prose — code comments, PR descriptions, Progress.md entries, commit messages — is written in
plain, simple, concise English.

Agent writing runs wordy by default. Keep it easy to digest instead:

- One idea per sentence. Short sentences.
- Active voice. "The generator skips the project", not "the project is skipped by the generator".
- Plain words. "use" not "leverage", "show" not "demonstrate", "fix" not "remediate".
- Cut filler: hedging, throat-clearing, and restating what the reader already knows.
- Be specific. Name the file, function, or flag instead of gesturing at it.

The test: a reviewer skimming a PR, or a future session reading Progress.md, gets the point in
one pass. A paragraph that needs a second read is too long or too twisted — rewrite it.

## Definition of done

A task is not done until all of these hold:

1. Its **acceptance criteria in the phase doc** are met — they are written to be mechanically
   checkable, so check them rather than assuming.
2. **The Linux build succeeds** (once Phase 0 has landed):
   ```bash
   python3 Code/Scripts/NinjaGen/NinjaGen.py && ninja -f Build/Linux/Esoterica.ninja
   ```
3. **The Windows MSBuild build still succeeds**, unchanged. This is not optional and it is not
   someone else's problem — a port that breaks Windows is worse than no port.
4. **[Docs/Linux/TouchedFiles.md](Docs/Linux/TouchedFiles.md)** lists every upstream file you
   edited, with its reason and status.
5. **[Docs/Linux/Progress.md](Docs/Linux/Progress.md)** records what you did and anything the next
   session needs to know.
6. **A PR is open** against `main`, with a description stating which acceptance criteria are met
   and which are not.

Items 4 and 5 are how a chain of independent sessions stays coherent. Skipping them is the most
expensive shortcut available. Include them **in the task's own PR** — the "directly to `main`"
allowance is for standalone doc updates, not for the bookkeeping that belongs with a task.

**Your work ends at "PR open."** Merging is a human action. If review turns up changes, they land as
new commits on the same branch.

## Reporting

Report outcomes honestly. If acceptance criteria are not met, say so and say **which** ones. Do not
mark a task complete because most of it works, and do not describe a stub as an implementation.

"Vulkan buffer creation done; texture creation is stubbed and halts" is far more useful to the next
session than an optimistic summary. This matters most in Phase 5, which spans months and where
"which of the 16 groups are actually real" is the single most important piece of state.

## Merging upstream

Merge upstream **weekly, or before starting a new phase**, whichever comes first. The cost of a
merge grows super-linearly with drift: a week is routine, six months is a research project.

Full procedure, including the post-merge audit that catches newly-introduced Windows-only code
paths: **[Docs/Linux/01-UpstreamMerges.md](Docs/Linux/01-UpstreamMerges.md)**.

The merge goes on an `upstream-merge/<date>` branch and reaches `main` through a PR like any other
code change — the post-merge audit and two-platform rebuild are exactly what review is for.

Do not merge onto a dirty tree, and land or shelve in-flight task branches first.

## Escalate rather than improvise

Stop and ask when:

- Your task needs an edit to an upstream file **not listed** in
  [TouchedFiles.md](Docs/Linux/TouchedFiles.md). That registry was derived from a full survey; a
  file outside it means either the survey missed something or your approach has drifted.
- Your task needs a public signature change in a shared header.
- Your task needs to modify anything under `Code/**/ThirdParty/` — upstream owns those directories.
- Your task needs a change to `Data/`.
- A shared abstraction genuinely cannot express what Linux needs.
- Two phases appear to conflict.

Expanding scope quietly is worse than pausing. The blast radius of an edit is the thing being
managed here.
