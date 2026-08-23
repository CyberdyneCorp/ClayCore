# claycore roadmap

Where the engine is, what it is missing, and in what order the gaps are worth
closing. Derived from the 3DCoat feature study and the ZBrush brush-system
comparison, reduced to **what this repository owns** — app-side and
retopo/UV/bake items live in the ClaySpace and CyberRemesherAndUV repos and are
named here only where ClayCore has to provide something for them.

Living requirements are in `openspec/specs/`; this file is the plan, not the
contract. A row becomes real when it becomes a change in `openspec/changes/`.

Where the engine stands against the tools it gets compared to — Blender, ZBrush
and 3DCoat — is in `docs/sculpt_comparison.md`: what it wins outright, what is
missing before an app built on it could compete, and which non-goals cap the
ceiling on purpose. The short version is that the brush vocabulary landed and
the **workflow tier** did not: masking that protects a surface from any
operation, sculpt layers, and alphas on SDF layers are document concepts this
file's brush rows were never going to deliver.

Last reconciled against `3dcoat_study/MISSING_FEATURES.md` and
`3DCOAT_FEATURE_STUDY.md` on 2026-08-05, after a review from the study's authors
caught five items this file had dropped. Every ClayCore-owned row in their
catalogue is now represented here or in the deferred list below.

## Where the engine is (2026-08-13, v0.30.0)

14 capabilities, 65 archived changes. Complete enough that the gaps below are
about *sculpting affordances*, not about the field engine. The bullets below
are the 2026-08-07 (v0.22.1) snapshot with corrections kept in place; what
landed between v0.22.1 and v0.30.0 is summarised at the end of this section:

- 30 primitives + stroke/curve chains, **16 combine ops**, 5 blend profiles,
  grid/radial repetition, mirror with blended seam. Every kernel capability is
  reachable from a document — loft was the last one that was not. Relief and
  incise are the only ops whose item is a REGION rather than geometry.
- **16 deformers** — twist and bend (whole-item, and ranged across a span
  and held beyond it, which is what a gizmo box does), taper, displace,
  wrap_around, elongate,
  elongate_axis, bend_linear, bend_radial, plus grab, pose and pose_line with
  finite support, plus magnify (signed: magnify and pinch are one deformation)
  and noise. Every point-warp implemented in the kernel headers is reachable
  from a document; there is nothing left stranded.
- Voxel engine: palette grids, cube/sphere brushes with 4 falloff curves and
  strength, **10 sculpt verbs** — smooth, inflate, flatten, pinch, magnify,
  scrape, smudge, grab, fill_cavities and carve_alpha — fills, mirrored edits,
  flood select, greedy meshing, SDF↔voxel bridges, paintable mask fields
  gating every verb
- Brush stroke engine: samples in, edit items out, with versioned presets;
  paintable per-layer mask fields, painted along a stroke by that same engine,
  freezing every verb on both representations, with a bounded complement and
  mask extrude (a masked patch pulled off as a solid); ghosted and locked layers
- Control-point curves (hard / Catmull-Rom / B-spline / Bezier, closed,
  tessellated to a document tolerance) and the cut tool (rect / circle /
  polygon / spline lasso, swept as a prism)
- Editing and opt-in undo over one command vocabulary shared with the file
  format — including an item's deformer chain, which `SetDeformersCmd` made an
  ordinary edit; **303 capabilities** gated for binding parity (268 at the
  v0.22.1 snapshot); the Swift package verified in the iOS Simulator
- Four backends registered. ~~**CPU and Metal are verified on device as of
  v0.22.1**~~ — **"on device" there meant a Mac with the `metal` preset.** No
  iPad ran claycore until 2026-08-10, and could not have: the metallib was
  compiled against the macOS SDK whatever the target, and the xcframework
  shipped CPU-only. Corrected by `add-device-perf-gates`, which is also where
  the measured latency lives. CUDA and OpenCL remain manual hardware checks
  (docs/RELEASE.md)

Since that snapshot, v0.23–v0.30 landed (in archive order): the tube and Trim
Curve tools, `move_topological`, stroke strength on relief; the compiled-tape
cache and the interactive-path speedups; the brick cache exposed across the
ABI — the WebGPU host path, LOD meshing, device interop; hardened boundary
validation; curve-point and voxel-effect readback; the device gate (the first
iPad ever to run claycore, 2026-08-10); armatures with per-node signs
(negative ZSpheres); layer node enumeration and rename; the incremental voxel
display path; and partial backend registration with per-operation
introspection. The per-release detail is `docs/RELEASE.md`; each change's
decisions are in `openspec/changes/archive/`.

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
| ~~`add-mask-field`~~ **landed 2026-08-05** | Paintable per-layer scalar gating edit strength — freeze works. Representation-independence is enforced structurally rather than tested for: the mask is addressed in world units on its own lattice, so a resolution change cannot misalign it, and it lives beside voxel content rather than in the evaluated document, so its presence cannot change what a document evaluates to. **Scope note:** masking gates edits where they are *authored*. Voxel edits consume it per cell; SDF edits are declarative items with no per-point strength, so they consume it when a stroke becomes items — which is `add-brush-stroke-engine` below. Masked shell landed as `add-mask-extrude`; the stroke consumer and the freeze on the field verbs landed as `add-mask-stroke-brush`. Mask-referenced pose regions are still a follow-up, and `mask_to_field` is now the query they would use. |
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
redistancing anywhere — `add-consolidation-policy` added the last of those on
2026-08-09, as `field::redistance`). The brick cache samples an existing tape; it does not
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

## Sculpting verbs on SDF layers — all landed

Read the table before concluding anything is missing here. Every row below
except `add-blob-brush` has **landed**, and the surface is not on `Layer`:
`Volume.relaxed` is the smooth verb, `Volume.flattened` / `flattened_from` is
flatten, `Volume.moved_topologically_from` and `Layer.move_surface` are Move,
`clay.snakehook` is the tendril, and `MaskField` is a full mask brush with
`mask=` accepted by the region verbs. An audit that looks only at `dir(Layer)`
and `dir(VoxelGrid)` concludes the SDF side is empty and is wrong; that mistake
was made on 2026-08-09 and is recorded here so it is not repeated.

What these verbs cannot do is CHAIN — see the consolidation row below.

`add-sdf-relax` closed the first of these; the pattern it established — sample,
rewrite the stored samples under a region, hand back a volume — makes the rest
tractable, and each one is a row rather than a project.

| Change | Notes |
|---|---|
| ~~`add-surface-relief`~~ **landed 2026-08-07** | ZBrush's Standard and ClayBuildup building a surface up, and Crease and DamStandard cutting into one. The first ops whose item is a **region** rather than geometry: they offset the accumulated field by an amplitude weighted by the item's own field, so the surface already built moves along its own normal and the item contributes no shape. Scoped as ONE op with a signed amplitude, which cannot work — the amplitude rides on `blend_k`, validated non-negative in three places including the blend constructor, which has no op to be aware of. Two ops sharing one kernel branch instead, which is also the convention add/subtract and engrave/emboss already follow. The item's rounding does **double duty**: it is the falloff width and it rounds the region's own field, so the reach is region + rounding + falloff. |
| ~~`add-sdf-flatten`~~ **landed 2026-08-07** | Voxels have `sculpt_flatten`; SDF layers have nothing but the cut tool, which is global to its prism and has no falloff. Raised first as "add the Clip brush" and **that framing was wrong**: as a solid, ZBrush's Clip is exactly Trim — the clamp map sends everything past the plane onto it, and the image has no volume. Clip's distinctive look is a zero-thickness fin a field cannot represent and users delete anyway. Flatten is the verb with no equivalent today. The Lipschitz argument was the expected difficulty and not the real one. Flatten cannot rewrite a volume's samples the way relax does at all — a band tracks the surface only while the surface stays inside it, and flatten moves it many band widths, so the isosurface came apart. It samples a fresh volume instead, which makes the blend closed-form. The region also turned out to be **required**: where flatten's weight is one the result IS the plane, so with no region it replaces the shape with a half-space. |
| ~~`add-snakehook`~~ **landed 2026-08-07** | Horns and tendrils. The claim that it "needs geometry that GROWS along a drag" was wrong: the stroke opcode already sweeps a sphere along a chain with a radius per point, which IS a tendril once the radii taper — and the field stays EXACT, so it costs the raymarcher nothing. It became a resolver, like the cut tool. What it owns is the arc-length taper and prepending the anchor; what it does not is pushing that anchor inward, which was specified and measured to do nothing. |
| ~~`add-magnify-pinch`~~ **landed 2026-08-07** | Scoped as `add-magnify-blob`; Blob was carved out and is blocked on `add-noise-field`, because its irregular response is the brush and the `displace` deformer's sine is regular by construction. Magnify and pinch turned out to be ONE deformation with a signed strength, which is what Maxon's own page says. The centre of a radial scale is its fixed point, which caught the tests twice in the same shape. |
| ~~`add-mask-stroke-brush`~~ **landed 2026-08-07** | The mask field landed with the right shape and without the two things that make it a *brush*. It had no stroke consumer — `apply_to_grid` and `stamps_to_nodes` were the only two — so painting a mask along a drag meant re-implementing spacing, pressure and taper per caller, and getting the one non-obvious conversion (a stamp's WORLD radius into a footprint in MASK cells) differently each time. And **the freeze did not reach `relax` or `flatten`**: they take a sphere region and nothing else, so "a mask blocks any effect of sculpting" was false for exactly the verbs SDF layers gained most recently — a bug, not a gap. Also `invert_within`: `invert()` flips only the chunks that have been touched, which is correct for an unbounded sparse lattice and is *not* what "mask a limb, invert, sculpt everything else" means. The field verbs take the mask as a CALLABLE, not as a `MaskField`: a sampled field is a leaf below `scene` while a mask sits above it, so naming the type there would have made field → voxel → scene → field a cycle. |
| ~~`add-mask-extrude`~~ **landed 2026-08-07** | ZBrush's Extract, and what a mask is *for* once it can do more than freeze. Almost nothing new was needed: `op_shell_union`'s operand is already the shell of a field, `FieldVolume` is already blob-carried and backend-portable, and `flatten` already established "sample a fresh volume and hand it back". The one real blocker was that **a mask is a [0,1] scalar on a lattice and not a distance field** — composing one directly puts a step in the result and the Lipschitz bound becomes a fiction — so `mask_to_field` measures it with an exact Euclidean distance transform first. THE MASK IS THE REGION: no `region_radius`, unlike relax and flatten, because the painted region bounds itself. Two paths that must agree — SDF samples, voxels stay in cell space and keep their palette — checked against each other rather than asserted. It lives in `brush` rather than `field` for the cycle above. Not done: no parametric link back to the source (that needs a tape op referencing another layer), no rim profile, no mesh-level extract (meshing the result already works). |
| ~~`add-noise-field`~~ **landed 2026-08-07** | Gradient noise on an integer lattice. The three open decisions answered each other: parity is tolerance-based (1e-6 CPU / 1e-4 GPU), and a float hash turns each backend's own `sin` into an O(1) disagreement, so the hash had to be INTEGER — which decided the noise and forced the dialect's first integer type into the shim. The seed is a plain deformer parameter. Blob is now unblocked. |
| ~~`add-move-brush`~~ **landed 2026-08-07** | ZBrush's Move for SDF layers. The deformation was never missing — `grab` has been there since `add-region-deformers` — but three things stood between it and a brush, all of them the kind of geometric step the cut tool and snakehook exist to absorb. A deformer is per ITEM and its centre is in that item's LOCAL frame, so grabbing one item of a blended form pulls its share and leaves the rest (measured: 0.070 and 0.000 on two blended balls). The warp has to go at the FRONT of the chain, because `deformers[0]` is the outermost warp on the geometry and one appended behind an existing deformer has its region weight read at a point that deformer already moved. And there was **nowhere to put the result**: the command vocabulary had no way to change a node's deformers at all, so a deformer could only be set when its node was created — `SetDeformersCmd` is the other half of this row. The expected hard part, accumulating a transform chain through groups, turned out not to exist: a group's transform never reaches its children, which is worth knowing on its own. Followed by `add-move-drag-continuity`: a Move is a stream of drags, not one, and each frame prepended another warp — 120 of them on a two-second drag at 60fps, each multiplying into the declared Lipschitz. A drag now coalesces on its fixed centre and radius, and can be previewed. |
| `add-blob-brush` | Now unblocked by `add-noise-field`. ZBrush's Blob: an irregular surface response under a brush region, which is noise applied locally rather than to a whole item. |

Not planned: Morph (needs a stored morph target, which is a document concept
rather than a brush), Elastic and ZProject (both mesh-era ideas that do not
survive the representation change intact).

## Consolidation, which two brushes needed — landed 2026-08-09

Two verbs hit the same wall from opposite sides, so it is worth naming as a row
rather than as two footnotes.

**Move** stacks a grab per drag, and a stroke is many drags: the declared
Lipschitz compounds and the safe step scale decays geometrically, about x0.615
per drag — 79x the marching cost by nine drags. Coalescing covers frames of one
drag, where the centre and radius are fixed; a stroke moves the centre.

**hPolish** bakes, and a second pass samples the first pass's volume rather than
the document. Outside the band a volume reports a lower bound rather than a
distance, so the blend works from the wrong value: the Lipschitz goes 1.00 to
14.0 on the second pass whatever the falloff, and by the third the form is
visibly corrupt rather than merely expensive.

`add-consolidation-policy` closed both. `Layer.consolidate` /
`clay_layer_consolidate` collapses a layer into one volume as a single undoable
step, and `Layer.field_report` / `clay_layer_field_report` is the advisory
trigger — it reports the step scale alongside the two things that cost it, and
never bakes on its own, because a bake discards parameters and the artist is the
one who pays for that.

**The row assumed a bake was enough, and it was not.** Collapsing an edit list
into a volume was already possible with `clay_item_volume_from_document`, so the
proposal said no new primitive was needed. Measuring it showed otherwise: the
bake of a two-pass polish chain stores samples varying at 14× the cell size, and
a finer cell makes that *worse*. Steepness is a property of the field, and
resampling it onto a lattice reproduces it. What removes it is **redistancing** —
replacing the samples with the distance to their own zero set, `field::redistance`
— which is a genuinely new primitive and the one this row turned out to need. The
same measurement also caught `FieldVolume::sample` declaring its result
1-Lipschitz without ever measuring it, which made every `from_document` bake of a
steep chain an overclaim.

Measured in `examples/27_move_strokes.py` and `examples/28_hpolish.py`, which
pin the degradation, and in `examples/38_consolidation.py`, which pins the cure.

## The sculpting ceiling, proposed 2026-08-09

Four changes raised by trying to sculpt two complete assets end to end
(`examples/34_organic_character.py`, `examples/35_hard_surface_helmet.py`).
They are ordered by how much each one costs a sculptor today.

| Change | Why it ranks here |
|---|---|
| `add-multi-resolution` | **The ceiling, and the only one that is not additive.** `VoxelGrid` takes its cell size in the constructor and there is no resample, resize, subdivide or adaptive refinement anywhere in `voxel/`, `mesh/` or `brick/` — the brick cache is a sparse narrow band, not an LOD hierarchy. So the finest detail in a model must be chosen before the first stroke and paid for everywhere, and cannot be added locally afterwards. This removes the loop sculpting is made of: block out coarse, subdivide, refine. Recommends discrete levels over an octree, because the falloff dither hashes a CELL COORDINATE and the parity suite enforces that strokes reproduce across platforms — a uniform lattice per level keeps that property, an adaptive one puts it in question. Do it first: retrofitting levels under verbs, a file format and an ABI that all assume one cell size is harder than building on them. |
| ~~`add-consolidation-policy`~~ **landed 2026-08-09** | The SDF verbs existed and did not chain, for two different reasons: hPolish sampled the previous pass's VOLUME (1.00 -> 14.0 Lipschitz on the second pass, corrupt by the third) and Move stacked a grab per drag (x0.615 per drag, 79x by nine). Advisory reporting plus a layer-scoped bake that redistances; see the section above for what the row got wrong about baking. |
| `add-representation-round-trip` | The bridge runs one way. SDF to voxel is `rasterize_tape`; voxel back is only mesh -> `to_field` -> volume, which resamples onto a frozen lattice and drops the palette. So a sculptor picks a representation and lives inside its half of the toolkit, when the natural workflow is to keep moving between them. Honest framing is a conversion, not a view: quantisation and lost procedural history are the price and the spec should say so. |
| `add-sculpt-layers` | No way to record a pass and dial it back. Undo is a stack — removing an old pass discards everything after it; a sculpt layer is addressable. Partial strength on binary occupancy is the interesting part, and the answer is the dither the falloff brushes already use. |

Two changes proposed on the same day were **withdrawn as wrong**:
`add-sdf-sculpt-verbs` and `add-sdf-masking`. Both were raised from an audit
that read `dir(Layer)` and `dir(VoxelGrid)`, missed `Volume` and `MaskField`
entirely, and concluded the SDF side had neither verbs nor masks. It has both.
The real gap in that area is consolidation, above.

## What can run in parallel, and what cannot

Six changes are open. The constraint is not their size — it is that three of
them rewrite the same object and three do not touch it at all.

### Three are disjoint in code and can run together

| Change | Touches | Contends with |
|---|---|---|
| `expose-scene-groups` | the binding surface and example 35 | nothing; the engine already implements groups, so there is no engine change |
| `add-consolidation-policy` | `sdf-kernels`, the scene commands, `Volume`, examples 27 and 28 | nothing; never touches `VoxelGrid` |
| `add-mesh-layers` | `scene-model`, `io/clayspace`, export | nothing; never touches `VoxelGrid` or the field chain |

Three separate areas — the binding surface, the SDF field chain, the document
and its format. The only files all three share are `bindings/c/clay.h` and the
`c-abi` spec, and both are append-only: new declarations at the end, new
requirements at the end.

`expose-scene-groups` is much the smallest of the three and needs no engine
work at all, so it is the natural one to run alongside something large.

### Three contend on `VoxelGrid` and must serialise

    add-multi-resolution  ->  add-sculpt-layers
                          ->  add-representation-round-trip

`add-multi-resolution` goes **first and alone**. It is the only non-additive
change in the set: it changes what a grid *is*, which reaches storage, the
footprint walk every verb shares, meshing, the brick cache and the ABI. Anything
else editing `VoxelGrid` at the same time is being written against a foundation
that is still moving.

Once it has landed the other two can run together, because they touch different
parts of the grid — sculpt layers touch history and storage, the round trip
touches conversion — and both need the level rule to exist before they can be
specified. Each says so in its own open questions.

### The format version is the cross-group trap

    kClaySpaceMinor = 4     include/clay/io/clayspace.h:47
    kSceneMinor     = 4     include/clay/scene/commands.h:147
    static_assert(kClaySpaceMinor == scene::kSceneMinor)   src/io/clayspace.cpp:18

**Three of the six add a `.clayspace` chunk and will each want to bump 4 to 5**
— `add-mesh-layers` from the parallel group, `add-multi-resolution` and
`add-sculpt-layers` from the serialised one. Two of them bumping independently
is a one-line textual conflict with a bad outcome: a document that claims minor
5 while carrying only one of the two features.

So the minors are **assigned here rather than taken on a first-come basis**, and
each change's tasks carry its own number:

| Change | `.clayspace` minor |
|---|---|
| `add-mesh-layers` | 5 — taken |
| `add-multi-resolution` | 6 — taken |
| `add-armature` | 7 — **taken**, see below |
| `add-sculpt-layers` | 8 |

`add-consolidation-policy` and `expose-scene-groups` need no bump — volumes and
groups both already serialise. `add-representation-round-trip` needs one only if
it introduces a new layer kind, which is one of its open questions; if it does,
it takes 9.

Armature was assigned 9 when it was proposed and took **7** when it was built,
because 7 and 8 were reserved for changes that are still only proposals and
nothing was in flight to collide with. The point of assigning is to stop two
concurrent changes claiming one number, not to leave holes for work that may
never happen.

### The plan

**Wave 1, four in flight:** `add-multi-resolution` on its own track, and
`expose-scene-groups`, `add-consolidation-policy` and `add-mesh-layers` beside
it.

**Wave 2:** `add-sculpt-layers` and `add-representation-round-trip`, once
multi-resolution has landed.

That is the most parallelism the dependency structure actually allows. Running
more means rebasing voxel work onto a moving foundation, which costs more than
the concurrency buys.

## Armatures, proposed 2026-08-10 — landed, extended with per-node signs in v0.30.0

`add-armature` landed 2026-08-10 as proposed below. `add-armature-node-signs`
followed in v0.30.0 (archived 2026-08-13): a sign per node so a rig carries
negative ZSpheres — a hollow is sculpted by the same tree that builds the limb.
`clay_item_set_armature_signs` / `clay_layer_armature_signs`, carried by
`.clayspace` 1.7 → 1.8 (backward-open).

`add-armature` — a tree of spheres that skins to a form, which is ZBrush's
ZSphere workflow. Raised by `examples/34_organic_character.py`: a humanoid
there is forty-odd primitives whose positions are hand-written coordinates,
because there is no way to say "an arm hangs from this shoulder", only to
compute where the arm's capsule would be if it did.

The proposal's finding is that most of it already exists. `ctape_stroke` is a
CHAIN of sphere-swept segments with a radius per point and a smooth union
between them — an armature is that same loop over `(i, parent[i])` instead of
`(i, i + 1)`. So the primitive is a generalisation rather than a subsystem, and
a chain-shaped armature must evaluate identically to the stroke it came from.
The rest of the pipeline is also already here and the proposal says so rather
than re-inventing it: a field needs no preview mesh, `Document.mesh` IS the
adaptive skin, the brick cache already does the incremental work, and
`.clayspace` is already the save format.

Two scoping notes worth keeping. The brief it came from describes a mobile
app — gestures, compute shaders, draw calls, frame rate, battery, a cloud file
format — and none of that is this library's to own; the host builds it on top.
And per-node ROTATION is deliberately absent: a sphere is isotropic, so a
rotation changes no distance and no surface. It earns its place in ZBrush
because the adaptive skin lays out quads whose edge flow follows the node
frames, and none of marching cubes, surface nets or dual contouring consults
such a frame. Storing it would be a promise this engine does not keep.

## The interactive path, proposed 2026-08-10

Seven changes, from a review of what the library actually costs on the device it
exists for. The theme is that the *feature* work is ahead of the *latency* work:
the sculpting vocabulary landed, and the engine has never been measured on a
tablet.

The budget these are judged against is the one
`speed-the-interactive-path` wrote down: 4–8 ms per Pencil event at 120–240 Hz,
16.7 ms for a preview frame. Everything below either defends it, or is the only
way to know whether it is met.

| Change | Why |
|---|---|
| `add-item-spatial-index` | **The one that matters most.** A dab's brick count is flat with document size; its cost is not. `clay_brick_cache_eval_requests` compiles a culled tape per brick and the cull walks every item — ~64 ns per item per brick, ~24 bricks per dab, so ~3.6 ms at 2 400 items and past the whole budget at 10 000, before a sample is evaluated. The tape cache cannot help: consecutive bricks want different cull regions. Fanning out halves the constant and leaves the slope. |
| `add-cpu-simd-path` | The spec has required a SIMD batch path since v1 — "Apple `simd` on Apple platforms, SSE/NEON via xsimd elsewhere", with a parity scenario gating it — and there is none. `xsimd` is fetched by CMake and included by nothing; the "batch path" is the scalar evaluator sliced across threads. This is the path brick fills actually run on, per the 0.24.0 measurement that keeps them off Metal. |
| `speed-the-metal-path` | Every dispatch re-uploads the whole tape, allocates and frees six buffers, blocks on `waitUntilCompleted` and copies results back out of shared memory. That is the dispatch cost the 288 µs-per-brick measurement was measuring. Also: `device_meshing` is false while the spec says the backend meshes on device, and gradients fall back to the CPU for the whole batch. |
| `add-mobile-thread-scheduling` | "The caller owns threading and queues" is not true of the CPU backend: a process-wide pool spawns `hardware_concurrency - 1` threads with no QoS class, counts efficiency cores as equal workers, and spins on `yield()` at the join — on the thread the user is waiting for. |
| `add-brick-cache-eviction` | The memory budget can be hit and never backed away from: no evict, no trim, no clear. Past the budget a submit is refused, so the surface stops updating where the artist is working, and the only recourse is destroying the cache. iOS asks for memory back and then takes it. |
| `add-tape-abi-export` **landed** | Carried since Phase 2 and closed as issue #43 item 5. `clay_tape_export` hands out an immutable snapshot the caller releases — an edit installs a new tape rather than mutating the old, so borrowed buffers cannot be invalidated and a warm export costs a refcount (0.000 ms measured). Culled tapes export too, with the header saying plainly that they compile where the whole-document one does not. Measured against what it replaces: a 512x512 preview round-trip is 8.4 MB and 131 ms per FRAME at 50 items, against 8 KB and 0.02 ms per EDIT. |
| `add-device-perf-budgets` | Every number in this repository was taken on a desktop or an M2 Max. Nothing measures the budget, nothing measures the path end to end, nothing measures sustained behaviour, and the decision to keep brick fills on the CPU rests on a crossover found on a machine with a fan. This is how the six rows above are judged. |
| `close-webgpu-host-abi-gaps` **landed** | Issue #43, from ClaySpaceDesktop: the brick cache is a GPU upload path that stopped one step short. Now carries an opt-in RGBA8 colour lattice and an apron on the readback, so a host uploads the narrow band as a filterable `r16float` + `rgba8unorm` atlas and traces it in WGSL with **no kernel math in the shader** — a second, cheaper route to the anti-drift property `docs/06` exists for, and the only one that works in a shading language our dialect does not target. `clay_brick_cache_mesh` takes a key list and reports per-key ranges (22.6 ms → 0.64 ms on the benchmark scene for a dab's worth of bricks), `clay_mesh_copy_vertices` writes a host's own interleaved layout, and the brick raycast has a batched form. |
| `mesh-brick-cache-lod` **landed** | Issue #93, also from ClaySpaceDesktop. The LOD half that shipped was the half a MESHING host could not use: `clay_brick_cache_build_mip` built a level, `clay_brick_cache_read_bricks` read one and `clay_brick_cache_current_lod` reported one, while `clay_brick_cache_mesh` took a key list and no level — so coarse triangles meant reimplementing the marcher over the fp16 samples, which is the thing a host adopts this cache to avoid. The mip turned out to need nothing from the mesher: it is the cache's own lattice at twice the spacing, so `clay_brick_cache_mesh_lod` is plumbing plus two rules — an unbuilt level is `CLAY_ERROR_NOT_FOUND` rather than the empty mesh that already means "no surface", and field attributes stay at level 0 where the culled tape's exactness argument holds. |
| `add-device-interop` **landed (Vulkan verified, Metal CI-only)** | The other half of #43. Even on the atlas route every brick and every mesh crosses host memory, because `eval::Backend` has no notion of a device — `eval_grid` writes `float*` by type — so a host that was going to draw on a GPU pays an upload it should not need. Lend claycore your `VkDevice` or `MTLDevice` and have evaluation land in your own buffer. Pairs with `add-vulkan-backend`, which made this the same physical device on both supported platforms. The limit worth stating up front: this makes evaluation OUTPUT device-resident, not brick STORAGE — the cache's generation and classification state machine is host code. Vulkan adoption is verified on lavapipe (device output bit-identical to host output); Metal adoption is written and compiled by CI but has not run on hardware. CUDA reports that it cannot adopt. |

`add-vulkan-backend` is proposed alongside these and is **not** one of them: on
Apple hardware Vulkan means MoltenVK over Metal, which cannot beat the Metal
backend it translates into. Its case is portability and the retirement path for
OpenCL, whose CI job was removed because pocl's arithmetic is the CPU's. The
likely route needs no fifth dialect — the OpenCL amalgamation already is the
C-compatible subset, and clspv compiles that to SPIR-V.

## The device gate, landed 2026-08-10

`add-device-perf-gates`. **This file had no performance row at all until this
one, which is why the gap below survived to v0.25.0.** Everything the roadmap
tracked was about what the engine can express; nothing tracked what it costs
on the hardware it ships to, so nobody noticed that the answer was unknown.

The gap was worse than "untested". `evaluation-backends` has called Metal "the
iPad app's production path" since the beginning, and **no iPad had ever run
it** — not from neglect, but because it was *unbuildable* for the platform.
`CMakeLists.txt` compiled the kernels with `xcrun -sdk macosx` regardless of
target and embedded the result in the library, so an iOS slice with the
backend enabled carried **macOS AIR that loads on no device**. The failure
surfaces as a backend that never registers, which is indistinguishable at the
ABI from one that was never enabled. `build_xcframework.sh` then shipped every
slice CPU-only by design, so even a correct metallib would not have reached a
host.

### What the first measurement pass found

Written in the style of the "what actually bit" entries above, because the
plan's guesses and the measurements disagreed in four places.

| Expected | Actually |
|---|---|
| Metal is the fast path | Metal is **slower than the CPU at small documents** — 0.44 ms vs 0.08 ms p95 at ten stamps — because dispatch overhead dominates until the work amortises it, and wins by 2.5x by a thousand. A host that always selects Metal is slower through the whole blockout phase. The spec says "production path" unconditionally and the measurement does not support that; the crossover is the routing rule. |
| The parity corpus covers the vocabulary | It covered every PRIMITIVE, because a guard existed for those, and **12 of the 16 combine ops and 4 of the 14 deformers reached no scene at all** — including twist, bend, taper and displace, the four *original* deformers. The four covered ops were the ones whose own changes happened to add a scene. All sixteen new scenes pass, so the opcodes were right; only the evidence was missing. |
| A hostless XCTest bundle can run on device | It cannot. `xcodebuild` refuses outright, and SwiftPM cannot declare a test host, so a package reaches the simulator and never the iPad. The harness needs a generated Xcode project and an empty host app. |
| Baking a latency number is the easy part | It is the hardest part, and it went wrong twice. Timing a verb without asserting it SUCCEEDED measures the error path: `mask_extrude` was being refused at 100 and 1000 stamps, so its first figures were the cost of a refusal at two of three points. And a verb that MUTATES what it measures times its own side effects — the stamp cases grew the document per iteration, `consolidate` re-consolidated an already-consolidated layer. Both were invisible until the sample count changed and the numbers moved with it. |
| The incremental path is the cheap one | It is not, at these sizes: driving the brick cache costs 5.60 ms against the global lattice's 4.41 ms at 1000 stamps. Bricks per stamp is constant at ~13 across the axis, so the cost is the culled tape compiled per brick — which is what `add-item-spatial-index` above already predicted, now measured on hardware. |

Two more worth keeping. Thermal state is not noise to be averaged out: several
runs back to back take an iPad to `serious`, and the guard that invalidates
such a run **fired for real** during this change rather than in theory. And
the Metal shader cache is worth 1400x on a first call (14.172 s cold vs
0.010 s warm), which is why warm-up is excluded from every sample.

### The numbers, and what they mean for the ceiling

From `tests/device/baseline.json` — iPad Air 13-inch (M3), iOS 26.5.2,
worst-point p95 across a 10/100/1000-stamp axis:

| | p95 at 1000 stamps | grows as |
|---|---|---|
| every voxel verb (11) | < 0.03 ms | flat |
| one SDF stamp (edit + evaluate), CPU | 4.41 ms | `N^0.88` |
| one SDF stamp, Metal | 1.77 ms | `N^0.30` |
| one SDF stamp, through the brick cache | 5.60 ms | `N^0.64` |
| Move drag | 0.10 ms | `N^1.02` |
| consolidate | 1.57 s | `N^0.84` |
| mask extrude | 2.53 s | `N^0.91` |

An earlier draft of this table was wrong in two places and the corrections are
worth keeping, because the same mistake is easy to repeat. It recorded
`consolidate` as a **flat 5 s**; it is 1.57 s and scales as `N^0.84`. And it
put the stamp growth at `N^0.65`; it is `N^0.88`. Both came from fixtures that
mutated what they measured — the stamp cases grew the document by one per
iteration, and `consolidate` collapses its layer, so every iteration after the
first re-consolidated an already-consolidated one. **The tell was numbers that
moved when only the SAMPLE COUNT changed**, which cannot happen to an honest
measurement.

**The SDF stamp curve is a product ceiling, and it belongs beside
`add-multi-resolution` in the section above rather than in a test report.** At
1000 stamps one stamp already exceeds the engine's half of a 120 Hz frame, and
a real sculpt is far more than 1000 stamps. The voxel path being flat across
all eleven verbs is the control that makes that a property of the field path
rather than of the measurement.

### It confirms `add-item-spatial-index`, on hardware

The section above predicts that a dab's brick count is flat with document size
while its cost is not, because `clay_brick_cache_eval_requests` compiles a
culled tape per brick and the cull walks every item. **Measured on the iPad,
that is exactly what happens.** Driving the brick cache the way a host does —
dirty the new node, drain, evaluate, submit — refreshes a *constant* 12.8–13.6
bricks per stamp across the whole 10/100/1000 axis, and per-stamp cost still
climbs. Culling reduces what is evaluated, not what is compiled.

The consequence is worth stating because it is counter-intuitive: **the
incremental path is not cheaper than re-evaluating the whole working volume**
at these document sizes. That is a measurement of today's engine, not an
argument against the brick cache — and it makes `add-item-spatial-index` the
row this section's numbers most directly support.

An earlier draft of this section attributed the stamp growth to the tape
recompile alone, citing Finding 2. That was a cause the experiment did not
isolate: recompilation is backend-independent, so it can be no larger than the
Metal case's rise over the same axis, which bounds it well under half of what
the CPU case grows by. The per-brick cull is the larger term.

Nothing here was optimised. The change measures, gates and records; acting on
what it found is the next change, and keeping the two apart is what makes the
first baseline trustworthy.

## The display path and the host seam, landed 2026-08-13 (v0.30.0)

Seven changes, archived 2026-08-13, released as
[v0.30.0](https://github.com/CyberdyneCorp/ClayCore/releases/tag/v0.30.0).
The device gate ran on the reference iPad from a clean tree — 59 cases, the
provisional display-path and level-stack budgets re-seeded from the measured
run — and `release_check.py` passed every gate, `device` and `wheel`
included, for the first time.

The section above found that showing a voxel sculpt cost ~130× what editing
it did; this is the release that acted on it:

- `speed-the-voxel-mesh-sweep` — the mesh sweep stops probing the chunk map
  per cell: 4.12 → 0.157 ms per occupied chunk (~26×), byte-identical output
  over 157 fixtures.
- `add-voxel-incremental-mesh` — a dab re-meshes only the chunks it dirtied:
  0.65 ms against 23.3 ms whole-grid (~36×), the voxel side of the
  `mark_dirty → take_dirty → mesh` shape the brick cache already had.
  `clay_voxel_mesh` keeps meaning "mesh the whole grid".
- `mesh-brick-cache-lod` — detail in its row in the interactive-path table.

The rest is the host seam — gaps hosts actually hit, each from an issue:

- `enumerate-layer-nodes` (#91) and `rename-a-layer` (#92) — an outliner can
  be drawn and a layer renamed without probing sparse ids or losing the name
  on save.
- `add-armature-node-signs` (#99) — detail in the armatures section above.
- `register-a-partial-backend` (#63) — a backend that lost one pipeline
  registers on its core operations and says what it lost:
  `clay_backend_supports` / `clay_backend_diagnostic`. On Apple Paravirtual
  GPUs `clay_list_backends` now answers `cpu,metal` where it answered `cpu`.

## Phase 3 — the pipeline

| Change | Notes |
|---|---|
| `add-mesh-layers` **first slice landed 2026-08-09** | A document can CARRY an imported mesh, as opposed to sampling one: a third `LayerKind`, the triangles stored beside the document keyed by layer id where voxel grids and masks already live, a `MESH` chunk in `.clayspace` (1.4 → 1.5, backward-open), and the attach/lookup/bounds surface in both bindings. The placement is the design: `tools/check_layering.py` withholds `mesh` from `clay::scene`, so "a mesh layer does not change what the document evaluates to" is structural rather than maintained. **Not in the slice:** the merged export (`clay_mesh_transform` / `_concat` plus the convenience call that appends every visible mesh layer to the meshed field), and `max_file_bytes` on `clay_import_budget` with a budget-taking document load beside the existing one. Both are separable and neither is about meshes reaching the field, which stays out permanently. |
| `mesh-fixed-topology-brushes` **landed 2026-08-14** | Vertex displacement on a mesh layer's own triangles — the eleven classical verbs, with `indices` and `quads` byte-identical before and after. The hole the round trip left: sculpt SDF → quad export → retopo elsewhere → and then the retopologized mesh could only re-enter through `Volume::from_mesh`, which resamples it and throws away what was paid for. Three prerequisites, all new: adjacency over **weld classes** (a ring built over raw indices stops at every UV seam), a **ray query** on the BVH so mesh layers are pickable at all, and **sparse vertex deltas** because a vertex displacement is not an edit item and `scene::Command` has no variant for one. Two decisions were made from renders rather than from tests: the surface-measured region is bounded by the brush's BALL and weighed by the STRAIGHT LINE, because a falloff driven by an edge-path distance bands visibly and a region bounded by one leaves a ragged rim; and polish's gate reads neighbouring CLASS normals, spread by one ring and then feathered, because per-face normals cannot tell noise from a feature and an unfeathered gate leaves a bead along everything it protected. The non-goal below was narrowed to topology-CHANGING sculpting rather than deleted. |
| `rasterize-mesh` **landed 2026-08-14** | Triangles straight to cells, closing the input direction the way `quad-mesh-export` closed the output one. An imported model reached an SDF layer in one step and the voxel verbs in four, paying TWO samplings — triangles into a narrow band, band into cells — so the second quantised a field that was already quantised. Membership is the generalized winding number at the cell centre, applied once instead of inherited through a band, so a holed model rasterizes without flipping a half-space. The region is optional here and required for a tape, because a document can be unbounded and a mesh cannot. Two things fell out of sampling once that were not in the issue: a feature thinner than two cells survives where the detour lost it, and **the model's vertex colours reach the palette** — `Volume::from_mesh` samples a DISTANCE field and carries no colour, so the detour had none left to quantise by the time it reached the grid. Needed one addition: `Bvh::closest` names the triangle a nearest point landed on, which is what any attribute transfer off a mesh wants and nothing could do before; `unsigned_distance` is now that query with the answer discarded. |
| `parallel-brick-meshing` **landed 2026-08-15** | The dominant interactive cost, and the first row of #119's threading inventory to land. `clay_brick_cache_mesh` cost a flat ~0.11 ms per brick from 1 brick to 343 — flat means serial, and meshing had become **56x the per-brick cost of refill** because refill takes 20.5x from Metal and meshing took nothing. THE INVENTORY WAS WRONG ABOUT THE SHAPE: it filed this as "concatenate per-brick buffers in key order", and the source already said why that cannot work — ONE builder serves every brick so a lattice edge shared by two yields one vertex, which is what keeps the sparse set watertight at seams. The march is independent; the WELD is not. So the march records per brick in parallel and a serial pass replays those recordings through the single builder in key order, which makes byte-identity a construction rather than a tolerance (verified against main: same hash). Measured back to back on 24 cores: 276 bricks 32.2 -> 7.56 ms, 80 bricks with gradients 11.3 -> 4.31 ms. Recording every brick before welding any of them made the transient buffers scale with the SURFACE — 94 MB on a 2,327-brick sphere — so the march runs in waves of 512 and that cost is a constant instead. Preceded by two prerequisites neither issue named: the pool was not nested-safe, and it lived inside a backend where the layering rule forbade the core library from reaching it. |
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

- ~~**Output descriptors are filled unbounded.**~~ — **and the reasoning that
  deferred it was wrong.** This entry claimed all seven remaining sites were
  latent because "none of those structs has grown since its callers' header".
  Measured instead of assumed: `clay_brick_config` (24 → 32),
  `clay_consolidation_cost` (76 → 80), `clay_quad_report` (36 → 40) and
  `clay_repair_report` (36 → 40) had **already** grown, so all four were live
  overruns for any host built against the older layout. `clay_brick_stats` was
  not the first to grow, only the first to grow while `tools/check_c_abi.py`
  was watching — it reads that one back at its original layout and segfaulted;
  it does not read the other four back. Fixed by
  `bound-output-descriptor-fills`, which adopts `write_desc` at all six sites
  that already probe `struct_size`.

- ~~**The two `_defaults` entry points still fill unbounded.**~~ Decided and
  done in `require-struct-size-on-defaults` (ABI 0.35.0): they now require
  `struct_size` on input like every other descriptor, which turns a silent
  8-byte overrun into a clean `CLAY_ERROR_INVALID_ARGUMENT`. Breaking on a
  minor, which 0.x allows and `docs/RELEASE.md` now records. Worth keeping
  visible that this does NOT rescue an already-compiled old host — it declares
  nothing, so refusing it is the whole of the improvement.

  The sweep before it was also incomplete, and instructively so:
  `clay_stroke_preset_deserialize` filled its output descriptor by DELEGATING
  to the defaults call, so it matched no grep for `*out = clay_thing{}` and was
  invisible to a search that felt exhaustive. `tools/check_c_abi.py` now walks
  the header for every entry point taking a descriptor by mutable pointer and
  requires a bounded fill, so the next one is caught by construction.

- ~~**Colour on a mesh layer's brushes.**~~ Closed by
  `add-mesh-colour-brushes` (ABI 0.36.0). `paint` and `smear` are Blender's
  pair, and they are the only two verbs in the vocabulary that move no vertex —
  `positions` and `normals` come out byte-identical, the exact mirror of what
  the other fourteen guarantee about `colors`. The test that pinned the
  omission was narrowed to the verbs it is still about rather than deleted.
  Colour is now editable on every representation the library has rather than on
  two of three. What remains genuinely absent is PBR channels, which is a
  declared non-goal rather than a gap — see `docs/sculpt_comparison.md`.

- **Deformers on a mesh layer.** `Deformer` has twenty-one entries and every
  one applies to an SDF item; a mesh layer takes a lattice cage and nothing
  else, so ZBrush's Deformation palette — Taper, Twist, Bend — is unreachable
  on the representation an artist holds after a retopo pass or an import.
  Scoped by `add-mesh-deformers`. Worth recording why it is cheaper than it
  looks: an SDF deformer must run BACKWARDS, which for free-form deformation
  has no closed-form inverse (the SDF lattice accepts ~1.5% error and a 4³ cap
  for it), while a mesh deformer runs FORWARDS once per vertex and inherits
  neither. It is the same math in the easier direction.

- **Procedural noise as a tape opcode.** `displace` is by-callable today, which
  is not portable across backends. A tape-expressible 3D noise field is the
  answer if node-style procedural detail ever becomes a goal.
- **Voxel layers beyond 256³.** The spec guarantees ≥256³ per layer, with a
  memory budget and typed errors past it. There is no streaming story for
  scenes larger than that; per-layer grids have been sufficient so far.
  **Sharpened 2026-08-21:** "at least 256³" reads as a floor and is also the
  ceiling — a grid's `dims` product must be ≤ `CLAY_MAX_BATCH`, which is
  16,777,216, which is 256³ exactly. So the guarantee and the limit are the
  same number, and a host asking for 512³ is refused rather than served slowly.
  Worth stating that way, because a floor and a ceiling read very differently
  to someone sizing a project against it.
- ~~**glTF/GLB import.**~~ Closed by `add-glb-import` (ABI 0.37.0).
  `clay_mesh_load` and `clay.load_mesh` gained `.glb` through the existing
  extension dispatch, so no new entry point was needed. It reads every mesh and
  every TRIANGLES primitive with the node hierarchy's world transforms applied,
  and accepts the accessor forms real exporters emit rather than only the ones
  this library writes. `.gltf` is still refused on purpose — its buffers are
  separate files, and resolving them means reading files the caller never
  handed us.
- **The voxel and mask chunks' orphan behaviour.** A mesh chunk is written only
  for a layer that still exists and dropped on load when it names none;
  `VOXL` and `MASK` do neither, so a removed voxel layer's grid is still
  written and read back. Harmless today because layer ids are never reused, and
  now visibly inconsistent.
## An outside review of the specs, audited — 2026-08-21

An external review of `openspec/specs` and this file argued, in thirty points,
that ClayCore has a sculpting **vocabulary** and not yet a sculpting **workflow
engine**, and proposed splitting the 14 capabilities into roughly 35.

**The thesis is right and this file said it first** — "the brush vocabulary
landed and the workflow tier did not". What follows is every point checked
against the tree rather than accepted, because a review of a specification can
only see what the specification says, and the interesting failures are where
those two disagree.

They disagreed once, badly, and in the direction nobody looks.

### What the audit found that the review could not

**`scene-model` requires a command that does not exist.** The undo vocabulary
requirement lists "add/remove item, set parameter, **voxel-span edit**, layer
add/remove/…". `Command` is a `std::variant` of nineteen alternatives and not
one of them edits a voxel grid; `include/clay/scene/commands.h` contains the
word "voxel" exactly once, in a comment about a serialization minor. The
requirement has been false for as long as voxel layers have existed.

A spec sentence has no test, so nothing caught it, and the review could not
have: it read the sentence and reasonably believed it. What is actually true is
that there are **three unrelated history mechanisms, one per representation** —
the command vocabulary for the SDF edit list and layer state, sculpt layers for
voxel grids, sparse vertex deltas for mesh layers — and **no single undo step
spans two of them**. Corrected in `correct-the-undo-scope`.

That is the argument for this kind of review, and also its limit.

### Already shipped, including three of the review's P0s

Ranked by how much the review would have changed if it had known.

| Review's item | What exists | Evidence |
|---|---|---|
| **P0 "Adaptive local resolution"** — refine the face without refining the back | `clay_voxel_add_level_region` refines a level over a REGION in world units. Outside it the level has no storage and reads its parent's value, so the lattice stays uniform and complete — **watertight transitions are a construction, not a tolerance**, which is the review's second requirement satisfied structurally. Writing outside the region refines what the write touched, so a brush straddling the boundary works | `bindings/c/clay.h`, multi-resolution block |
| **P0 "Persistent symmetry"** — "symmetry centre must be explicit, persistent and gizmo-editable" | A layer mirror on any of x/y/z reflects through the plane where that **layer-local** coordinate is zero, so the layer transform moves the plane, and it persists in the document. Plus a Mirror Blend seam and per-item opt-out. Measured: lumps at x=±0.5 both read −0.2; translating the layer by +1 moves the pair to +0.5 and +1.5 and x=−0.5 reads +0.8 | `SetLayerMirrorCmd`; `scene-model` requirement with four scenarios |
| **P0 "Sculpt layers"** | Landed on voxel layers — record a pass, dial its strength, reorder, merge down. The SDF side is one open DECIDE in `add-sculpt-layers`, and it was gated on scene groups, which have now landed | `clay_voxel_*_sculpt_layer*` |
| **#26 "Boolean groups / live modifier hierarchy"** | Landed. `expose-scene-groups`; `examples/37_groups.py` builds plates as INTERSECT groups under a SUBTRACT group with a chamfer blend | |
| **#7 alphas, #29 polypaint** | Both landed — alphas on SDF layers, and colour editable on all three representations since `add-mesh-colour-brushes` | `docs/sculpt_comparison.md` |
| **#12 "hidden is not deleted"** | Guaranteed for a LAYER. Measured: 0.5 at the hidden sphere's centre, still 0.5 after save and reload, −0.5 once shown. **Not** available for a region — see below | |
| **#1 spatial index** | **Partially, and the review's framing is the more useful one.** `CullIndex`/`CullPlan` already exist: per-revision bound caching plus a coarse cull of every chain against a batch's union region, emitting byte-identical tapes. That removed a large constant. It is still **linear in document size per batch**, so the slope the review is asking about is exactly what remains | `include/clay/scene/cull_index.h` |

The last row is the one to keep. `add-item-spatial-index` task 1.10 already
says it: *"a 2× constant improvement passing as a fix for this is the failure
mode"*. Half of that constant has now been taken, which makes the remaining
work harder to justify by benchmark and no less necessary.

### Real gaps, verified absent

Each was checked by searching the public surface, not inferred.

| Gap | What is actually there | Severity |
|---|---|---|
| **Surface groups / PolyGroups / Face Sets** | Nothing, on any representation. Visibility is per LAYER; a layer holds exactly ONE mask (`clay_document_add_mask` "replaces any mask the layer already had"), so N named regions cannot even be emulated with N masks; scene groups group edit-list NODES, not surface | **Highest.** It is the substrate procedural masks, extract, and per-region anything all attach to. Scoped: `add-surface-groups` |
| **Partial visibility** | Layer-only. "Hide the armour" requires the armour to have been authored as its own layer, decided before the artist knew | Same change — it is the same primitive |
| **Unbounded undo history** | `std::vector<Entry> undo_` with **no cap, no byte accounting, no eviction, no query**. The only control is `enable_undo`, which is a light switch. And the expensive entries are counter-intuitive: the stack stores INVERSES, so removing an item records a whole `Node` (440 bytes plus its deformer chain) while adding one records 8 bytes | **High, and now urgent** — an iPad at 120 Hz for hours, on an OS that does not warn twice. Scoped: `add-history-budget` |
| **Procedural masks** — cavity, curvature, normal, thickness, AO | Mask verbs are paint, fill, expand, contract, smooth, invert, `to_field`. Nothing derives a mask from the surface | High, and cheap on a field representation — curvature is a gradient the engine already computes. Next to scope |
| **Morph target** | Absent. The word appears only in `mesh_io.h`, as a glTF feature deliberately not imported | Medium; pairs naturally with sculpt layers |
| **Stroke input completeness** | `clay_stroke_sample` is position, pressure, tilt. No **azimuth**, no velocity, no timestamp | Medium — azimuth is what makes a directional or rake brush possible at all, and it is five floats to add before hosts depend on the current layout |
| **Radial symmetry** | Three mirror planes, no radial | Medium |
| **Instancing / scatter** | Absent. Phase 4 already names `add-surface-scatter`; the review is right that scatter without instancing duplicates geometry | Medium |
| **Generic named attributes** | `colors`, `uvs`, `normals` and nothing else. A host cannot carry `material_id` or a custom channel through the engine | Medium. The review is right to separate this from PBR: allowing an app to carry channels is not the same as rendering them, and only the second is a declared non-goal |
| **Voxel beyond 256³** | Real: a grid's `dims` product must be ≤ `CLAY_MAX_BATCH`, which is 256³ exactly. The spec's "at least 256³" reads as a floor and is also the ceiling | Medium — already recorded under "Deferred, but recorded", now with the number that makes it concrete |
| **Local remesh** | Absent, and topology-changing sculpting is a declared non-goal. **The review's half-agreement is the right one**: the non-goal is about not building dyntopo, and it does not answer what happens when a mesh-layer snakehook stretches triangles past usefulness. That is a recovery operation, not a sculpting mode | Medium; needs a decision before a proposal |
| **Surface conform / shrinkwrap** | Absent | Low-medium |

### Misframed, or a decision before an implementation

- **#15, automatic background consolidation.** The engine deliberately never
  bakes on its own: `clay_layer_field_report` reports the step scale and what
  costs it, and the host decides. The review wants that automatic, and it is
  the right instinct — *the sculptor cannot be expected to know what
  "consolidate" means*. But consolidation is destructive and undoable, so an
  engine that fires it on a background thread is mutating a document behind a
  host that may be mid-undo-group or mid-save. **The answer is probably a
  recommendation the host can act on with one call, not an autonomous action**,
  and the difference is a design decision worth writing down rather than a
  feature to build.

- **#14, a general Preview → Commit protocol.** Preview exists per operation —
  `move_surface_preview`, `lattice_gizmo_preview`,
  `mesh_lattice_displacement` — rather than as a protocol. Generalising it is
  attractive and would touch every destructive verb at once. Note the ROADMAP
  requirement it would finally satisfy: *"every destructive operation is
  preview-committed and undoable, including hide"*, which is currently the one
  competitor-bug requirement that is **not** met.

- **#16, a representation manager that picks SDF / voxel / mesh per stroke.**
  The most interesting idea in the review and the one to be most careful with.
  The representations are not interchangeable — they differ in what they
  GUARANTEE, not only in speed. An SDF layer is exact and non-destructive; a
  voxel grid is quantised; a mesh layer has fixed topology. An engine that
  silently moved a stroke from one to another would silently change what the
  document promises, and the host would have no way to explain the result to
  the user. A **policy the host can ask for a recommendation from** is
  buildable. A policy that acts on its own is a correctness hazard wearing a
  convenience label.

- **The 14 → 35 capability split.** Not adopted. Capability count is not a
  measure of coverage, and a split is churn unless a requirement has nowhere to
  live. The audit above found exactly one requirement in the wrong place and
  one requirement that was false — a reorganisation would have fixed neither.
  New capabilities are worth creating when a change needs one, which is how
  `surface-groups` may yet become the fifteenth.

### Revised priorities

Replacing the review's P0 list with what the audit supports. The two rows it
moves are the ones already shipped.

| | Item | Why here |
|---|---|---|
| **P0** | `add-mobile-thread-scheduling` | The handoff is now, and the pool declares no QoS class on the one platform where that decides whether the UI thread wins |
| **P0** | `add-history-budget` | Unbounded allocation in a multi-hour session on an OS that kills for memory |
| **P0** | `add-surface-groups` | The largest genuinely-absent workflow primitive, and the substrate for four more |
| **P0** | A version tag | The device gate is green on main at ABI 0.39.0 (#190, the first stamp under the median-of-three statistic). What is still missing is the TAG: without one the team's reports cannot be pinned to a build |
| **P1** | `add-operation-cancellation` | The third budget class has no exit. `mask_extrude` measures 4403 ms and `sdf_consolidate` 661 ms on the reference iPad, and a host can neither cancel one nor draw a progress bar for it — the threading rule forbids reading the document from another thread while it runs. Proposed 2026-08-23 after an audit found `cancel`, `progress` and `interrupt` in neither `clay.h` nor this file |
| **P1** | Procedural masks | Cheap on a field representation, high artist value |
| **P1** | `add-item-spatial-index` | Still the slope. Re-measure first: `CullIndex` already took the constant |
| **P1** | SDF sculpt layers (`add-sculpt-layers` 1.9) | Unblocked by scene groups landing |
| **P1** | Stroke input: azimuth, velocity, timestamp | Five floats, and cheapest before hosts depend on the current sample layout |
| **P1** | `add-field-stamps` | The review is right that this is a differentiator rather than parity, and right that a captured field can carry more than displacement |
| **P2** | Morph targets · generic attributes · instancing · radial symmetry · conform | Real, none blocking |
| **Decide, do not build** | auto-consolidation · preview/commit protocol · representation policy · local remesh · >256³ voxels | Each needs a written decision before it needs a proposal |

The review's closing criterion is worth adopting verbatim, because it is
testable and nothing here currently tests it end to end:

> A host using only ClayCore's public APIs must be able to load or create a
> character, work for hours across thousands of strokes, move between SDF,
> voxel and mesh, use masks, layers, symmetry and alphas, stay inside a frame,
> and save and reopen the document without semantic loss.

`reference/host_loop.py` is the beginning of that test. It covers the sequence.
It does not yet cover the **hours**, and `add-history-budget` is the first
reason to believe the hours are where it would fail.

## Deliberately not doing

Recorded so they are decisions rather than oversights:

- **Topology-CHANGING mesh sculpting** — dyntopo, multires, remeshing,
  subdivision (their LiveClay, ZBrush's dynamic tessellation). An SDF sidesteps
  topology entirely; competing on dynamic tessellation is not this engine's
  fight. **Amended 2026-08-14, not dropped**: this row used to say "mesh
  surface-mode sculpting", which was wider than the decision behind it. Moving
  the vertices that already exist is a different claim, and
  `mesh-fixed-topology-brushes` makes it; tessellating new ones stays out.
- **Mesh-level booleans (composition on a mesh layer).** Decided 2026-08-21,
  against. Composition here means being an operand in the SDF edit list —
  `compile_document` chains visible SDF layers, and a voxel or mesh layer is
  not in the tape at all — so "compose a mesh" means a destructive
  triangle-level boolean producing a new mesh.

  The NON-destructive version is already here and is better: parts kept, the
  relationship re-editable, watertight by construction, exactness tracked per
  node, cheap to re-evaluate. A mesh modifier stack re-runs a fragile boolean
  on every edit; there is no version of it that beats the edit list.

  The real advantage a mesh boolean WOULD have is worth stating precisely,
  because the intuitive answer is wrong. It is not attribute preservation: a
  boolean has to interpolate uvs onto the geometry it creates along the cut,
  which is the same barycentric interpolation `Bvh::closest` already does, and
  no boolean can invent uvs for faces that did not exist. It is that
  **geometry away from the cut stays bit-identical**. `Volume.from_mesh`
  resamples the whole model, so a 2 cm hole in a 2 m character costs the
  retopology everywhere; a mesh boolean only disturbs the neighbourhood of the
  intersection curve.

  Against that: it breaks the mesh layer's defining, test-enforced invariant
  (`indices` and `quads` byte-identical), so it could not be a verb; robustness
  — coplanar faces, degenerate triangles, near-coincident vertices — is where
  mesh tools lose users' trust; and it would spend the differentiator, since
  `docs/sculpt_comparison.md` positions ours as "watertight by construction"
  against Blender's "fragile on bad input" and this roadmap states that as an
  architectural guarantee. A second boolean path that is not watertight
  undercuts the first.

  **What would reverse this**, named so the decision can be re-examined rather
  than merely cited: the primary workflow shifting from sculpting to
  hard-surface kit-bashing on imported retopo assets. Then topology
  preservation away from the cut is the product and nothing substitutes. The
  tell is users importing meshes to COMBINE rather than to REFINE. The
  realistic route then is a dependency (Manifold, Apache-2.0, would pass the
  licence gate) rather than an implementation.

  `add-mesh-attribute-transfer` is the cheaper answer to most of what this was
  asked for — it gives back polypaint and uvs, and explicitly not topology.

- **A crease VERB on a voxel layer.** Scoped as `add-voxel-crease`, implemented,
  measured, and **removed 2026-08-21** — which is what its own task 1.1 was
  written to make possible: *"If they do not differ, the verb has no reason to
  exist and the answer is a documented recipe instead."* They did not differ.

  Stroked along a line, `sculpt_crease` and a plain `sculpt_inflate(-depth)`
  produced **identical** surface profiles. On a single dab the crease was
  measurably *shallower* than the plain erode — the squeeze was filling the
  groove back in.

  The reason is structural and does not go away with a better implementation.
  On a MESH the pinch moves *vertices* tangentially: it deforms a surface sheet,
  steepening the walls without adding material. On a voxel grid, moving material
  toward the groove centre puts material INTO the groove, because a lattice
  holds a volume rather than a sheet. Measured directly: cut a groove, then
  pinch, and the surface at the centre rises. **The operation that sharpens a
  mesh crease fills a voxel one.**

  What is left of DamStandard on a lattice is the depth profile, and that is the
  falloff — so the whole content of the verb is "erode with the right falloff",
  which `sculpt_inflate` already does. The recipe is in
  `docs/07-brushes-and-features.md`; the surprise in it is that a CONSTANT
  falloff is the right one, because a fractional weight is dithered per cell and
  a three-cell brush has too few cells for a smooth taper to average out.

  DamStandard remains available where it is meaningful: `Op::Incise` on an SDF
  layer, and `MeshBrush::Crease` on a mesh layer.

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

**Audited 2026-08-21**, because "worth writing into the specs" had been true
for months and nothing had been written. Each row now says whether it holds,
checked against the built library rather than against intent:

| Requirement | State |
|---|---|
| masks survive resolution changes and representation bridges | **Holds, structurally.** The mask lattice is addressed in WORLD units and sampleable at an arbitrary position, so a consumer at any resolution reads the same mask. Wants a scenario in `voxel-engine` |
| hidden is not deleted, and hidden state survives resampling | **Holds for a LAYER** — measured across a save and reload. Unreachable for a REGION, which is the case the competitor's users hit. `add-surface-groups` |
| import density is decoupled from object scale | Belongs in `file-io`; not audited here |
| brushes never touch ghosted or locked layers | **Holds, and further than expected**: protection refuses REORDERING too, checked before the operation rather than by it. Now pinned |
| symmetry centre is explicit, persistent and gizmo-edited, never implicit | **Holds.** The mirror plane is the layer's local zero, so the layer transform moves it, and it persists in the document. Now pinned |
| presets survive engine versioning (versioned schema) | `brush-engine` already requires it |
| every destructive operation is preview-committed and undoable, including hide | **DOES NOT HOLD, and is the only one that does not.** Preview exists per operation rather than as a protocol, and voxel and mesh edits are outside the undo vocabulary entirely |

Three are now written into `scene-model` by `correct-the-undo-scope`, with the
scenarios that make them testable rather than aspirational.

Two more the architecture gives us for free and should be stated rather than
assumed: smoothing cannot act across a gap, because blends are rigid and local;
and boolean results are watertight by construction.
