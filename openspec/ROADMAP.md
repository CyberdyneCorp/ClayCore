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

## Where the engine is (2026-08-06, v0.19.0)

14 capabilities, 31 archived changes. Complete enough that the gaps below are
about *sculpting affordances*, not about the field engine:

- 30 primitives + stroke/curve chains, 14 combine ops, 5 blend profiles,
  grid/radial repetition, mirror with blended seam. Every kernel capability is
  reachable from a document — loft was the last one that was not.
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
`add-pose-line-regions`. ~~The remaining gap is narrower and worth naming
precisely: there is no way to relax an SDF surface.~~ **Closed 2026-08-06** by
`add-sdf-relax`. The reasoning here was half wrong and worth keeping for that:
convolving a distance field does break exactness, but it cannot raise the
Lipschitz bound, and a 1-Lipschitz field is automatically a conservative bound
on the distance to its own zero set — so the evaluator stays correct. Relax is
still not a deformer; it bakes to a sampled volume.

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
| ~~`add-sdf-relax`~~ **landed 2026-08-06** | The last ZBrush core brush. Settled on the sampled route: a field-space re-blend would have made an edit list mean "shapes plus a rule about how they interact" and still could not smooth a bump inside one item. The roadmap's worry was half right — convolution destroys exactness but cannot raise the Lipschitz bound, and a 1-Lipschitz field is automatically a conservative bound on the distance to its own zero set, so tracing stays safe. Relax **bakes**, which is stated everywhere a caller looks. |
| ~~`add-sampled-fields`~~ **landed 2026-08-06** | Sparse narrow-band volumes as a tape primitive. The plan's estimate that this needed a resource mechanism was arithmetic on a DENSE grid and 20x too pessimistic; a narrow band is O(area) and rides in the blob. |
| ~~`add-loft-opcode`~~ **landed 2026-08-06** | Took N profiles rather than two — nothing about the opcode wanted the limit. Loft is header-only and flagged in the specs as not tape-expressible; it is 3DCoat's base-mesh generator and the core of their 2026 parametric direction. Needs an item to carry two profiles. |
| ~~`add-swept-n`~~ **landed 2026-08-06** | Turned out to be its own row about GUIDES, not "the same opcode with a count", because loft already took N. Generalizes loft from two profiles to N across a guide, once two-profile loft is proven. Minor and arguably implied by the row above, but named so it is not assumed done when loft lands. |
| ~~`add-voxel-verbs`~~ **landed 2026-08-06** | fill-cavities, scrape (flatten+smooth), smudge, carve-with-alpha — the verbs our four are missing against their voxel set. |
| ~~`add-voxel-repair`~~ **landed 2026-08-06** | Close holes and fill interior voids, so a voxel layer can be made airtight before meshing. Lower priority than it sounds: SDF layers are watertight by construction, and the mesh importer's winding-number sign tolerates small holes — this is only for voxel layers that were sculpted into a non-manifold state. Their "Close Invisible Holes + Fill Voids" is the standard pre-bake step. |
| ~~`add-mesh-to-field-import`~~ **landed 2026-08-06** | Triangle mesh → field, by BVH distance and generalized winding number for sign. Neither binding could LOAD a mesh, only save one, so the import had nothing to import until this row added it. |
| `add-tape-abi-export` | The compiled tape (instrs / params / blob) across the C ABI, so a host can upload a **live** document to its own GPU. `add-host-kernel-package` did the hard half — the headers ship, so the host-side evaluator is `ctape_eval` compiled from our own source, and the parity fixture already proves it agrees. What is left is three buffers and their lifetime across the boundary. Blocks WYSIWYG preview-vs-bake for any host that draws its own frames. |

## Phase 2 — the plan for what is left

Seven rows, in two tracks. Written 2026-08-06 after checking the tree rather
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

### Track A — complete 2026-08-06

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
| ~~`add-swept-n`~~ **landed 2026-08-06** | Profiles carried along a guide, with **parallel-transported** frames computed when the item compiles — a Frenet frame flips at an inflection and is undefined where the guide is straight, and transport is sequential so it cannot be per-sample. Profiles distribute by arc length; the ends are the profile itself, flat, because a profile need not be a circle. The Lipschitz has two terms — curvature `R/(R-r)` **and** the profile lerp — and leaving the second out made a straight tapering sweep report Lipschitz 1, the exact defect the spec warns against. Curvature is estimated by circumradius, not turn-angle-over-arc, which is fooled by tessellation density. Closed guides are out: transport around a loop does not close the seam, and that is refused rather than ignored. |
| `add-tape-abi-export` **(new row)** | Three buffers across the C ABI, and no new math: `add-host-kernel-package` shipped the headers, so the host-side evaluator is `ctape_eval` compiled from our own source and the parity fixture already proves it agrees. What will bite is **lifetime, not content**. The tape is recompiled on every edit (Finding 2 above), so the boundary has to say who owns the buffers and when a handle a host is mid-upload with goes stale — an opaque handle with an explicit release, not a pointer into a `std::vector` that the next edit reallocates. Second: a host that uploads instrs/params/blob must also get `safe_step_scale` and the bounds, or its raymarcher oversteps a tape ours would have stepped conservatively. |

### Track B — gated on a prerequisite

**All three landed 2026-08-06.** The dependency chain held: sampled fields
first, designed with all three consumers in view, then mesh import, then relax.

| Change | What actually bit |
|---|---|
| ~~`add-sampled-fields`~~ | The plan's three "hard parts" were mostly not the hard parts. **(1)** No resource mechanism was needed — that estimate assumed dense storage and was 20x too pessimistic; a narrow band is O(area), and the blob is uploaded per eval call rather than per edit. **(3)** No compression story either, for the same reason. **(2)** was real and then some: the exactness contract divides by *where the samples are*, not by the band, because a brick is kept whole and holds samples well beyond it; the interpolant reaches sqrt(3) so declaring Lipschitz 1 would overstep; and two defects were invisible to tests that probed `eval` at points and only appeared on **rendering** it — a flat far-field bound that made the marcher crawl until it ran out of iterations, and a box distance that fell to zero on the sampled box's face so every ray hit an invisible shell. |
| ~~`add-mesh-to-field-import`~~ | The distance was the easy half, as predicted. The sign was the row: parity breaks on one hole, the closest-triangle pseudonormal is meaningless near an opening, so the generalized winding number with per-node dipole summarization. What the plan missed is that **neither binding could load a mesh** — both could only save one — so the import had nothing to import. Also: the Swift smoke consumes the prebuilt xcframework rather than the working tree, so it had been passing against a stale one. |
| ~~`add-sdf-relax`~~ | The design question, settled in the proposal, was most of the row. The implementation bit twice on the same misconception in different clothes: a volume's value where it has **no samples is a bound, not a measurement**, so neither smoothing through `eval()` nor averaging bounds in as though they were data is sound. Relax rewrites the stored samples in place. |

### Order, and why

1. ~~**`add-voxel-verbs` + `add-voxel-repair`**~~ **done 2026-08-06.** They did
   share an operation, though not the connected-component pass predicted here
   — see Track A above.
2. ~~**`add-loft-opcode` → `add-swept-n`**~~ **done 2026-08-06.** Loft took N
   profiles rather than two, which turned swept-N from "the same opcode with a
   count" into its own row about guides — see Track A above.
3. ~~**`add-sampled-fields` → `add-mesh-to-field-import` → `add-sdf-relax`**~~
   **done 2026-08-06.** The prerequisite was designed with all three consumers
   in view, and it held: neither later row needed it changed.
4. **`add-tape-abi-export`** — the one row of this phase still open, and freely
   orderable because it adds no math. Worth pulling forward the moment a host
   wants WYSIWYG preview-vs-bake, since the half that used to make it expensive
   — a host-side evaluator, and a way to trust it — landed with
   `add-host-kernel-package`.

**Every Phase 2 row except `add-tape-abi-export` is complete.** The gallery went
from 15 examples to 21, which is what this section said done would look like.

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
  tests rather than screenshots. Phase 2 takes the gallery from 15 to 22.
- **Swift smoke coverage** wherever the C ABI grows, run on macOS and in the
  simulator.
- **Four presets green** (release, metal, opencl, asan-ubsan) plus
  `release_check`.

One gate is still missing. The gallery guards that every *primitive* class has
an example (`01_primitives.py`, which caught `Cut` shipping without one), but
nothing guards that every *capability* does. The same mechanism extends — a
named table, so an uncovered capability is an error and an exemption is a
decision on the record.

~~**This was supposed to land with the first of these rows and did not.**~~
**Delivered 2026-08-06** with `add-sampled-fields`: `CAPABILITY_EXAMPLES` in
`examples/run_all.py` names an example for every living capability, so an
uncovered one is an error and an exemption is a decision on the record.

## Sculpting verbs still missing on SDF layers

`add-sdf-relax` closed the first of these; the pattern it established — sample,
rewrite the stored samples under a region, hand back a volume — makes the rest
tractable, and each one is a row rather than a project.

| Change | Notes |
|---|---|
| ~~`add-sdf-flatten`~~ **landed 2026-08-07** | Voxels have `sculpt_flatten`; SDF layers have nothing but the cut tool, which is global to its prism and has no falloff. Raised first as "add the Clip brush" and **that framing was wrong**: as a solid, ZBrush's Clip is exactly Trim — the clamp map sends everything past the plane onto it, and the image has no volume. Clip's distinctive look is a zero-thickness fin a field cannot represent and users delete anyway. Flatten is the verb with no equivalent today. The Lipschitz argument was the expected difficulty and not the real one. Flatten cannot rewrite a volume's samples the way relax does at all — a band tracks the surface only while the surface stays inside it, and flatten moves it many band widths, so the isosurface came apart. It samples a fresh volume instead, which makes the blend closed-form. The region also turned out to be **required**: where flatten's weight is one the result IS the plane, so with no region it replaces the shape with a half-space. |
| ~~`add-snakehook`~~ **landed 2026-08-07** | Horns and tendrils. The claim that it "needs geometry that GROWS along a drag" was wrong: the stroke opcode already sweeps a sphere along a chain with a radius per point, which IS a tendril once the radii taper — and the field stays EXACT, so it costs the raymarcher nothing. It became a resolver, like the cut tool. What it owns is the arc-length taper and prepending the anchor; what it does not is pushing that anchor inward, which was specified and measured to do nothing. |
| `add-magnify-blob` | Magnify is Pinch's inverse and nearly free given Pinch. Blob is not — its irregular response wants a noise source the engine does not have. |

Not planned: Morph (needs a stored morph target, which is a document concept
rather than a brush), Elastic and ZProject (both mesh-era ideas that do not
survive the representation change intact).

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
