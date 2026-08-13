# Upstream Merge Discipline

Upstream: `https://github.com/BobbyAnguelov/Esoterica` (branch `main`).
This fork's `main` carries the Linux work.

This document is the canonical **merge procedure**. For branching strategy and how task work
reaches `main`, see [/AGENTS.md § Branching](../../AGENTS.md#branching). There is deliberately no
long-lived `linux` branch — `upstream/main` already serves as the pristine reference, so a second
long-lived branch would only add a second drift axis to manage.

Upstream has no releases or version tags — it is a continuously-developed `main`. That means
there is no "stable point" to sync to; you sync to a commit and record which one.

## One-time setup

```bash
git remote add upstream https://github.com/BobbyAnguelov/Esoterica.git
git fetch upstream
```

## Merge cadence

**Merge upstream weekly, or before starting any new phase, whichever comes first.**

This is the single highest-leverage habit in the whole project. The cost of a merge grows
super-linearly with drift: a week of upstream commits is a routine merge, six months is a
research project. Do not let the fork drift because a phase is "nearly done".

## The merge procedure

```bash
# 1. Land or shelve all in-flight Linux work first. Never merge onto a dirty tree.
git status --porcelain          # must be empty

# 2. Fetch and inspect what is coming.
git fetch upstream
git log --oneline HEAD..upstream/main

# 3. Check whether upstream touched anything in the registry. This is the whole game.
git diff --name-only HEAD..upstream/main > /tmp/upstream-changed.txt
#    Cross-reference /tmp/upstream-changed.txt against TouchedFiles.md.

# 4. Merge onto a branch, not directly onto main.
git checkout main && git pull
git checkout -b upstream-merge/$( date +%Y-%m-%d )
git merge upstream/main

# 5. Rebuild both platforms before declaring success.
#    Windows: msbuild, per README.md
#    Linux:   python3 Code/Scripts/NinjaGen/NinjaGen.py && ninja -f Build/Linux/Esoterica.ninja

# 6. Run the post-merge audit below, then open a PR. Do not merge it yourself.
git push -u origin HEAD && gh pr create --fill
```

Step 4 uses a branch because an upstream merge can break either platform, and it reaches `main`
through a reviewed PR like any other code change — see
[/AGENTS.md § Branching](../../AGENTS.md#branching).

### Step 3 is the important one

The registry in [TouchedFiles.md](TouchedFiles.md) exists so that this check is mechanical.
Because every Linux edit to an upstream file is a 2-line `#elif` addition (Conventions rule 2),
conflicts are rare and trivially resolved even when upstream does touch the same file —
you keep both branches.

The genuinely risky cases, which need real attention on every merge:

| Upstream change | Consequence | Mitigation |
|---|---|---|
| A new `#if _WIN32` guard in a shared file | Linux build breaks with a link error at best, silently wrong behaviour at worst | Grep for new `_WIN32` occurrences after each merge (see below) |
| A new function added to `RHI.h` | `RHI_Vulkan.cpp` no longer implements the full interface → link error | Link error catches it; implement the new function |
| A signature change in `RHI.h` | `RHI_Vulkan.cpp` breaks | Compile error catches it |
| A new `.vcxproj` project added to `.slnx` | Silently absent from the Linux build | Generator should warn on unrecognised projects |
| A new source file with an unrecognised platform suffix | Silently excluded or wrongly included | Generator should warn on unknown suffixes |
| A new `External/` dependency | Linux build cannot find it | Update [03-Dependencies.md](03-Dependencies.md) |
| Upstream renames a `Platform/` directory | Your `_Linux.cpp` is orphaned | Compile error catches it |

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

Also re-check the invariant that makes the build generator work:

```bash
# Should be 0. A non-zero count means upstream started using per-config file exclusion,
# which the generator does not model.
grep -c 'ExcludedFromBuild' Code/*/*.vcxproj Code/*/*/*.vcxproj | grep -v ':0'
```

## Special case: `Code/Scripts/NinjaGen/NinjaGen.py`

This is the one upstream file this port **substantially rewrites** rather than minimally
edits. That is a deliberate exception:

- It is a stale, non-functional experiment upstream — it parses the old `.sln` format the repo
  no longer uses, and emits no link rules or library flags at all. It cannot currently run.
- It is not part of the shipped build; nothing depends on it.
- Upstream is unlikely to develop it further, precisely because it is dead.
- Rewriting it is far cheaper than maintaining a parallel CMake tree that must be hand-synced
  against `.vcxproj` file lists on every merge.

Accept that this one file will conflict wholesale if upstream ever revives it, and that
resolving that conflict means re-reading upstream's version and deciding. Everything else in
the port stays minimal-diff.

## What to do when a merge conflict is not trivial

1. **Do not resolve by deleting the Linux side.** That silently regresses the port.
2. **Do not resolve by deleting the upstream side.** That silently forks behaviour and the
   next merge is worse.
3. Understand what upstream changed and why, then re-apply the Linux intent on top of the new
   upstream shape.
4. Record the resolution in [Progress.md](Progress.md) under "Merge notes" — especially if
   upstream restructured something the plan assumed.

## Recording sync points

Append to the table below on every merge. This is the fork's provenance record; without it,
"why does this file look like that" becomes unanswerable.

| Date | Upstream commit | Notes |
|---|---|---|
| 2026-08-13 | `6813cf9` | Fork point for the Linux port plan. Survey in [README.md](README.md) reflects this commit. |

## Merge notes

<!-- Append notable resolutions here. Newest first. -->

*(none yet)*
