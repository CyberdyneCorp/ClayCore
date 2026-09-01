# Proposal: the shared brush runtime

## Why

`add-shared-brush-kernels` did what it said: the deformation math for all
sixteen verbs lives in `mesh/sculpt_kernels.h` behind an interface that names
no mesh and no vertex, `MultiresSculptor` calls it, `DynamicSculptor` calls it,
and the fixed-mesh goldens did not move by a bit. That change is complete and
this one does not reopen it.

What it did NOT finish is everything AROUND the kernels. The math is shared;
the runtime that feeds it is not, and the gap is not cosmetic. Four things are
true of the tree today and every one of them is a defect a user can hit:

**1. The automask is silently absent on the adaptive surface.**
`MeshBrushSettings::automask` is part of the brush. `MeshSculptor` honours it,
`MultiresSculptor` forwards `set_automask_inputs` to the level sculptor that
honours it, and `DynamicSculptor::gather` never looks at it — it composes
falloff, taper, gate and alpha and stops. So an artist who turns on "do not
cross onto a face pointing the other way" gets it on two representations out of
three, with no error and no diagnostic. It is worse than a missing feature at
the C ABI, where the header states the contract it is breaking:
`clay_dynamic_sculptor_stamp` takes `clay_mesh_brush_desc` and says *"`brush`
is the same descriptor the fixed path takes, so a host carries one brush model
across both representations"* — and `read_mesh_brush` dutifully fills
`settings.automask` from the four fields the descriptor carries for it, which
the adaptive path then drops on the floor.

**2. The workset is the fixed mesh's, and the adaptive sculptor keeps a
private copy of it.** `SculptWorkset` addresses work by 32-bit weld class,
which is the fixed representation's identity and nothing else's.
`DynamicSculptor` therefore holds `region_vertices_`, `region_weights_`,
`region_positions_`, `region_normals_`, `region_distance_` and its own `slot_`
— five parallel arrays and a reverse map that are the same idea under different
names. That is why the automask is missing: `compute_automask` takes a
`SculptWorkset`, and the adaptive sculptor has not got one. The next thing that
composes into the weight will be missing there too, for the same reason, and
nothing in the build will say so.

**3. A stamp allocates, on paths the allocation gate cannot see.**
`tests/unit/test_sculpt_allocation.cpp` asserts that a warm fixed-mesh stamp
performs no heap allocation, and it does — *with automasking off*, which is
what its fixture uses. Turn on the boundary or connectivity factor and
`compute_automask` allocates a `std::vector<int> depth`, two frontiers, a
`std::vector<char> reached` and a `std::vector<std::uint32_t> stack`, per dab.
The adaptive path is worse and has no gate at all: each of
`euclidean_region` and `geodesic_region` builds three vectors per stamp to sort
the region, `euclidean_region` builds a fourth for the ball query,
`write_positions` builds one PER MOVED VERTEX for the incident faces, and
`DynamicSurface::refresh_normals` builds another for the vertices it touches.
"A stamp costs what it touches" is the rule the whole mesh brush path is built
around, and on the representation that grows under the brush it is currently
not true.

**4. There is no directional stamp frame, so the whole directional brush family
is unreachable.** Rake, Chisel, Clay Strips, a rotated alpha and a directional
scratch are all one idea: an orthonormal basis on the surface, oriented by the
stroke's azimuth, that (u, v) is measured in. `AlphaFrame` in
`sculpt_kernels.h` is that basis with the alpha's name on it and no rotation,
built only when an alpha is present. A named brush that needs a new code path
is evidence an axis is missing — the brush model's own words — and this is the
axis.

None of the four is a refactor looking for a justification. Three are
behavioural divergences between representations of a library whose stated
reason for extracting the kernels was that *"an artist who learns a brush on a
mesh layer and finds it behaves differently on an adaptive one has not found a
bug they can report; they have found that the tool is untrustworthy."*

## What changes

- **`BrushScratchArena`** — one bump arena per sculptor, serving the workset's
  transient buffers: the automask's frontiers, the region sort, the incident
  faces, the neighbour normals. Capacity tracks the largest recent footprint
  and `reset()` keeps it. The gate is the existing one, widened: after warm-up,
  an ordinary stamp allocates nothing — *with the automask on*, and on all
  three representations.
- **A neutral `WorkItemId`** — 64 bits, because that is the width the adaptive
  surface's `VertexId` and the hierarchy's (level, vertex) already are.
  `SculptWorkset` addresses work by it, and the three representations supply
  adapters that fill one.
- **`DynamicSculptor` on the shared workset, with the automask reaching it.**
  Its five private arrays become the shared type; `set_automask_inputs` appears
  where the other two sculptors have it; `compute_automask` splits into a
  neutral core over a workset and a per-representation topology adapter.
- **`StampFrame`** — origin, normal, tangent, bitangent and rotation, built
  once per stamp, feeding the alpha and the directional verbs identically on
  every representation. `AlphaFrame` becomes a projection of it.
- **Parity gates beyond the fixed mesh** — `test_dynamic_shared_brush_parity`
  and `test_multires_shared_brush_parity`, asserting both what MUST agree and
  what legitimately differs, because a gate that only checks agreement is a
  gate that will be satisfied by making the two representations equally wrong.
- **`examples/69_shared_brush_runtime.py`** — the same `reference_presets()`
  gesture over fixed, adaptive and multires surfaces, rendered, with every
  claim asserted.

## Approach

The acceptance criterion is inherited and it is absolute: the fixed-mesh
goldens do not move. `mesh_sculpt_goldens_linux_x64.inc`,
`..._macos_arm64.inc` and `..._msvc_x64.inc` are not touched by this change,
and a moved golden is a failed refactor rather than a new baseline.

That constrains the order of work more than the design. Every step is a
relocation of arithmetic, never a re-association of it: the arena replaces
where a buffer comes from and not what is summed into it, `WorkItemId` changes
what indexes an array and not what the array holds, and `StampFrame` reaches
`kernel::calpha_frame` with the same two arguments the alpha frame reaches it
with today.

The one place the rule is genuinely delicate is the azimuth, and it is called
out in the design: a rotation by zero is not free in floating point once a
`-0.0` component is in play, so the identity path skips the rotation rather
than multiplying by `cos 0` and `sin 0`.

## Open questions

- Whether the neutral workset's `slot` reverse map can stay one array or has to
  become the adapter's business. The fixed path keys it by weld class, the
  adaptive one by vertex slot and the hierarchy by level vertex; all three are
  dense and local, so one array typed by the adapter is plausible, but the
  hierarchy's is per level.
- Whether `MeshSculptor::write_region()` widens to `WorkItemId` or keeps its
  weld-class spelling with a projection. Two callers exist, so either is cheap;
  the question is which one a host reads more honestly.
- How much of the arena a host should see. A device that kills an app for
  memory rather than warning it twice is this library's stated target, and a
  per-stroke scratch high-water is exactly the number such a host budgets
  against — but it is also a number that invites being tuned rather than
  measured.

## Impact

`meshing` gains the neutral work item, the arena discipline and the
cross-representation parity requirement. `brush-engine` gains the stamp frame
and the requirement that an automask reaches every representation that offers
the verb. `c-abi` and `python-bindings` gain the arena's statistics and the
stamp azimuth; the ABI moves to **0.75.0 / `CLAY_ABI_MINOR 75`**, appending to
`clay_mesh_brush_desc` rather than changing it, so a host compiled against 74
builds and behaves identically. `examples` gains 69.

No fixed-mesh behaviour changes. The adaptive path's behaviour changes in
exactly one direction — it starts honouring an automask it was given and
ignored — and that change is what a regression test in this branch pins.
