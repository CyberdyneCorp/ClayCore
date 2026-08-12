# Design: a sign per armature node

## Context

An armature is a stroke plus a tree: nodes ride `Node::stroke` (xyzr), the
tree rides `Node::armature_parents`, and the kernel folds one
`csweep_link` per (node, parent) pair in ascending index —
`ctape_armature_dist`, `include/clay/kernel/tape.h:414`. The whole item then
combines into the layer under one `Node::op`. Hosts wanting ZBrush's negative
ZSphere split the rig: positive spheres in the armature, each negative one a
separate subtract sphere placed after it. Issue #99 names the three defects of
that split — the membrane is not cut, the sign does not survive a save, and a
negative has to be a leaf.

Constraints inherited from the specs:

- c-abi: armature surface is purely additive — no signature changes, no
  struct grows.
- sdf-kernels: fold order is deterministic and stated; a chain armature
  evaluates identically to the stroke with the same points.
- file-io: node-record growth is gated on the format minor (parents set the
  precedent at minor 7, documented in `include/clay/io/clayspace.h:58`).
- python-bindings: no pyclay capability without a C counterpart.

## Goals / Non-Goals

**Goals:**

- One sign per node, +1/-1, positive by default, carried through the builder
  setter, the placed tree edits, the readback and the file.
- Negative links cut the membrane: subtraction happens after the positive
  fold, on the final skin, so a hollow reads as a continuous scoop.
- A negative node may carry children (no leaf restriction).
- An all-positive armature evaluates bit-identically to today.

**Non-Goals:**

- Per-node blend or per-node op generality — the sign is a bit, not an op.
  `CLAY_OP_*` stays an item property.
- Sign-aware `clay_item_add_child` — new children are positive; the placed
  `SET_SIGN` edit or the builder signs array flips them. Keeps every existing
  signature untouched.
- The negative-radius convention in `xyzr` (issue option 2) — it legalises
  input the setter refuses today.
- Adaptive-skin meshing semantics — hosts mesh the evaluated field; nothing
  here changes meshing.

## Decisions

**1. Signs are a parallel array, not a bit on the point.** `Node` gains
`std::vector<std::int8_t> armature_signs;` beside `armature_parents`, same
length discipline: shorter-than-nodes reads as positive-padded, exactly as
short parents read as roots. `StrokePoint` is shared with every curve
primitive; growing it would tax all of them for an armature-only property.

**2. The signed field is "positive armature minus negative armature".** One
sentence, applied literally in `ctape_armature_dist`: pass one builds the
armature of the positive nodes exactly as the unsigned fold does — when no
node is negative this is the identical instruction sequence, preserving
chain-equals-stroke bit-for-bit — and pass two builds the armature of the
negative nodes by the same rules and subtracts it, segment by segment in
ascending index, with the house subtract idiom `-smin(-d, seg, k)`
(`ctape_combine_values`' `ccombine_subtract`), hard `cmax(d, -seg)` at
`blend_k == 0`.

The consequences fall out rather than being cases: a link exists only between
two nodes of the same sign (a node whose parent has the other sign reads as a
root of its own half), so skin along a negative node's links is never drawn —
the membrane cut stated structurally, not sphere-patched after the fact — and
a carve never sweeps a positive parent's radius, so an eye-socket child does
not swallow the head it is cut into. A negative parent-child pair carves its
link as one swept segment, which is what makes a deep hollow a scoop. The
referenced-root suppression applies per half, because overlapping terms
over-carve under a soft subtraction exactly as they over-add under a soft
union. Subtracting after the whole positive fold means a sleeve from any
OTHER branch running through a hollow is cut and no later union re-fills it —
the ordering defect of the host-side workaround.

An earlier draft subtracted each negative node's link swept at both end
radii; it was discarded because the round cone spans the parent's sphere, so
any negative child annihilated its positive parent — marking an eye socket
negative would have eaten the head.

**3. Tape layout: a fifth prim param.** `CLAY_TAPE_PRIM_PARAMS` is 7 and the
armature uses 4, so signs travel as `prim_params[4]` = blob offset of count
floats (+1/-1), written unconditionally beside the parents. The kernel is one
header shared by every backend (Metal and CUDA `#include` it), so the signed
fold lands everywhere at once; the parity corpus pins it.

**4. ABI mirrors the parents surface exactly.**
- `clay_result clay_item_set_armature_signs(clay_item*, const int8_t* signs,
  size_t count)` — refuses null/zero as the parents setter does, and refuses
  any value other than +1/-1 as a typed invalid argument.
- `clay_result clay_layer_armature_signs(const clay_document*, clay_layer_id,
  clay_node_id, int8_t* out_signs, size_t* count)` — size-query pattern,
  counted in nodes, `CLAY_ERROR_BUFFER_TOO_SMALL` carrying the needed count,
  short-stored signs padded to +1 (the reading evaluation makes), non-armature
  refused invalid-argument, never refused on protected/hidden layers.
- `#define CLAY_ARMATURE_SET_SIGN 4` — the sign rides the existing `radius`
  argument (+1.0f / -1.0f, anything else refused), `target` names the node,
  `mirrored` ignored as it is for `SET_RADIUS`. One `SetArmatureCmd`, one undo
  step, refused on a protected layer like the other four edits.

**5. Persistence at format minor 8.** Node record: `if (minor >= 8)` write
`u32 count` + one byte per sign; reader bounds-checks as the parents reader
does. `SetArmatureCmd` grows the signs the same way, gated on the same minor.
`kClaySpaceMinor`/`kSceneMinor` go 7 → 8; a compatibility note joins the
minor-7 note in `clayspace.h` (same shape: the count word goes out for every
node, a pre-8 build desynchronises and fails rather than misreads — the
format's stated trade). Writing at `minor <= 7` drops signs and reproduces
today's bytes, which is the existing older-build escape hatch.

**6. pyclay: `signs` beside `parents`.** `Armature(nodes, parents=None,
signs=None, ...)` — `None` means all positive; a read-write `signs` property
(builder state, like `parents`); `armature_edit(op="set_sign", node=...,
target=..., sign=+1|-1)` mapping to the placed edit. Every capability mirrors
a C call, so `check_binding_parity` stays clean. `examples/40_armature.py`
grows a negative-node section: eye sockets carved on the blocked-out figure
by flipping two nodes, rendered beside the all-positive rig.

## Risks / Trade-offs

- [Subtraction breaks the field's bound status] → the armature already
  declares itself a bound, not exact (sdf-kernels); subtraction only removes
  material, so the existing union-of-node-spheres bound still covers.
  `bounds.cpp` is untouched — conservative, correct.
- [A pre-8 build cannot open a minor-8 document] → the format's documented
  behaviour since minor 7: scene-payload growth desynchronises an old reader,
  which *fails* rather than misreads; writing at an older minor is the escape
  hatch and drops only the signs.
- [All-negative armature evaluates to empty] → correct by the semantics
  (nothing to carve from), same as a lone subtract item on an empty layer;
  the kernel returns FAR as it does for `count <= 0`. Stated in the spec
  scenario rather than refused — a host mid-edit may pass through it.
- [Two-pass loop costs a branch per node on all backends] → the branch is on
  a blob float, uniform across a warp/simdgroup for a given item; the
  device perf gates (add-device-perf-gates) would catch a regression.

## Open Questions

None — the issue's reporter pre-agreed to option 1's shape, and the placed
edit is their option 3, which composes rather than competes.
