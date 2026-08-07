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
workflow tier absent."* The brushes landed; masking-as-protection, sculpt
layers, alphas on SDF layers and the asset-finishing pipeline have not.

---

## Axis by axis

| Axis | claycore | ZBrush | 3DCoat | Blender |
|---|---|---|---|---|
| **Representation** | SDF edit list + sparse voxel grids | Multires mesh (+ DynaMesh) | Voxel + surface mesh | Multires mesh |
| **Non-destructive history** | **The document *is* the history.** Every edit stays re-editable forever | Layers record passes; the mesh itself is baked | Largely destructive past a bake | Modifier stack, but sculpt strokes are destructive |
| **Topology management** | **Not a concept** — an SDF has none | DynaMesh, ZRemesher | Auto-retopo, its strength | Dyntopo, Remesh |
| **Field correctness** | **Exactness + Lipschitz tracked per node**, so step size is derived, not tuned | n/a — mesh | n/a — mixed | n/a — mesh |
| **Booleans** | **Watertight by construction**, 2-manifold meshing | Live Boolean, then remesh | Voxel booleans, robust | BMesh booleans, fragile on bad input |
| **Brush vocabulary** | Core set complete (see below) | The reference: ~36 surface brushes plus the core | Broad, voxel + surface modes | Solid core set |
| **Masking** | **Weak** — voxel-scoped, reaches SDF only via stroke stamps | First class, protects the surface from *any* op | First class | First class |
| **Sculpt layers** | **Absent** | Headline feature | Present | Present |
| **Alphas / stamps** | Voxel only | Deep, VDM support | Deep | Present |
| **Surface colour** | Per-item colour + `Paint` regions; vertex colours at mesh time | Polypaint | **PBR texture painting — its moat** | Vertex paint + texture paint |
| **Scale** | ≥256³ per voxel layer, no streaming; SDF edit lists degrade step scale as they grow | Tens of millions of polys | Very large voxel scenes | Large, memory-bound |
| **Embeddable** | **Yes — C ABI, SwiftPM, Python, headless** | No | No | No (as a library) |
| **GPU portability** | **One kernel source → CPU / Metal / CUDA / OpenCL, parity-gated** | Proprietary CPU-centric | GPU-assisted | GPU sculpt, single path |

---

## Brush-level parity

The core sculpting verbs, and where claycore stands on each. Per-verb detail is
in [`07-brushes-and-features.md`](07-brushes-and-features.md).

| Brush (ZBrush name) | claycore | Status |
|---|---|---|
| Standard, ClayBuildup | `Op::Relief` | ✅ |
| Crease, DamStandard | `Op::Incise` | ✅ |
| Inflate | `Op::Relief`, `sculpt_inflate` | ✅ |
| Move | `brush::move_brush` | 🟡 drags the assembled surface, but **buds rather than stretches** on a large pull, and a stroke's drags compound the step scale — see below |
| Rotate | `pose`, `pose_line` | ✅ |
| Pinch / Magnify | `magnify` (signed), `sculpt_pinch` / `sculpt_magnify` | ✅ one deformation, one sign |
| Smooth | `field::relax`, `sculpt_smooth` | ✅ bakes on the SDF side |
| Flatten | `field::flatten`, `sculpt_flatten` | ✅ region required on the SDF side |
| Trim / Clip | `cut::cut_item` | ✅ as a solid, Clip *is* Trim |
| SnakeHook | `brush::snakehook` | ✅ adds material rather than pulling it |
| Surface Noise | `noise` deformer | ✅ integer hash, so all backends agree |
| Blob | — | ⬜ `add-blob-brush`, unblocked |
| Pulling a lobe out | `brush::snakehook` | ✅ the verb for growing form; Move is the verb for nudging it |
| Morph | — | ⬜ needs a stored morph target — a *document* concept |
| Layers | — | ⬜ the same missing concept |
| Alphas | `sculpt_carve_alpha` | 🟡 voxel only |
| Masking | mask fields | 🟡 voxel-scoped; see below |
| Elastic, ZProject | — | ❌ mesh-era ideas that do not survive the representation change |
| Slice / Knife | — | ❌ polygroup splits need two items; no single-solid equivalent |
| The ~36 surface brushes | — | ❌ deliberate: an SDF sidesteps topology, and dynamic tessellation is not this engine's fight |

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
| **Masking as a field concept** | Masks are stored beside *voxel* content and reach SDF edits only through the stroke engine, where a masked stamp is dropped or attenuated | ZBrush masking protects the **surface** from *any* operation. Nothing here gates an arbitrary op, or protects an existing surface from the next boolean. The single biggest missing concept. |
| **Sculpt layers / morph targets** | Absent | A layer that records a pass and replays it at an intensity is a *document* concept, not a brush. Its absence is why Morph is filed "not planned". |
| **Alphas on SDF layers** | `sculpt_carve_alpha` is voxel only | Detail work in all three tools is alpha-driven. |

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
to **consolidate** — bake the chain into a volume with
`clay_item_volume_from_document` — rather than keep appending. There is no policy
for that today, which is the Tier 3 consolidation gap seen from the other side.

The verb for *growing* form is `snakehook`, which sweeps a tapered item along the
drag: it reaches as far as the drag goes and the field stays exact (step scale
1.0 with three lobes on it, against 0.05 for the Move version).
`examples/27_move_strokes.py` builds both and measures them.

### Tier 2 — finishing an asset

- **The pipeline seam.** `add-claycore-bridge` (retopo/UV/bake) has not started;
  UV and baking live in a sibling repository and neither engine owns the seam
  yet. Without it you can sculpt but not ship.
- **Surface colour authoring.** Colour is per-item plus `Paint` regions, with
  vertex colours derived at mesh time. There is no polypaint and no PBR
  painting. This is 3DCoat's moat, and deferring it caps the ceiling — worth
  being deliberate about rather than drifting into.

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

- **Mesh surface-mode sculpting** (ZBrush's surface brushes, 3DCoat's LiveClay).
  An SDF sidesteps topology entirely; competing on dynamic tessellation is not
  this engine's fight.
- **Subdivision multires.** Resolution is an evaluation parameter here, so the
  Res+/Resample apparatus has nothing to attach to.
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
| 16 combine ops, 14 deformers, 5 blends, 10 voxel verbs | `docs/07-brushes-and-features.md` — its coverage is asserted against the enums |
| Brush parity table | `examples/` — every verb has a runnable script with committed renders and self-checks |
| Exactness / Lipschitz is real | `tests/unit/test_*.cpp` measure the declared bound against the field's actual steepest slope |
| Backend parity | `tests/unit/test_parity.cpp`, and `clay parity-fixture` for host-side checking |
| Watertight meshing | `tests/unit/test_mesh.cpp`, and `clay validate` |
| Masking is voxel-scoped | `openspec/specs/scene-model/spec.md`, "A layer may carry a mask" |
| The gaps above | `openspec/ROADMAP.md` — pending rows and the deferred list |

If a claim here stops being true, the test or gate that backs it should fail
first. Where that is not yet the case — the performance claim in Tier 3 — the
document says so rather than asserting it.
