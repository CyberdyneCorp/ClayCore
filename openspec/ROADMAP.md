# claycore roadmap

Where the engine is, what it is missing, and in what order the gaps are worth
closing. Derived from the 3DCoat feature study and the ZBrush brush-system
comparison, reduced to **what this repository owns** — app-side and
retopo/UV/bake items live in the ClaySpace and CyberRemesherAndUV repos and are
named here only where ClayCore has to provide something for them.

Living requirements are in `openspec/specs/`; this file is the plan, not the
contract. A row becomes real when it becomes a change in `openspec/changes/`.

Last reconciled against `3dcoat_study/MISSING_FEATURES.md` and
`3DCOAT_FEATURE_STUDY.md` on 2026-08-05, after a review from the study's authors
caught five items this file had dropped. Every ClayCore-owned row in their
catalogue is now represented here or in the deferred list below.

## Where the engine is (2026-08-06, v0.17.0)

14 capabilities, 29 archived changes. Complete enough that the gaps below are
about *sculpting affordances*, not about the field engine:

- 28 primitives + stroke/curve chains, 14 combine ops, 5 blend profiles,
  grid/radial repetition, mirror with blended seam
- **12 deformers** — twist, bend, taper, displace, wrap_around, elongate,
  elongate_axis, bend_linear, bend_radial, plus grab, pose and pose_line with
  finite support. Every point-warp implemented in the kernel headers is
  reachable from a document; there is nothing left stranded.
- Voxel engine: palette grids, cube/sphere brushes with 4 falloff curves and
  strength, sculpt verbs smooth / inflate / flatten / pinch, fills, mirrored
  edits, flood select, greedy meshing, SDF↔voxel bridges, paintable mask
  fields gating every verb
- Brush stroke engine: samples in, edit items out, with versioned presets;
  paintable per-layer mask fields; ghosted and locked layers
- Control-point curves (hard / Catmull-Rom / B-spline / Bezier, closed,
  tessellated to a document tolerance) and the cut tool (rect / circle /
  polygon / spline lasso, swept as a prism)
- Editing and opt-in undo over one command vocabulary shared with the file
  format; 228 capabilities gated for binding parity; four backends verified on
  device; the Swift package verified in the iOS Simulator

### Corrections to the study's baseline

The study was written against a slightly older tree. Three of its rows are now
wrong, in our favour:

| Study says | Actually |
|---|---|
| 17 archived changes | 27 |
| `bend_linear` / `bend_radial` exactness kernels "present but unused, fine to leave" (P3) | Both are implemented, tape-expressible and parity-checked as of 2026-08-05 |
| ABI enumerator `CLAY_DEFORM_WRAP` | `CLAY_DEFORM_WRAP_AROUND` |

## The gap, in one sentence

~~Every deformer acts on a whole item, so nothing can push on a *patch* of
surface.~~ **Closed 2026-08-05** by `add-region-deformers` and
`add-pose-line-regions`. The remaining gap is narrower and worth naming
precisely: **there is no way to relax an SDF surface.** Smoothing a signed
distance field locally means convolving it, which breaks the distance property
the evaluator depends on, so it is not a deformer — voxels have
`sculpt_smooth`, SDF layers have nothing, and the only route today is the
one-way voxel bridge. See `add-sdf-relax` in Phase 2.

## Phase 1 — make it sculptable

The smallest set that turns a modelling kernel into something an artist can
sculpt with.

| Change | Why it is first |
|---|---|
| ~~`add-region-deformers`~~ **landed 2026-08-05** | Grab and pose with finite support, on SDF and voxels. Shipped with **sphere (radial) region weights only**. |
| ~~`add-pose-line-regions`~~ **landed 2026-08-05** | Pose now covers two of the study's three region sources; only the mask reference is left, and it waits on `add-mask-field`. Originally: 3DCoat's Pose *defaults* to a line gradient with 15°-snapped angle — the taper workflow the hard-surface videos lean on — and the study's own draft requirement names it alongside sphere and mask. It needs anchor + end + axis + angle = 10 parameters against the deformer record's 9 slots, so it widens the extension array; contained, but a real change rather than an add. |
| ~~`add-mask-field`~~ **landed 2026-08-05** | Paintable per-layer scalar gating edit strength — freeze works. Representation-independence is enforced structurally rather than tested for: the mask is addressed in world units on its own lattice, so a resolution change cannot misalign it, and it lives beside voxel content rather than in the evaluated document, so its presence cannot change what a document evaluates to. **Scope note:** masking gates edits where they are *authored*. Voxel edits consume it per cell; SDF edits are declarative items with no per-point strength, so they consume it when a stroke becomes items — which is `add-brush-stroke-engine` below. Masked shell and mask-referenced pose regions are follow-ups now that the query exists. |
| ~~`add-brush-stroke-engine`~~ **landed 2026-08-05** | Stroke → spaced stamps: arc-length spacing, pressure curves, deterministic jitter, rotate-along-stroke, taper, steady stroke, buildup vs clamped. The interface is fixed as "stroke samples in, edit items out" — resolution is pure and a stamp becomes an ordinary voxel brush application or an ordinary edit-list node, so undo, coalescing, serialization and picking apply to a stroked edit unchanged. Presets carry a schema version from the first release; a newer one is refused rather than reinterpreted. This is also where a mask reaches SDF edits: a stamp in a frozen region emits no item. Image-based alpha stamps wait on a texture pipeline; alpha is a scalar along the stroke so one can be added without redesign. |
| ~~`add-layer-ghost-lock`~~ **landed 2026-08-05** | Per-layer ghost (visible, unpickable, edit-excluded) and lock (visible and pickable, edit-excluded), both undoable and serialized. An edit naming a protected layer is refused with a typed error rather than silently dropped — a host that greys the layer out wants to know, and one that does not must not discard the artist's work. Neither flag changes what a document evaluates to; how a host *draws* a ghost is its own business. |

**Gate:** an artist can block out a form, grab it into shape, freeze a region
and detail around it, without leaving the engine's vocabulary. **Met
2026-08-05** — every row above has landed. Phase 2 is next, and the largest
structural gap in it was `add-curve-objects`, which landed 2026-08-06.

## Phase 2 — depth and breadth

| Change | Notes |
|---|---|
| ~~`add-cut-tool`~~ **landed 2026-08-06** | The study's P0 and the practitioners' "90% tool" (ZBrush Trim Rect/Circle/Lasso, 3DCoat Cut Off). A frame plus a drawn shape resolves to an ordinary extruded item. **The cut is a prism, not a frustum** — a converging cut has a non-flat face and a result that depends on where the camera stood. **No camera enters the engine**: the caller passes the frame it already has, in world units, and the engine owns the error-prone parts (sweep depth, orientation, which side survives). Keep-inner/keep-outer is the op, not a flag. Angled cut walls are the one named gap: they need a taper about the sweep axis, the same relationship `elongate_axis` has to `elongate`. |
| ~~`add-curve-objects`~~ **landed 2026-08-06** | Control-point curves: per-point type (hard / Catmull-Rom / B-spline / Bezier with local-space handles), closed curves, and adaptive tessellation to a document-level tolerance. **A curve is not a new primitive** — the stroke opcode already sweeps a sphere along a segment chain exactly and with finite support, so typed points lower into it at compile time. That bought four backends, culling, exactness, picking, undo, masks and the file format for nothing, and an all-hard chain compiles to a bit-identical tape. Also added the versioned scene chunk, so the next field a node gains needs no packing trick. Cross-section sweeps (`add-loft-opcode`, `add-swept-n`) and radius profiles are unblocked but deliberately not included. |
| `add-sdf-relax` | **The one ZBrush core brush still missing.** Voxels smooth; SDF layers cannot. Not a deformer: convolving a distance field breaks the distance property. Three possible routes and an order-of-magnitude size difference between them — see the plan below. |
| `add-sampled-fields` | **Prerequisite for three rows below, and it does not exist in any form today.** Build a field from sampled data and let the tape evaluate it. See the plan below. |
| `add-loft-opcode` | Loft is header-only and flagged in the specs as not tape-expressible; it is 3DCoat's base-mesh generator and the core of their 2026 parametric direction. Needs an item to carry two profiles. |
| `add-swept-n` | Generalizes loft from two profiles to N across a guide, once two-profile loft is proven. Minor and arguably implied by the row above, but named so it is not assumed done when loft lands. |
| `add-voxel-verbs` | fill-cavities, scrape (flatten+smooth), smudge, carve-with-alpha — the verbs our four are missing against their voxel set. |
| `add-voxel-repair` | Close holes and fill interior voids, so a voxel layer can be made airtight before meshing. Lower priority than it sounds: SDF layers are watertight by construction, and the mesh importer's winding-number sign tolerates small holes — this is only for voxel layers that were sculpted into a non-manifold state. Their "Close Invisible Holes + Fill Voids" is the standard pre-bake step. |
| `add-mesh-to-field-import` | Triangle mesh → field, via BVH distance and generalized winding number for sign. Unlocks scan cleanup, kitbashing, booleans on imported meshes. Density must be specified in voxels-per-unit, decoupled from object scale — their scale↔resolution entanglement is a documented pain. |

## Phase 2 — the plan for what is left

Six rows, in two tracks. Written 2026-08-06 after checking the tree rather
than the table: two things are true that the one-line summaries hid.

**Finding 1: three rows share a prerequisite that does not exist.** There is no
way to build a field from sampled data. `sample_step_field` returns
-voxel_size/2 or +voxel_size/2 — a bound, not a distance — and nothing in the
tree does a distance transform (checked: no eikonal, no fast march, no
redistancing anywhere). The brick cache samples an existing tape; it does not
build a field from samples. So `add-mesh-to-field-import` cannot produce a
layer, `add-sdf-relax` has no route through voxels, and `add-voxel-repair`'s
output cannot become an SDF. That prerequisite is `add-sampled-fields` below.

**Finding 2: a sampled field is a tape problem, not a data problem.** Bulk data
already reaches every backend — `tape.blob` is a `device const float*` on
Metal and its equivalents elsewhere, and stroke points and polygon vertices
already ride in it. The obstacle is that **the tape is recompiled on every
edit**, so a 256³ fp16 volume in the blob means re-uploading 32 MB per
brushstroke. Sampled volumes therefore have to live outside the tape and be
referenced by handle, uploaded once and cached — which means the tape needs a
notion of external resources it does not have today. That is the real cost of
the mesh-import row, and it is invisible in "BVH + winding number".

### Track A — ready now, no prerequisites

**Both voxel rows landed 2026-08-06.** The plan guessed they shared "a
connected-component pass"; writing them showed the shared operation is a
**pocket-fill rule**, and that the obvious implementation — morphological
closing — is wrong for both. A ball of radius r fits *into* a dent wider than
r, so a larger structuring element fills less rather than more; and a closing
cannot seal a one-cell perforation in a one-cell wall at all, because the
erosion reaches through from the void behind it. The rule that works is that an
empty cell with at least four of its six face neighbours occupied is inside a
pocket. Only fill-voids needed a flood, and it needed one over *empty* cells
from outside, which the engine did not have.

| Change | What will bite |
|---|---|
| ~~`add-voxel-verbs`~~ **landed 2026-08-06** | fill-cavities, scrape, smudge, carve-with-alpha. Carve takes a **caller-supplied scalar grid** — a host with an alpha has already loaded the PNG, so the engine decodes no images. Scrape flattens and smooths from **one** snapshot, because two calls would let the flatten's output feed the smooth's neighbourhood. |
| ~~`add-voxel-repair`~~ **landed 2026-08-06** | Report (non-destructive), close holes, fill voids, all mask-gated. Enclosure is decided by a flood over **empty** cells from outside the bounds — `flood_select` walks occupied cells from a seed, which is a different question. |
| ~~`add-loft-opcode`~~ **landed 2026-08-06** | N profiles along Z, not two — nothing about the opcode wanted the limit, and taking N now means the guide row changes where profiles are *placed* rather than how they are *stored*. Three or more are bracketed, so wide-narrow-wide gives a waist. The Lipschitz warning was real: a loft's safe step scale falls from 0.53 to 0.10 as the depth shrinks, and the example fails if that ordering stops holding. `cop_extrude_to` became `cop_loft`, taking the interpolation parameter instead of deriving it, because a signature that derived it could only ever serve exactly two. |
| `add-swept-n` | **Re-scoped by building loft.** It is not "the same opcode with a count" — loft already takes N. The distinguishing feature is the word the roadmap always used: profiles across a **guide**. That means transporting a frame along a curve and a closest-point search per sample, which is a domain warp with its own Lipschitz story (the warp compresses on the inside of a bend, so it is unbounded as the profile extent approaches the radius of curvature). The storage question is settled; the geometry one is not. |

### Track B — gated on a prerequisite

| Change | What will bite |
|---|---|
| `add-sampled-fields` **(new row)** | Build a narrow-band signed distance field from an inside/outside + closest-point oracle, and make it layer content the tape can evaluate. Three hard parts, none of them the distance transform: **(1)** external resources — a volume referenced by handle, uploaded once, not rebuilt per edit (see Finding 2); **(2)** exactness — a sampled, interpolated field is neither exact nor Lipschitz-1, and it must declare that through `CFieldInfo` or `safe_step_scale` will overstep and the raymarcher will miss surfaces; **(3)** the file format — a volume is orders of magnitude larger than anything `.clayspace` carries today, so it needs its own chunk and a compression story. |
| `add-mesh-to-field-import` | Triangle mesh → field. Loading is already done (OBJ, PLY, FBX all import today), so the work is a BVH for closest-distance, a generalized winding number for sign, and then `add-sampled-fields` to hold the result. Density in voxels-per-unit, decoupled from object scale — their scale↔resolution entanglement is a documented pain. |
| `add-sdf-relax` | The last ZBrush core brush. **The design question is open and should be settled before anything is written.** Three routes: (a) round-trip through voxels, which needs `add-sampled-fields` and gives a bake, not a live edit; (b) a field-space local re-blend, which needs no prerequisite but changes what an edit list means; (c) mesh-space extract/smooth/re-import, which needs the same prerequisite as (a) plus meshing round-trip cost. My reading is that (a) is the honest one and that the cheapest version of this row is **making the voxel round trip lossless rather than inventing a field operator** — which is `add-sampled-fields` again. Settle this first; the row's size varies by an order of magnitude across the three answers. |

### Order, and why

1. ~~**`add-voxel-verbs` + `add-voxel-repair`**~~ **done 2026-08-06.** They did
   share an operation, though not the connected-component pass predicted here
   — see Track A above.
2. **`add-loft-opcode` → `add-swept-n`** — independent of Track B, and loft is
   the one row whose blocker ("an item can carry two profiles") is already
   written down as a sentence in the spec.
3. **`add-sampled-fields` → `add-mesh-to-field-import` → `add-sdf-relax`** —
   the prerequisite designed with all three consumers in view rather than
   retrofitted around the first one.

**If kitbashing or scan cleanup is the near-term product need, Track B jumps
the queue and Track A waits.** That is the only reason to reorder, and it is a
product call, not an engineering one.

### What "done" means for every row

The gallery is how these get inspected, so each row ships an example, not only
tests. Concretely, per row:

- **C++ tests** covering every feature the spec delta names, including the
  refusal cases — a feature with no test does not count as shipped.
- **Both bindings**, with the parity gate green. It already fails a pyclay
  capability with no C entry point.
- **A numbered example** in `examples/`, run by `examples/run_all.py` in CI, that
  **renders what it does and asserts what it claims** — the existing ones raise
  `SystemExit` when their own claim stops holding, and that is what makes them
  tests rather than screenshots. Phase 2 takes the gallery from 15 to 21.
- **Swift smoke coverage** wherever the C ABI grows, run on macOS and in the
  simulator.
- **Four presets green** (release, metal, opencl, asan-ubsan) plus
  `release_check`.

One gate is still missing. The gallery guards that every *primitive* class has
an example (`01_primitives.py`, which caught `Cut` shipping without one), but
nothing guards that every *capability* does. The same mechanism extends — a
named table, so an uncovered capability is an error and an exemption is a
decision on the record.

**This was supposed to land with the first of these rows and did not.** It is
outstanding as of the voxel pair, and it is the one item in this section that
has been stated and not delivered.

## Phase 3 — the pipeline

| Change | Notes |
|---|---|
| `add-claycore-bridge` (ClayCore half) | A retopo-oriented mesh export profile, plus a field-evaluation callback so a baker can sample exact normals and AO from the field rather than raycasting a mesh. The other half lives in CyberRemesherAndUV. This is the product story, and neither engine has the seam yet. |

## Phase 4 — parametric and scatter

`add-lattice-deformer` (FFD) · `add-surface-scatter` (instances sampled on an
isosurface) · `add-blend-profile-curves` (user-defined bevel cross-section,
against their 2026 custom profiles) · `add-convenience-transforms`
(snap-to-ground, centre-mass, zero-to-origin as single ABI calls) ·
`add-field-stamps` (capture a region's field as a reusable brush — the VDM
analog, and a differentiator rather than a parity item).

## Deferred, but recorded

Not scheduled, and not rejected either — small enough to slot in when something
needs them, and listed so they are not mistaken for oversights:

- **Procedural noise as a tape opcode.** `displace` is by-callable today, which
  is not portable across backends. A tape-expressible 3D noise field is the
  answer if node-style procedural detail ever becomes a goal.
- **Voxel layers beyond 256³.** The spec guarantees ≥256³ per layer, with a
  memory budget and typed errors past it. There is no streaming story for
  scenes larger than that; per-layer grids have been sufficient so far.
## Deliberately not doing

Recorded so they are decisions rather than oversights:

- **Mesh surface-mode sculpting** (their LiveClay, ZBrush's 36 surface brushes).
  An SDF sidesteps topology entirely; competing on dynamic tessellation is not
  this engine's fight.
- **Subdivision multires.** Resolution is an evaluation parameter here, so the
  whole Res+/Resample/multires apparatus has nothing to attach to.
- **Node-graph texturing UI.** The edit list already *is* non-destructive
  procedural sculpting; the research corpus ruled the node-graph out as primary
  UX.
- **Text, SVG-to-shape, logo-to-volume.** Glyph and SVG tessellation belong
  upstream; polygon profiles already accept the output.
- **Cloth, tree and muscle generators, VR, texture painting.** Out of scope;
  the pipeline exit is bake-and-export.
- **Scripted brushes.** Decided against 2026-08-05: no embedded scripting
  runtime for authoring brushes. Nothing is lost by the decision — the brush
  engine's interface above emits ordinary edit items for its own reasons, so a
  future reversal would extend that boundary rather than redesign it.

## Requirements taken from their bugs

Worth writing into the specs they touch, because a competitor's known failure is
a free test case:

- masks survive resolution changes and representation bridges
- hidden is not deleted, and hidden state survives resampling
- import density is decoupled from object scale
- brushes never touch ghosted or locked layers
- symmetry centre is explicit, persistent and gizmo-edited, never implicit
- presets survive engine versioning (versioned schema)
- every destructive operation is preview-committed and undoable, including hide

Two of these the architecture already gives us for free and should be stated
rather than assumed: smoothing cannot act across a gap, because blends are rigid
and local; and boolean results are watertight by construction.
