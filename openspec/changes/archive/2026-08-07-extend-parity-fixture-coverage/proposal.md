# Proposal: extend the host parity fixture to the current kernel set

## Why

Found while verifying the v0.22.0 artifacts before publishing.

`claycore-kernels.zip` ships the kernel headers a host compiles into its own
preview, and `kernel_parity.json` beside them so the host can assert its GPU
agrees with ours. v0.22.0's zip contains the new headers — `noise.h` is in it,
and `tape.h` carries the relief ops — but the fixture's 37 cases exercise none
of them. Nor magnify, which landed a release earlier.

So a host that pins the artifact and runs the fixture gets a green result that
says **nothing** about whether its GPU agrees on the new math. That is worse
than no coverage: it reads as validation.

The case set is not violating the spec, because the spec enumerates the surface
by name — blend profiles, extended modes, a deformer chain, a composed document
— and that list was written before these kernels existed. The list is the bug:
it made the fixture's coverage a snapshot rather than a standing obligation.

## What it is

Five cases covering every kernel feature added since the list was written:

- `relief_build_up` and `relief_cut_in` — the two ops whose item is a REGION
  rather than geometry, so a host that treats the second operand as a shape
  disagrees immediately. Both directions, because they share one kernel branch
  with the sign taken from the mode and a backend could get the sign wrong.
- `deformer_noise` — the integer-hashed gradient noise. The case exists mostly
  to catch the failure the design was chosen to avoid: a host reaching for the
  familiar `fract(sin(...))` hash instead of compiling ours diverges by O(1),
  not by a tolerance.
- `deformer_magnify` and `deformer_pinch` — one deformation with a signed
  strength, so both signs, for the same reason relief takes two cases.

And a change to the requirement itself: the case set SHALL track the kernel
set rather than a list fixed at one point in time, so the next kernel feature
that ships without a case is a gate failure rather than a discovery.

## What it is not

Not a change to any kernel, and not a change to the tolerances. The fixture's
expectations are already gated against the tape interpreter and every
registered backend, so these cases are held to the same standard the existing
ones are.

Regenerating `kernel_parity.json` changes the artifact, which is pinned by
release tag — that is the intended consequence, and the v0.22.0 notes already
tell hosts to re-run the fixture rather than assume the previous one passes.
