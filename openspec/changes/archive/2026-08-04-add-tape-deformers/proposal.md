# Proposal: deformer opcodes in the tape

## Why

`deform.h` has shipped since the kernel work: twist, bend, taper, displacement, each with a Lipschitz factor in `exactness.h` and property tests proving that tracked safe-step scales stay conservative under them. None of it is reachable from a document. The tape has no deformer opcodes, so the scene compiler cannot emit them, no backend can run them, and `pyclay` cannot bind them — the `.twist()` call in this repo's own API sample (`docs/05` §10) does not exist. That is the last documented gap between the C++ vocabulary and what a document can express.

Deformers are also the piece that makes the influence-bound machinery interesting: a warped item no longer occupies its untransformed AABB, so bounds must be widened conservatively or brick culling silently corrupts the field.

## What Changes

- **Tape**: a per-item deformer chain. Each primitive's parameter block gains a count plus fixed-size deformer records; the interpreter applies them to the local point (in authoring order) before the primitive's distance function, and applies each deformer's distance correction after. Primitive parameter blocks become fixed-width so the deformer block sits at a known offset.
- **Deformers supported**: `twist` (about Y), `bend` (along X), `taper` (eased cross-section scale between two heights), and `displace` (procedural sine displacement of given amplitude and frequency). Each is already implemented in `deform.h`; this change only routes them.
- **Scene**: `Node` carries an ordered `deformers` list; the tape compiler emits it, folds each deformer's Lipschitz factor into the tape's tracked field info (lowering the safe step scale), and the command vocabulary serializes it so deformed documents round-trip.
- **Influence bounds**: each deformer widens an item's local bound conservatively — twist and bend are rotations, so the deformed shape stays inside the axis-aligned hull of the original's rotational sweep; taper scales the cross-section by its largest factor; displacement dilates by its amplitude.
- **Python**: `.twist(k)`, `.bend(k)`, `.taper(...)`, `.displace(...)` as chainable primitive modifiers, exactly as `docs/05` §10 shows.
- **Backends**: no per-backend work — Metal, CUDA, and OpenCL interpret the shared tape header, so they inherit deformers with the parity suite as the check.

## Capabilities

### New Capabilities

_None._

### Modified Capabilities

- `sdf-kernels`: the deformer requirement gains the tape-reachability guarantee (deformers are usable from a document, not only as headers).
- `scene-model`: influence bounds must account for domain warps.
- `python-bindings`: deformers stop being the documented exception in the module's API surface.

## Impact

- `include/clay/kernel/tape.h` (opcode/layout), `include/clay/scene/types.h`, `src/scene/{bounds,tape_build,commands}.cpp`, `bindings/python/pyclay_module.cpp`, tests, and the reference tree evaluator in `tests/unit/scene_utils.h`.
- **Tape layout change**: primitive parameter blocks become fixed-width and gain a trailing deformer block. Backends read the shared header, so they follow automatically; any hand-written tape would not.
- `.clayspace` documents written before this change still load (the command reader defaults the new list to empty); documents that use deformers are readable only by this version onward, which the format's major version already governs.
- Non-goals: group/subtree-level deformation (this change is per-item, matching the `docs/05` API sample), `wrap_around`, and the two-subtree `transition_*` morphs — all remain header-only until a change gives them a home in the tree.
