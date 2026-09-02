# Read an item's gate in world space

## Why

`clay_item_set_gate` is the call that makes a painted mask protect a surface
from an **operation** rather than only from a brush. Issue #394 reports it as
accepted and inert: a gated subtraction eats the protected region at every
threshold and width tried, and returns `CLAY_OK` every time. The host that
filed it has a test written to **fail** the day the engine honours the gate,
and does not call the API.

It is not inert. It is read in the wrong frame.

`voxel::MaskField` is stored in world units on its own lattice deliberately —
`voxel/mask.h` explains that a mask in cell indices would misalign the moment a
resolution changed — and `brush::mask_to_field` measures it there, so the
volume an item carries as its gate holds world distances at world positions.
The tape then placed that volume by `layer.xform * item.xform`, squashed by the
item's per-axis scale. So the protected region was moved by the transform of
the very item it was meant to hold back.

At the origin the two frames coincide, which is why every existing fixture
passes: `test_masked_combine.cpp` and `examples/54_masked_operations.py` both
gate an item with an identity transform. A real host places its cuts, and then
the protection lands somewhere the artist never painted — which from outside is
indistinguishable from a gate that does nothing at all.

The step-scale accounting had the matching error, in the unsafe direction:
`gate_width` is in world units, and the tape charged it against the layer's
scale. A wider gate costs less, so a layer scaled up declared a step scale it
had not earned, and a marcher stepping by it would punch through the masked
boundary.

## What Changes

- The tape places a gate volume by IDENTITY at unit scale. It is world-addressed
  and stays where the mask was painted.
- The Lipschitz charge uses `gate_width` as given, in world units, with no layer
  scale applied.
- The frame is written down: in `scene::Node::gate`, in `clay_item_set_gate`, and
  in `Prim.gate`. None of the three said which frame it was, which is how the
  wrong one survived.

No format or ABI change: the gate blob keeps its fifteen floats and the kernel
that reads them is untouched, so no tape-encoding version bump and no backend
sweep. Documents written by older versions load and now evaluate correctly.

**BREAKING for one shape of caller**: a host that had discovered the old
behaviour and pre-compensated by painting its mask in the gated item's local
frame will find its masks now land in world space. There is no evidence any
host did — #394 is a host that gave up on the call instead.

## Impact

- Affected specs: `sdf-kernels`
- Affected code: `src/scene/tape_build.cpp`, `include/clay/scene/types.h`,
  `bindings/c/clay.h`, `bindings/python/pyclay_module.cpp`
- Affected tests: `tests/unit/test_masked_combine.cpp`, `tests/c_api/smoke.c`
