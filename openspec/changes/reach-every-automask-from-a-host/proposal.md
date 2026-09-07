## Why

**Two of the five automask factors were declared in the C ABI and did nothing.**
`CLAY_AUTOMASK_CAVITY` and `CLAY_AUTOMASK_SURFACE_GROUP` have been in
`clay_automask_factor` since automasking shipped, with the header saying at the
field itself:

> THREE OF THE FIVE FACTORS CROSS HERE. The other two — CAVITY and SURFACE_GROUP
> — need an input this descriptor cannot carry … Setting their bits from C is
> inert rather than an error, and the descriptor that carries their inputs is a
> follow-up rather than a guess made against a sample of one.

The binding-parity gate carried the same statement as an exemption, and pyclay
has been able to wire both since it shipped. So the position was: the engine can
do it, Python can reach it, C cannot, and a C host that set the bit got silence.
That is precisely the failure mode the roadmap names — "engine can do it, host
cannot reach it" — and it is the one a unit test cannot catch, because every
internal test reaches the feature the way the engine does.

**Building it found two defects underneath it**, both reachable from C++ and
pyclay before this change and neither depending on any of it: the cavity and
group lattices were sampled at UNPLACED vertex positions while the painted mask
beside them was placed, and `automask_cavity_strength` read zero as "unset, take
the engine default", inverting a slider. Those are `place-the-automask-lattices`,
a separate change against `main` — a live defect should not wait behind an ABI
addition that happened to find it. This change assumes it.

## What Changes

**`clay_automask_sources`**, a versioned descriptor naming the two inputs, and
`clay_mesh_sculptor_set_automask_sources` / `_dynamic_` / `_multires_`. Session
state, not per stamp: the engine holds these as `std::function`s and rebuilding
them per dab is an allocation per dab.

**The same two objects pyclay takes** — a `clay_mask` and a `clay_groups` — and
deliberately so. pyclay refuses a Python callable for the same reason a C
function pointer is not offered: these are evaluated per vertex from worker
threads. Taking the same two lattices is what makes the bindings agree about
what a cavity automask *means* rather than each having reached the feature its
own way.

**Setting sources enables nothing.** The bits still decide which factors run. A
source with no bit is never evaluated; a bit with no source stays inert rather
than becoming an error, which is the behaviour those two bits always had.

**The vertex is placed before either lattice is asked**, by the session's
declared frame in C and by `MeshStrokeOptions::mesh_to_world` in C++. The frame
is read when a lattice is asked rather than captured, so declaring it after
naming the sources is not a mistake.

**`clay_layer_node_color`**, the reader `clay_layer_set_color` never had. The
header recorded that gap too — "the one setter with no reader here; a host that
needs one should say so" — and a host reloading a document was keeping colours
in a table beside the `.clay`, correct across undo and redo by hand.

**The three parity exemptions are removed rather than reworded.** A capability
that has become reachable in C is what that gate exists to notice.

## What Measuring Refuted

**A document handle and measure params, not a baked mask.** The first design
took a `const clay_document*` and a `clay_measure_params`, mirroring
`clay_mask_from_surface`, and built the cavity closure over `doc->tape()` — the
estimator evaluated directly, as the C++ path does. Two things were wrong with
it. It evaluates a tape per vertex from a worker thread, which is a threading
claim this change had not established and pyclay had explicitly declined to
make. And it is a *second* way to reach the same estimator: pyclay bakes, C
would have measured, and the two would agree only as long as nobody changed
either. Taking the mask is one model for both bindings, and the resolution
difference is the caller's own `cell_size` rather than something hidden.

**"The transform is a stroke-level concern."** The obvious fix for the space bug
was to place the point in `mesh_automask_inputs` and stop — which fixes
`apply_stroke` and leaves `clay_mesh_sculptor_stamp` wrong, because a single
stamp never builds a `MeshStrokeOptions` at all. The C ABI path needed the
sculptor's own declared frame, and capturing it at set time made the result
depend on whether the host declared its frame before or after it named its
sources. Reading the frame through the sculptor's own pointer at evaluation
removes the ordering question rather than documenting it.

**A fixture that does not place its layer cannot see the bug.** Every existing
automask test uses an untransformed mesh, where `mesh_to_world` is the identity
and the defect is exactly zero. That is why it survived: not because it was
subtle, but because nothing had a reason to move the layer.

## Measurements

`clay_mesh_sculptor_stamp`, Draw r=0.6 s=0.5 on a 25x25 grid, one stamp at the
centre. Lift is the summed |y| displacement, which on a flat grid under Draw is
the whole of what the stamp did. Reported by the suite itself
(`test_c_automask_reach.cpp`, `MESSAGE` lines) rather than transcribed.

| factors | sources | lift | |
|---|---|---|---|
| none | -- | 14.6555 | the ungated stamp |
| CAVITY | none | 14.6555 | the old behaviour, unchanged |
| none | cavity=1 | 14.6555 | a source with no bit costs nothing |
| CAVITY | cavity=1 | 0 | reaches the weight |

The strength, on the same stamp with a cavity of 1 everywhere:

| strength | lift | expected |
|---|---|---|
| 0.00 | 14.6555 | 14.6555 (off) |
| 0.25 | 10.9916 | 10.9916 |
| 0.50 | 7.32773 | 7.32775 |

The surface group, with the plane split at x = 0 into groups 1 and 2 and the
brush landing on the border:

| active group | lift |
|---|---|
| 1 | 6.2480 |
| 2 | 8.4075 |
| 1 + 2 | 14.6555 = the ungated stamp, exactly |
| 7 (nothing carries it) | 0 |

The two halves are NOT equal and expecting them to be was wrong: the fill
decides membership at the cell centre, the lattice cell is 0.05 and the grid
spacing is 1/12, so the column at x = 0 falls to one side. What holds is that
they partition the stamp, and that is the assertion the test makes.

**The declared frame**, same stamp on a session declaring a frame 5 units along
+X, with group 1 covering only where the mesh ends up:

| | lift |
|---|---|
| the ungated stamp | 14.6555 |
| SURFACE_GROUP, sources named, frame declared | 14.6555 |
| SURFACE_GROUP, the same sources, no frame declared | 0 |

The third row is the claim: it is the FRAME that moved the sample, not some
accident of the lattice's extent. `read_automask_sources` reads it through the
sculptor's own pointer when a lattice is asked rather than capturing it, so
declaring the frame after naming the sources gives the same answer as before —
which has its own case, because capturing was the first design and is correct
only in the order a host happened to use.

## The Setter/Getter Audit, In Full

The roadmap asks for the audit rather than for one fix, so here is what all 40
`clay_*_set_*` calls answer. **One real gap, and it is the one the header had
already named.**

| setter | reader | |
|---|---|---|
| `clay_layer_set_color` | **none** | closed here as `clay_layer_node_color` |
| `clay_document_set_layer_visible` | `clay_layer_info.visible` | the field says so by name |
| `clay_document_set_layer_name` | `clay_layer_name` | |
| `clay_document_set_layer_transform[_nonuniform]` | `clay_document_layer_transform[_nonuniform]` | |
| `clay_layer_set_prim` / `_op_blend` / `_transform[_nonuniform]` | `clay_layer_node_prim` / `_op_blend` / `_transform[_nonuniform]` | |
| `clay_item_set_*` (23 calls) | — | the ITEM BUILDER, independent of any document; its values become readable through the `clay_layer_node_*` readers once appended |
| `clay_voxel_set_many` / `_mirrored` / `_brush` | `clay_voxel_get` | cell writes, not properties |
| `clay_voxel_set_sculpt_layer_visible` / `_strength` | `clay_voxel_sculpt_layer_visible` / `_strength` | |
| `clay_multires_set_sculpt_layer_visible` / `_strength` / `_locked` | `clay_sculpt_layer_info` fields of the same names | |
| `clay_multires_set_sculpt_layer_detail` / `_mask` | `clay_multires_sculpt_layer_detail` / `_mask` | not on the info struct; their own readers |
| `clay_groups_set_visible` | `clay_groups_visible` | |
| `clay_*_sculptor_set_stage_report_enabled` | `clay_*_sculptor_stage_report` | the report IS the readback |
| `clay_multires_sculpt_layer_stroke_set_write_domain` | — | a command on a stroke builder |
| `clay_document_set_history_budget` | — | see below |
| `clay_sdf_prefix_cache_set_max_bytes` | — | see below |

**Two caps are deliberately left without readers**, and they are the same shape:
`clay_document_set_history_budget` and `clay_sdf_prefix_cache_set_max_bytes`.
The roadmap's own rule is "for persistent/queryable state, ask whether a
corresponding read API exists", and neither is state: each is a host policy the
host itself chose, neither survives a reload, and neither is something a second
host opening the same document could need. What a host cannot answer for itself
is what the thing is actually COSTING, and both have that —
`clay_document_history_bytes` and `clay_sdf_prefix_stats.bytes`. Note that
`clay_sdf_prefix_stats` reports `bytes` and not the cap, which is the
distinction: usage is the library's answer, the cap is the caller's own.

**Stable voxel identity was already done.** `clay_document_voxel_layer_by_id` and
`clay_document_mesh_layer_by_id` both exist, and `test_c_layer_rename.cpp`
already covers duplicate names explicitly — the lookup answers the first in
stack order, the shadowing is positional rather than creation-ordered, and the
test says in as many words that the shadowing "is not a defect being fixed
here". Nothing to add.

## What Else Building It Found

**A vacuous fixture, caught by its own guard.** The regression cases for the
placement defect live in `place-the-automask-lattices` rather than here, and the
first of them was worthless: written on a sphere, which is CONVEX, so its cavity
is zero at the placed point and the unplaced one alike and every assertion held
with the fix deleted. An explicit `any_differed` assertion is what failed. Worth
recording here because the same shape applies to any A/B gate — assert that the
two sides being compared actually differ somewhere, or the comparison proves
nothing.

## Verification

- `cpu-only` suite green; `test_c_automask_reach.cpp` adds 9 cases.
- The frame regression proven by reverting the placement in
  `read_automask_sources`: 2 cases and 3 assertions fail, reporting
  `placed sample 0 of an ungated 14.6555`.
- `tools/check_binding_parity.py` OK against a built pyclay, with the three
  automask exemptions gone and aliased instead.
- ABI 0.95.0 -> 0.96.0.
