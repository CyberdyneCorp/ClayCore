---
name: claycore-release
description: Cut and tag a claycore release — decide the version, write the notes, record the RELEASE.md history entry, run the device gate and the checklist in the order that does not waste an hour, tag, and hand over the draft release. Use whenever the user says "bump a version", "cut a release", "tag vX.Y.Z", or asks what the next version is.
---

# Releasing claycore

`docs/RELEASE.md` is the authority; this skill is the order of operations and
the traps, which the doc states but scatters.

## 1. The version is already decided — go and read it

**The tag is whatever `CMakeLists.txt` says.** Minor versions are bumped by the
feature PRs that need them, one at a time, and a release tag is cut at whatever
the line has reached. There is normally **nothing to bump at release time**.

```sh
grep -A1 '^project' CMakeLists.txt        # project(... VERSION x.y.z)
grep CLAY_ABI_ bindings/c/clay.h | head -3
grep '^version' pyproject.toml
git tag --sort=-v:refname | head -3
```

All three must agree or the checklist's `version` gate fails. If they do agree
and the newest tag is *older*, that is the release: v0.78.0 covered minors
0.74.0–0.78.0, v0.84.0 covered 0.79.0–0.84.0. Say so rather than bumping again —
an extra bump produces a minor nobody implemented.

Only bump if you are shipping a change in the release PR itself, which you
normally are not.

## 2. Order of operations (this order, for a real reason)

1. **Finish every `src/`, `include/`, `backends/`, `bindings/` and
   `CMakeLists.txt` edit and commit it.** `release_check.py` fails the device
   row for *any* change under those paths since the gate ran — a header comment
   included, because it diffs paths, not semantics. On v0.78.0 a doc-comment fix
   noticed after the gate started cost the choice between an 80-minute re-run
   and shipping a gap.
2. **Write the release notes and the `RELEASE.md` history entry, and commit
   them.** `docs/`, `tests/unit/`, `tests/swift/` and `.github/` are *not*
   device-relevant, so they can land either side — but the gate's own
   `treeDirty` flag is computed from `git status --porcelain` at the END of the
   run, so anything uncommitted while the bench runs poisons it. Commit first
   and touch nothing during the run.
3. **Run the device gate** — see the `claycore-device-gate` skill. ~40 minutes,
   plus 30 minutes of idle iPad before you start. Commit
   `tests/device/last-gate.json`.
4. **Run the checklist**: `python3 tools/release_check.py`. ~15 min plus the
   benchmark and wheel gates. Start it in the background and write while it
   runs — it only reads the tree.
5. **Open the release PR**, get it merged, then tag `main`.

## 3. The release notes

`docs/release-notes/vX.Y.Z.md`, in the repository — not only in the GitHub
release body, so they are reviewable beside the change and survive the release
being edited. Paste the same file as the draft release's body afterwards.

`docs/release-notes/v0.84.0.md` and `v0.60.0.md` are the worked examples. Two
sections are **not optional**:

- **What moves under you** — what answers differently to a caller who changes
  nothing: an entry point returning a different value, a format that round-trips
  lossily, a struct that grew, a C++ member that moved. Split it into "the
  formats" and "the entry points".
- **Known limits, stated rather than discovered** — what is still broken,
  unmeasured or deferred, including the items carried forward unchanged from the
  previous release. Carrying one forward with "Unchanged from vX.Y.Z" is the
  convention; silently dropping it is not.

Plus an **Upgrading** section ordered by how much risk each item carries.

**Every measurement must name what it was measured on.** A device number without
its model and OS, or a Mac number presented as a device number, is worse than no
number.

Gather the material from the OpenSpec changes rather than from the commit log —
`openspec/changes/<name>/proposal.md` carries the *measured* before/after tables
and the "what building it found" notes that are the substance of a release note.

```sh
git log --oneline v<prev>..HEAD              # the range
gh pr list --state merged --limit 40 --json number,title,mergedAt
git diff --name-only v<prev>..HEAD -- openspec/changes | cut -d/ -f3 | sort -u
```

For the ABI section, diff the header rather than trusting memory:

```sh
git show v<prev>:bindings/c/clay.h | grep -oE 'clay_[a-z0-9_]+\(' | sort -u > /tmp/old
grep -oE 'clay_[a-z0-9_]+\(' bindings/c/clay.h | sort -u > /tmp/new
comm -13 /tmp/old /tmp/new    # added
comm -23 /tmp/old /tmp/new    # removed — must be empty below a major bump
git diff v<prev>..HEAD -- bindings/c/clay.h   # read every hunk with a '-' line
```

A hunk with no `-` lines inside a `typedef struct` is an **append**, which the
`struct_size` pattern makes compatible. A `-` line inside one is a re-layout and
a breaking change that must be called out by name.

## 4. The RELEASE.md history entry

`docs/RELEASE.md` step 3 carries a per-minor entry saying, for every version,
whether it **IS** a release a caller can observe without calling anything new.
Add one block per new minor. This is the single most-read part of the doc and
the convention is strict:

- Group the additive minors into one paragraph naming what each added.
- Give every observable change its own **bolded** sentence and the measurement.
- Say when a minor was **never tagged**, so nobody reads the header's history
  literally and concludes a symbol was removed.
- Record commits that carry an already-released version line but are not in that
  release (they happen whenever PRs merge between a tag and the next bump).

## 5. Tagging

```sh
git tag -a v0.84.0 -m "claycore v0.84.0"
git push origin v0.84.0
```

`.github/workflows/release.yml` re-runs the checklist on Linux, builds wheels
via cibuildwheel, builds `claycore.xcframework` with its SwiftPM checksum,
packages `claycore-kernels.zip`, and opens a **draft** release with everything
attached. Drafts are deliberate — review the artifacts before publishing.

**A tag that fails the workflow stays for the record.** v0.24.0, v0.27.0,
v0.27.1, v0.27.2, v0.52.0 and v0.52.1 all exist and published nothing. Do not
delete or move a tag; cut the next patch.

**Gate any build config in CI before the release path uses it.** Two tags
published nothing because the release workflow was the first thing ever to run
a configuration.

If the release changes kernel math, the kernels artifact's host parity fixture
changes too — say so in the notes, because hosts pin it by release tag.
