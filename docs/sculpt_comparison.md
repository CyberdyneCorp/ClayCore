# Where claycore stands: Blender, ZBrush, 3DCoat

An honest positioning of claycore's sculpting capability against the three tools
it gets compared to, written to be checkable rather than persuasive.

**Read the framing first, because it decides most of the answer.** Those three
are *applications*. claycore is an *engine* — headless, embeddable, no UI, no
viewport, no file browser. It is the core of **ClaySpace**, and it is ClaySpace
that would compete with them. So this document separates two questions that get
conflated:

1. **Is the engine capable enough** to sit under a competitive sculpting app?
2. **What is missing from the engine** before that app could ship?

Question 1 is mostly yes. Question 2 has concrete answers, and they are not
mostly brushes.

## A caveat on the other three

claycore's side of every claim here is checkable against this repository, and
the last section says how. The other three are described from their published
behaviour and general knowledge of how they work; they ship on their own
schedules and specifics drift between versions. Where a competitor's exact
feature list matters to a decision, verify it against their current release
rather than against this table.

The comparison is therefore drawn at the level of **architecture classes** —
what a representation makes easy, hard or impossible — rather than as a
marketing checklist. That level is stable across versions and is the level that
actually predicts what an engine can and cannot be made to do.

---

## The one-paragraph verdict

As an **engine**, claycore is plausibly ahead of what sits inside any of the
three on three specific axes: a fully non-destructive parametric edit list,
per-node exactness and Lipschitz tracking that makes raymarching provably
correct, and portability across four backends from one kernel source with gated
parity. As a **sculpting product**, it is at *"core brush vocabulary complete,
workflow tier absent."* The brushes landed, sculpt layers landed on voxel
layers and then on a mesh subdivision hierarchy's detail, alphas landed on SDF
layers, and masking now protects against any operation rather than only against
brushes; sculpt layers on SDF layers and the asset-finishing pipeline have not.
SDF layers and the asset-finishing pipeline have not.

---

## Axis by axis

| Axis | claycore | ZBrush | 3DCoat | Blender |
|---|---|---|---|---|
| **Representation** | SDF edit list + sparse voxel grids | Multires mesh (+ DynaMesh) | Voxel + surface mesh | Multires mesh |
| **Non-destructive history** | **The document *is* the history.** Every edit stays re-editable forever | Layers record passes; the mesh itself is baked | Largely destructive past a bake | Modifier stack, but sculpt strokes are destructive |
| **Topology management** | **Three explicit modes rather than one policy.** An SDF has no topology; a mesh layer's is PRESERVED (the mesh brushes never touch the index buffer); a `DynamicSurface` adapts it locally under the brush; and `mesh::voxel_remesh` REPLACES it globally through a signed field. What is missing is the fourth — quad retopology — and nothing here pretends otherwise | DynaMesh, ZRemesher | Auto-retopo, its strength | Dyntopo, Remesh |
| **Field correctness** | **Exactness + Lipschitz tracked per node**, so step size is derived, not tuned | n/a — mesh | n/a — mixed | n/a — mesh |
| **Booleans** | **Watertight by construction**, 2-manifold meshing | Live Boolean, then remesh | Voxel booleans, robust | BMesh booleans, fragile on bad input |
| **Brush vocabulary** | Core set complete on fields and voxels, plus 16 fixed-topology verbs (14 that move vertices, 2 that write colour) and a lattice cage on a mesh layer (see below) | The reference: ~36 surface brushes plus the core | Broad, voxel + surface modes | Solid core set |
| **Masking** | **Protects the surface from any op**, on either representation — a gated item does not act where the mask protects | First class, protects the surface from *any* op | First class | First class |
| **Sculpt layers** | **On voxel layers and on a multiresolution mesh.** A pass is bracketed and what it changed is recorded, so its strength stays adjustable long after the strokes are finished. The mesh stack is the closer match to what the other three mean by the words — additive displacement in a transported tangent frame, so a fractional strength is an exact fraction rather than a dither, reordering changes nothing, and dialling a pass is itself undoable. SDF layers do not have them yet | Headline feature | Present | Present |
| **Alphas / stamps** | **Both representations**, scalar stamps only — no vector displacement maps | Deep, VDM support | Deep | Present |
| **Surface colour** | **Polypaint on all three representations**: per-item colour and freehand `Paint` strokes on SDF layers, per-sample colour in sampled volumes, a 256-entry voxel palette, and — since `add-mesh-colour-brushes` — `paint` and `smear` on a mesh layer's own vertices. No PBR channels — a declared non-goal for painting | Polypaint | **PBR texture painting — its moat** | Vertex paint + texture paint |
| **Scale** | ≥256³ per voxel layer, no streaming; SDF edit lists degrade step scale as they grow | Tens of millions of polys | Very large voxel scenes | Large, memory-bound |
| **Embeddable** | **Yes — C ABI, SwiftPM, Python, headless** | No | No | No (as a library) |
| **GPU portability** | **One kernel source → CPU / Metal / CUDA / OpenCL, parity-gated** | Proprietary CPU-centric | GPU-assisted | GPU sculpt, single path |

---

## Brush-level parity

The core sculpting verbs, and where claycore stands on each. Per-verb detail is
in [`07-brushes-and-features.md`](07-brushes-and-features.md).

| Brush (ZBrush name) | claycore | Status |
|---|---|---|
| Standard | `Op::Relief` | ✅ |
| ClayBuildup | `Op::Relief` along a stroke | ✅ buildup accumulation scales each stamp's amplitude |
| Smooth (as a pair with the above) | `field::relax` | ✅ the blockout pair — see `examples/29_claybuildup_smooth.py` |
| Crease, DamStandard | `Op::Incise` (SDF), `MeshBrush::Crease` (mesh), and on voxels a stroked erode with a CONSTANT falloff | ✅ on all three. The voxel form is a recipe rather than a verb, and that is a measured decision: a crease verb was built and produced a profile identical to the plain erode, because the pinch that sharpens a mesh crease FILLS a voxel one — a lattice holds a volume, not a sheet |
| Inflate | `Op::Relief`, `sculpt_inflate` | ✅ |
| Move | `brush::move_brush` | 🟡 drags the assembled surface, but **buds rather than stretches** on a large pull, and a stroke's drags compound the step scale — see below |
| Move Topological | `field::move_topological` | ✅ geodesic falloff; bakes, so single-gesture |
| Trim Curve | `CutShape::from_open_curve` | ✅ (listed above) |
| Rotate | `pose`, `pose_line` | ✅ |
| Pinch / Magnify | `magnify` (signed), `sculpt_pinch` / `sculpt_magnify` | ✅ one deformation, one sign |
| Smooth | `field::relax`, `sculpt_smooth` | ✅ bakes on the SDF side |
| Flatten | `field::flatten`, `sculpt_flatten` | ✅ region required on the SDF side |
| hPolish, Planar, Trim | `field::flatten` cut-only | 🟡 planes down without filling, but **single-pass**: chaining bakes samples a volume and degrades |
| Trim / Clip | `cut::cut_item` | ✅ as a solid, Clip *is* Trim |
| SnakeHook | `brush::snakehook` | ✅ adds material rather than pulling it |
| Tubes (Nomad Sculpt) | `brush::tube` | ✅ path, B-spline toggle, variable radius, profile, closed — a round tube stays exact |
| Surface Noise | `noise` deformer | ✅ integer hash, so all backends agree |
| Deformation palette (Taper, Twist, Bend, Flatten, Inflate, Noise) | 21 `Deformer`s on an SDF item; **`taper` and `twist` also on a mesh layer** (`MeshSculptor::deform`), plus a lattice cage on both | ✅ on SDF and mesh, ⬜ on voxels. The SDF forms are inverse point maps, so they compose and stay non-destructive; the mesh forms are the FORWARD maps, applied once per vertex — the easier direction and the exact one, so a tapered mesh and a tapered field are the same shape. **`bend` is SDF-only, by measurement**: it takes its angle from a coordinate it then moves, so it has no closed-form forward map, and past a gentle angle it has none at all — the deformation folds distinct points onto the same place. A VOXEL layer still takes neither |
| Blob | `blob` | ✅ noise under a brush region |
| Pulling a lobe out | `brush::snakehook` | ✅ the verb for growing form; Move is the verb for nudging it |
| Morph | — | 🟡 no named morph target, but a **base deformation layer** at level 0 of a multires surface is stored, dialable offsets against a rest pose, which is the substrate. Unblocked on voxels too now that layers exist; still absent on SDF |
| Layers | `VoxelGrid::begin_sculpt_layer`, `MultiresSurface::add_sculpt_layer` | 🟡 voxel and multires-mesh, not SDF — an SDF pass is a weighted group, and that is now the only representation without a stack |
| Alphas | `sculpt_carve_alpha` (voxel), `Deformer::alpha` (SDF) | ✅ both, scalar stamps |
| Masking | mask fields, `Node::gate` | ✅ gates any operation on either representation — a boolean included; the gate is a measured DISTANCE, so its cost follows a width you set |
| **PolyGroups / Face Sets** | `voxel::GroupField`, `Document.groups()` | ✅ on **every** representation from one mechanism. Named regions on a world-space lattice rather than per-face ids, so a group survives rasterize/mesh/convert **by construction** — the ids were never in the SDF, the voxels or the mesh, so nothing can lose them, and a voxel grid's 256³ memory guarantee is untouched. Isolate hides the complement, hiding is not deleting (the field is untouched and the produced mesh is filtered, so showing restores the same triangles exactly), and both the ids and what was hidden survive a save. **What it costs:** the boundary is quantised to the lattice, not to the representation, so a mesh that could have carried an exact per-face border does not. **Where it differs from ZBrush:** grow is VOLUMETRIC, not geodesic — a fold closer than `steps` cells is crossed rather than followed |
| Slice / Knife | — | ❌ polygroup splits need two items; no single-solid equivalent. Naming the two halves is now possible (row above); *splitting the solid* still is not |
| Surface brushes on a MESH (Standard, Move, Inflate, Smooth, Pinch, Flatten, Clay, DamStandard, Trim Dynamic, hPolish, SnakeHook, Layer, Nudge, Relax) | `mesh::MeshSculptor`, **14 verbs, with alphas** | ✅ on a mesh LAYER's own triangles, with topology fixed — see below |
| Polypaint / Smear on a MESH | `mesh::MeshSculptor` `paint`, `smear` | ✅ the only two verbs that move no vertex; they refuse a mesh with no colour attribute rather than creating one |
| Elastic (Blender), ZProject | — | 🟡 Elastic was filed "does not survive the representation change", which was true for fields and is no longer true on a mesh layer. Undecided rather than rejected; it is the one entry that is new *math* rather than a new composition of the eleven |
| DynaMesh / global voxel remesh | `mesh::voxel_remesh` | ✅ **and this is the row where the comparison is closest.** A whole surface sampled into a signed narrow-band field at an explicit world voxel size and rebuilt from it: overlaps fuse, self-intersections resolve, density comes out uniform, the result is validated watertight before it is returned, and the cost is preflighted so an oversized request is refused before it allocates. **Where it differs from DynaMesh:** the sampling domain follows the surface and its band rather than the bounding box, so the expensive work scales with area and not volume; the resolution is a WORLD VOXEL SIZE with longest-axis as a convenience over it, rather than a unitless slider; and the failure modes are typed rather than silent — an open surface takes an explicit policy, and a resolution over budget is refused rather than quietly lowered. **What it does not do:** preserve UVs (dropped, and the API says dropped), infer edge loops, or keep anything thinner than the voxel size |
| Dyntopo, LiveClay, multires | — | ❌ **absent, and no longer deliberate.** The reasoning — an SDF sidesteps topology, and dynamic tessellation is not this engine's fight — held until fixed-topology mesh brushes shipped and made the stretch below a reason to leave the engine rather than a boundary. Reversed 2026-08-29 and scoped as separate representations beside the fixed-topology one (`openspec/ROADMAP.md` Phase 5). None of it is implemented; this row moves when it is |

### Surface brushes: the row that moved

This is the one line of the comparison that changed direction, so it is worth
stating rather than leaving to a table cell.

**What ZBrush's ~36 surface brushes and claycore's fourteen mesh verbs have in
common** is that both move vertices on a mesh. **Where they part** is that
ZBrush's re-tessellate as they go — that is what Dyntopo, LiveClay and multires
are for, and it is what lets a Move brush draw a lobe out of a sheet
indefinitely. claycore's do not: `indices` and `quads` come out byte for byte,
and a large grab stretches the triangles it has instead. That stretch is the
signal the mesh wants retopo — the same signal Blender gives with Dyntopo off —
and it is where this stops today. **The word that changed on 2026-08-29 is
"purpose":** the stretch is still what happens and is still documented, but it
is now the reason an adaptive representation is scoped rather than the reason
one is not. See `openspec/ROADMAP.md` Phase 5.

So the honest position is neither "we have surface brushes now" nor the old
"❌ out of scope":

- **The verb set is there.** Grab, draw, inflate, smooth, pinch, flatten, clay,
  crease, scrape, polish, snakehook, **relax, layer and nudge**, with masks,
  alphas, strokes and undo, on a mesh layer's own triangles.

  Three of those close gaps the eleven left. **Relax** slides vertices along the
  surface to even their spacing where smooth reshapes it — and it matters more
  here than in a tool that can subdivide, because topology is fixed, so a large
  grab stretches the triangles it has and this recovers them without a round
  trip. Measured, its advantage over smooth is largest on a COARSE mesh (3.1x
  less surface motion at 22k triangles, 1.7x at 180k), which is exactly where
  stretching shows. **Layer** deposits to a ceiling instead of accumulating, so
  a slow stroke and a fast one over the same path agree. **Nudge** pushes
  material along the surface where grab carries it off.

  And **alphas reach mesh brushes**, which they did not: voxels have had them
  since 0.24 and SDF items gained them recently, leaving the representation an
  artist reaches for *after* a retopo pass — when detail is the point — as the
  one that could not stamp one. The stamp multiplies the brush's weight, so it
  composes with every verb and falloff at once, and it is sampled by the kernel
  function the SDF alpha uses, so one stamp reads the same on both.
- **The topology tier is not, and will not be.** No brush here adds a polygon,
  so detail beyond what the mesh already carries needs a retopo pass — which
  is the pipeline this exists to serve rather than a workaround for it.
- **The use it earns is the RETURN TRIP**, not free-form sculpting. Sculpt on
  SDF or voxels, quad-export, retopo and UV elsewhere, then refine on the mesh
  that came back — which was impossible before, because the only way in was
  `Volume.from_mesh` and it resamples the topology away.

---

## Where claycore wins outright

These are structural, not effort — they follow from the representation, and none
of the three can retrofit them cheaply.

- **The edit list is the document.** A sculpt stays parametric forever: reorder
  a boolean from an hour ago, change a blend radius, retarget a stroke. ZBrush
  and 3DCoat bake; Blender's modifier stack does not compose this way over
  sculpt strokes.
- **Correctness is derived, not tuned.** Every node carries whether it is an
  exact distance and what its Lipschitz bound is, so the safe step scale is
  computed. A raymarcher cannot be made to step through a surface by an
  unlucky parameter — the field says how far it is safe to step.
- **Booleans are watertight by construction.** No degenerate-input failure mode,
  because there is no mesh to degenerate.
- **One kernel source, four backends, tolerance-gated parity** — and a host can
  compile the *same headers* into its own GPU preview and assert against an
  exported fixture rather than reimplementing the math.
- **Headless and embeddable.** It is a library with a stable C ABI. The other
  three are applications.

---

## What is missing, in the order that matters

The capability list is the tell: all 14 capabilities are *mechanism* — kernels,
scene, voxel, meshing, I/O, picking, bindings. **None is a workflow capability**,
and that is the shape of the gap.

### Tier 1 — sculpting workflow concepts

| Gap | Today | Why it blocks parity |
|---|---|---|
| **Masking as a field concept** | **Landed.** An item carries a gate — the signed distance to a painted mask's region — and does not act where it protects | ZBrush masking protects the **surface** from *any* operation, and this now does too: the gate rides the combine record rather than being a mode, so it gates a boolean, a smooth union or an add alike. Both ends are exact — fully protected is the accumulated field bit for bit. What it costs is honest and visible: mixing two fields by a spatially varying weight is not a distance, so a narrow gate costs an order of magnitude of step scale and a wide one much less (`examples/54_masked_operations.py` measures it). The cost follows the falloff width you choose rather than how hard the brush edge that painted the mask was, which is why the gate carries a distance rather than paint. |
| **Sculpt layers / morph targets** | **On voxel layers and on a multiresolution mesh**; not on SDF layers | A layer that records a pass and replays it at an intensity is a *document* concept, not a brush. The voxel side has it: `begin_sculpt_layer`/`end_sculpt_layer` bracket a pass, and strength, visibility, reordering and merge-down follow (`examples/52_sculpt_layers.py`). What a **fraction** means differs from ZBrush by representation — ZBrush interpolates vertex offsets, and binary occupancy has nothing to interpolate, so a fractional strength is a reproducible fraction of the *cells*, dithered against the same cell-coordinate hash the falloff brushes use. **The mesh stack is where the ZBrush arithmetic applies literally**: a `MultiresSurface` stores detail as offsets in a transported tangent frame, so `add-mesh-sculpt-layers` interpolates exactly what ZBrush interpolates — 0.5 is half the offset, not half the cells — and it goes further in two places, a mask per layer distinct from the brush gate, and layer property changes inside the undo history rather than outside it (`examples/69_mesh_sculpt_layers.py`). Two differences from ZBrush remain and are deliberate: reordering an additive stack is organisation and not geometry, because the sum commutes; and merge-down is defined by visual parity of the evaluated surface rather than by concatenating coefficients, which would divide by the lower layer's strength and be undefined at zero. On an SDF layer the equivalent is a weighted group rather than a diff, which waits on `expose-scene-groups`. A morph target is not named as such anywhere, but a base deformation layer at level 0 is its storage; it stays "not planned" for the SDF side only. |
| **Alphas on SDF layers** | **Landed.** `Deformer::alpha` / `Prim.alpha(...)` / `clay_item_add_alpha` | Detail work in all three tools is alpha-driven, and it now works on the non-destructive representation rather than only the baked one. An alpha is a deformer — a distance offset under finite support — because it modulates an existing surface rather than adding material in the stamp's shape. The engine decodes no images; a host passes the samples. |

### Tier 1b — Move is not a mesh Move, and a stroke compounds

Worth its own entry because it is the one place a ZBrush user's expectation
breaks against the representation rather than against a missing feature.

**A mesh stretches; a field moves what is already there.** ZBrush's Move drags a
region of vertices and the sheet between them stretches. `grab` samples the field
at `p - w·d`, so where the weight is one the material is rigidly displaced and
where it falls to zero nothing happens. A large pull **buds a lump** off the
surface rather than drawing a lobe out of it, and pulling harder barely helps:
measured on a unit sphere, a displacement of 1.1 gains +0.34 and one of 2.5 gains
+0.42, because the falloff bounds the reach rather than the drag.

**And a stroke is many drags.** An artist walks the brush outward; each drag is
another grab on the chain and each multiplies the declared Lipschitz, so the safe
step scale decays **geometrically** — about ×0.615 per drag:

| drags | 1 | 3 | 6 | 9 |
|---|---|---|---|---|
| step scale | 0.615 | 0.233 | 0.054 | 0.013 (79× marching cost) |

Coalescing covers frames of *one* drag, where the centre and radius are fixed; a
stroke moves the centre, so those stack by design. A host pulling a long lobe has
to **consolidate** rather than keep appending: `clay_layer_consolidate` /
`Layer.consolidate` collapses the layer into one volume and redistances it, which
takes the nine-drag stroke from a step scale of 0.013 back to 0.577 in one
undoable step. `clay_layer_field_report` is how a host knows to offer it — it
reports the step scale alongside the two things that cost it, a steepening volume
and a lengthening deformer chain, and never bakes on its own.

The verb for *growing* form is `snakehook`, which sweeps a tapered item along the
drag: it reaches as far as the drag goes and the field stays exact (step scale
1.0 with three lobes on it, against 0.05 for the Move version).
`examples/27_move_strokes.py` builds both and measures them.

### Tier 2 — finishing an asset

- ~~**The pipeline seam.**~~ **Closed 2026-08-24.** This row read "neither engine
  owns the seam yet", and it was wrong in both directions once someone read the
  other repository instead of assuming.

  **They owned more of it than we knew.** CyberRemesherAndUV had already
  specified a sculpt handoff, shipped the reader, and built a `FieldEvaluator`
  with a three-callback C ABI for field-sampled baking — and recorded that
  agreement with ClayCore was outstanding because no negotiation had taken
  place. Their CLI already assumed our half existed.

  **And we owned more of it than the roadmap said.** The bridge row asked for "a
  field-evaluation callback so a baker can sample exact normals", which
  `clay_eval_points` and `clay_eval_gradients` had done for releases.

  The seam is now decided and both halves exist: **their engine bakes, this one
  answers field queries**. `clay_mesh_save_handoff` writes what their reader
  accepts — verified against their actual CLI, where a ClayCore quad export
  retopologises to 708 quads with zero dropped faces. You can sculpt *and* ship.
- **PBR material authoring.** Corrected by measurement
  (`decide-surface-colour`): this row used to read "there is no polypaint and no
  PBR painting", and the first half is false. **Polypaint works** — a stroke
  applied with the `Paint` op is a freehand colour brush with pressure, spacing,
  jitter and masking, it leaves the field bit-identical, and the colour it
  writes is the one authored. A sampled volume carries per-sample RGB the tape
  reads on every backend, and consolidating a painted layer preserves that
  colour exactly, which is how colour resolution moves from ITEM-bound to
  TEXEL-bound without repainting.

  The one representation that could only CARRY colour was the mesh layer, and
  that closed with `add-mesh-colour-brushes`: `paint` and `smear` are Blender's
  pair, they are the only verbs in the vocabulary that move no vertex, and they
  refuse a mesh with no colour attribute rather than creating one behind a
  brush stroke. So colour is now editable on every representation the library
  has, rather than on two of three.

  What is genuinely absent is **PBR channels** — roughness, metallic, normal —
  and that is a declared non-goal for *painting* rather than a gap. **Baking
  them is a different thing and is now reachable**: their `cyber_bake_field`
  drives a normal/AO/curvature bake from a field, and ClayCore fills its three
  callbacks with `clay_eval_points`, `clay_eval_gradients` and
  `clay_measure_points`. What ClayCore deliberately does not learn is UV
  semantics — seams, islands, padding, texel density — because that is what the
  sibling owns and a second implementation would disagree with theirs about
  precisely the details that make a bake look right. 3DCoat's moat is the
  texture pipeline, not the colour channel.

### Tier 3 — scale, and the quiet one

- **Performance at sculpt density is unproven.** There are four benchmarks with
  generous floors, and **none exercises a deep edit list**. A real sculpt is
  thousands of items, and every relief, grab and noise item costs step scale.
  Consolidation exists (`clay_item_volume_from_document`) but there is no policy
  and no LOD strategy. A benchmark over a 2,000-item document would be the
  honest next measurement.
- **No streaming** past 256³ per voxel layer.

---

## Deliberate non-goals

Recorded so they read as decisions rather than oversights. In full in
[`../openspec/ROADMAP.md`](../openspec/ROADMAP.md).

- ~~**Topology-CHANGING mesh sculpting** — dyntopo, multires, remeshing,
  subdivision (3DCoat's LiveClay, ZBrush's dynamic tessellation).~~
  **REVERSED 2026-08-29** — scoped as Phase 5 in
  [`../openspec/ROADMAP.md`](../openspec/ROADMAP.md), and implemented nowhere
  yet. The original reasoning is kept because it still frames what remains: an
  SDF sidesteps topology entirely; competing on dynamic tessellation is not
  this engine's fight. What it never answered is what happens to a mesh layer
  after a snakehook has stretched it, and shipping those brushes is what made
  the question live. The reversal is bounded: adaptive topology is a SEPARATE
  representation, and `MeshSculptor` keeps its byte-identical `indices` and
  `quads` unchanged.

  **Amended, not deleted.** This row used to read "mesh surface-mode
  sculpting", and that was wider than the decision behind it. Moving the
  vertices that already exist is a different claim from tessellating new ones,
  and it is the one the pipeline needed: a retopologized quad mesh re-enters a
  document as a mesh layer, and resampling it through `Volume.from_mesh` to
  edit it destroys the topology somebody just paid for. So fixed-topology mesh
  brushes landed — eleven verbs, vertices only, `indices` and `quads`
  byte-identical before and after — and the boundary moved to where the
  original reasoning actually put it. See
  [`docs/07-brushes-and-features.md` § 8](07-brushes-and-features.md). A large
  grab stretches triangles and `snakehook` stretches them badly; that is the
  signal the mesh wants retopo, and it is where this stops.
- **Subdivision multires on the SDF side**, and there only. Resolution is an
  evaluation parameter for an SDF layer, so the Res+/Resample apparatus has
  nothing to attach to there — which is exactly why the sentence never applied
  to a mesh layer, whose resolution is fixed by its import and is neither
  evaluated nor stacked. **A mesh subdivision hierarchy has since landed** as
  `add-mesh-multires`, beside the fixed-topology layer rather than inside it —
  `MultiresSurface` is its own handle — and `add-mesh-sculpt-layers` put a
  dialable stack over its detail. Voxel layers now DO carry a level stack — level 0 coarsest,
  half the cell size per level, detail held as offsets so a coarse stroke does
  not flatten fine work — because a voxel layer's resolution is real storage
  rather than a sampling choice. Discrete levels rather than an octree, so the
  cell-coordinate hash the falloff dither depends on keeps working.
- **Texture painting, node-graph texturing UI, scripted brushes.** The edit list
  already *is* non-destructive procedural sculpting; the pipeline exit is
  bake-and-export.

Each of these narrows the ceiling on purpose. Texture painting is the one worth
revisiting deliberately, because it is the axis on which 3DCoat is strongest.

---

## Checking these claims

Every claycore claim above is verifiable in this repository:

| Claim | How to check |
|---|---|
| 16 combine ops, 20 deformers, 5 blends, 10 voxel verbs, 11 mesh verbs | `docs/07-brushes-and-features.md` — its coverage is asserted against the enums |
| A mesh brush never changes topology | `tests/unit/test_mesh_sculpt.cpp` compares `indices` and `quads` BYTE FOR BYTE after every verb, on a quad-exported mesh |
| Brush parity table | `examples/` — every verb has a runnable script with committed renders and self-checks |
| Exactness / Lipschitz is real | `tests/unit/test_*.cpp` measure the declared bound against the field's actual steepest slope |
| Backend parity | `tests/unit/test_parity.cpp`, and `clay parity-fixture` for host-side checking |
| Watertight meshing | `tests/unit/test_mesh.cpp`, and `clay validate` |
| Masking gates any operation | `tests/unit/test_masked_combine.cpp` — a gated boolean leaves the protected region exactly as it was, and marching by the declared step scale does not overshoot |
| The gaps above | `openspec/ROADMAP.md` — pending rows and the deferred list |

If a claim here stops being true, the test or gate that backs it should fail
first. Where that is not yet the case — the performance claim in Tier 3 — the
document says so rather than asserting it.
