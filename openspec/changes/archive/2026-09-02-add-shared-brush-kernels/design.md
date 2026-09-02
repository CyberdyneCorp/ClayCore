# Design: shared brush kernels

## Context

`src/mesh/sculpt.cpp` is 1,465 lines holding four things that are only
accidentally in one file: the deformation math for sixteen verbs, the
weld-class plumbing that feeds it, `VertexDeltas` and its byte encoding, and
`MeshSculptor`'s spatial-index bookkeeping. Two more sculptors are coming and
both specifications say to reuse the first of those four. Today a second caller
can only have the math by taking the other three with it.

The extraction has an exact acceptance criterion for exactly as long as there
is one caller: the fixed-mesh results must not move by a bit. That is why this
change is first in Phase 5 rather than third.

## Goals / Non-Goals

**Goals.** A kernel interface that names no `Mesh`, no `Adjacency` and no
vertex index. Brush axes named separately from the verb that composes them. A
plan compiled once per stroke. A workset whose cost is the footprint. Automask
factors over that workset. A versioned preset.

**Non-Goals.** No new verb. No behaviour change on any existing path — that is
the acceptance criterion, not an aspiration. No dynamic topology and no
hierarchy; both are separate changes that consume this one.

## Decisions

### D1 — The scratch lives in `mesh`, and there is no new module

Task 1.1 offered a new `memory` module or an addition to `parallel`. Neither is
taken. The scratch is `mesh/sculpt_workset.h`, beside the workset it serves.

`parallel` is the precedent cited for a new leaf module and it is the opposite
case: it was extracted because callers in *many* modules were locked out of the
only pool in the tree by the layering rule. Here every consumer Phase 5 names —
the fixed sculptor, `DynamicSculptor`, `MultiresSculptor` — is in `mesh`. A
module created for one module's callers is a directory, not a boundary. If a
second module ever needs the arena, moving it is a rename and this decision is
cheap to reverse; creating the module now and finding nothing else wants it is
not.

`parallel` is also the wrong home on its own terms: it means concurrency, and
an allocator in it would make the module mean two things.

### D2 — `MeshBrushSettings` is kept, unchanged, and projected

Both bindings, every compiled host and `brush::apply_to_mesh` pass it. It stays
as the public mesh surface and is projected onto the axes internally, once per
stroke when the plan is compiled, never per stamp.

Replacing it would break every caller in exchange for no behaviour it does not
already reach, and this change's acceptance criterion is that no caller sees a
difference. The projection is where the redundancy gets named rather than
removed: `polish_angle` and `layer_height` are one verb's business each, and
the plan records that they are read by one kernel rather than deleting the
fields that carry them.

### D3 — The composition order is normative, because float multiplication is not associative

The rule is written once:

```
w = falloff(d)                        // curve over the straight-line distance
w *= path_taper                       // geodesic walks only
w *= 1 - clamp(gate(p), 0, 1)         // the freeze
w *= alpha(p)                         // the borrowed stamp
w *= automask                         // new here; last, so it cannot move bits
w  = (w <= 0) ? drop : w              // one boundary test, not a clamp per factor
```

**The ORDER is part of the rule, not an implementation detail.** These are five
separate multiplications and float multiplication is not associative, so
re-associating them changes the last bit of the weight and therefore of every
displacement. Automasking is appended LAST specifically so that a stamp with no
automask reproduces the pre-extraction bits exactly: multiplying by a factor
that is identically 1.0 is the identity in IEEE-754, but multiplying it in
earlier would re-associate the four that already exist.

The clamp on `gate` STAYS per-factor and is not folded into the boundary test.
It is a domain guard on a caller-supplied callback, not a composition clamp: a
gate returning 1.2 clamps to 1 and freezes, where an unclamped `1 - 1.2` would
be dropped by the boundary test and reach the same answer by luck — but a gate
returning -0.5 would AMPLIFY the weight to 1.5x instead of leaving it alone.
"A single clamp at the boundary" is about the composed weight; it was never
about the callback's range.

### D4 — The C ABI takes the preset and nothing else, this change

The preset crosses — create, destroy, serialize to bytes, deserialize from
bytes — together with the axes the mesh path already honours through
`clay_mesh_brush_desc`. The footprint, accumulation and post-policy axes do
not cross yet.

Each of those three has exactly one implementation today, so a descriptor
mirroring them would be a public surface designed against a sample of one, and
the C ABI is the one place in this repository where getting it wrong is
expensive to correct. `add-dynamic-topology` is the change that gives them a
second implementation and it is the right change to widen the descriptor.

### D5 — The axes live in `mesh`, not in `brush`. Tasks 3.1 and 3.3 are corrected

Tasks 3.1 and 3.3 place `BrushModel` and `BrushRuntimePlan` in
`include/clay/brush/`. **That cannot be built.** `tools/check_layering.py`
records `brush -> mesh` (`apply_to_mesh` is the stroke engine's fourth
consumer), so `mesh` may not include `brush` — and the per-vertex loop that has
to read the axes is `MeshSculptor::stamp`, in `mesh`. Writing the model where
the tasks say puts a cycle in the layering gate on the first include.

So:

| Type | Header | Module |
|---|---|---|
| `MeshBrush`, `MeshFalloff`, `MeshBrushSettings` | `mesh/sculpt_common.h` | mesh |
| the axes — footprint, weight, frame, kernel, accumulation, target, post | `mesh/brush_model.h` | mesh |
| `BrushRuntimePlan` | `mesh/brush_model.h` | mesh |
| the kernels | `mesh/sculpt_kernels.h` | mesh |
| `BrushPreset` = name + `StrokePreset` + `BrushModel` | `brush/preset.h` | brush |

The preset is the one type that genuinely belongs in `brush`: it pairs the
stroke vocabulary with the brush vocabulary, and `brush` is the module that can
see both. The axes belong below it because the loop that reads them is below
it.

This is a relocation, not a scope change. The `brush-engine` capability is a
spec capability and does not name a C++ namespace; every requirement in the
delta is satisfied by the table above.

### D6 — Parity is pinned by digest, and the digest is generated before the refactor

`tests/unit/test_mesh_sculpt_parity.cpp` hashes positions, normals and colours
after every verb on five fixtures, following `test_voxel_mesh_fixture.cpp`,
which is this repository's precedent for pinning a buffer rather than a count.
The goldens are generated on unmodified `main` and are what the extraction is
compared against.

**A tolerance would defeat it.** The mistake this refactor is most likely to
make is a re-associated accumulation, and re-association moves the last bit —
which every tolerance in the tree admits.

**What the digest cannot promise, stated rather than discovered later:** these
hashes are not portable across libm implementations. `class_normal` calls
`acos` through `corner_angle` for every region class, and the Gaussian falloff
calls `exp`; neither is correctly rounded, and glibc and Apple's libm disagree
about the last bit of both. The fixtures use exactly-representable operands
wherever a value is chosen rather than computed — the lesson
`test_voxel_mesh_fixture.cpp` already recorded about FMA contraction — but that
cannot reach a transcendental. If a non-x86-64-Linux preset reports a different
hash, the answer is a per-platform table and NOT a tolerance, because the
question the gate asks is about this machine's before and after.

The permanent guard that IS portable arrives with the extraction: the fixed
path and a direct call into the shared kernel with a hand-built snapshot must
produce byte-identical displacements in the same build. That one holds on every
platform, and it is the property the other two sculptors will depend on.

## Risks / Trade-offs

- **A refactor with no behaviour change is invisible when it goes right and
  silent when it goes wrong.** Mitigated by generating the goldens before
  touching a line, which is the only ordering that makes the gate mean
  anything.
- **The plan adds a layer between the settings and the loop.** Kept flat and
  POD, compiled once per stroke, and it records what the kernel reads so the
  gather can skip what no verb will use.
- **Extracting `Paint` and `Smear` alongside the displacement verbs** puts two
  different output channels behind one interface. Taken deliberately: a colour
  write is a kernel with a different write target, and the alternative is an
  exception in the model that every later sculptor has to reproduce.

## Migration Plan

Additive and internally reordered. No public signature changes; `MeshSculptor`
keeps its constructor, its verbs, its settings and its results. The preset and
the axes are new surface. A host compiled against the current header builds and
behaves identically, which the C ABI delta states as a requirement.

## Open Questions

- Whether the workset's read halo should be materialized as a list or stayed
  implicit in `BrushRegion::slot`, which today distinguishes the two by
  returning `kNoClass` for a neighbour outside the region.
- Whether the automask factors belong on the plan or on the settings. On the
  plan reads better; the settings are what a host already serializes.
