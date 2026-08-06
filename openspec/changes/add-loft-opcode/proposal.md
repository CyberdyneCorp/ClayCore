# Proposal: the loft opcode

## Why

`lift.h` has had a loft since the beginning and no document has ever been able
to use one. The spec says why in one sentence: *"Loft remains header-only until
an item can carry two profiles."* It is 3DCoat's base-mesh generator and the
core of their 2026 parametric direction, and it is the only kernel capability
this engine has that is unreachable from a document.

## What this row is, and what `add-swept-n` is

The roadmap describes swept-N as generalising loft "to N across a **guide**".
Reading that carefully separates the two rows better than "two profiles versus
N" does:

- **This row**: N profiles along the item's **Z axis**, evenly spaced. Two is
  the interesting case and the one the spec named, but nothing about the
  opcode wants to be limited to two, so it is not.
- **`add-swept-n`**: the same profiles carried along a **guide curve** rather
  than a straight axis. That needs frames transported along a curve and a
  closest-point search per sample — a different piece of work, not a bigger
  count.

Building N now settles the storage question the plan flagged, so the guide row
changes how profiles are *placed* without changing how they are *stored*.

## What Changes

- **A `Loft` primitive and a `ctape_loft` opcode.** Because every backend
  shares one kernel dialect, a new opcode is a change to one header plus a
  parity corpus row — the four backends get it from the same source.
- **An item carries a list of profiles.** Existing single-profile lifts keep
  the field they use; loft uses a new list, sized two or more. A polygon
  profile's vertices already live out of line and index the blob, so the list
  needs no new mechanism to carry them.
- **`cop_extrude_to` becomes `cop_loft`.** The old signature computed its own
  interpolation parameter, which only works for exactly two profiles. The new
  one takes the parameter, so the tape can bracket among N and the header
  keeps one function rather than a general one and a stranded special case.
- **Exactness is declared, not assumed.** A loft lerps two distance fields, so
  it is a bound — and, more importantly, its Lipschitz constant is **not one**:
  interpolating along Z adds a term proportional to how much the profiles
  differ over how short a depth. `transition` already faces exactly this and
  the engine already has the shape of the answer; a loft that reported
  Lipschitz 1 would let the raymarcher overstep and miss surfaces.

## What this change does not do

- **No explicit stations.** Profiles are evenly spaced. Per-profile positions
  are one float each and purely additive — the scene chunk is versioned now, so
  adding them later costs nothing and needs no re-layout.
- **No guide.** That is `add-swept-n`.

## Capabilities

### Modified Capabilities

- `sdf-kernels`, `scene-model`, `python-bindings`, `c-abi`.

## Impact

- `include/clay/kernel/lift.h`, `include/clay/kernel/tape.h`,
  `include/clay/kernel/exactness.h`, `include/clay/scene/types.h`,
  `src/scene/tape_build.cpp`, `src/scene/bounds.cpp`, `src/scene/commands.cpp`,
  both bindings, the parity corpus, tests, docs, an example.
- ABI 0.18.0 — additive.
