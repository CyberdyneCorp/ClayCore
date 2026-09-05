---
name: claycore-change
description: Land a change in claycore — when it needs an OpenSpec proposal, where the ABI version lines live and when to move them, what a C ABI addition must carry, the format-minor rules, and the PR checklist this repo actually enforces. Use when starting or reviewing a feature, fix or perf change here.
---

# Landing a change in claycore

## Does it need an OpenSpec change?

Yes for anything medium or larger, anything touching the ABI, a format, or a
data model. `openspec/changes/<kebab-name>/` with `proposal.md`, `design.md`
and `tasks.md`; the `openspec-propose` / `openspec-apply-change` /
`openspec-archive-change` skills drive the CLI.

A proposal here is expected to carry **measurements, not intentions** — the
before/after table, the fixture it was taken on, and a "what building it found"
section recording what the plan got wrong. Those sections are what the release
notes are later written from, so writing them badly costs twice. Several
proposals in this tree openly refute their own design; that is the house style
and it is correct.

Small, self-contained fixes go straight to a PR.

## Version lines

Three files, and the checklist's `version` gate fails if they disagree:

- `CMakeLists.txt` — `project(... VERSION x.y.z)`
- `bindings/c/clay.h` — `CLAY_ABI_MAJOR` / `MINOR` / `PATCH`
- `pyproject.toml` — `version`

**Move the minor in the PR that adds the entry point**, not at release time. A
release tag is cut at whatever the line has reached (see `claycore-release`).
Note the ABI transition in the PR body (`ABI 0.83.0 -> 0.84.0`) and in the
change's `tasks.md`; `docs/RELEASE.md` gets its entry when the release is cut.

Two branches in flight both bumping the minor is normal — the second rebases
onto whatever landed first and takes the next number.

## Adding to the C ABI

The boundary rules are stated at the top of `bindings/c/clay.h` and are not
negotiable: opaque handles, integer error codes, caller-owned buffers, no C++
types, no exceptions, no variadics, no bitfields, UTF-8 strings, and every
descriptor struct starting with a `uint32_t struct_size` the caller sets.

- **Grow a descriptor by APPENDING**, behind the `struct_size` it already
  negotiates. A caller compiled against the shorter layout passes the shorter
  size and gets exactly the fields it had. Inserting a field is a re-layout and
  a breaking change.
- **A new field's zero must be the old behaviour.** `clay_sculpt_policy`'s three
  prefix knobs are the worked example — all three zero is today's behaviour
  exactly, so a host that has not heard of the feature is not opted into it.
- **Size-query pattern** for anything variable-length: call with a null out
  buffer to get the count, then again with storage.
- **A short buffer is `CLAY_ERROR_BUFFER_TOO_SMALL`, not
  `CLAY_ERROR_INVALID_ARGUMENT`.** To a host those mean opposite things — one is
  retryable, the other says the call was malformed and retrying it is a spin.
  Getting this wrong once left a viewport a frame behind with nothing on screen
  saying so.
- **Document what a call does NOT promise**, in the header, beside it. The
  headers here carry the failure modes, the measured costs and the reasons a
  design was rejected. That is deliberate: it is the only documentation a host
  integrator reliably reads.
- pyclay and the Swift surface must follow, or `check_binding_parity.py` fails —
  and read that gate's output line, because it can pass without checking
  anything (see the `claycore-verify` skill).

## Formats

The scene / `.clayspace` minor lives at `kSceneMinor` in
`include/clay/scene/commands.h`, with the reader and writer gates side by side
in `src/scene/commands.cpp`.

- Records are **not length-prefixed**, so an older build meeting a newer minor
  *fails* rather than misreading. That is the safe direction and it is the
  deliberate trade.
- **A new minor must be writable at the previous one**, degrading to whatever
  that minor meant, and the notes must say exactly what the downgrade loses.
- **Both halves or neither.** A writer that deduplicates met by a reader that
  does not has saved disk and rebuilt the duplication in memory, and the next
  save undoes it.
- Regenerate the gallery documents and run `tools/check_gallery.py`.

## The PR checklist

- **Every bug fix carries a regression test in the same PR.** Prove the test
  works by reverting the fix and watching it fail — several tests here were
  written, passed, and could not have caught the defect.
- **Assert the count, not the clock**, whenever the claim is a count. See
  `claycore-bench`.
- Update the docs or the OpenSpec spec when behaviour changes. `docs/05` is the
  library reference; `docs/07` brushes; `docs/RELEASE.md` per-version history at
  release time.
- Keep per-function cognitive complexity inside the backend target (15). Where a
  function is genuinely irreducible, say so in the PR with its score rather than
  mangling it — that is the accepted outcome here.
- Run `python3 tools/release_check.py --skip-slow` before pushing.
- **No mention of Claude or AI anywhere in a commit message or PR body**, and no
  `Co-Authored-By` trailer for it.
- PR bodies here are substantial: why, what lands, what measuring refuted, the
  verification (suite counts, the gates run, the ABI transition). Read
  `gh pr view 462` or `465` for the shape.

## Things this repo has learned the hard way

- **Verify a claim on `main` before calling it pre-existing**, and diff
  linter/type-check/test output against `main` before saying an error is not
  yours.
- **A gate that only runs on the release path fails on the release path.** If
  you add a build configuration, wire it into CI before the release workflow
  depends on it — two tags published nothing that way.
- **A perf win can switch off the gate protecting it** by pushing its case under
  the measurement floor. Check the case still gates afterwards.
- An issue's diagnosis is a hypothesis. #451's reporter was right that it
  regressed and wrong about why, and the first two fixes addressed things their
  fixture did not have — one of them silently unsound. Reproduce and measure
  before implementing the suggested fix.
