## Why

`refine-one-region-of-a-hierarchy` landed regional multires and left three
residuals unticked, each with its reason written down: transition polygons for
export (2.3), cross-level neighbours for smooth/relax/normals (3.4), and brushes
across a transition (5.3, which needs both). This change closes them.

An external design guide proposed how. **It was audited against the tree rather
than accepted, and the audit moved the work** — the same thing that happened to
the last two guides read here, one of which had three of its four pillars
already built. What follows is what was measured, not what was proposed.

## What the audit found, measured

Two fixtures, both read-only probes against this worktree's `libclaycore.a`.
**Fixture A** is a closed 96-patch cube-sphere cage (98 cage vertices, cage edge
≈0.5) refined over a region to level 3 — depth histogram L0:64 L1:16 L2:12 L3:4.
**Fixture B** is an open 6×6 quad grid (36 base patches, 49 cage vertices, 2.0
across) with the middle 2×2 refined to level 3, where one level-3 edge is
≈0.0417. No timing was taken and none is quoted: every number below is a count,
a byte comparison or a geometric value.

### The residual that has a user today is not one of the three

**Normals and frames are already wrong at a region boundary, before any
transition exists.** Positions are bit-identical — the shipped watertightness
claim is true — but the frame a detail coefficient rides in is not.

| fixture A | vertices compared | positions differ | display normals differ | frames differ |
|---|---:|---:|---:|---:|
| level 1 | 41 | 0 (worst 0.000000000) | 16 (worst 0.104052432) | 16 (worst 0.104052435) |
| level 2 | 25 | 0 (worst 0.000000000) | 16 (worst 0.048561923) | 16 (worst 0.048561923) |
| level 3 | 9 | 0 (worst 0.000000000) | 8 (worst 0.029364093) | 8 (worst 0.029364093) |

0.104 on unit vectors is about 6 degrees. They differ at **exactly** the vertices
whose face ring at that level is incomplete: the predicates "normal differs" and
"face ring smaller than the dense hierarchy's" were checked against each other
and **disagreed on 0 vertices at every level**.

This is not a shading complaint. `P(n) = S(n) + Frame · Detail`, so the
bit-identity the change shipped holds only while the detail at a boundary vertex
is **zero** — which is the only case the existing gate exercises. On fixture B,
writing the *same* `LocalDetail{0.01, 0.0, 0.05}` into every level-3 vertex of a
dense and a regional hierarchy:

- 64 of 289 level-3 vertices have fewer incident faces than the dense hierarchy's
- all 64 have a different display normal (worst 0.154028) and a different
  transported frame (worst 0.253474)
- **63 of those 64 then evaluate to a different position** (worst 0.00776535),
  while all 225 interior vertices stay bit-identical

A coefficient authored at a transition does not mean what the same coefficient
means densely. That is a **storage** defect, on a path artists use today, and it
is the root of 3.4 rather than a consequence of 2.3.

### Brushes across a transition are wrong today, and quietly

Fixture B, `MultiresSculptor` bound to level 3, `MeshBrush::Smooth` anchored on a
transition vertex, radius 0.25, strength 1: the dense hierarchy moves 107 weld
classes, the regional one 58. Of the vertices both hold, **11 finish somewhere
different, all 11 at the transition, worst 0.0255854** — about 60% of the level-3
edge spacing, not a hairline. `laplacian_pass` divides by the ring size AS FOUND,
which is short and one-sided at the rim, so the border is dragged inward.

The boundary automask makes it worse in a way an artist cannot see: on fixture B
**all 64 transition classes report `is_boundary_class` true, and none of them is
on the cage's own outer edge**. With `AutomaskFactor::Boundary` and 2 rings the
regional stamp drops from 58 moved classes to 47 while the dense one stays at
107. The brush fades at a seam that is not a border.

### The export gap is larger than a crack, and the polygon is its smaller half

Assembling exactly what the C ABI tells a host to assemble —
`clay_multires_copy_block` at `clay_multires_effective_level(patch, display)` for
every patch, welded by position — on fixture A:

| assembly | welded verts | tris | boundary edges | empty blocks |
|---|---:|---:|---:|---:|
| display 0, one level everywhere | 98 | 192 | **0** | 0 |
| display 3, per-patch effective level | 698 | 1152 | **240** | 0 |
| display 2, per-patch effective level | 458 | 768 | **144** | 0 |
| display 1, per-patch effective level | 218 | 384 | **48** | 0 |

Two defects, and the guide names only the smaller one:

- **corner gap** — the same cage vertex evaluated on the coarse side and the fine
  side is **0.043439553** apart, 8.7% of a 0.5 cage edge
- **T-junction** — the fine edge point sits **0.023213101** off the coarse chord,
  **half** the corner gap

A design that inserts the midpoint into the coarse face and stops fixes about a
third of the gap. The corner has no polygon fix at all: it is a choice of which
side's value the shared vertex takes, plus a ripple to every coarse face incident
to it.

Separately, `mesh_at_level(3)` on fixture B returns 289 positions and 512
triangles covering **4 of 36 base patches** — the other 32 are absent, not
cracked — and `build_block(3, patch 0)` on a non-resident patch returns **true
with 0 vertices and 0 indices**. A host that forgets `effective_level` draws a
hole with no error.

## What the audit corrected in the guide

The guide is a proposal; where the tree contradicts it the tree wins.

- **"Mixed-depth export is a transition-polygon problem."** Refuted by the two
  numbers above: the corner gap is nearly twice the T-junction and is not a
  polygon.
- **"Normal evaluation belongs inside the transition design."** It does not — the
  normals are already wrong with no transition present, at exactly the vertices
  with an incomplete face ring. This **inverts the tasks file's own ordering**:
  3.4 does not "follow 2.3", it is the thing under both, and it ships first here.
- **"For each legal 2:1 configuration, define the transition topology."** There is
  effectively one bridging case. `resolve_keep` refuses to refine a patch unless
  it *and its whole vertex ring* are resident one level down, which is stricter
  than 2:1: measured max depth spread across a shared cage vertex is **1** on five
  graded hierarchies, and **1** again on a deliberately awkward direct sequence
  where 2 of 4 requests were refused. There is no chain of templates and no
  rebalancing pass. The set is also **wider** than the guide's in one place the
  tasks file also missed: a **corner-only** coarse patch, touching a finer one at
  a cage vertex with no split edge and therefore no T-junction, still has a
  conflicting corner — 4 of 44 transition patches on a 2×2 region, 19 on three
  scattered ones. Task 2.3's wording ("a coarse face *beside* a finer patch")
  undercounts by exactly these.
- **"Provide one traversal, `for_each_surface_neighbor(vertex, callback)`."**
  Refuted twice. On shape: `smooth_targets` runs the ring up to
  `kMaxSmoothIterations` times over the same materialized CSR, and
  `build_neighbors`' own comment says the walk was moved out of the verbs for
  that reason — the tree wants one traversal *materialized once*, not one
  traversal *function*. On payload: across the sites a neighbour is nine
  different things over three topologies. `design.md` decides this.
- **"Define normal evaluation for the transition."** There are **two** normal
  evaluators with different math already live over a multires level —
  `class_normal` is angle-weighted over triangles, `level_normals` is an
  unweighted Newell sum over faces — so one traversal cannot serve both without
  changing one of their results.
- **"Define how a future per-face grouping would inherit."** This asks the tree a
  question it cannot answer: `native-mesh-polygroups` is a design proposal that
  writes no code, and its chosen unit is the triangle. A transition face inherits
  the `face_patch` of the coarse face it replaces; that is derivable, needs no
  bytes, and is as far as this change should commit.
- **"Do not weld cracks by tolerance."** Correct, and the tree's reason is
  stronger than the guide's: `level_adjacency` welds at exactly `0.0f` on the
  grounds that a level's vertices *are* the geometric points of the surface, and
  the mismatch is 8.7% of a cage edge — any epsilon large enough to close it
  would weld unrelated geometry.
- **"Cache only if profiling justifies it."** Already the tree's discipline, with
  the justification written into the code. Restating it as an open question
  invites removing a cache that was already paid for.

## Already true — not scheduled, and not to be built twice

- **The shared boundary identity the guide asks to define already exists.**
  Residency is per BASE PATCH (`patch_kept`), base patch ids are stable for the
  life of the hierarchy, and the base-patch vertex ring is already CSR
  (`patch_neighbours`). Every transition is a base-patch boundary; there is no
  second identity to invent.
- **"Watertight by construction rather than by repair" is already true for the
  STORED surface, and gated.** A regional level runs the same stencils against
  the same parent through `ChildIndex`, so its stored vertices are the dense
  hierarchy's bit for bit — re-measured independently at **0.000000000** on
  levels 1, 2 and 3. Cite it; do not rebuild the argument.
- **Determinism of a derived transition set is free.** `full_of` is ascending,
  faces are patch-major in the parent's order, ring growth is ascending patch id,
  and "the same request twice is the same hierarchy" is already gated by a
  byte-equal `encode()` with the patch list reversed.
- **The caching mechanism and its discipline exist.** `LevelCache` holds every
  derived structure, `drop_inactive_caches` / `drop_intermediate_caches` /
  `drop_all_caches` release them, and `cache_generation` moves on both create and
  release so a host holding a pointer rebinds.
- **Attribute interpolation is already regional.** `build_attr_level` restricts
  the attribute hierarchy by the geometric level's own `patch_kept`, and a split
  cage gets its own topology so a UV seam interpolates along itself. What is
  missing is only the rule for a *synthesized* face.
- **"Never authoritative" is structural, not a discipline to keep.**
  Serialization stores only the per-level patch sets and replays the build, and
  the file header already refuses to write derived face lists.
- **The materialized traversal with opt-in payloads exists**, one level below
  where the guide puts it: `SculptNeighbors` plus `build_neighbors`, with
  `want_normals` / `want_colors` because a per-neighbour geometric normal costs a
  second-order walk. Extend its identity space; do not add a second one.
- **`refit_bvh` is already correct at a transition** — it only ever needs the
  triangles this level has. Listed so nobody "fixes" it.

## What Changes

Ordered by who is waiting, which is the audit's correction to the tasks file's
own ordering.

- **The frame at a transition vertex stops depending on which patches are
  resident.** `child_frame_of` takes its `target_normal` from the child's own
  level normal, and at a region boundary that normal is one-sided. For an artist
  sculpting a regional hierarchy *today*, this is the difference between a
  coefficient that means what it means densely and one that does not. Gated
  first, because it fails today and the gate is cheap to write.
- **One cross-level topology helper, and it is a FACE ring, not a vertex ring.**
  Every divergent reader — display normals, transported frames, the propagation
  halo, the brush's angle-weighted normal, the write-back normal pass, the
  boundary automask — is a face/corner walk, and the vertex ring falls out of the
  corners. `design.md` decides this against the guide's single callback.
- **The readers are separate and stay separate**: positions, `S(n)`,
  `LocalDetail` coefficients, incident faces, incident triangle + corner + angle,
  shared-face count, workset slot. Two normal evaluators stay two.
- **`is_boundary_class` learns that a transition is not a border**, so boundary
  automasking stops fading a brush at a seam an artist cannot see.
- **Brushes across a transition** (5.3), which is two write lists absorbed into
  two levels with two frames, not one wider stamp.
- **Mixed-depth export as one watertight mesh** (2.3), quad-only, derived, last —
  see the scope note below.
- Docs and the example both currently state the export gap correctly
  (`docs/09-brush-latency-and-coverage.md`, `examples/74_regional_multires.py`);
  both are updated by whatever lands.

## Who each half is for, said plainly

**The frame and neighbourhood half has users today.** It is storage, not display:
a coefficient authored at a boundary reconstructs through a frame up to 10% off
the dense hierarchy's, and a Smooth stamp across a seam already finishes 0.0256
away from where the same stamp finishes densely. This half is not deferrable.

**The export half has no user waiting.** `mesh_at_level` and
`clay_multires_copy_level_mesh` are called only from tests, the C and Python
bindings, and examples — there is no host loop, and **the one host we can check
does not export multires hierarchies at all**. That is not an argument against
doing it: the residual is real and the change that deferred it said so. It is an
argument for the export work being **correct rather than fast**, for it going
last, and for nobody treating a slip in it as a slip against a user.

## Capabilities

### Modified Capabilities
- `mesh-multires`: a vertex's neighbourhood, its normal and its transported frame
  are complete across a depth boundary, and a mixed-depth hierarchy exports as
  one watertight mesh.

## Impact

What the branch has touched, and what its plan named and it has not. Written from
the diff rather than from the plan, because the two had already parted.

- New: `include/clay/mesh/cross_level.h` and `src/mesh/cross_level.cpp` — the
  neighbourhood a regional level does not store — and `src/mesh/multires_mixed.cpp`,
  the mixed-depth export.
- `src/mesh/multires_eval.cpp`, `src/mesh/multires_internal.h`,
  `src/mesh/multires.cpp`, `include/clay/mesh/multires.h`: where the
  neighbourhood is built, cached, re-read and priced, beside the mixed-depth
  reads.
- `src/mesh/multires_sculpt.cpp`, `include/clay/mesh/multires_sculpt.h`,
  `src/mesh/sculpt.cpp`, `include/clay/mesh/sculpt.h`, `src/mesh/automask.cpp`,
  `include/clay/mesh/automask.h`: a stamp that crosses a depth boundary, and an
  automask that stops reading one as a border. `src/mesh/sculpt_kernels.cpp` and
  `src/mesh/layered_sculpt.cpp` were named here and are untouched — the
  neighbourhood is taken where the neighbours are GATHERED, and the verbs below
  that never learn a level exists.
- `bindings/c/clay.h`, `bindings/python/pyclay_module.cpp`,
  `tests/unit/test_multires_regional.cpp`, `tests/unit/test_multires_sculpt.cpp`,
  `tests/CMakeLists.txt`.
- `docs/09-brush-latency-and-coverage.md`, and WHICH PARTS, because the first
  pass over it brought one section current and left the section beside it
  contradicting the code. Brought current: the crossing stamp, and the export
  paragraph — which had said a mixed-depth export "is not done yet" while this
  change shipped `mixed_mesh_at_level` and `build_mixed_block`, and now states
  the open-edge counts the export closes, the quad and attribute limits, and
  that it is a read that does not promise residency. NOT brought current, and
  deliberately: the memory and cold-evaluation table above it still quotes
  `examples/74_regional_multires.py`, which task 6.4 owns and which still states
  the export gap in the artist's vocabulary, so the table's figures stand and its
  source does not.
- `tools/check_task_symbols.py`, `tools/task_symbols_baseline.txt`, and a step in
  the `checks` job of `.github/workflows/ci.yml`: written here rather than
  planned, because this change's own tasks file was found citing names that were
  not in the tree.
- STILL UNTOUCHED, each an open task rather than a change of plan:
  `src/mesh/surface_frame.cpp` — section 1, the frame at a boundary, the half
  this proposal calls the one with users today and the one still unbuilt — and
  `examples/74_regional_multires.py`, which task 6.4 owns and which still states
  the export gap in the artist's vocabulary. Neither `clay.h` nor
  `pyclay_module.cpp` gains a mixed-export symbol, so a host on either binding
  still assembles per patch; `docs/09` says so where a host reads it.
- **No serialization change.** The transition topology is a pure function of the
  cage, the rule and the per-level patch sets, all already in a version-3 stream;
  `kSurfaceVersion` does not move and there is no format-minor bump.
- **The ABI does not grow, and the version lines move anyway.** Task 6.1's export
  entry point is unbuilt: the `bindings/c/clay.h` diff adds no function, only
  comments and the version. The three lines move to **0.88.0** together for a
  field of an entry point that already existed and now means something new —
  `clay_multires_stamp_report.moved_vertices` counted the weld classes a stamp
  moved at the bound level, because a stamp only ever wrote one level, and it now
  sums the classes moved on every level a crossing stamp wrote. Same layout, same
  type, new meaning, which is worse than a new field because nothing a host
  compiles against tells it to look. The number is 0.88.0 and not the 0.87.0 this
  section first carried: the branch was cut when the tree was at 0.85.0, and
  0.86.0 and 0.87.0 have both landed on main since. 6.1 would add its entry point
  at this same minor and not move it again.
