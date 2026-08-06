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

## Where the engine is (2026-08-06, v0.15.0)

13 capabilities, 25 archived changes. Complete enough that the gaps below are
about *sculpting affordances*, not about the field engine:

- 28 primitives + stroke/curve chains, 14 combine ops, 5 blend profiles,
  grid/radial repetition, mirror with blended seam
- **9 deformers** — twist, bend, taper, displace, wrap_around, elongate,
  elongate_axis, bend_linear, bend_radial. Every point-warp implemented in the
  kernel headers is reachable from a document; there is nothing left stranded.
- Voxel engine: palette grids, cube/sphere brushes with 4 falloff curves and
  strength, sculpt verbs smooth / inflate / flatten / pinch, fills, mirrored
  edits, flood select, greedy meshing, SDF↔voxel bridges, paintable mask
  fields gating every verb
- Brush stroke engine: samples in, edit items out, with versioned presets;
  paintable per-layer mask fields; ghosted and locked layers
- Editing and opt-in undo over one command vocabulary shared with the file
  format; 217 capabilities gated for binding parity; four backends verified on
  device; the Swift package verified in the iOS Simulator

### Corrections to the study's baseline

The study was written against a slightly older tree. Three of its rows are now
wrong, in our favour:

| Study says | Actually |
|---|---|
| 17 archived changes | 20 |
| `bend_linear` / `bend_radial` exactness kernels "present but unused, fine to leave" (P3) | Both are implemented, tape-expressible and parity-checked as of 2026-08-05 |
| ABI enumerator `CLAY_DEFORM_WRAP` | `CLAY_DEFORM_WRAP_AROUND` |

## The gap, in one sentence

Every deformer acts on a whole item, so nothing can push on a *patch* of
surface — which is the first tool a sculptor reaches for, and the whole
Manipulation category in both ZBrush (Move, Snake Hook, Nudge) and 3DCoat
(Move, Pose) at once.

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
| `add-loft-opcode` | Loft is header-only and flagged in the specs as not tape-expressible; it is 3DCoat's base-mesh generator and the core of their 2026 parametric direction. Needs an item to carry two profiles. |
| `add-swept-n` | Generalizes loft from two profiles to N across a guide, once two-profile loft is proven. Minor and arguably implied by the row above, but named so it is not assumed done when loft lands. |
| `add-voxel-verbs` | fill-cavities, scrape (flatten+smooth), smudge, carve-with-alpha — the verbs our four are missing against their voxel set. |
| `add-voxel-repair` | Close holes and fill interior voids, so a voxel layer can be made airtight before meshing. Lower priority than it sounds: SDF layers are watertight by construction, and the mesh importer's winding-number sign tolerates small holes — this is only for voxel layers that were sculpted into a non-manifold state. Their "Close Invisible Holes + Fill Voids" is the standard pre-bake step. |
| `add-mesh-to-field-import` | Triangle mesh → field, via BVH distance and generalized winding number for sign. Unlocks scan cleanup, kitbashing, booleans on imported meshes. Density must be specified in voxels-per-unit, decoupled from object scale — their scale↔resolution entanglement is a documented pain. |

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
