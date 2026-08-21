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
layers, alphas landed on SDF layers, and masking now protects against any
operation rather than only against brushes; sculpt layers on SDF layers and the
asset-finishing pipeline have not.
SDF layers and the asset-finishing pipeline have not.

---

## Axis by axis

| Axis | claycore | ZBrush | 3DCoat | Blender |
|---|---|---|---|---|
| **Representation** | SDF edit list + sparse voxel grids | Multires mesh (+ DynaMesh) | Voxel + surface mesh | Multires mesh |
| **Non-destructive history** | **The document *is* the history.** Every edit stays re-editable forever | Layers record passes; the mesh itself is baked | Largely destructive past a bake | Modifier stack, but sculpt strokes are destructive |
| **Topology management** | **Not a concept** — an SDF has none, and a mesh layer's is PRESERVED rather than managed: the mesh brushes move vertices and never touch the index buffer | DynaMesh, ZRemesher | Auto-retopo, its strength | Dyntopo, Remesh |
| **Field correctness** | **Exactness + Lipschitz tracked per node**, so step size is derived, not tuned | n/a — mesh | n/a — mixed | n/a — mesh |
| **Booleans** | **Watertight by construction**, 2-manifold meshing | Live Boolean, then remesh | Voxel booleans, robust | BMesh booleans, fragile on bad input |
| **Brush vocabulary** | Core set complete on fields and voxels, plus 16 fixed-topology verbs (14 that move vertices, 2 that write colour) and a lattice cage on a mesh layer (see below) | The reference: ~36 surface brushes plus the core | Broad, voxel + surface modes | Solid core set |
| **Masking** | **Protects the surface from any op**, on either representation — a gated item does not act where the mask protects | First class, protects the surface from *any* op | First class | First class |
| **Sculpt layers** | **On voxel layers.** A pass is bracketed and its changed cells recorded, so its strength stays adjustable long after the strokes are finished; SDF layers do not have them yet | Headline feature | Present | Present |
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
| Crease, DamStandard | `Op::Incise` | ✅ |
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
| Deformation palette (Taper, Twist, Bend, Flatten, Inflate, Noise) | 21 `Deformer`s — `taper`, `twist` (+`twist_range`), `bend` (+`bend_range`, `bend_linear`, `bend_radial`, `bend_curve`), `elongate`, `wrap_around`, `magnify`, `noise`, `displace`, `blob`, `alpha`, `grab`, `pose`, `pose_line`, `lattice` | 🟡 **SDF items only.** They are inverse point maps on an item's local space, so they compose and stay non-destructive — but a MESH layer takes a lattice cage instead (`clay_mesh_lattice_*`), and a VOXEL layer takes none. Deforming a mesh by taper/twist today means converting it to an SDF item first, which resamples |
| Blob | `blob` | ✅ noise under a brush region |
| Pulling a lobe out | `brush::snakehook` | ✅ the verb for growing form; Move is the verb for nudging it |
| Morph | — | ⬜ needs a stored morph target; unblocked on voxels now that layers exist, still absent on SDF |
| Layers | `VoxelGrid::begin_sculpt_layer` | 🟡 voxel layers only — an SDF pass is a weighted group, which waits on `expose-scene-groups` |
| Alphas | `sculpt_carve_alpha` (voxel), `Deformer::alpha` (SDF) | ✅ both, scalar stamps |
| Masking | mask fields, `Node::gate` | ✅ gates any operation on either representation — a boolean included; the gate is a measured DISTANCE, so its cost follows a width you set |
| Slice / Knife | — | ❌ polygroup splits need two items; no single-solid equivalent |
| Surface brushes on a MESH (Standard, Move, Inflate, Smooth, Pinch, Flatten, Clay, DamStandard, Trim Dynamic, hPolish, SnakeHook, Layer, Nudge, Relax) | `mesh::MeshSculptor`, **14 verbs, with alphas** | ✅ on a mesh LAYER's own triangles, with topology fixed — see below |
| Polypaint / Smear on a MESH | `mesh::MeshSculptor` `paint`, `smear` | ✅ the only two verbs that move no vertex; they refuse a mesh with no colour attribute rather than creating one |
| Elastic (Blender), ZProject | — | 🟡 Elastic was filed "does not survive the representation change", which was true for fields and is no longer true on a mesh layer. Undecided rather than rejected; it is the one entry that is new *math* rather than a new composition of the eleven |
| Dyntopo, LiveClay, multires | — | ❌ deliberate: an SDF sidesteps topology, and dynamic tessellation is not this engine's fight |

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
and it is where this stops on purpose.

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
| **Sculpt layers / morph targets** | **On voxel layers**; not on SDF layers | A layer that records a pass and replays it at an intensity is a *document* concept, not a brush. The voxel side has it: `begin_sculpt_layer`/`end_sculpt_layer` bracket a pass, and strength, visibility, reordering and merge-down follow (`examples/52_sculpt_layers.py`). What a **fraction** means differs from ZBrush by representation — ZBrush interpolates vertex offsets, and binary occupancy has nothing to interpolate, so a fractional strength is a reproducible fraction of the *cells*, dithered against the same cell-coordinate hash the falloff brushes use. On an SDF layer the equivalent is a weighted group rather than a diff, which waits on `expose-scene-groups`. Morph stays "not planned" for the SDF side only. |
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

- **The pipeline seam.** `add-claycore-bridge` (retopo/UV/bake) has not started;
  UV and baking live in a sibling repository and neither engine owns the seam
  yet. Without it you can sculpt but not ship.
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
  and that is now a declared non-goal for painting rather than a gap: material
  authoring wants a UV parameterisation and a texture set, UVs live in
  CyberRemesherAndUV, and `add-claycore-bridge` is where a baker's
  field-sampling callback belongs. 3DCoat's moat is the texture pipeline, not
  the colour channel.

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

- **Topology-CHANGING mesh sculpting** — dyntopo, multires, remeshing,
  subdivision (3DCoat's LiveClay, ZBrush's dynamic tessellation). An SDF
  sidesteps topology entirely; competing on dynamic tessellation is not this
  engine's fight.

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
- **Subdivision multires on the SDF side.** Resolution is an evaluation
  parameter for an SDF layer, so the Res+/Resample apparatus has nothing to
  attach to there. Voxel layers now DO carry a level stack — level 0 coarsest,
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
