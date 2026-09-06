# Design: mesh sculpt layers

## Context

`add-mesh-multires` landed (PR #406) and the claims this change is stacked on
were checked against the tree rather than taken from the proposal:

- `mesh::MultiresSurface` exists, with the three revisions, the base-patch
  block transport, `preflight_add_level`, `drop_*_caches` and a versioned
  `encode`/`decode` at `kSurfaceVersion = 1`.
- `mesh::DetailField` is the blocked sparse field, 1024 vertices a block,
  promoting to dense only at full coverage. Its own header already says it was
  shaped for this change: *"a layer will be another `DetailField`, composed,
  not a second mechanism."* This design keeps that promise literally.
- `mesh::MultiresSculptor` binds a `MeshSculptor` to the sculpt level's mesh
  and routes everything back through one write path,
  `MultiresSurface::absorb_level_edit`.
- `session::History` already carries `Step::Kind::Multires`, a
  `MultiresDelta` payload and a `set_multires_resolver` — set once, for the
  reason `DynamicMeshFor` gives.

Three facts in the brief needed correcting against the tree:

1. **`tools/release_check.py` has no version ROW to edit.** `check_versions`
   parses CMake, `CLAY_ABI_MINOR` and `pyproject.toml` and asserts the three
   agree. Three lines move for 0.76.0, not four.
2. **`io::ClaySpaceDoc` does not hold a `MultiresSurface`.** Nothing in
   `include/clay/io` or `src/io` names one; a hierarchy is a standalone handle
   whose bytes a host stores itself. So `scene-model`'s "the memory roll-up
   reports layer content separately from the caches derived from it" is
   answered on `MultiresMemory` — which already splits authoritative from
   rebuildable — and NOT by a new `io::MemoryReport` row, which would be a
   claim about an ownership that does not exist.
3. **The multires stream is not backward-open by extension.** `decode` ignores
   trailing bytes, so appending a layer chunk under version 1 would make a
   predating reader load base detail only and present a partial composite as
   the whole surface — exactly what the `file-io` delta forbids. Hence D9.

The naming ground is worse than the proposal states: `Layer` already has THREE
meanings in this library — `MeshBrush::Layer` (a brush algorithm),
`scene::LayerId` (a document layer, which the history keys every step by), and
now an artist channel. D1 is written against three, not two.

## Goals / Non-Goals

**Goals:**

- A recorded pass is dialled — 0, half, full, hidden — without replaying a
  stroke and without re-evaluating anything the pass does not cover.
- A layer contribution and a base detail coefficient are ONE quantity in ONE
  type, composed. No second displacement representation.
- Merge-down and bake are defined by the surface they leave, not by arithmetic
  on coefficients.
- A stroke into a layer is one transaction and one undo step; a property change
  is also one undo step.
- A surface with no layers evaluates bit-identically to today, at today's cost.

**Non-Goals:**

- A colour layer stack (D4).
- Layers on `MeshSculptor`'s fixed-topology mesh as a second code path (D2).
- Procedural layers. The kind is versioned and reserved (D3); nothing
  procedural ships.
- Blend modes other than addition. Reordering is supported and, today, has no
  geometric effect; the requirement says so rather than pretending otherwise.
- Baking detail out to normal or displacement MAPS. Still the multires
  change's non-goal and still not this one's.

## Decisions

- **D1 — The channel is never spelled `Layer`, and the discipline is gated
  rather than remembered.** `MeshBrush::Layer` keeps its name: it is a shipped
  enumerator in `clay.h`, in the Swift enum and in every host's serialized
  preset, and renaming it would break all three to fix a documentation problem.
  So the NEW vocabulary carries the whole burden and carries it everywhere:
  `mesh::SculptLayer`, `SculptLayerId`, `SculptLayerStack`, `SculptLayerDelta`;
  `clay_multires_sculpt_layer_*` in C — the same `sculpt_layer` prefix the
  voxel stack already spends, so the two artist stacks read alike and neither
  reads like the brush; `surface.sculpt_layer(...)` in pyclay, matching
  `grid.sculpt_layer(...)`. No public name in `mesh` or in the ABI says `layer`
  unqualified.

  A rule nobody checks is a rule that lasts one contributor, so
  `tools/check_c_abi.py` — which already does lexical rules on `clay.h` —
  gains one: an entry point whose name contains `layer` must contain
  `sculpt_layer`, `mesh_layer`, or be one of the named `clay_layer_*`
  document-layer calls. The rejected alternative was documentation alone, and
  its cost is the support burden the proposal names: a host author reading
  "layer strength" has no way to know which of three things it dials.

  The brush algorithm additionally gains a name that is not a homonym in prose
  and in the docs: `BrushKernelId::DepositCeiling` is already what it is called
  internally, and `docs/07-brushes-and-features.md` will lead with it.

- **D2 — Layers exist on a hierarchy, and a fixed-topology mesh reaches them
  as a ONE-LEVEL hierarchy.** The tempting answer is yes-and-cheaply: a sparse
  per-vertex offset needs no levels. It is wrong for the reason multires
  exists. On a `MeshSculptor` mesh the surface IS the vertex positions, so a
  layer offset there has nothing to be measured FROM — it would be a
  world-space delta, and a world-space delta shears off the form the moment
  anything beneath it moves. That is D2 of `add-mesh-multires`, re-derived.

  `MultiresSurface::from_mesh` over a cage with no levels added is exactly a
  fixed-topology mesh: `level_count() == 1`, no subdivision, the cage
  authoritative, and — with D5 below — base deformation layers on it. So the
  product answer is YES and the implementation answer is NO SECOND PATH. The
  cost of the alternative was two composition engines that would have to agree
  about strength, masking, merge and undo, and one of them without a frame to
  make the answer stable.

- **D3 — `SculptLayerKind` is written, versioned and REFUSED when unknown.**
  A `uint16_t` kind per layer, `Sampled = 0` shipping and `Procedural = 1`
  reserved and refused by the decoder. Refused rather than skipped: a stream
  carrying a procedural pore layer that a reader silently drops presents a
  surface missing an artist's work while claiming to be complete, which is the
  same failure D9 is about. Each layer's payload is also length-prefixed, so a
  LATER format can offer skipping deliberately where refusing is wrong; the
  bytes to make that choice exist from the first release, and the choice itself
  is not made now. Rejected: an unversioned kind, whose cost is a format break
  the first time a layer is not a sampled field.

- **D4 — No colour stack here, and the reason is requirement 3.1.** Vertex
  colours blend; blending does not commute. Putting a colour layer in this
  stack would make "additive layers commute" conditional on a layer's kind,
  which is precisely the thing the spec says must be stated plainly rather than
  implied. There is also nothing to store against: `Mesh::colors` at a level is
  a SUBDIVIDED ATTRIBUTE CACHE (`AttrLevel`), rebuildable, not per-level
  authoritative state, so a colour layer would need its own storage, its own
  serialization and its own history kind — a parallel change, not a corner of
  this one. Cost of including it: the one requirement this change is proudest
  of becomes a footnote with an exception.

- **D5 — Composition is `E = B + Σ sᵢ·mᵢ·Lᵢ`, materialized per level as a
  DetailField, and `apply_detail` reads E instead of B.** The evaluation model
  in `multires.h` is untouched — `P(n) = S(n) + Frame(n)·Detail(n)` — and only
  the meaning of `Detail(n)` widens from "the level's field" to "the level's
  composed field". `MultiresLevel::detail` stays exactly what it is today: the
  BASE detail, authoritative, what a stroke with no active layer writes, what
  `detail_checksum` hashes. A sibling `MultiresLevel::composed` holds E and is
  rebuildable.

  E is materialized rather than summed on the fly because partial evaluation
  re-reads the same vertices every dab: a stroke's halo is re-framed and
  re-applied per stamp, and summing 128 layers there would put the stack depth
  inside the pointer loop. When the stack is empty, `composed` is never
  allocated and `apply_detail` reads `detail` through the same call it makes
  today — the "additive: a surface with no layers evaluates exactly as it did"
  promise is one branch on a per-level bool, not a code path.

- **D6 — The evaluated-detail cache is BLOCKED ON THE SAME GRID, and that is
  what makes both scale gates arithmetic rather than hopeful.** Every layer's
  `DetailField`, every layer's mask and the composed field share block size, so
  block *b* means the same 1024 vertices in all of them. Per level: a
  `std::vector<std::uint64_t>` of block stamps and a dirty-block set. Then

  - a CONTENT write knows the block it wrote and dirties one;
  - a COMPOSITION change (strength, visibility, mask, order, add, remove)
    dirties the blocks that layer has ALLOCATED — `DetailField::slot_block_`
    already makes a walk over stored blocks cost the stored ones, which is
    task 5.4's gate stated as a data-structure property;
  - a METADATA change (rename, set-active) dirties nothing, which is 5.2.

  Recomposing a dirty block visits the stack once per block and reads each
  layer's `block_slot_[b]` — an O(1) miss for a layer that does not reach
  there. A stamp on the top of a deep stack therefore touches its own blocks
  and no others, and within them sums only the layers actually present: task
  5.5. Prefix checkpoints stay POSSIBLE without being built, because a
  checkpoint is just a synthetic layer over a contiguous range of the stack
  with its own composition revision — the keys already admit one. Whether one
  is needed is 5.6's measurement to answer, not this document's.

  Rejected: keying the cache on a single stack revision. It is simpler and it
  makes a rename re-evaluate the model, which is the failure 5.2 names.

- **D7 — Three revisions on the stack, mirroring the three the surface already
  has.** `metadata_revision`, `composition_revision`, `content_revision`. The
  surface's existing `detail_revision` and `evaluated_revision` keep their
  meanings and are bumped by composition and content, so a host written
  against the multires ABI keeps working without learning anything new; the
  three new counters are what a host that wants to be precise reads. One
  counter cannot say which of the three happened, which is the argument
  `multires.h` already makes for its own three.

- **D8 — A stroke records the pen, not the pen times the strength, and the
  write path is a DIFFERENCE rather than a residual.** Today `absorb_level_edit`
  computes `P_written − S(n)` and stores the whole thing. With an active layer
  that is wrong twice over: it would attribute the base's detail and every
  other layer's contribution to the layer being written.

  So the layered path stores the DELTA: `ΔE = frame⁻¹(P_written − P_before)`,
  and `L_active(v) += ΔE`. The frames do not move inside one stamp, so this is
  exactly the displacement the brush applied — recorded at full size whatever
  the layer's strength is, which is requirement 3.2. The visible consequence is
  stated rather than hidden: sculpting on a layer at strength 0.5 moves the
  surface by half of what the pen asked for, and raising the strength to 1
  doubles it. That is what the delta spec's scenario says and it is the only
  reading under which "no work was lost" is true.

  NOTHING DIVIDES BY A STRENGTH, anywhere in this change. That is the same
  rule as D10 and it is why both are safe at zero.

- **D9 — The stack lives inside the multires stream at `kSurfaceVersion = 2`,
  and version 1 readers are REFUSED rather than served a partial composite.**
  `MultiresSurface::decode` checks `version != kSurfaceVersion` exactly and
  ignores trailing bytes, so the cheap route — append a chunk, leave the
  version alone — makes an old binary open a layered document, load the base
  detail, and present a surface missing the artist's passes with no signal.
  The `file-io` delta forbids exactly that. Version 2 it is; this build's
  decoder accepts 1 and 2, an older build refuses 2 by the check it already
  has, and the refusal is the "reports that it cannot present the surface"
  branch of the scenario. Cost of the rejected alternative: silent data loss
  that looks like a successful load.

  Not written into `mesh::Mesh`'s flat stream for the reason the file-io delta
  gives: its readers expect interchange arrays.

- **D10 — Merge and bake are defined by the surface they leave.** For merging
  layer *u* into *l*: for every vertex in the union of their coverage, the
  quantity that must not change is `s_u·m_u·L_u + s_l·m_l·L_l`. The naive
  concatenation solves for `L_l' = L_l + (s_u·m_u/(s_l·m_l))·L_u` and is
  undefined at `s_l = 0` — a state one slider reaches. So the operation
  instead SETS the target's composition to the identity it needs: the merged
  layer takes strength 1 and a cleared mask, and stores
  `L_l' = s_u·m_u·L_u + s_l·m_l·L_l` directly. The evaluated surface is
  unchanged by construction, at zero strength and everywhere else, and the
  operation is total. Bake-to-base is the same statement with the base detail
  as the target, where the identity already holds.

  What is lost is real and is named rather than smoothed over: the merged
  layer's strength slider no longer scales what the upper layer contributed
  independently. That is what merging MEANS, it is why merge is undoable, and
  it is why `record_barrier` is not the answer here.

- **D11 — The layer mask is a stored per-vertex field whose identity is 1; the
  brush gate is a world-space callable.** `field::MaskGate` is
  `std::function<float(cfloat3)>`, evaluated per stamp, owned by the gesture.
  A layer mask is `mesh::SparseWeightField` — the same blocking, the same block
  size, one float a vertex — stored with the layer, serialized with it, and
  read at composition time. Absent means 1.0, and writing 1.0 releases storage,
  mirroring `DetailField`'s "writing a zero releases": a mask the artist has
  not touched must not erase the layer it belongs to.

  A new small type rather than a template over `DetailField`: templating a
  shipped header would re-instantiate it for every existing includer to serve
  one new field, and the two differ in their identity element anyway, which is
  the one thing a template could not share.

- **D12 — Base deformation layers store their offsets in the cage's REST
  frame, built lazily and only when one exists.** At level 0 the cage is
  authoritative and `LevelCache::frames` is built FROM the evaluated positions,
  which with a base layer would include that layer's own contribution — a
  frame that moves with the thing it measures. So a base layer's coefficients
  are read against frames built over the cage's REST positions
  (`State::base`, which stays authoritative and unlayered), and
  `LevelCache::frames` keeps its present meaning as the transport frame for
  level 1. One extra array, at the smallest level, allocated only when a
  level-0 layer exists — zero cost for every hierarchy that has none.

  Rejected: world-space offsets at level 0, on the grounds that "there is
  nothing underneath the cage". There is: the cage itself is sculptable
  underneath a proportion pass, and a world-space offset would not follow it.
  Also rejected: giving level 0 a base `DetailField`, which would change what
  `base_mesh()` means for every existing caller.

- **D13 — The stack is OWNED by `MultiresSurface`, not held beside it.**
  Composition happens inside `apply_detail`, which is called from partial
  evaluation with a vertex list; a stack living outside the surface could only
  participate through a per-vertex callback in the hot path, or by having the
  host recompose into `detail_mutable()` — which overwrites the base detail and
  is precisely the second displacement representation this change exists not to
  create. Owning it also puts the stack inside `encode()`, inside
  `MultiresMemory`, and behind the `MultiresFor` resolver the history already
  has. Cost: `multires.h` grows an accessor pair and a memory row.

- **D14 — The history reuses `MultiresFor` and adds TWO step kinds.**
  `Step::Kind::MultiresLayer` carries a `SculptLayerDelta` (content: layer id,
  level, changed entries, optional mask changes, existence flags each side);
  `Step::Kind::MultiresLayerProperty` carries the before/after of one property
  operation. Two kinds rather than one tagged payload because the `scene-model`
  delta asks for undo memory to be MEASURABLE per kind, and `step_bytes` can
  only separate what the kind separates. No fifth resolver: a sculpt layer is
  reached through the surface the layer id lives on, and
  `set_multires_resolver` already returns it — adding a parameter to `undo`,
  `redo` and `replay` would break every host compiled against the header, which
  is the argument that header already makes twice.

- **D15 — Height and vector-displacement stamps go through the SAME square
  projection and the SAME `calpha_sample` the mesh alpha already uses.** The
  existing alpha is a weight in [0,1] that multiplies the per-vertex weight;
  `MeshBrushSettings` already carries `alpha_direction`, `alpha_tangent` and
  `alpha_extent` to place the square. Height and vector stamps reuse all of it
  and change only what the sampled value MEANS: a signed displacement along a
  chosen direction, or three components read in the vertex's transported frame.
  Vector displacement is interpreted in that frame and never in world space —
  a world-space stamp is orientation-dependent and unusable across a curved
  surface, which is the same argument D2 of `add-mesh-multires` makes for
  detail itself. Image data is borrowed for the call; the engine decodes no
  images. Rejected: a second sampler with its own orientation rules, whose cost
  is two bilinear lookups that drift apart.

- **D16 — Smoothing modes act on named FREQUENCIES, and the split is
  representational rather than a filter parameter.** `Geometry` is today's
  Laplacian over positions. `DetailOnly` smooths coefficients in the active
  layer, touching neither the base nor the form. `PreserveDetail` smooths the
  level's SUBDIVIDED positions — the form — and re-applies the composed detail
  unchanged, which is available for free because the hierarchy already stores
  the two apart. That is the mode an artist correcting anatomy under pores is
  asking for, and it is only cheap because of this representation. The eraser
  is `DetailOnly` toward zero on the active layer.

## The files

**New headers and sources**

| File | Why |
|---|---|
| `include/clay/mesh/sculpt_layer.h` | `SculptLayerId`, `SculptLayerKind`, `SculptLayer`, `SculptLayerStack`, `SparseWeightField`; the stack vocabulary and its accounting (D1, D3, D11) |
| `src/mesh/sculpt_layer.cpp` | Stack lifecycle: add, remove, move, rename, set-*, merge-down, bake; byte and coverage accounting; the dirty-block bookkeeping each operation owes (D6, D10) |
| `src/mesh/sculpt_layer_eval.cpp` | The composed-detail block cache and the per-block recompose. Split from the file above for the reason `multires_eval.cpp` is split from `multires.cpp`: one is shape over time, one is measured in microseconds |
| `include/clay/mesh/detail_stamp.h` | Height and tangent-space vector-displacement sampling, over the existing alpha square (D15) |
| `src/mesh/detail_stamp.cpp` | Its implementation; no stack knowledge, independently testable |
| `include/clay/mesh/layered_sculpt.h` | `LayeredMultiresSculptor` and its begin/stamp/commit/cancel transaction, write domain, erase, smoothing modes (D8, D16) |
| `src/mesh/layered_sculpt.cpp` | The transaction and the coalescing record |
| `src/mesh/sculpt_layer_serialize.cpp` | The version-2 stack chunk and `SculptLayerDelta`'s byte form (D3, D9) |
| `tests/unit/test_mesh_sculpt_layers.cpp` | Stack semantics, commuting, strength, lock, remove, merge, bake |
| `tests/unit/test_mesh_sculpt_layer_stroke.cpp` | Transaction, coalescing, cancel exactness, symmetry, stamps, smoothing modes |
| `tests/unit/test_mesh_sculpt_layer_cache.cpp` | Revisions by kind and the two scale gates |
| `tests/unit/test_mesh_sculpt_layer_io.cpp` | Round-trip, id survival, refusal before allocation, the version-2 boundary |
| `tests/unit/test_mesh_sculpt_layer_history.cpp` | Content and property undo, journal encode/decode/replay |
| `tests/unit/test_c_mesh_sculpt_layers.cpp` | The ABI: identity across a reorder, names into caller buffers, block readback, `struct_size` |
| `examples/69_mesh_sculpt_layers.py` | The milestone (task 8.6) |

**Existing files changed**

| File | Why |
|---|---|
| `include/clay/mesh/multires.h` | Owns the stack: accessors, the three new revisions, memory rows, the version-2 note (D5, D7, D13) |
| `src/mesh/multires_internal.h` | `MultiresLevel` gains `composed`, block stamps and a dirty-block set; `State` gains the stack and the lazy rest frames (D6, D12) |
| `src/mesh/multires_eval.cpp` | `apply_detail` reads the composed field; `absorb_level_edit` routes a write into the active layer as a delta (D5, D8) |
| `src/mesh/multires.cpp` | Memory rows, `drop_*_caches` releasing the composed field, per-level sizing on add/remove |
| `src/mesh/multires_serialize.cpp` | `kSurfaceVersion = 2`, the stack chunk, and accepting 1 (D9) |
| `include/clay/mesh/multires_sculpt.h`, `src/mesh/multires_sculpt.cpp` | The plain sculptor learns that a write may be destined for a layer, and refuses a locked one |
| `include/clay/mesh/detail_field.h` | Block presence and stored-block iteration exposed for the composer; no storage change |
| `include/clay/session/history.h`, `src/session/history.cpp` | The two step kinds, their journal events and `step_bytes` rows (D14) |
| `bindings/c/clay.h`, `bindings/c/clay_c.cpp` | The layer surface, the stamp descriptors, `CLAY_ABI_MINOR 76` |
| `bindings/python/pyclay_module.cpp`, `bindings/python/tests/test_pyclay.py` | The stack and `with surface.sculpt_layer(...)` |
| `bindings/swift/**` | The smoke case (task 8.5) |
| `tools/check_c_abi.py` | D1's naming gate |
| `tools/check_binding_parity.py` | `CLASS_PREFIX` and alias rows for the new classes |
| `benchmarks/bench_main.cpp` | The 1/4/16/64/128 sweeps (task 5.6) |
| `CMakeLists.txt`, `tests/CMakeLists.txt` | Register the new sources and tests; `VERSION 0.76.0` |
| `pyproject.toml` | `version = "0.76.0"` |
| `examples/run_all.py` | `69_mesh_sculpt_layers` in the list and against the `mesh-sculpt-layers` capability |
| `docs/07-brushes-and-features.md`, `README.md` | The stack, the distinction from the brush algorithm, and widening the README's sculpt-layer claim past voxels (task 8.8) |

`tools/release_check.py` is NOT edited: `check_versions` derives the three
numbers rather than storing a row.

## The gates

Each line is a claim, the test that proves it, and what a failure means.

| Claim | Gate | A failure means |
|---|---|---|
| 0 contributes nothing, 1 contributes fully, invisible contributes nothing | `test_mesh_sculpt_layers` — evaluated positions against the no-layer surface, bit-compared | Composition is not `B + Σ s·m·L`; the model in D5 is wrong or unevenly applied |
| A strength change replays no stroke | Same suite, asserting `MultiresEvalStats` and a stamp counter across the change | Something is re-running the gesture; the dial is not composition |
| Additive layers commute | Swap two overlapping layers, compare evaluated positions bit for bit | Composition has an order dependence the spec says does not exist — probably an accumulation that rounds |
| A stroke at strength 0.5 records full size | Stroke, then set strength 1, compare the contribution to twice the visible one | D8's delta path is dividing or scaling by the strength — the trap the whole change is written around |
| Merge and bake are visually invisible, including at strength 0 | Parity assertions before/after, with the target at 1, at 0.37 and at 0 | The concatenation formula crept back in; the zero case is the canary |
| Removing a layer re-evaluates its coverage only | Remove the middle of three overlapping layers; compare the other two's contributions and the recomposed block count | Removal is touching state it does not own, or costing the surface |
| Cancel restores exactly; commit is one delta; 100 stamps coalesce to one entry | `test_mesh_sculpt_layer_stroke` — checksum equality after cancel, `SculptLayerDelta::size()` after commit | The transaction is not the SDF transaction's shape; a partially-applied gesture is reachable |
| Symmetry enters one layer and one step | Mirrored stroke, assert one layer, one delta, union coverage | A mirrored write is creating a second channel |
| Vector displacement follows the tangent frame | Same stamp at two orientations on a curved surface | D15 is being read in world space; the stamp is unusable in production |
| A stamp finer than the level reports the shortfall | Stamp with features under the level's mean edge length; assert the report, not a smoothed silence | The library is implying a resolution it does not have |
| A rename does not invalidate geometry | `test_mesh_sculpt_layer_cache` — recomposed-block counter across a rename | D7's three revisions collapsed back into one |
| **Task 5.4** — a strength change costs coverage | Large level, layer over a small fraction; assert recomposed blocks ≈ the layer's allocated blocks and NOT the level's | The invalidation is keyed on the stack rather than on the layer's blocks |
| **Task 5.5** — a stamp on a deep stack does not sum from zero | 128 layers, local coverage; assert the layers visited over unrelated blocks is zero | Composition is a full walk; prefix checkpoints become mandatory rather than optional |
| Undo restores sparse detail EXACTLY | `test_mesh_sculpt_layer_history` — `DetailField::checksum` equality, and block counts, after undo | Undo is reconstructing rather than restoring |
| Property changes are user history | Strength change, undo, assert the value and the evaluated surface | The change fell into the cache layer instead of the step list |
| Journal replays, old journals still replay, malformed refused | Encode/decode/replay plus fuzzed payloads | A step kind was added without its inversion or its bounds |
| Round-trip; absurd declared size refused BEFORE allocation | `test_mesh_sculpt_layer_io`, mirroring `test_multires_io.cpp`'s refusal cases | A hostile stream reaches an allocator |
| A layer id survives save, load and reorder | Same suite | Ids are indices somewhere |
| Recording never stops silently | Assert bytes are reported and nothing is dropped at any budget | A cap turned into a correctness bug (task 5.7) |
| Parity | `tools/check_binding_parity.py` | A capability is C-only or Python-only |
| Naming discipline | `tools/check_c_abi.py`'s new rule | D1 eroded |
| **The milestone** | `examples/69_mesh_sculpt_layers.py` — a wrinkle pass at 0 / 50% / 100% over a form that never changes, plus one layer removed with the others untouched | The feature does not read as a feature |

Benchmarks (task 5.6): `BM_SculptLayerCompose`, `BM_SculptLayerStrengthChange`
and `BM_SculptLayerStampOnStack` over 1, 4, 16, 64 and 128 layers, each with
local, overlapping and dense coverage. They report; the two gates above are what
fails a build.

## Risks / Trade-offs

- **Memory, and it multiplies twice.** A layer costs its coverage per level,
  and 128 layers over the same region cost 128 copies of it. The composed field
  costs the union once more. The answer is reporting rather than capping (5.7),
  plus merge, bake and delete as the artist-facing levers — but a host that
  ignores the report will run a device out of memory, and this change makes
  that easier to do than it was.
- **The stamp on a low-strength layer looks damped.** D8 is right and it will
  read as a bug to an artist who has not been told. It needs to be in the docs
  and in the example, not only in a spec.
- **A brush stroke through the level mesh now depends on composition.** The
  sculptor reads evaluated positions, which include every visible layer, so
  hiding a layer mid-stroke changes what the next stamp sees. The transaction
  should refuse a composition change while a stroke is open, and that refusal
  is a requirement nobody has written yet.
- **Bit-parity of the no-layer path.** The promise is that a surface with no
  layers evaluates exactly as today. It holds only if `apply_detail` reads the
  base field through the same arithmetic; an implementation that always
  composes — even summing an empty stack — would move the last bits of every
  vertex. It needs a golden test, not care.
- **`SparseWeightField` is a second sparse container.** Justified in D11, and
  it is still a second thing to keep in step with `DetailField`'s blocking.
  If they drift, the shared block index in D6 stops being shared and both scale
  gates quietly weaken.
- **Level-0 rest frames (D12) are a third frame notion in one file.** Cheap in
  memory, not cheap in reading comprehension. It needs the comment `multires.h`
  would write for it.

## Migration Plan

Additive. A hierarchy with no layers evaluates, serializes and costs exactly
what it does today; `MultiresSurface`'s existing signatures are unchanged and
the two new step kinds are added beside the ones `session::History` already
carries. The one non-additive edge is the stream: documents written by 0.76.0
carry `kSurfaceVersion = 2` and an older binary refuses them, deliberately and
loudly (D9). Documents written before 0.76.0 load unchanged.

## Open Questions

- **Whether prefix checkpoints are needed at 128 layers.** D6 keeps them
  possible; 5.6's measurement decides, and the answer belongs in this file
  once it exists rather than in a commit message.
- **Whether a layer may span levels it has no detail on.** It may today —
  per-level fields, most of them empty — and whether a host wants to SEE a
  layer's level set, or only its coverage, is a UI question this change should
  not pre-answer in the ABI.
- **What a composition change during an open stroke should do.** Refusing is
  proposed above; deferring until commit is the alternative and it is not
  obviously worse.
- **Whether `MeshBrush::Layer` should eventually gain
  `MeshBrush::DepositCeiling` as an ABI alias.** Free and additive in C, but
  the parity gate reads enumerators from both sides and an alias would have to
  exist in pyclay too. Deferred rather than dismissed.
