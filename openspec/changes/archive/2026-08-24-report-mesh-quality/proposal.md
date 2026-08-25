# Proposal: report mesh quality, instead of two booleans

## Why

`mesh::ValidationReport` (`include/clay/mesh/validate.h`) carries eleven
fields. `clay_mesh_validate` returns **two `int32_t`**:

    clay_result clay_mesh_validate(const clay_mesh* mesh, int32_t* out_watertight,
                                   int32_t* out_manifold);

Nine measured quantities are computed and then dropped on the floor at the
boundary: `boundary_edges`, `non_manifold_edges`, `degenerate_triangles`,
`sliver_triangles`, `intersecting_pairs`, `euler_characteristic`, `oriented`,
`vertices` and `triangles`. So a host can be told an export is bad and never
told **why** — which is the difference between "this mesh has 4 boundary edges
along one hole" and a red cross.

Three consequences, in the order they matter.

**A required validation primitive is unreachable.** The `meshing` capability
states that the module "SHALL provide validation primitives: watertightness,
2-manifoldness, degenerate-triangle detection, and sampled self-intersection
checks. These back both CI export gates and **any consumer's** 'clean geometry'
claims." The sampled self-intersection pass is real, and its cap
(`max_intersection_pairs`) is a parameter of `mesh::validate`. It is reachable
from **neither binding**: `clay_mesh_validate` never passes one and pyclay
never passes one, so both always take the default of `0`, which SKIPS the pass.
The only callers that have ever run it are `tests/unit/test_mesh.cpp` (at 20000
and 5000). A requirement that names a consumer-facing primitive is satisfied
only inside the repository — the same class of finding as
`correct-the-undo-scope`, where a spec sentence had been false for as long as
the feature existed.

**`clean()` currently overclaims through that gap.** `ValidationReport::clean()`
is `watertight && manifold && oriented && degenerate_triangles == 0 &&
intersecting_pairs == 0`. With the pass skipped, `intersecting_pairs` is zero
because nothing looked, so `clean()` reads as clean on a self-intersecting
mesh. The report must say whether the pass ran, not leave a zero that means two
different things.

**The spec's own scenario is not met.** `meshing` says: "WHEN a mesh with one
deleted triangle is validated THEN the watertight check fails **and reports the
open edge loop**." `boundary_edges` is exactly that report, and it does not
cross.

And two mass properties are stranded beside it. `mesh::signed_volume` and
`mesh::surface_area` are declared in the same header, are `double` deliberately
(a signed-volume sum cancels heavily), and reach neither binding. There is no
enclosed-volume or surface-area query anywhere in the library's public surface,
on any representation.

Finally, pyclay pays for this twice: `Mesh.is_watertight()` and
`Mesh.is_manifold()` each call `mesh::validate()` in full, so a host asking
both questions builds the edge map twice.

## What changes

- **`clay_validation_report`**, a versioned output descriptor carrying every
  field `ValidationReport` computes, filled through `write_desc` per the
  `struct_size` rule in both directions.
- **`clay_mesh_validation_report`**, which takes the self-intersection cap the
  C++ has always accepted, so the pass named in the `meshing` spec becomes
  reachable from outside the repository for the first time.
- **The report says whether the intersection pass ran**, by echoing the cap it
  was given. `intersecting_pairs == 0` with a cap of 0 means "not tested" and
  must not read as "none found".
- **`clay_mesh_measure`**, returning signed volume and surface area, in the
  plain out-parameter shape `clay_mesh_bounds` already uses.
- **`clay_mesh_validate` keeps working, unchanged**, defined as sugar over the
  report — the same treatment `clay_item_desc` got when the item builder
  landed.
- **pyclay gains `Mesh.validation_report(...)`** returning a dict, the shape
  `Mesh.quad_report` already established, plus `signed_volume` and
  `surface_area`, and its two existing predicates stop running a full
  validation each.

## What this is NOT

**Not new geometry analysis.** Every number here is already computed by
`src/mesh/validate.cpp`. This change moves nothing into the engine; it stops
the boundary from discarding what the engine returns.

**Not a repair.** `clay_voxel_repair_*` repairs a voxel grid; there is no mesh
repair here and this proposes none. Reporting a hole and closing it are
different operations, and only the first is cheap and non-destructive.

**Not a change to what `clean` means.** The definition stays exactly as
`ValidationReport::clean()` states it. What changes is that a caller can see
the inputs to it, and can see when one of them was never measured.

**Not a mass-properties suite.** Signed volume and surface area cross because
they are already written and already stranded. Centre of mass, inertia tensor
and a convex hull are not in scope and are not implied.
