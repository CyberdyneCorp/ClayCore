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

## Where the engine is (2026-09-06, v0.87.0)

21 capabilities, 205 archived changes, 19 still open. Complete enough that the
gaps below are about *sculpting affordances*, not about the field engine — and
as of the 2026-09-06 reconciliation below, about what a HOST can reach rather
than about what the engine can do.

**Read the host's ranking before this file's own.** Every phase table here was
written from the engine's side. `## What the host actually needs — 2026-09-06`
near the end is ClaySpaceDesktop's, taken from a working application pinned to
v0.84.0, and it disagrees with the ordering above it in three places. Where they
disagree, theirs is the one with a shipping product behind it.

### Why this file went stale, and what fixes it

**The batch above — 205 archived, up from 119 — is the fix applied late.** The
changes had shipped and read 0 open tasks for weeks; none had been archived. A
complete change sitting in `openspec/changes/` is indistinguishable from an
in-flight one, so `openspec/specs/` described a library several ABI minors
behind the one in the tree, and every reader downstream of it read the same lag:
**eight of the twelve rows in "Real gaps, verified absent" below were presenting
shipped capabilities as gaps**, three Phase 4 rows named delivered work as
pending, and issue #243 was rewritten *twice* re-deriving work that had already
landed.

The failure was not that the work was undocumented — each change carried its own
proposal, design and tasks the whole time. **Archiving is what makes finished
legible**, and the rule it wants is: a change that reads 0 open tasks and has
shipped gets archived by the PR that finishes it, not in a later sweep.

**And the sweep proved its own point by happening twice.** Two branches
reconciled the same backlog on the same day without either knowing: this one
archived fourteen of the changes with their four stale deltas repaired, and
`chore/reconcile-roadmap-and-archive` archived those fourteen among a wider
eighty-six and landed first. Thirteen of the fourteen were duplicate work
thrown away at the merge, and the one that was not —
`record-the-layer-a-crossing-creates` — is still open here because the other
sweep left it so. That is the same defect one level up: an unarchived change is
invisible as *finished*, and an unfinished reconciliation is invisible as *in
progress*. Two lessons from the discarded batch are worth keeping anyway,
because they will recur: archive in **dependency order** rather than list order
(the resume chain has to go refill → colour → layers → per-brick → seed key,
because each modifies the requirement the one before it wrote), and expect
deltas that were never refreshed against their own base to abort the archive —
four of the fourteen named blocks `MODIFIED` against requirements no living spec
carries, or dropped scenarios their base still had, which a validator that
treats `MODIFIED` as a whole-requirement replacement would have deleted.

The older snapshot below is kept because its corrections are still
load-bearing. The bullets are the 2026-08-07 (v0.22.1) snapshot with corrections
kept in place; what landed between v0.22.1 and v0.30.0 is summarised at the end
of this section:

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
| `add-claycore-bridge` (ClayCore half) **seam decided and the ClayCore side landed 2026-08-24** | **This row was half wrong when written, and reading the other repository is what showed it.** It asked for "a field-evaluation callback so a baker can sample exact normals" — and `clay_eval_points` and `clay_eval_gradients` had shipped long before, so a baker could already do that. What was genuinely absent was everything around it: no ray could be BOUNDED (`clay_raycast` searches to infinity, so "look 5 mm along this normal" cannot be said and a miss is indistinguishable from a hit on the far side of the model — which is what puts garbage in bake seams), no ambient occlusion, no thickness, no per-point curvature, and no notion of a UV layout anywhere. **The seam is decided: their engine bakes, this one answers field queries.** Baking wants UV semantics — seams, islands, padding, dilation, texel density — which CyberRemesherAndUV owns, and a second implementation here would disagree with theirs about exactly the details that make a bake look right. **And the other half was already built there**: they have a `FieldEvaluator`, a `CyberFieldEvaluator` of three C callbacks, `cyber_bake_field` and a Python base — with no volumetric engine to plug in, so their `fieldSampledMaps` is always empty. ClayCore is that engine and needs nothing further; the correspondence is in `docs/08-mesh-readback.md`, **including the two traps** (their `occlusion` is OPENNESS where 1 is fully open and ours is occlusion where 1 is fully enclosed, so passing ours straight through bakes an inverted AO map that looks plausible; and `CLAY_MEASURE_CURVATURE` is a saturated [0,1] masking value while theirs is signed mean curvature in 1/length). |
| `add-sculpt-handoff-export` **landed 2026-08-24** | The other question this row implied — does CyberRemesherAndUV need anything we do not emit — answered by reading that repository rather than guessing, and the answer was **not "nothing"**. `docs/sculpt-handoff-format.md` defines a sculpt handoff, ships the READING half only, and records that agreement with ClayCore was outstanding because no negotiation ever took place; their CLI already assumed our half existed. **Two hazards their reader enforces that we would have violated, neither findable from this side:** `save_ply` declares a mesh's QUADS as its faces and they reject any other arity — so our BEST export was exactly the file it would refuse — and normals are required while a mesh meshed without gradients has none. Both are now the writer's guarantee. `material_mix` comes from a MASK, because a mask is already a painted scalar in [0,1] and ClayCore has no material slots to invent. **Verified against their actual CLI**, not our own parser: a ClayCore quad export retopologises to 708 quads with zero dropped faces, and the same mesh written with ordinary `save()` is rejected. |

## Phase 4 — parametric and scatter

**Reconciled 2026-09-06: three of the five had been delivered and this list
still named them as work.** Two of those three should never have stayed on it —
the lattice and the field stamps shipped changes ago — which is the same
archive-discipline failure the "Real gaps" table records below. Struck rows keep
their text and name what delivered them.

| Change | State |
|---|---|
| ~~`add-lattice-deformer` (FFD)~~ | **Delivered, on both representations.** `clay_item_add_lattice` places a cage over an item and `clay_layer_add_lattice` over a layer's node; `clay_layer_lattice_gizmo` / `_preview` drive it as a gesture; and the mesh side is the `clay_mesh_lattice_*` family — `_divisions`, `_set_offset`, `_offset`, `_rest`, `_position`, `_is_identity`, `_displacement` — with `clay_mesh_sculptor_lattice` applying it. **The row's own word "FFD" is where the header now routes**: an SDF deformer is an *inverse* point map and forward FFD has no closed-form inverse, so `clay.h` tells a caller wanting forward FFD with no approximation to use the mesh-layer lattice instead. That routing is the delivered answer, not a gap in it |
| `add-surface-scatter` (instances sampled on an isosurface) | **Still open, and now unblocked.** The instancing half it depended on landed (`instance-a-layer`), so scatter no longer has to duplicate geometry; nothing yet samples an isosurface for placements |
| `add-blend-profile-curves` (user-defined bevel cross-section, against their 2026 custom profiles) | **Still open.** `clay_profile` names the closed 2D profiles a *lift* primitive carries; a blend's cross-section is not authorable — `blend_k` is a radius and the extended modes ignore the profile outright |
| ~~`add-convenience-transforms` (snap-to-ground, centre-mass, zero-to-origin as single ABI calls)~~ | **Landed 2026-09-06 (ABI 0.86.0).** `clay_layer_snap_to_ground`, `clay_layer_centre_bounds`, `clay_layer_zero_to_origin`. **The middle name is a correction to this row, not a typo:** the engine holds no density, so an occupancy-weighted centroid is not merely more expensive, it is not expressible by `(doc, layer)` — a sampled centroid has no answer until someone names a cell size, and the call would silently depend on a resolution it invented. It centres the tight world BOX and is named for it |
| ~~`add-field-stamps` (capture a region's field as a reusable brush — the VDM analog, and a differentiator rather than a parity item)~~ | **Delivered as `stamp-a-captured-field` (ABI 0.83.0), archived 2026-09-05.** `clay_item_stamp_from_document` captures, `clay_item_stamp_save_memory` / `_load_memory` persist, `clay_item_stamp_content_id` identifies, `clay_stamp_frame_from_surface` orients one against a hit and its stylus azimuth, and `clay_layer_place_stamps` plants one at every stamp a resolved stroke produced |

## Phase 5 — the surface tier, proposed 2026-08-29

Five changes raised by four external implementation specifications
(`ClayCore_Sculpt_Engine_Gap_and_Roadmap`,
`ClayCore_Dynamic_Topology_and_Multiresolution_Implementation_Spec`,
`ClayCore_High_Frequency_Detail_and_Mesh_Sculpt_Layers_Spec`,
`ClayCore_Professional_Brush_and_Extreme_Poly_iPad_Implementation_Guide`),
audited against the tree on 2026-08-29 rather than accepted.

**The documents are technically sound and their reading of `main` is accurate**
— checked claim by claim: the flat `Mesh` and its quad invariant, `Adjacency`
over weld classes going stale on a count change, `MeshSculptor`'s pre-stamp
snapshot and local refit, `Bvh::refit` refusing topology change, meshoptimizer
behind `decimate`, `transfer_attributes` as attribute-only, `VertexDeltas`
holding no indices, `session::History`'s resolver inversion, the pure
`resolve_stroke`, the borrowed alpha, `kMaxSmoothIterations`, the voxel
sculpt-layer vocabulary, and the layering rule that lets `mesh` see
`{parallel, kernel, math, scene, eval, brick, field}`. Their central
architectural instinct — **do not make `MeshSculptor` topology-mutable; build
new representations beside it and share the brush kernels** — is the right one
and matches how this repository has taken every other representation.

**But they are a reversal of a standing non-goal, and they never say so.**
Dyntopo, multires, remeshing and subdivision are recorded twice below as
decisions, and once as a normative `SHALL NOT` in `openspec/specs/meshing`
("the library SHALL NOT re-tessellate to recover from a deformation …
remeshing remains outside this engine's scope"). A specification that
contradicts a shipped requirement is a change delta, not a plan. That is what
Phase 5 is: the reversal written down, with its trigger, and the requirement
modified rather than quietly outlived.

### What the audit corrected

Six of the documents' premises are stale, all in our favour, and each one moves
work out of the plan rather than into it:

| The documents say | Actually, as of 2026-08-29 |
|---|---|
| **Stage 0: finish the SDF P0** — prefix cache, lazy Smooth initialization, incremental C-ABI Smooth preview | **All three landed** (#371, `add-sdf-prefix-cache`), before the documents were dated. The programme starts at its item #1 |
| Face sets / polygroups **incomplete**, surface hide/isolation **incomplete (P2)** | `add-surface-groups` landed 2026-08-24: a world-lattice `GroupField`, grow/shrink/border, isolate, a `'GRUP'` chunk, undo, both bindings, and hiding that filters the produced mesh. **The proposed per-face `uint32 face_set` contradicts that design on purpose-built grounds** — groups are a world lattice precisely so they survive a representation bridge — so what is missing is group-aware *automasking*, not the group primitive |
| Curvature / cavity / normal **automasking incomplete (P2)** | The estimators shipped 2026-08-24 (`brush/procedural_mask.h`, and the per-point measure behind `add-claycore-bridge`). Evaluating them over a brush WORKSET landed with `add-shared-brush-kernels`, and it reused the estimator so a cavity mask and a cavity automask cannot disagree. **Closed by `add-shared-brush-runtime`**, which found the half of it nobody had checked: the composition ran on the fixed mesh and on the hierarchy above it, and not on the adaptive surface, which decoded the factors and dropped them. The row's remaining question is not the engine's — the automask is now reachable from a stroke only where a stroke engine exists, and there is no `brush::apply_to_dynamic` |
| Undo memory budget, and cancellation, proposed as new work | `add-history-budget` and `add-operation-cancellation` both landed 2026-08-24. `cancel()` is already the one call safe from another thread and the token carries progress the host POLLS |
| Advanced stroke stabilization **needs maturation** | Spacing, steady/lazy stroke, taper, deterministic jitter, buildup-vs-clamped, versioned presets and — since 2026-08-24 — azimuth, velocity and timestamp all ship. **The gap is a consumer, not the engine, and it is one line wide**: `resolve_stroke` puts azimuth into `Stamp::rotation`, `stamps_to_nodes` applies it to the node transform, and `apply_to_mesh` never reads it — so a rake or chisel brush is inexpressible on a mesh layer because the stamp's orientation does not reach the alpha's. **CLOSED, in two halves.** `add-shared-brush-kernels` made `apply_to_mesh` set `alpha_tangent` from `Stamp::rotation`, so the stroke's azimuth does reach the alpha. `add-shared-brush-runtime` adds the other half, which was not a consumer gap: `MeshBrushSettings::stamp_azimuth` turns the stamp's own in-plane axes about its facing — the BRUSH's grain rather than the STROKE's heading — read by all three mesh sculptors and carried by `BrushPreset` at format version 2, which is the only place an artist can put one because nothing in `brush` derives an azimuth from the direction of travel yet. What is left of the row is not stabilization: a stroke reaches the fixed mesh and the hierarchy (`apply_to_mesh`, `apply_to_multires`) and not the adaptive surface, because there is no `brush::apply_to_dynamic` |
| `io::MemoryBreakdown` should gain categories | The type is `io::MemoryReport` and it already separates essential content from history from rebuildable caches. It gains rows here; it is not built here |

One correction the other way, and it is the expensive one: the documents assume
a **five**-representation library (SDF, voxel, fixed mesh, dynamic surface,
multires) and cost only the new two. Every promise this repository makes in the
singular — one stroke engine, one mask model, one document, one undo history —
is a cross-product that grows with the representation count. That is the real
price of Phase 5 and it belongs in the proposal of every change below.

### The decision, and what would reverse it back

**Reversed 2026-08-29: topology-changing mesh sculpting moves from a non-goal
to a scoped programme.** The trigger is the one this file already named when it
audited the outside review: *the non-goal is about not building dyntopo, and it
does not answer what happens when a mesh-layer snakehook stretches triangles
past usefulness.* Fixed-topology brushes shipped 2026-08-14 and made that
question live rather than theoretical — the library now invites an artist to
sculpt a mesh and then hands them a stretch it calls "the signal the mesh wants
retopo", which is a signal to leave the engine.

Two things are NOT reversed, and stating them is what keeps the reversal
bounded:

- **The fixed-topology contract stands, unmodified, for `MeshSculptor`.** An
  imported retopologised mesh must still come back with `indices` and `quads`
  byte-identical. Adaptive topology is a DIFFERENT representation with a
  different invariant, chosen explicitly, never a mode the existing sculptor
  slips into.
- **Resolution stays an evaluation parameter on SDF layers.** The multires row
  below is about a MESH subdivision hierarchy, where resolution is fixed by the
  import and there is nothing to evaluate; the old non-goal's reasoning
  ("resolution is an evaluation parameter here, so multires has nothing to
  attach to") was always about the field and never held for a mesh layer.

**What would reverse it back**, so the decision can be re-examined rather than
cited: if the sculpting workflow settles on SDF blockout → voxel free-form →
quad export → *external* retopology, and mesh layers stay a refinement stop
rather than a place work is created, then two authoritative representations
have been added to serve a detour. The tell is a dynamic surface that is
converted to a mesh layer immediately after every session rather than lived in.

### The order, and why it is not the documents' order

The documents order the work topology → multires → layers → brush framework.
The audit moves the brush work FIRST, for a reason the documents themselves
supply and then do not act on: all three of the new sculptors are specified to
share extracted brush kernels, and extracting them from `src/mesh/sculpt.cpp`
is a refactor with an exact parity gate (the fixed-mesh results must not move
by a bit) — which is much easier to hold before two new callers exist than
after.

| Order | Change | Why here |
|---|---|---|
| 1 | `add-shared-brush-kernels` | **Landed** (#377). The prerequisite the other four assume. Representation-neutral kernels, a compiled per-stroke runtime plan, a reusable workset, brush frames and automasking over the workset, and a versioned `BrushPreset` so the artist-facing families (ClayBuildup, DamStandard, hPolish, TrimDynamic, Rake) become presets rather than engine paths. **Shipped zero behaviour change on the fixed path, which was the acceptance criterion.** Two items of this row's scope were NOT in it and are row 1b: the scratch arena, which was named here and never written, and "neutral", which was true of the kernels and not of the runtime around them |
| 1b | `add-shared-brush-runtime` | **Landed.** Row 1's residual, and it is a behaviour fix rather than a refactor: `DynamicSculptor` decoded `clay_mesh_brush_desc.automask` and never read it, so an automask an artist enabled was **silently absent** on the adaptive representation while the header promised "the same descriptor the fixed path takes". What shipped: `BrushScratchArena` (bump, one per sculptor, reset not freed — a warm stamp on a stable surface allocates nothing on any of the three), a 64-bit `WorkItemId` and a two-question `WorkItemTopology` so one automask serves three representations instead of three copies serving one each, `StampFrame` and `MeshBrushSettings::stamp_azimuth` (the grain a rake, a chisel and a rotated alpha are presets over), and parity suites on the adaptive surface and the hierarchy where only the fixed mesh had one. The neutral runtime stays in `mesh/` rather than the `brush/` the implementation guide proposes — `check_layering.py` records `brush -> mesh`, so the guide's layout is a cycle on the first include. The three golden `.inc` tables are byte-identical |
| 2 | `add-dynamic-topology` | A stable-ID mutable triangular surface beside `mesh::Mesh`, local split/collapse/flip under constraints, a chunked mutable spatial index, a local remesher driven by brush-relative detail, sparse topology undo, and a dirty-chunk C ABI. The largest single row in the file |
| 2b | `add-voxel-remesher` | **Landed.** The GLOBAL counterpart to row 2's local one, and a different operation rather than a bigger version of it: a whole surface sampled into a signed narrow-band field at an explicit world voxel size and rebuilt from it — overlaps fused, open surfaces closed under policy, the result validated watertight, the cost preflighted, cancellation and a typed refusal for everything that can go wrong. It is an INTEGRATION change and not new mathematics: the BVH, the generalized winding sign, the sparse sampled field, the watertight marcher, the validator and the attribute transfer all existed; what did not was the operation that composes them and owns the decisions between them. Its one piece of new engineering is the sampling domain, which follows the source's surface and band instead of the bounding box the existing converter walks. Slotted here rather than after row 3 because it depends on nothing rows 1 and 2 build, and because a stretched or kitbashed surface wants a global reset before it wants a subdivision hierarchy |
| 3 | `add-mesh-multires` | **Landed.** A deterministic Catmull-Clark hierarchy with detail stored in a transported local frame, sculpt level independent of display level, local low→high propagation. Ordered after dynamic topology because the free-form construction stage feeds it, and because both want the same chunk runtime. What shipped: `mesh::MultiresSurface`, `mesh::MultiresSculptor` over the fixed sculptor rather than a second copy of it, blocked-sparse fp32 `DetailField`, per-level preflight that refuses over budget rather than allocating half, base-patch changed-block transport, `project_surface` with `transfer_attributes` untouched, a `Multires` undo kind, and a versioned encoding that prices a declared depth before building it. It is a STANDALONE handle like `DynamicSurface` — no `scene::Layer` owns one, and `io::document_memory` therefore does not see one; the accounting is `MultiresSurface::memory()`, per surface. Region-scoped levels (the mesh analogue of `add_level_region`) are deferred and the reason is recorded in the change |
| 4 | `add-mesh-sculpt-layers` | **Landed.** Non-destructive detail passes on the multires detail representation — the mesh answer to what voxel sculpt layers already are, and the second of three representations to get them. The instruction held: a layer's coefficients ARE `DetailField` coefficients, in the same transported frame at the same block size, so `E(n) = B(n) + SUM s_i * m_i * L_i(n)` composes one representation instead of reconciling two. What shipped: `mesh::SculptLayerStack` with stable 64-bit ids that are never vector indices, a sparse per-layer mask distinct from the brush gate, base deformation layers at level 0 over the cage's rest frames, `mesh::LayeredMultiresSculptor` as a begin/stamp/commit/cancel transaction that pins its channel per dab and holds the composition, height and tangent-space vector `stamp_detail`, three smoothing modes plus `erase` and `restore`, `SculptLayerDelta` and `SculptLayerProperty` as two new `session::History` kinds — so layer PROPERTY changes are undoable, which the voxel stack still does not do — surface version 2 with version 1 still loading as a hierarchy with no layers, the C ABI, pyclay with a cancelling context manager, and `examples/69_mesh_sculpt_layers.py`. Three decisions worth carrying: additive displacement **commutes** where voxel layers replay cell writes and do not, so reordering is organisation and not geometry — enforced rather than assumed, since a reorder invalidates no block and float addition does not *associate*, so composition sums a block's contributors in **layer-id** order and a drag stays free; merge-down and bake are defined by **visual parity** rather than by concatenating coefficients, which divides by the lower layer's strength and is undefined at zero; and there is no memory cap, because a cap that stops recording leaves the pass on the surface and un-dialable. Colour layers were ruled out of scope — colour blends and blending does not commute |
| 5 | `add-extreme-poly-runtime` | **Implemented on `feat/extreme-poly-runtime`** (v0.78.0, PR #423, cut from main at a44b1f5 and stacked LAST of the three; rows 1 and 4 merged first as #419 and #417, and this branch is merged onto both — 31 conflict hunks over 17 files, of which four did not compile after a clean three-way merge, so the branch carries the resolution rather than a rebase). Chunk revisions, dirty-chunk transport, memory profiles and pressure trim, subdivision preflight, and the scaling gates that make "a dab costs what it touches" testable at 1M–20M vertices. Last because it optimises an architecture rather than compensating for a missing one — and first to be pulled forward if an iPad build stalls. What shipped: `mesh::ChunkTable`, ONE chunk unit under the fixed mesh, the adaptive surface and every multires level — the multires chunk id is `(base patch, quadrant at depth d)` so a base patch, which quadruples per level, is still a fixed SIZE and still the only identity subdivision preserves; four revisions (topology, geometry, normals, attributes) so a stable-topology dab re-uploads positions and not an index buffer; an epoch-marked dirty set; `clay_surface_view` as ONE caller-owned transport over all three representations beside the two that already shipped, with the whole-surface path kept as the correctness reference the tests reconstruct against; a `memory` leaf module (`budget`, `capacity`, `scratch`) with its own `check_layering.py` entry; a HOST-filled `SculptMemoryProfile` with no device detection anywhere in the portable core; `trim(pressure)` in the published eviction order with `memory::MemoryPin`; five checked-arithmetic preflights that refuse on the PEAK before allocating; and `mesh::MaintenanceQueue`, which a host services between gestures and in which the normal flush is the one item that is not optional. **The chunk size is 128 faces because a matrix over 64/128/256/512/1024 was run here** — the number is not adopted from prior art, and the one place the measurement falsifies the rule that chose it is recorded in the change's `design.md` D2a. Gated rather than claimed: locality, allocation (in BYTES as well as counts, because a gate that counts touches cannot see an O(surface) read), preview and memory-pressure all run as ordinary tests, and each was proven by reverting the mechanism and watching the gate fail. Measured: 200x the vertices at the same touched region is 0.92–1.00x the dab on P50, with an IDENTICAL gathered workset from 100k to 20M. Two defects it found and fixed: a cache generation that moved on BUILD and not on RELEASE, so a memory warning landing between two dabs of a drag silently lost every second dab; and a short chunk buffer reported as `CLAY_ERROR_INVALID_ARGUMENT` rather than `CLAY_ERROR_BUFFER_TOO_SMALL`, which tells a host to stop retrying the one call it should retry. Not done, and why: no reference iPad on the development box (7.8); the hierarchy's query path still SCANS when no caller supplies a seed, so 3.1 stays open even though 3.2 shipped the seed that avoids it; per-stage timing is six stages of fourteen, the eight inside `MeshSculptor::stamp` deliberately left un-instrumented while two branches edit that file; and the `multires with layers` benchmark rows are scripted but unrun |
| 5 | `add-extreme-poly-runtime` | Chunk revisions, dirty-chunk transport, memory profiles and pressure trim, subdivision preflight, and the scaling gates that make "a dab costs what it touches" testable at 1M–20M vertices. Last because it optimises an architecture rather than compensating for a missing one — and first to be pulled forward if an iPad build stalls |

### What the documents leave out, and this file requires

Four repository gates appear in none of the four specifications, and every one
of them has already caught a shipped feature here:

- **Both bindings, gated.** Three capabilities in a row landed reachable from
  no host. Every change below carries the C ABI *and* pyclay, with
  `tools/check_binding_parity.py` green.
- **A numbered example that renders and asserts.** The gallery is how a
  sculpting feature gets inspected; a benchmark is not an example.
- **Determinism, stated per requirement.** The mesh verbs promise bit-identical
  positions on every run and every platform, and the voxel dither is hashed on
  a cell coordinate to keep that promise. A topology operator ordered by hash
  map iteration breaks it silently. Dyntopo's determinism contract has to be
  written into its requirement, not assumed from the CPU being single-threaded.
- **The version lines.** Every ABI-growing change touches `CMakeLists.txt`,
  `bindings/c/clay.h`, `pyproject.toml` and `release_check.py` together, and
  the two most recent releases both shipped with them out of step.

Two overlaps with existing rows, named so nothing is built twice:
`add-field-stamps` (Phase 4, currently the highest-value unstarted item) is the
FIELD-space VDM analog and `add-mesh-sculpt-layers` carries the tangent-space
image one — they are two mechanisms for one artist verb and should share the
stamp vocabulary. **The mesh half has now landed and went first**, so the
vocabulary exists to be read rather than negotiated:
`include/clay/mesh/detail_stamp.h`, `clay_detail_stamp_desc`, planar and
borrowed image data, placement and sampling through `kernel::calpha_frame` and
`kernel::calpha_sample` — the same two the scalar alpha already uses — and an
over-resolved map reported rather than silently blurred. A field stamp should
differ in what it writes and in nothing else. And the mesh multires level stack should read
`clay_voxel_add_level_region` first: the voxel side already refines over a
region with watertight transitions as a construction, which is the property the
mesh hierarchy will be asked for next.

## Phase 6 — the asset, the region and the stack, proposed 2026-09-03

Three changes raised by `ClayCore_Field_Stamps_Regional_Multires_Layer_Boolean_Implementation_Guide`, audited against the tree rather than accepted — the same discipline Phase 5 used, and it moved work out of the plan again.

| Order | Change | Why here |
|---|---|---|
| 1 | `stamp-a-captured-field` **landed 2026-09-05** | **Smaller than the guide describes**, because three of its four pillars already exist: `PrimType::Volume` compiles through the tape, the Node holds a volume by `shared_ptr` so "a thousand uses of one 4 MB asset must not consume ~4 GB" is already true, and `clay_item_volume_from_document` already captures a finite world region with redistance. What is missing is an ORIENTED capture frame (today's region is world-axis-aligned), an asset IDENTITY with a standalone form, a placement helper on `calpha_frame`, and stroke integration. First because it is the smallest and touches nothing the other two need |
| 2 | `refine-one-region-of-a-hierarchy` **landed 2026-09-05, three residuals named** | Transition polygons for EXPORT are not built, cross-level neighbours are absent so smooth/relax/normals are gated across a transition, and brushes across a transition wait on both. **The host does not need any of the three** — it exports no hierarchies — and names a different multires gap as its rank 2: a `.clayspace` carries no hierarchy and the engine reports a hierarchy's layer as a MESH layer, so a host's side-car is the only record that a row ever was one. See the host section. Originally: the gap `add-mesh-multires` recorded in its own row. Depth becomes a property of a base patch, with 2:1 balance in stable patch-id order, transitions watertight by construction rather than by repair, and refinement monotonic in v1 — removal needs a policy for the detail authored there, and picking one silently is worse than not offering it. Reuses the extreme-poly chunk identity; adds no second table |
| 3 | `fold-the-layers-with-an-operator` **not started (0/27) — now P0** | **Ordered last here and first by the host, and the host wins.** It is the only open row that changes what an application built on this engine can ship: a subtool IS a layer there, so a subtractive item does not reach the workflow and what ships instead is a resolved boolean that stops tracking its operands. Also where the intersect-drag measurement belongs — `BoundedByLayer` poses per item exactly the question a non-union layer fold poses per layer. Last, and the audit sharpened the reason. The inter-layer hard union is not one line in `compile_document`: it is **eight sites**, including `compile_document_part`'s "the union to fold them with is a HARD Add … anything else is a different field" and the brick refill's own multi-layer fold in `clay_c.cpp`. A layer fold that is not a hard Add breaks the multi-layer resume unless each is taught the operator, and the failure is SILENT — a refill folding wrongly returns a field that never existed. The change decides what each site does before writing any of them, and leans toward REFUSING the split on a non-union fold because it is the only option that cannot be quietly wrong |

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

- ~~**A mask was a FOURTH representation with no history mechanism.**~~ Closed
  by `masks-in-the-history` (#245). Twenty mutating ABI entry points, zero
  command variants — the audit behind `correct-the-undo-scope` counted three
  mechanisms and did not count the thing that has none. It surfaced while
  building `unify-the-undo-history`, forced a caveat into TWO shipped features
  (undo stopped counting at a mask edit; a journal could not recover past one),
  and both caveats are now deleted rather than reworded.

  Worth keeping: **it needed a different mechanism from voxels, and assuming
  otherwise is what made it look small.** `VoxelGrid::set` is the one choke
  point every voxel verb funnels through; a mask's `invert`, `clear`, `expand`,
  `contract` and `smooth` write chunk data directly, and only `fill` and
  `invert_within` go through `set`. A sink there would have recorded two
  mutators and silently missed five. The choke point that holds is `touch()`,
  which the header documents and an existing test already walks every mutator
  for — so a step snapshots on the first touch and diffs when it closes.

- ~~**Every format was buffer-shaped in the engine and path-only at the
  boundary.**~~ Closed by `serialize-without-a-file` (ABI 0.42.0). `clayspace`,
  `obj`, `ply`, `fbx` and `glb` all have byte forms in `include/clay/io/` and
  the file entry points are wrappers over them; only the wrappers crossed. That
  is a host-seam gap rather than a feature one, and it is where this library is
  pointed: an iPadOS host receives documents from a document provider behind a
  security-scoped URL whose lifetime it does not own, a syncing host needs bytes
  to send, a host with its own container needs bytes to store, and a WASM build
  has no filesystem. All were paying for a temporary file. Sibling to
  `report-mesh-quality` and found the same way — the engine had it and the
  boundary discarded it. It also unblocks crash recovery, which wants to append
  serialized commands to a journal and so needs bytes rather than a path.

- ~~**The mesh validation report was two bits of eleven.**~~ Closed by
  `report-mesh-quality` (ABI 0.41.0). `mesh::ValidationReport` computes eleven
  quantities and `clay_mesh_validate` returned `watertight` and `manifold`, so
  a host could be told an export was bad and never told why — while the
  `meshing` spec's own scenario has always said the validator "reports the open
  edge loop". Sibling to the output-descriptor family above and found the same
  way: this one was bounded correctly and simply never carried its fields.

  The sharper half is that the **sampled self-intersection pass had never run
  outside this repository**. The engine has always taken a cap; neither binding
  ever passed one, so both took the default of 0, which skips it — and
  `ValidationReport::clean()` requires `intersecting_pairs == 0`, which is what
  an unrun pass leaves behind. So `clean` read as clean on a self-intersecting
  mesh. The report now echoes the cap it was given, which is the only thing
  separating "none found" from "none looked for". `mesh::signed_volume` and
  `mesh::surface_area` crossed with it as `clay_mesh_measure`; they were
  declared in the same header and reached neither binding, and they are the
  first `double`s in `clay.h`, because a signed volume cancels heavily and
  narrowing it at the boundary would discard the precision the engine chose.

- **A smooth GROUP's reported extent omits its own blend ring.** Found and
  measured while building `fold-the-layers-with-an-operator`, deliberately not
  fixed there. A group's `tape.bounds` is the plain union of its children, so a
  smooth or extended group combine bulges past the box the tape reports — the
  same defect the layer fold had until that change added
  `scene::chain_blend_support`, where reverting the one line left 11,618 lattice
  samples carrying material outside the reported box (missing surface in a mesh,
  a lost ray hit in a preview). **The reason it could not be fixed in place is
  the interesting half:** `resume` unwinds group frames from a
  `TapeCheckpointFrame` that carries op, blend and rounding and NO EXTENT, so a
  ring added in `compile_group` lands in a full compile and not in a resumed one
  — implemented, and `test_tape_prefix_reuse.cpp`'s group-append case went 0.2
  short in x on every dab. Closing it means giving the checkpoint the subtree's
  extent, which is a schema change to the resumable checkpoint. Pinned meanwhile
  by a test asserting the layer form's box contains the group form's and exceeds
  it by exactly one ring.
  **Latent rather than live for the one host we can check:** ClaySpaceDesktop
  creates no item groups at all — `clay_layer_add_group` and
  `clay_layer_add_item_in_group` have no wrapper and no call site anywhere in its
  workspace, so every item it adds goes to the layer root. The path it WOULD have
  come down is its own: `place_layer` refills the union of a layer's extent either
  side of a move, so an extent missing a blend ring leaves surface unmeshed where
  the old form stood. That is why the layer fold's widening landing first is the
  right order — the correctness half arrives before the feature that would expose
  it.

- **Bounds NARROWED per operator, on both the item and the layer path.**
  `fold-the-layers-with-an-operator` widens a fold's extent by its own support,
  which is the half that can lose surface. It does NOT narrow: a subtract is
  still bounded by the union rather than by its left operand, and an intersect by
  the union rather than by the intersection, exactly as the ITEM path has always
  been. Narrowing one side alone breaks that change's own parity gate — a
  subtracting layer and a subtracting item are the same document — and narrowing
  both changes the meshing region of every document that already carries a
  subtract or a paint, and has to be threaded through `compile_group`'s rollback
  and every resumable entry point that copies a prefix's bounds. Its own change,
  with its own measurement.

- **Deformers on a mesh layer.** `Deformer` has twenty-one entries and every
  one applies to an SDF item; a mesh layer takes a lattice cage and nothing
  else, so ZBrush's Deformation palette — Taper, Twist, Bend — is unreachable
  on the representation an artist holds after a retopo pass or an import.
  Scoped by `add-mesh-deformers`. Worth recording why it is cheaper than it
  looks: an SDF deformer must run BACKWARDS, which for free-form deformation
  has no closed-form inverse (the SDF lattice accepts ~1.5% error and a 4³ cap
  for it), while a mesh deformer runs FORWARDS once per vertex and inherits
  neither. It is the same math in the easier direction.

- **A stroke is walked segment by segment on every sample.** Confirmed in the
  kernel rather than inferred: `ctape_stroke_dist`
  (`include/clay/kernel/tape.h:488`) loops `for (i; i + 1 < count; ++i)` over
  every segment for every sample, with no early-out and no spatial structure —
  **O(control points) per sample, unconditionally.** A mirrored stroke pays it
  twice, because each mirror copy is its own `emit_item_instance` with its own
  placement.
  Measured by ClaySpaceDesktop on a live snake-hook pull, one segment of the
  gesture, timing each step against the bricks it dirtied: mirrored, 96 bricks
  at 0.95 ms with a 6-point curve and 96 bricks at 3.77 ms with a 39-point one —
  **the same brick count by every measure the host controls, four times the
  cost.** Per brick, 0.0099 → 0.0392 ms mirrored and 0.0069 → 0.0177 unmirrored.
  Not a defect: walking the segments is the ordinary implementation and it only
  becomes visible when a host grows ONE item to tens of points during a live
  gesture, which a snake hook does and the primitive was not shaped for.
  **The tractable fix is an early-out rather than a tree.** A smooth blend
  forbids skipping a far segment outright — `csmin_quadratic` accumulates from
  everything within `k` — but a per-segment bound is enough: a segment whose
  bounding sphere is further than the running `d` plus `k` cannot change the
  result, so the test is sound and costs one distance per segment. A BVH over
  the curve is the version that flattens it entirely and is a real piece of work.
  Not scheduled. **Recorded because a host is choosing a workaround against it**
  — chaining several shorter stroke items instead of growing one long one, which
  only became possible once their taper was anchored to arc length — and that
  workaround stops being worth considering the day this lands, so its status is
  worth their knowing either way.

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
| **P0 "Sculpt layers"** | Landed on **two** representations — record a pass, dial its strength, reorder, merge down. Voxel layers first; `add-mesh-sculpt-layers` then put the same stack over a multiresolution hierarchy's detail, where the arithmetic is additive displacement rather than dithered occupancy, so a fractional strength is an exact fraction and a reorder changes nothing at all. The mesh side also carries the two things the voxel one still owes: layer property changes inside the undo history, and a mask per layer. The SDF side remains one open DECIDE in `add-sculpt-layers`, no longer gated on anything | `clay_voxel_*_sculpt_layer*`, `clay_multires_sculpt_layer_*` |
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

**Reconciled 2026-09-06, and eight of the twelve rows had shipped.** Each was
re-checked against the tree rather than against this file, because the file was
the thing that had gone wrong: the changes that closed them had been sitting
complete-but-unarchived, so `openspec/specs/` still described a library several
ABI minors behind the one in the tree and every reader downstream of it —
this table and the issue tracker alike — kept re-deriving finished work. The
struck rows below name what shipped them. Four rows survive whole — morph
target, generic named attributes, voxels beyond 256³ and surface conform — plus
the SCATTER half of the instancing row; each of the five was confirmed absent
from `bindings/c/clay.h` and `include/` on the same pass that struck the others.
Radial symmetry was already struck before this reconciliation and is left as it
was.

| Gap | What is actually there | Severity |
|---|---|---|
| ~~**Surface groups / PolyGroups / Face Sets**~~ **shipped** | ~~Nothing, on any representation. Visibility is per LAYER; a layer holds exactly ONE mask (`clay_document_add_mask` "replaces any mask the layer already had"), so N named regions cannot even be emulated with N masks; scene groups group edit-list NODES, not surface~~ **`add-surface-groups` (landed 2026-08-24) built the primitive as a world-space `clay_groups` lattice — `clay_groups_fill` / `_at` / `_reassign` / `_grow` / `_shrink` / `_border` / `_ids` / `_cell_count`, a `'GRUP'` chunk and undo — and `mask-a-named-region` (ABI 0.85.0) closed the loop with `clay_mask_fill_from_group` beside the existing `clay_groups_fill_from_mask`, so a group is a SELECTION every mask-respecting verb already honours.** | ~~Highest~~ **Closed.** One thing the row did not anticipate is still open and is now a design proposal rather than a gap: the lattice quantises a group BORDER, so the border is not an edge set. `native-mesh-polygroups` answers the nine locking questions and deliberately writes no code |
| ~~**Partial visibility**~~ **shipped** | ~~Layer-only. "Hide the armour" requires the armour to have been authored as its own layer, decided before the artist knew~~ **Same change, as predicted: `clay_groups_set_visible` / `_isolate` / `_show_all` / `_invert_visibility` / `_any_hidden` / `_point_hidden`. Hiding filters the produced MESH rather than the field, so it is exactly reversible and cannot change what the document evaluates to** | Same change — it was the same primitive |
| ~~**Unbounded undo history**~~ **shipped** | ~~`std::vector<Entry> undo_` with **no cap, no byte accounting, no eviction, no query**. The only control is `enable_undo`, which is a light switch. And the expensive entries are counter-intuitive: the stack stores INVERSES, so removing an item records a whole `Node` (440 bytes plus its deformer chain) while adding one records 8 bytes~~ **`add-history-budget` (landed 2026-08-24): `clay_document_set_history_budget`, `clay_history_bytes` and an on-demand trim with a horizon a host can show. The counter-intuitive costing was confirmed rather than corrected, and `roll-up-document-memory` then found `node_bytes` six members behind the type it walks — so the budget had been measuring LOW, and lowest on exactly the documents where it matters** | ~~High, and now urgent~~ **Closed** |
| ~~**Procedural masks** — cavity, curvature, normal, thickness, AO~~ **shipped** | ~~Mask verbs are paint, fill, expand, contract, smooth, invert, `to_field`. Nothing derives a mask from the surface~~ **`clay_mask_from_surface` over `clay_surface_measure`, reachable through `add-claycore-bridge`. The mask is one CALLER of the per-point measure rather than a second implementation, so a cavity mask and a baked map cannot disagree about the same surface. It repeated `add-surface-groups`' mistake first: curvature, cavity, convexity and normal-direction shipped in C++ with tests, no C entry point and no pyclay** | ~~High~~ **Closed** |
| **Morph target** | Absent as a named feature, and **closer than it was**: a base deformation layer at level 0 of a multiresolution hierarchy is a stored, dialable, blendable set of vertex offsets against a rest pose, which is most of what a morph target is minus the naming and the multi-target blend. The word still appears only in `mesh_io.h`, as a glTF feature deliberately not imported | Medium; the remaining work is a vocabulary over `add-mesh-sculpt-layers` rather than new storage |
| ~~**Stroke input completeness**~~ **shipped** | ~~`clay_stroke_sample` is position, pressure, tilt. No **azimuth**, no velocity, no timestamp~~ **A SECOND entry point rather than a widened one: `clay_stroke_resolve` takes a FLAT `count*5` float array, so repacking in place would have changed the stride under every compiled host. `clay_stroke_sample_full` carries azimuth, velocity and a `double` timestamp, `clay_stroke_resolve_full` consumes it, and the older call is sugar over it. pyclay needed neither, because a numpy array carries its own shape** | ~~Medium~~ **Closed.** Azimuth was the one that unlocked a capability rather than refining one: tilt says how far the stylus leans, azimuth which way, and `clay_stamp_frame_from_surface` now rotates the tangent about the normal by it |
| ~~**Radial symmetry**~~ | Three mirror planes, no radial — and the two were asymmetric in DESIGN, not just in coverage: the mirror is a layer mode with a seam blend and a per-item opt-out, while radial existed only as `Repeat::radial`, a per-item modifier a stroke cannot reach. Scoped and built: `add-radial-symmetry`, issue #256 | Medium |
| ~~**Instancing**~~ **shipped** / **surface scatter** still absent | ~~Absent. Phase 4 already names `add-surface-scatter`; the review is right that scatter without instancing duplicates geometry~~ **The instancing half landed as `instance-a-layer` (`.clayspace` minor 15) with `bound-an-edit-across-instances` behind it: `clay_document_instance_layer` is a second layer over the same edit list, so a duplicated subtool costs a layer record. Shared is the edit list and ONLY the edit list; `clay_layer_node_influence_bound` reports the union over every sharer so a host does not leave nine of them stale. The review's premise is therefore satisfied — scatter no longer has to duplicate geometry.** `add-surface-scatter` itself is still absent: nothing samples an isosurface for placements | Medium, and now unblocked rather than blocked |
| **Generic named attributes** | `colors`, `uvs`, `normals` and nothing else. A host cannot carry `material_id` or a custom channel through the engine | Medium. The review is right to separate this from PBR: allowing an app to carry channels is not the same as rendering them, and only the second is a declared non-goal |
| **Voxel beyond 256³** | Real, and re-verified 2026-09-06: `CLAY_MAX_BATCH` is still `16777216 /* 1 << 24 */`, which is 256³ exactly, so a grid's `dims` product must be ≤ that. The spec's "at least 256³" reads as a floor and is also the ceiling | Medium — already recorded under "Deferred, but recorded", now with the number that makes it concrete |
| ~~**Local remesh**~~ **shipped** | ~~Absent, and topology-changing sculpting was a declared non-goal.~~ **Decided 2026-08-29 and built: `add-dynamic-topology`, archived 2026-09-02, with `add-voxel-remesher` and `remesh-through-the-document` beside it. The decision the row was waiting for is the one that held — a recovery operation is NOT enough, because the same local split/collapse/flip that repairs a stretch is the whole of Dyntopo minus the policy that drives it, so it was scoped as the sculpting mode rather than as a repair bolted to the fixed-topology layer** | ~~Medium; needs a decision before a proposal~~ ~~P0 of Phase 5~~ **Closed** |
| **Surface conform / shrinkwrap** | Absent | Low-medium |

### Misframed, or a decision before an implementation

- **#15, automatic background consolidation. SETTLED 2026-09-06, and built as
  the recommendation rather than the action** — `advise-a-consolidation`,
  `clay_layer_consolidation_advice` (ABI 0.86.0), `Layer.consolidation_advice`
  in pyclay.

  **The reasoning this bullet carried is unchanged, and it is why the decision
  went against the review.** The engine deliberately never bakes on its own: consolidation is destructive, it discards
  the parameters of everything it absorbs, and an engine firing it on a
  background thread mutates a document behind a host that may be mid-undo-group
  or mid-save. An engine deciding on an artist's behalf that a sphere's radius
  is no longer editable would be making the wrong person pay.

  What was genuinely missing was the other half, and the bullet above named it
  without naming the cost: `clay_layer_field_report` told a host it *should*
  bake, and the next call it needs takes a `clay_consolidation_params` whose
  `cell_size` is required and `> 0`, with that field's own comment saying why
  nothing will guess it. So the engine was telling a host to bake and then
  making it invent the one number it has no basis for — a constant compiled into
  the app, or a slider put in front of the sculptor the review correctly said
  cannot be expected to know what consolidate means. `clay_layer_consolidation_advice`
  fills the struct, and the design is worth two lines here because it refutes
  the obvious implementation twice:

  - **It is not merely a params helper.** `*out_advises` is 1 only when the
    field report advises at the caller's threshold AND the *projected*
    `safe_step_scale` reaches it. A sampled volume declares `sqrt(3)` times its
    samples' Lipschitz, so a consolidated layer's step scale is at best
    `1/sqrt(3) = 0.577`; a host whose frame budget wants 0.8 is asking for
    something no bake can deliver, and handing it params would trade a
    parametric layer for a dense volume and still miss the budget. It is told 0,
    and NOT ADVISED MEANS ZEROED, so a host that ignores the flag gets
    `CLAY_ERROR_INVALID_ARGUMENT` from the destructive call rather than a bake
    at a resolution nobody chose.
  - **The declared Lipschitz was rejected as the source of the resolution**, and
    this is the finding worth carrying forward. `clay_field_report.lipschitz`
    bounds the step a marcher may take; it does NOT bound `|grad f|`. An
    ellipsoid declares 1 and measures 1.09 near its tips, 3.6 for a needle, and
    `taper`, `wrap_around` and `bend_curve` exceed their declared factors
    outright — so a sampling rate derived from it would look principled and be
    unsound exactly on the shapes that motivate a bake. The cell size comes from
    the layer's own extent and its finest content instead — four cells across
    the smallest feature, clamped to `[E/512, E/32]`, measured on a 0.06 dab
    where the surface moves 27% of the dab's radius at 2 cells, 7.0% at 4 and
    1.8% at 8 — and the Lipschitz enters the *advice* as `sample_lipschitz`,
    measured on the samples a real bake produced.

  Still open, deliberately: a REGION-scoped advice. This advises a whole layer,
  and `clay_layer_consolidate_region` is what a sculptor working one area
  actually reaches for — the advice has no way to say "at this box".

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

### What landed 2026-08-23 to 2026-08-24, and what it cost to find out

Rows that had no entry here at all, recorded so the file is not read as if this
work were still pending.

| Change | What it is, and what building it settled |
|---|---|
| `unify-the-undo-history` | Three history mechanisms and no step spanning two. Closed by a `session` module above scene/voxel/mesh, because `check_layering.py` forbids `scene` from seeing the other two. **The finding worth carrying forward:** `voxel::MaskField` was a FOURTH representation — twenty mutating ABI entry points, zero command variants — which the audit that counted three did not count. Masks record now. |
| `report-mesh-quality` | The validator measured watertightness and manifoldness and threw the numbers away at the ABI. A host could learn a mesh was bad and not why. |
| `serialize-without-a-file` | Save and load through memory rather than a path. An iPad host holding a document in a `Data` had to write a temporary file to save it. |
| `survive-a-crash` | Snapshot plus an append-only journal. **The headline was wrong and measurement corrected it:** "a journal is cheaper than re-saving" is false for voxel-heavy edits — the journal is raw 14 B/cell while the document RLE- and palette-compresses. Measured 507 B against 3595 B for three ordinary edits (7.1x cheaper) and 7189 B against 590 B for one big fill (12x WORSE). The rule became "re-snapshot when the journal grows past the snapshot", which is the opposite advice in the case that matters. **Finished 2026-09-06 with the pairing check and the barrier's missing caller — see below.** |
| `roll-up-document-memory` | iOS asks what a document costs and every subsystem accounted for itself while **nothing rolled up**. The breakdown is the feature, not the total: under pressure a host needs to know WHICH PART, since that decides what it may release. **It also found a real defect:** `node_bytes` was six members behind the type it walks — it missed the armature binding, three profile arrays, a lattice cage, and both `shared_ptr<FieldVolume>` members, typically the largest thing a node owns by two orders of magnitude. So `add-history-budget` had been measuring LOW, and lowest on exactly the documents where a budget matters. |
| `add-sculpt-handoff-export` | See the Phase 3 table above. |

#### Pairing a journal with its snapshot — landed 2026-09-06 (ABI 0.86.0)

**The journal carries the snapshot's identity, and a mismatched pair is a typed
refusal with nothing applied.** What was at stake is that without the check the
wrong pair does not fail, it *succeeds*: commands name layer ids, two sessions
of the same shape allocate the same ones, so an `AddNodeCmd` applies happily to
a document that already holds that work, and voxel events are written by
absolute cell coordinate onto a grid that never had them. The test measures it —
replaying onto the wrong snapshot left the SDF layer holding **two nodes where
the snapshot had one**, silently.

- **Computed over the serialized bytes** (`io::snapshot_identity`), stamped by
  `io::save_clayspace` / `io::load_clayspace` so a host gets it by writing the
  ordinary recovery path. Content, not a session token: two snapshots with the
  same bytes *are* the same snapshot, and a token would refuse a pair that
  recovers perfectly.
- **Keyed on the journal INDEX, not "the last thing serialized" — the first
  design was wrong here.** The documented rule is *re-snapshot when the journal
  grows past the snapshot*, and a host sizing that comparison by serializing
  again would have had a journal it already took repointed: refused against the
  snapshot it kept, and **accepted against the second image, where the events
  apply twice**. `journal_since(from)` now names the newest snapshot at or
  before `from`.
- **An empty segment names no snapshot** — found by an existing pyclay test
  going red: asking below the trimmed floor turned a documented no-op into
  "wrong snapshot".
- **Mismatch returns `CLAY_ERROR_SNAPSHOT_MISMATCH` (= 10, appended)**, distinct
  from `CLAY_ERROR_INVALID_ARGUMENT` because they mean opposite things to a host
  (discard the file vs. find the right snapshot), and it is the **only
  all-or-nothing refusal replay has** — the identity is in the header.
- **One-directional**: journal format 1 → 2, and version 1 is still read as
  "names no snapshot". Refusing it would have made the first launch after an
  upgrade discard exactly the recovery file a crash just left.
- **Cost, measured**: byte-at-a-time FNV-1a is 1.36 ms on a 1.13 MB snapshot
  against the 1.52 ms save that produced it — **90% on top of every save**, paid
  by hosts that never journal. Word-wise: **0.24 ms, 15%**. That is what shipped.
- **Stated as not-promised in `clay.h`**: it does not catch replaying the same
  journal twice (the indices do), it is not a checksum, and a never-serialized
  document names no snapshot.

**It carried code twice, and the second time was a defect this change surfaced
rather than one it introduced.** Implementing the `c-abi` scenarios found the
**barrier had lost its last caller**: `record_barrier`'s only caller was the
mask step, and masks-in-the-history correctly removed it — so no host-reachable
operation recorded a barrier, and `clay_voxel_drop_level` (the header's and
`docs/05`'s own example) journaled nothing. A replay across one rebuilt a grid
that still held the dropped level *plus every edit after it*, with no flag. It
records one now in both bindings. And rule 1 of the design ("taking the journal
tells the host") had no entry point at all — `clay_document_journal_barrier` /
`Document.journal_barrier` were added for it.

**One thing worth generalising from all of it.** Three capabilities in a row —
surface groups, procedural masks, and the first half of the memory work — were
reported landed while reachable from NO HOST: implemented in C++, tested, and
absent from `clay.h` and `pyclay_module.cpp`. A passing test suite does not
show it. The check that does is a sweep of every public header against both
bindings, and it now runs as a task in any change that adds a capability.

### Revised priorities

Replacing the review's P0 list with what the audit supports. The two rows it
moves are the ones already shipped.

| | Item | Why here |
|---|---|---|
| **P0** | `fold-the-layers-with-an-operator` | **Raised to P0 on 2026-09-06 by the host that consumes this engine, and it is the only open row that changes what they can ship.** A subtool IS a layer there, so a subtractive ITEM does not reach the workflow; what they ship instead is a resolved boolean that stops tracking its operands the moment one moves. Still 0/27 and still gated on its own decision task 0.1. See the host section for where its measurement belongs |
| ~~**P0**~~ **P1** | `add-mobile-thread-scheduling` | **Demoted 2026-09-06 at the host's request.** The handoff is now and the pool still declares no QoS class, but the P0 was written for the iPad and the desktop host says QoS classes and performance-core sizing buy it nothing. It stays open for the mobile reason and stops blocking a desktop release. **The threading ask that replaced it is `add-mesh-sculptor-off-thread`**: `clay_mesh_sculptor_create` is a weld and an adjacency pass, 160 ms over 296,216 triangles, on the interface thread, with no other route to a mesh layer's surface — see the host section below |
| ~~**P0**~~ | ~~`add-history-budget`~~ **landed 2026-08-24** | Unbounded allocation in a multi-hour session on an OS that kills for memory. Bytes, a budget, on-demand trim, and a horizon a host can show. **What building it found:** the expensive entries are the inverses of REMOVALS, since the stack stores inverses — deleting an item records a whole node while adding one records an id, so a session of deletes and a session of adds cost very differently and nothing told the host which it was in |
| ~~**P0**~~ | ~~`add-surface-groups`~~ **landed 2026-08-24** | The largest genuinely-absent workflow primitive. **Landed TWICE, and the first time did not count**: the lattice, `isolate` and the visibility flags shipped with tests and were reachable from no host at all — no C entry point, no pyclay, no serialisation, and the mesher never asked, so hiding a group hid nothing. The second half is what made it a workflow: grow/shrink/border, a `'GRUP'` chunk, undo, both bindings, and geometry that actually disappears. Hiding filters the produced MESH rather than the field, so it is exactly reversible and cannot change what the document evaluates to |
| ~~**P0**~~ | ~~A version tag~~ **caught up 2026-08-25 (v0.52.0)** | The oldest row on this list, closed twice: v0.49.0 took the ten-minor backlog and v0.52.0 takes the three that landed after it. **What both bumps found is the same defect, and it is worth a gate rather than a habit:** the minor is bumped in `CMakeLists.txt` by the feature that grows the ABI, and `clay.h` and `pyproject.toml` are left behind — v0.49.0 shipped from a tree whose header and wheel said 0.48.0, and v0.52.0 from one where both said 0.49.0 against a CMake at 0.52.0. `release_check.py`'s `version` row is the only thing that catches it, and it catches it at tag time rather than at merge time |
| ~~**P0**~~ | ~~`unify-the-undo-history`~~ **landed 2026-08-23** | Three history mechanisms and no step spanning two — `correct-the-undo-scope` found it and only wrote it down. Closed by a `session` module above scene/voxel/mesh, because `check_layering.py` forbids `scene` from seeing the other two. Three things the plan got wrong, all caught by tests: consolidate IS undoable (the barrier examples were consolidate and rasterize, and both are recorded), the cell sink first journaled writes that changed nothing and a unit test enshrined it, and `UndoStack::begin_group` pushes its entry at BEGIN so grouped edits recorded no step at all. **The finding worth carrying forward: `voxel::MaskField` is a FOURTH representation** — twenty mutating ABI entry points, zero command variants — which the audit that counted three did not count. Still open: VOXEL sculpt-layer property changes, and masks. The mesh stack closed its half in `add-mesh-sculpt-layers` by registering two kinds through the existing resolver inversion rather than a fifth resolver, which is the shape the voxel side should copy |
| ~~**P1**~~ | ~~`add-operation-cancellation`~~ **landed 2026-08-24** | The third budget class had no exit: `mask_extrude` measures 4403 ms and `sdf_consolidate` 661 ms on the reference iPad, and a host could neither cancel one nor draw a progress bar — the threading rule forbids reading the document from another thread while it runs. `cancel()` is now the one call in the library safe from another thread, and the token carries progress the host POLLS rather than a callback the engine fires. A cancel is a DISCARD: the document is byte-identical afterwards, so a host never has to undo one. **The constraint that shaped it:** `parallel_for`'s join waits on `done >= num_tasks` and increments only after `fn` returns, so a cancelled chunk must return normally and never throw, or the join hangs forever |
| ~~**P1**~~ | ~~Procedural masks~~ **landed 2026-08-24, reachable 2026-08-24** | Cheap on a field representation, high artist value — and it repeated surface groups' mistake exactly: curvature, cavity, convexity and normal-direction shipped in C++ with tests, no C entry point, no pyclay, and no change folder. `add-claycore-bridge` closed it, and moved the measure to a per-point form with the mask as one of its callers, so a cavity mask and a baked map cannot disagree about the same surface |
| **P1** | `add-item-spatial-index` | **Measured 2026-08-24, and the implementation was REVERTED rather than shipped.** A median-split BVH made `plan()` 590x faster and the whole thing 2.4x SLOWER: build 2.584 -> 9.228 ms at 50 000 items against a query saving of 0.14 ms. The query really did become sublinear — per-item cost fell from a flat 2.8 ns to near zero across a 300x range — it is simply the smaller term. **The ratio that decides it is BUILD-TO-PLAN, and it is 1:1**: the index is cached on the document revision, every stamp bumps it, and `CullPlan` exists so one cull serves every brick in a dab. No tree amortises against that. The only remaining direction is incremental insertion, so the build is paid per EDIT rather than per document; the tests that would guard any index shipped without one.

**Re-read 2026-09-02 against a fixture that grows in EXTENT, and the earlier measurement was taken on the wrong axis.** Every SDF benchmark fixture — `sculpted_sphere`, `pole_dense_sphere`, `deep_sphere`, `spread_sculpt` — grows a unit sphere's DENSITY, so a dab's cull region keeps the same FRACTION of the model and survives a flat 28.3% of the items at 2 000, 10 000 and 50 000 alike. There `plan` is ~3% of a dab's cull and the per-brick compiles over its survivors are the other 97%, so the fastest imaginable broad phase wins 3% — which is how a 590x query landed inside a 2.4x slower operation. On dabs at a FIXED SPACING over a growing sheet, survivors stay at 36 and `plan` is 89% of the cull at 50 000. `BM_CullPlanLocal{10000,50000}` (`pack-the-cull-scan`) is that axis, and it is the row a broad phase has to flatten; the density rows cannot show one either way. **The BUILD-TO-PLAN argument above survives intact** and is now the harder of the two: `append` (`append-the-cull-index`) already pays the build per edit, and a prototype of the dynamic tree this row would need adds +0.140 ms to the `append_cached` copy at 50 000 against a query saving of 0.137 ms — so the copy alone can eat the whole win, and the guide's "measure this separately" is the decision, not a footnote. Two more numbers from that prototype, both against its own guide: unbalanced insertion of a fixed-spacing sheet gives height 450 at 50 000 (build 107 ms), so the rotations are not optional; and even balanced, building by repeated insertion costs 7.9 ms against the 2.6 ms full `CullIndex` build it would replace, so a bulk builder belongs in the first PR rather than a later one. **`pack-the-cull-scan` landed the cheap half meanwhile** — folding the constant clauses out of the survive test and packing the boxes it reads — for 5.3x on `plan` at 50 000, which lowers the constant and leaves this row's slope exactly where it was.

**Re-decided 2026-09-02 after that landed, and the answer is NO on the extent axis too.** Lowering the constant by 5.3x took most of what a broad phase was left to win. On the extent fixture with a 24-brick dab, `plan` is now 0.027 ms at 50 000, 0.102 at 200 000 and 0.272 at 500 000, against a whole-dab cull of 0.098 / 0.171 / 0.343 ms — so a query that answered instantly would save a quarter of a millisecond at half a million items, and the prototype's copy tax alone (+0.140 ms at 50 000, +0.905 at 200 000, both against `append_cached`'s copying branch) exceeds the saving at every size measured. The row is not blocked on engineering; it is blocked on there being nothing left to win. **Where the time actually went is the DENSITY axis, and no spatial index reaches it**: the same 24-brick dab at 50 000 items spends 15.0 ms in the per-brick compiles and 0.43 ms in the plan, because 21 633 items genuinely survive the batch cull and 9 634 genuinely reach one 8³ brick. `reject-a-brick-without-the-node` takes 1.7 ms of that by deciding the per-brick cull from the cached entry instead of the node behind it; the remaining 11.7 ms is real emission, and shrinking it is a blend-radius, LOD or consolidation question rather than a culling one |
| **P1** | SDF sculpt layers (`add-sculpt-layers` 1.9) | Unblocked by scene groups landing. The host confirms it is an ASYMMETRY rather than a blocker: voxel rows carry a stack of passes and hierarchy rows carry one, SDF rows carry none, and those sit next to each other in a layer stack. Worth taking if it is cheap as a weighted group; it is not worth taking ahead of a layer operator |
| ~~**P1**~~ | ~~Stroke input: azimuth, velocity, timestamp~~ **landed 2026-08-24** | And the row was right that it was cheapest now: `clay_stroke_resolve` takes a FLAT count*5 float array, so widening the packing in place would have changed the stride under every compiled host — a second entry point taking a real struct array instead, with the older call as sugar. pyclay needed neither, because a numpy array carries its own shape. **Azimuth is the one that unlocks a capability rather than refining one**: tilt says how far the stylus leans, azimuth says which way, and without it a rake or chisel brush is not expressible at all |
| ~~**P1**~~ | ~~`add-field-stamps`~~ **landed 2026-09-05 as `stamp-a-captured-field`** | The review was right that this is a differentiator rather than parity. It was also the row this file called "the highest-value unstarted item" for a week after it shipped, which is the failure mode a roadmap has: three of its four pillars already existed, so the change was an oriented capture frame, an asset identity with a standalone form, a placement helper on `calpha_frame` and stroke integration — Phase 6 row 1, and smaller than the guide describing it |
| **P2** | Morph targets · generic attributes · ~~instancing~~ (`instance-a-layer`, landed) · ~~radial symmetry~~ (`add-radial-symmetry`, landed) · conform | Real, none blocking |
| **Decide, do not build** | ~~auto-consolidation~~ **decided AND built 2026-09-06, as advice** · preview/commit protocol · representation policy · ~~local remesh~~ **decided 2026-08-29, Phase 5** · >256³ voxels | Each needs a written decision before it needs a proposal. Two now have one. Local remesh's made it the second row of Phase 5 rather than a recovery operation bolted to the fixed-topology layer; auto-consolidation's held the engine to never baking unasked and shipped the recommendation instead — `advise-a-consolidation`, `clay_layer_consolidation_advice`. Note which way that decision cuts: the decision was to keep the autonomy OUT, and what the change added was the number a host could not otherwise invent |

The review's closing criterion is worth adopting verbatim, because it is
testable and nothing here currently tests it end to end:

> A host using only ClayCore's public APIs must be able to load or create a
> character, work for hours across thousands of strokes, move between SDF,
> voxel and mesh, use masks, layers, symmetry and alphas, stay inside a frame,
> and save and reopen the document without semantic loss.

`reference/host_loop.py` is the beginning of that test. It covers the sequence.
It does not yet cover the **hours**, and `add-history-budget` is the first
reason to believe the hours are where it would fail.

## What the host actually needs — 2026-09-06

Everything above this line was written from the engine's side. This section is
ClaySpaceDesktop's, collected on 2026-09-06 from the session that maintains it,
against a pin at v0.84.0 with its full suite green (2235 tests) and 29 of the
engine's newly added entry points called by nothing. It is the first ranking in
this file that comes from an application rather than from a study, and it moves
three rows.

**Their one-sentence verdict:** `fold-the-layers-with-an-operator` (#321) is the
only open row that changes what they can ship. Everything else is latency,
symmetry, or not theirs.

### The ranking, and what each row costs them

| | Row | What the host says |
|---|---|---|
| **1** | `fold-the-layers-with-an-operator` | **A subtractive LAYER, not a subtractive item.** Their unit of "a thing an artist grabs and moves" IS the layer — a subtool is a layer — so an item-level cutter does not reach the workflow at all. What they ship instead is an honest RESOLVED boolean: each operand is sampled into a volume, the two are combined into a subtool of their own, and moving an operand afterwards does not update the result. The interface says so rather than implying otherwise, and the operands are kept so it can be re-run. Their own roadmap has said since the subtools work that the same vocabulary upgrades to a live boolean the day this lands, with no interface change |
| **2** | ~~A `.clayspace` does not carry a multires hierarchy~~ **LANDED** (`persist-a-multires-hierarchy`, ABI 0.88.0, container minor 19). An `'MRES'` chunk carries a mesh layer's hierarchy keyed by layer id, `clay_layer_multires_present` answers what a row is without a side-car, and `clay_layer_multires` hands the loaded hierarchy back as a borrowed handle. The host's either/or asked for the document carrying it OR a `LayerRepresentation`; the first was taken, and no such type was invented — a chunk keyed by layer id answers the question, and an enum describing payload presence would have been a second source of truth. Two things the change found: the cage exists TWICE and is not reconciled (a hierarchy is built from a mesh value and keeps no link back, so the two could always drift — `clay_layer_multires_matches_cage` makes it observable rather than preventing it), and the cost follows AUTHORED DETAIL rather than level count, so an untouched four-level hierarchy adds 128 bytes while a sculpted one adds 1.2 MB. Original: | **Not the transition polygons this file ranked.** They do not export hierarchies, so `refine-one-region-of-a-hierarchy`'s export residual does not bite them. What bites is one level up: a hierarchy row is TWO objects on their side — a mesh layer holding the cage, and a `clay_multires` beside it — and because the engine reports a hierarchy's layer as a MESH layer, with no `LayerRepresentation::Multires`, their side-car file is the only thing in the world that knows a row was ever a hierarchy. Lose the side-car and the sculptor's levels are gone and the row returns as the flat cage it demonstrably is. They made the loss loud in three panels and in a diagnostics report; loud is not fixed. **The ask is either the document carrying the hierarchy, or a `LayerRepresentation` that says what the row is** |
| ~~**3**~~ | ~~`add-mesh-sculptor` off the interface thread (their #368)~~ **premise already false, 2026-09-01** | The threading ask they DO have, and it is not the mobile one. `clay_mesh_sculptor_create` cannot be built off the interface thread: it is a weld and an adjacency pass, **160 ms over 296,216 triangles**, and a mesh layer has no other route to its surface because the pick after an activation is answered by `clay_mesh_sculptor_raycast`. Holding a sculptor per mesh took the repeated cost out; the FIRST weld of each mesh has nowhere to go. The call resolves its mesh through a mutable path into the document, and the ABI's only threading contract is the brick cache's. Either that contract extended to this call, or a split between an off-thread adjacency build and a cheap adopt |
| **3b** | Reuse a mesh's adjacency across sculptors, keyed on topology revision | What actually remains of #368 once the threading half is struck. Two sculptors over one mesh each build their own adjacency, and a rebuilt layer discards it. A different ticket from the one filed, and a smaller one |
| **4** | SDF sculpt layers (`add-sculpt-layers` 1.9) | Not blocking, and a visible asymmetry: voxel rows carry a stack of recorded passes and hierarchy rows carry one, SDF rows do not, and in their layer stack those sit next to each other. A user asks why; the answer is "the engine doesn't". Take it if it is cheap as a weighted group |
| **5** | `add-mobile-thread-scheduling` | **Drop to P1.** They are desktop. QoS classes and sizing a pool from performance cores buy them nothing, and they are not asking for "the host owns the pool" either. The P0 was written for the iPad handoff and should say so |
| **6** | `add-claycore-bridge`'s normal/AO map bakes | **Do not hold the roadmap for them.** They do no map bakes, bring no UV layout, and are not waiting |
| **7** | `native-mesh-polygroups` | Not theirs to ask for. Their own design record says polygroups are absent from ClayCore by deliberate decision and they planned around it. Build it for someone else's reasons |

### Claimed done here, not reachable there

The gate this file has relied on for "shipped reachable from no host" is a host
telling us. It told us three things.

- **Automask is unexercised.** They send `Automask::default()` and offer none of
  the five factors, so the capability whose absence this file recorded twice
  (surface groups, procedural masks) is confirmed inert from the one host you
  would expect to use it. Two of its factors — cavity and surface-group — do not
  cross the ABI at all, which v0.84.0's notes already say.
- **`clay_document_mesh_layer_revision` does not move when history replaces a
  layer's triangles**, which is the one moment it exists for. Measured on 0.73.0:
  a layer attached at revision 1 and rebuilt to revision 2 comes back to its
  original 119,100 triangles under undo and to the rebuilt 37,752 under redo,
  **at revision 2 throughout**. A sculptor who rebuilds, undoes and keeps working
  gets a refused stroke on the next dab. They hold the gap as a failing-when-fixed
  equality in their own suite and work around it by recording the engine depth
  each rebuild sits at
- **#451's residual**, below.

### #451 is 21% recovered, not closed, and the residual is `BoundedByLayer`

Measured on their `object` bench group, cuda backend, 1280x800, four runs, on a
QUIETER box than the campaign that filed it (0.14 load per core against 0.30 with
a runaway process):

| | v0.73.0 | v0.78.0 | v0.84.0 |
|---|---|---|---|
| subtracting (the control) | 25.49 ms | 25.82 ms | 25.46 ms |
| intersecting | 57.35 ms | 66.84 ms | 64.81 ms |
| ratio | 2.25x | 2.59x | 2.54x |

2.03 ms of a 9.49 ms regression came back. The control is flat across all three
pins, which is what makes the row readable.

**The fix was real and the fixture still regresses, and both are true because
they measure different things.** #454/#459/#461 fixed the layer extent BOUND
QUERY — 0.0669 ms a frame to 0.0003, 200 layer walks to 2 — and a fix worth
0.067 ms was never going to account for 9.49. The residual is the REGION, and it
is in `src/scene/bounds.cpp:1270`: an intersect item's influence bound is
`Nonlocality::BoundedByLayer`, the whole layer's extent, because `max(acc, item)`
can take material away anywhere the layer already occupies. `node_reach_bound`
returns that, `node_command_bound` unions it over the layers sharing the content,
and a refill dirties it. A subtract is `op_is_local` and dirties its own box. So
a dragged intersect re-evaluates and re-meshes the whole layer every frame and a
dragged subtract does not.

Their fixture is the worst arrangement for that bound and also the ordinary one:
`Scene::Reference` is ONE SDF layer holding a sphere plus 96 stamps, and
`place_object` routes a placed cutter to the ACTIVE layer, so the cylinder
(r=0.25, h=1.6, `CLAY_OP_INTERSECT`) sits at the root of the very layer that
holds the form, with no group above it. An artist places a cutter on the form
they are cutting; that is where placing puts it.

**Measured 2026-09-06 on a 10x scene, and the cost is a PRODUCT of two slopes.**
Same 12-frame drag, same fixture, both scenes holding the SAME 97 items and
differing only in extent (radius 1.0 against sqrt10 — ~10x surface, ~31.6x
volume):

| case | refilled bricks/frame | surface bricks | refill ÷ surface | ms/frame | µs/brick |
|---|---:|---:|---:|---:|---:|
| reference subtract | 535 | 1,209 | 0.4x | 3.51 | 6.56 |
| reference intersect | 5,040 | 192 | **26.2x** | 45.50 | 9.03 |
| 10x subtract | 535 | 10,536 | 0.1x | 11.94 | 22.30 |
| 10x intersect | 84,672 | 351 | **241.2x** | 7501.42 | 88.59 |

**The intersect walks a BOX, not a band.** It refills 26x the surface bricks of
the geometry it produces at reference size and 241x at 10x, and the ratio grows
with radius: the dirty region is the layer's AABB and the refill visits the
bricks of that VOLUME rather than the bricks that hold band. Brick count grows
16.8x for a 31.6x volume.

**Per-brick cost also grows, and a 2x2 settled what it is: NOT extent.** The
first reading — a second, extent-driven slope — was retracted by the host that
found it, within the hour, because its subtract control had a confound: the
cutter does not scale while the form does, so the same 6,424 bricks are
near-surface in one scene and deep interior in the other. The matrix that
separates the three variables (radius, dab size, cutter placement), intersect
rows, µs per brick:

| variable | held | varied | result |
|---|---|---|---|
| **Extent** | dab 0.18, cutter on the surface | r=1 → r=√10 | 9.95 → **9.15 µs**, 0.92x — FLAT |
| **Item overlap** | r=√10, cutter on the surface | dab 0.18 → 0.569 | 9.15 → **114.21 µs**, 12.5x |
| **Brick population** | r=√10, dab 0.18, brick count pinned at 741 | cutter on the surface → buried | 13.01 → **57.79 µs**, 4.4x |

**A larger document does not make a brick cost more.** The whole per-brick story
is what is IN the brick: how many items overlap it (a fixture whose dabs scale
√10 against a fixed 0.16 brick edge takes a dab from spanning ~2.2 bricks to
~7.1) and whether it is a rim brick that culls the document away or an interior
brick that culls nothing.

**So box-versus-band is the whole story, in two factors rather than one.** On the
realistic configuration — r=√10, dabs as the fixture builds them, cutter on the
surface so nothing is confounded — the intersect costs 779x its subtract control:
136x in brick COUNT and 5.7x in per-brick cost. A tighter region reaches both,
because the bricks a band walk stops visiting are precisely the expensive ones.
**And count matters on its own:** with the overlap effect entirely removed, 88,200
bricks at 9.15 µs is still 806 ms a frame, so a fix that only made bricks cheaper
would leave a 0.8-second frame.

**`resumed_bricks` is ZERO on every transform-driven refill measured**, with the
seed store at 1.0 MiB of a 64 MiB budget, so the budget is not what switches the
fast path off — it was never on. This is CORRECT and by design: a gizmo drag is a
transform edit, not an append, so `forget_appends()` / `forget_resume()`
(`bindings/c/clay_c.cpp`, `touch_regions` and its structural and frontier
siblings) retire every seed each frame. **The resumable path is the STROKE fast
path; a drag has no seed to resume from and never did.** The drag fast path is
`clay_layer_placement_begin/_update/_commit`, which is a LAYER gesture — and an
item dragged inside a layer, which is what this fixture does, has no fast path at
all today. Worth stating plainly because two separate readings of "the resume
stopped working" are both wrong.

The open question stays open: a moved intersect's FIELD changes layer-wide, but
its ZERO SET only moves where surface can appear or disappear, which for a drag
is bounded by the union of the old and new position. What would kill it is a
brick that holds band away from both positions and changes.

Filed as **#471** (this) and **#472** (`clay_document_mesh_layer_revision`, the
revision that does not move when history replaces a layer's triangles —
re-verified on v0.84.0 before filing).

**This belongs to `fold-the-layers-with-an-operator` as well as to #451**, and it
is where that change's measurement should be taken. A layer fold that is not a
hard union puts every layer through the same question an intersect item already
poses per item.

### Readers, names and transforms — the small ABI gaps a host works around

- **A colour reader is still absent.** `#317` gave them
  `clay_layer_node_transform`, `_transform_nonuniform`, `_params` and
  `_op_blend`, and their `objects` sidecar table is being deleted as a
  workaround for a limitation that no longer exists — **minus the colour
  column**, which stays because `clay_layer_set_color` is write-only. This is
  the fourth setter without a reader and the one still open (`read-a-placed-node`
  named it and deliberately did not build it).
- **A voxel grid is reachable only by name.** `clay_document_voxel_layer` takes a
  string, so two layers sharing a name shadow each other's grid and a stroke
  lands on the wrong one. Harmless with one grid; a scene of subtools is exactly
  where two layers come to share a name. They derive a unique default name on
  every path that can create one, because a collision made AFTER the fact
  shadows a grid just as surely.
- **A carried mesh layer's transform is not applied to its triangles.** A layer
  transform is composed for a mesh exactly as for a field, but a carried mesh's
  triangles are the engine's own vertex arrays and the tape evaluates nothing for
  that layer — measured, a mesh subtool moved five units along X drew its first
  vertex where it drew it before. Every crossing between world space and those
  vertices is the host's, or a subtool is drawn in one place and sculpted in
  another.

### A C-ABI host cannot choose the format minor it writes

`clay_document_save` takes a path and `clay_document_save_memory` takes a blob.
Neither takes a version, and no other save path does either: the minor is a
parameter on the C++ `scene::serialize_document` and does not cross the ABI.
**Three releases of upgrade notes have told hosts to "write at the older minor if
you exchange documents with an older build", and no C-ABI host has ever been able
to take that advice** — the host records it beside its own `FORMAT` constant for
15, 16 and 17.

It cost nothing while every downgrade lost only what no artist authored; 16 → 17
loses payload deduplication and nothing else. **Minor 18 is where it stops being
free**, because a subtractive layer written at 17 comes back as a union — a
cutter returning as a lump welded to the form, in a file that opens cleanly. So a
host that wants an interchange copy cannot offer one, and a host that wants to
REFUSE that downgrade has nothing to refuse, because it could never ask.

`fold-the-layers-with-an-operator` settles the half that is its own (design.md
§7): writing below 18 refuses a document carrying any non-default composition
rather than degrading it, stays byte-identical for documents where every layer
unions, and ships a query so a host can ask before it saves. **The selector is
the gap that remains** — a save-at-minor entry point, with the blob variant, the
autosave and journal paths, and the other lossy minors in scope. Its own change,
and the host named the reason it has to be: **an autosave writes a whole document
on a timer and a crash journal writes one on the way down, and neither would want
a different answer from the interactive save.** A change about interchange sees
all three paths; one bolted to a boolean operator would be shaped by whichever
document raised it.

**And it carries a coupling it must not miss.** `serialize_document` expresses
its refusal as an EMPTY VECTOR. `clay_document_save` reaches it through
`io::save_clayspace`, which calls `serialize_document` with the default minor —
always the current one — so **the refusal is unreachable across the C ABI today,
for exactly the reason above: a host cannot choose the minor.** The two gaps
cancel.

The day the selector lands they stop cancelling. A host calls it with 17, the
refusal returns emptiness, and unless the entry point translates emptiness into a
RESULT CODE the caller gets `CLAY_OK` and a file that is not its document — every
layer of the host behaving correctly, the sculptor told the save succeeded. **A
refusal expressed as emptiness cannot survive a result-code boundary, because
emptiness is not a result code.** So the selector owes a distinct code for "this
document cannot be written at that minor", in the same commit as the selector.

### Where a host cannot draw a progress bar or cancel

`add-operation-cancellation` shipped the token and the poll, and twelve of their
commands still hold the interface thread behind a busy cursor with no fraction
and no cancel: Undo, Redo, New, Open, OpenRecent, Save, SaveAs, RunImport,
InsertMesh, RunExport, CopySubtool, RunBoolean. Their job model already carries
`Progress { label, fraction: Option<f32> }` where `None` means "the job cannot
say"; for these twelve it is not a job at all, it is one blocking call. Two are
worth a contract:

- **CopySubtool: 4.3 s** on the reference form, the whole of which is the
  sampling an INSTANCE LAYER would not do — instance layers are specified with no
  constructor (their #364). Four seconds with a cursor and no way out.
- **Undo: 87 ms alone, 203 ms after a released solo** on a three-subtool
  document, and every millisecond of the difference is a hop paying a whole-layer
  refill.

### A fat swept-curve stroke meshes with pinholes

Reported 2026-09-06 by ClaySpaceDesktop, found by accident while fixing an
unrelated brush, and **it reproduces through `clay_document_mesh` on the same
document** — three independent paths (their incremental patching, their full
rebuild, and our own mesher) count the same holes, which is what says it is ours.

Six snake-hook tendrils on a sphere, background pixels enclosed by surface, with
the gesture, the brush, the path, the `PointType::Spline` and the stroke blend k
all fixed and only the tendril's RADIUS PROFILE varying:

| taper span | tip radius | pinholes |
|---:|---:|---:|
| 100 (no taper, uniformly fat) | 0.120 | 3 |
| 8 | 0.066 | 2 |
| 5 | 0.050 | 0 |
| 3 | 0.050 | 0 |

Monotonic in thickness, and the fat cases are 3.3 to 6 voxels across on a 0.02
grid — **the opposite end from a thin-feature-below-the-grid problem.** Two
pixels in one place at 1280x800, so a small artifact rather than a broken
surface.

**One hypothesis ruled out already, and it was the first place today would have
looked:** it is not the cull pad. `clay_document_mesh` meshes `doc->tape()`, the
whole-document tape, with no cull region — so no per-brick culling is involved
and the shortfall class this change spent the day on cannot be it. That leaves
the swept-curve field itself or the mesher's crossing detection, most plausibly
where a fat sweep's consecutive segment spheres overlap heavily under a smooth
blend.

**Latent for a long time, and hidden by a bug on the host's side:** their old
taper made every tendril thinner than the failing range by accident, so fixing
that defect uncovered this one. They have tuned their taper span to 5 to stay
inside the range that meshes cleanly and written the measurement into the
constant's doc comment, so the next person knows it was chosen against an
artifact rather than by eye. **That is tuning around an engine bug and they say
so**; the fix belongs here.

**STANDING, and the repro now exists and has been run HERE: the mesh has no
holes in it.** `/tmp/claycore-swept-curve-pinhole.clayspace`, 4154 bytes,
container minor 17, six fat tendrils on the starting sphere — produced through
the host's real brush at a taper span of 100, saved, and re-opened on a build
carrying none of that brush code, which is the check that makes it a document
rather than a process.

Loaded through pyclay on this tree and meshed at the resolution the host used,
and at two more:

| resolution | V | F | Euler | watertight | 2-manifold | slivers (area < 1e-9) |
|---:|---:|---:|---:|:---:|:---:|---:|
| 96 | 75,640 | 151,276 | **2** | yes | yes | 150 |
| 128 | 134,716 | 269,428 | **2** | yes | yes | 343 |
| 192 | 303,562 | 607,120 | **2** | yes | yes | 1,342 |

**Euler characteristic 2 is a topological sphere.** No tunnel, no handle, no
boundary — a pinhole you can see through would drop it by two and does not. The
mesh is watertight and 2-manifold at every resolution tried, so **whatever the
host counts as background pixels enclosed by surface, it is not a hole in the
surface this engine produces.**

What that leaves, in the order I would look: geometry thin enough to fall
between samples in a rasteriser (the sliver count grows with resolution, and
`min_area` is 7.6e-12 at 96 and 1.3e-13 at 192 — triangles far below a pixel),
the host's own render path, or its pixel-counting heuristic. **The cull pad was
already excluded** — `clay_document_mesh` takes the whole-document tape with no
cull region.

The finding that survives regardless is the one the host drew before the file
existed: **two documents holding the same items are not the same document.** A
hand-built reconstruction with identical items, spline type and blend produces
zero of whatever this produces, so something in how the host's document type
configures a document is the variable — and it is now diffable, because the
file is here and a hand-built equivalent is a few lines.

### The sculptor stall was fixable for five days before anyone read the header

Recorded because the failure is not the engine's and not the host's, and it will
happen again: `clay_mesh_sculptor_create` has been documented as safe off the
interface thread since **2026-09-01** (`50a19379`), on the same footing
`clay_brick_cache_eval_requests` documents, and the block says so in the
imperative — *"SO ARM A SCULPTOR OFF THE INTERFACE THREAD"* — with the cost
split out: ~116 ms adjacency and ~89 ms tree at 296k triangles.

The consuming host filed a P1 against us for that 205 ms stall, ranked it third
of seven, and an external design guide wrote a whole section proposing a
prepare/adopt API to solve it. **Both were describing a state of the world that
had ended five days earlier.** The host has since grepped its own vendored copy
and found the block present in its v0.78.0 pin as well as v0.84.0 — so it was
fixable on the older engine too.

**And the half that would have been missed anyway:** the tree is built LAZILY,
on first use, so a host that moves only `create` to a worker still pays the ~89
ms on whichever thread reaches `clay_mesh_sculptor_raycast` first — for that host
the interface thread, on the pick right after activation, which is where the
freeze already was. Moving `create` alone shifts 116 ms and leaves 89 in the same
place: a half-fix that reads as a regression later because nobody remembers it
was 205. `clay_mesh_sculptor_refresh` on the worker is the other half and the
block says that too.

**The lesson is about where a capability is announced.** This one was announced
in the header, which is the documentation a host integrator reliably reads — and
the host reads it when integrating, not when a ticket it filed months earlier
comes up. A capability that removes a host's known pain is worth telling that
host about directly; a header is where it is FOUND, not where it is DELIVERED.

### A mesh call carries ~0.65 ms of fixed cost, which dominates a small dab

Measured by ClaySpaceDesktop on 2026-09-06, six dab sizes on one warm document,
same tool and same caches, varying only the dirty set:

| bricks | sync ms | µs/brick |
|---:|---:|---:|
| 8 | 0.951 | 118.9 |
| 8 | 0.958 | 119.8 |
| 8 | 0.977 | 122.2 |
| 8 | 1.070 | 133.8 |
| 18 | 1.551 | 86.2 |
| 64 | 3.086 | 48.2 |

Fitted: **≈0.65 ms fixed plus ≈38 µs per brick.** At eight dirty bricks — an
ordinary small dab — **68% of the call is the fixed part.** It vanishes into the
noise on a large edit and dominates a small one, which is the shape that makes it
worth a row: the cost is invisible in exactly the measurements a benchmark tends
to take.

Six points rather than two, deliberately: their first estimate came from two
measurements and gave 2.3 ms, and the curve says 0.65. **A slope inferred from
two points was wrong twice in one day on this exchange.**

**Not yet attributed, and the host cannot see which it is.** Candidates on this
side, in the order they would be cheap to exclude: a per-call plan or cull build
that walks the item list regardless of dirty set; mesher setup that allocates per
call rather than per brick; a device submission or readback with a fixed cost;
and tape work that a warm revision should have made free but has not.

**The measurement that splits them is the host's and it is cheap:** hold the dab
at eight bricks and vary the DOCUMENT size. If the fixed part grows with item
count it is a per-call walk — plan, cull or tape. If it is flat, it is setup —
allocation, submission, readback. That is one axis and it decides which half of
this engine to open.

### What the same measurements CONFIRMED, which is worth as much

Two hypotheses the host went in expecting and the engine disproved, both measured:

- **The per-brick tape does cull a far mirror image.** A brick beside the
  original costs 1.05 µs with one tube, 1.10 with a second tube far away, and
  1.17 with a layer mirror — so a mirrored instance is not evaluated everywhere.
  The culling does what it claims.
- **The remaining cost of a mirrored stroke is therefore just twice the
  geometry** — 2x the keys, 1.7x the meshing time. Honest work rather than a
  defect, after a while spent looking for a villain that was not there.

Recorded because a negative result about our own culling, measured from outside,
is evidence nothing in this repository can produce for itself.

### Regional multires: the bit-identity gate passes because the fixture has no boundary detail

Found by auditing `finish-regional-multires` against the tree, and it is a defect
in SHIPPED code rather than in the change that found it.

`refine-one-region-of-a-hierarchy` ships a gate asserting that a regional level's
vertices are bit-identical to the dense hierarchy's, and the gate is real: the
same stencils run against the same parent, re-measured independently at
**0.000000000 difference at levels 1, 2 and 3.**

**But normals and FRAMES are already wrong at a region boundary, before any
transition polygon exists** — up to **0.104** (about 6 degrees) at level 1,
0.0486 at level 2, 0.0294 at level 3 — and they differ at exactly the vertices
whose face ring at that level is incomplete. The two predicates were checked
against each other: zero disagreements at every level.

**Why that reaches storage rather than display.** A multires surface is
`P(n) = S(n) + Frame · Detail`. A frame that is 6 degrees off means a coefficient
authored at a boundary vertex **reconstructs to a different world offset than the
same coefficient on a dense hierarchy**. So the bit-identity claim holds only
while the boundary detail is ZERO — which is the only case the shipped gate
exercises. Sculpt at a region boundary and the guarantee is gone, silently.

**This is why "no host exports hierarchies today" does not make it deferrable.**
The consuming host's position — a hierarchy contributes its cage, the sculpted
level is reached through a bake — means nobody meets it through export. It is
reached by SCULPTING near a boundary, which is the ordinary use.

It also inverts the residual ordering the change recorded: task 3.4, the
cross-level neighbourhood, is not something that follows 2.3's export
transitions. **It is the thing underneath both**, because a normal is a property
of the neighbourhood and not of the transition polygon.

### Refusals a host cannot render — a standing rule, and three instances

**A refusal that knows an id should return it, and a host should never have to
walk state to render a refusal.** Where it does, either the refusal is missing a
field or the host is guessing, and those are indistinguishable until someone is
wrong in front of an artist. Two calls in
`fold-the-layers-with-an-operator` hand back what blocks them — the composition
setter names what it refused, and `clay_document_writable_at_minor` returns the
blocking layer. A third was specified (`clay_brick_cache_eval_requests_below`,
returning the lowest visible SDF layer above the named one) and **an earlier
version of this paragraph claimed it had landed when it did not exist at all**,
which is the rule's own failure mode: a sentence in a roadmap is checked when
somebody leans on it, and the reviewer who leaned on it is the reason this
sentence is now accurate.

Swept with the host on 2026-09-06, in descending order of how much the engine
already knows and does not say:

1. **The brick cache's refusal on a dirty region does not say WHY.** A host
   refilling the union of a layer transform's old and new bounds gets a generic
   error: it cannot branch on the cause, cannot name the limit or the region, and
   cannot offer the artist either of the two controls that would resolve it (the
   scale, or the cell size). The engine knew the region it refused, the budget it
   measured against, and whether the limit was memory, brick count or extent.
   **Severity corrected 2026-09-06 by the host that raised it**, which is worth
   recording because the correction went against its own case: an earlier version
   of this row said a host was telling an artist something false. It is not —
   their `ModelError::Engine` carries the engine's own `clay_last_error` string
   and displays it unchanged, so a person sees OUR words, accurate if terse, and
   the mistaken inference lived only in one of their code comments. **So this is
   a vagueness problem, not a wrongness one, and it is ordered accordingly.**
   `voxel_remesh_result_code` remains the shape to copy — eight typed statuses
   mapped to distinct codes, with a comment saying one generic failure would make
   a host guess between them — because a status a host can BRANCH on beats a
   string it can only display, and "N cells against a budget of M" is a better
   sentence than prose either side writes.
2. **The boolean budget is computed twice.** The host predicts a sampled
   boolean's cost itself, reads `clay_brick_cache_stats.memory_budget`, takes the
   tighter of that and its own ceiling, and refuses BEFORE calling the engine so
   the artist gets a number rather than a wait. That is preemption rather than
   inference and it is the right shape — but it is the engine's arithmetic
   restated, and if the two ever disagree the artist meets a refusal nobody
   predicted or waits for one that was preventable. A "would this fit" query in
   the shape of `clay_document_writable_at_minor` collapses it to one source of
   truth. Low priority, recorded for the class.
3. **The sibling rule: a call that cannot fail on an ambiguity should take an id
   rather than a name.** `clay_document_voxel_layer` takes a string, so two
   layers sharing a name shadow each other's grid and a stroke lands on the wrong
   one — no refusal to explain, because the call succeeds and does the wrong
   thing. The host prevents the condition on every path that can create a layer.

4. **A call that computed a region does not hand it back.**
   `clay_layer_set_stroke_points` knows which part of the field it changed and
   returns nothing, so a host that wants to dirty only that region computes it
   again. The same family as the rule above — a call that knows a thing should
   return it — and recorded here rather than as a row because **the one host
   that met it says it does not need it**: its own fix dirties an explicit region
   through `clay_brick_cache_mark_dirty`, which has shipped as long as the cache
   has. Worth building only if a second host asks.

Deliberately NOT on this list: tool availability. The host keeps
`tool.availability(layer_state)` domain-side on purpose — the refusal belongs to
its vocabulary, and repeating it in the engine would let the two disagree. A rule
about refusals is not a claim that every refusal belongs to the engine.

### The device baseline declares one instrument for entries taken with several

`tests/device/baseline.json` carries a single file-level `abiVersion` — `0.56.0`
today — over 74 budget entries, none of which carries its own. The tree is at
0.86.0, and `add-device-transform-cases` already records that five of those
entries were measured at ABI 0.60.0 beside the rest. **So the file states one
provenance for figures that do not share one, and nothing can see it**: a
per-entry ABI is not a field, so no reader can announce the mismatch and no gate
can refuse on it.

`release_check.py` does refuse at the release path — it diffs the recorded
`claycoreCommit` against HEAD and fails the `device` row when the engine has
moved — which is the "refuse to compare" resolution and correct where it sits.
What it does not do, and cannot, is say anything about a baseline whose own
entries were taken with different instruments.

**A third resolution exists and is cheaper than the one this repository planned.**
The consuming host's Linux bench baseline declares its engine version AND
revision and prints, above every comparison table: *the baseline was recorded
against engine X and this run is engine Y; every change below is that difference
plus whatever else moved.* It deliberately does not refuse, on the argument that
a comparison across two pins is the whole point of an upgrade measurement and
refusing leaves the question the gate is best placed to answer with no
instrument. Their macOS baseline takes the refuse route instead, and carries a
`note` saying why.

So the class has three resolutions rather than two: **eliminate the mixture**
(re-run everything, which is what this repository's open task proposes),
**refuse to compare**, or **declare the instrument per entry and announce the
mismatch at read time.** The third is the only one that survives the next pin
move — re-running makes a file correct until someone splices again, declaring
makes it correct about what it is permanently. If `baseline.json`'s entries
carried the ABI they were taken at, the gate could print that sentence and the
re-run would not be owed.

**Do the per-entry field FIRST, and do not let the red row argue for it.** The
`device` row is failing on this repository's working branch right now, and
adopting "announce" would make it green — which is a bad reason to adopt a design
even when the design is right. The test before touching that row: **would you
still make this change if the row were passing?** If the red is doing the
arguing, it is suppression wearing a rationale.

The distinction that keeps both mechanisms is that they answer different
questions, and the `device` row is currently answering the wrong one.
`claycoreCommit` against HEAD is not "are these figures comparable", it is "was
this file recorded against this engine" — and the answer is no, and will be no
after every pin move, forever. That is a condition permanent by construction
rather than a gate catching something. Meanwhile the real comparability question
— do these 74 entries agree with EACH OTHER — has no field to be red about.

So the ordering is: **add the per-entry `abiVersion`, which creates the thing
that can disagree; then decide what the row should do with a disagreement.** In
the other order a red is removed and nothing that could ever be red again is
added. That second decision belongs to whoever owns the gate, not to a branch
that would benefit from it.

**The honest limit, stated by the host about its own design:** an announcement is
QUALITATIVE. A reader is told "plus whatever else moved" and not how much, so it
cannot say whether four pins of drift have eaten the tolerance a real regression
needs. They have that number for one hop of four (median ratio 0.9998x, 171 of
178 figures inside their own run-to-run spread) and none for the others. The
comparison is honest; it is not yet sensitive, and those are different
properties.

### Our bench gate has no load guard at all, and only a brief has been stopping it

Found by comparing against the consuming host's, which has the opposite defect
and is the more instructive one.

**Theirs refuses, and on the wrong act.** Their CI passes `--json` to keep a
run's figures as an artifact; their bench binary reads the presence of that flag
as *"we are recording a baseline"*, checks the load, and exits 2 on a busy runner
**before it ever reaches the comparison**. Two branches failed identically on
`refusing to record a baseline: load 17.28 across 3 cores`, for a baseline nobody
asked to record, with no comparison run. **Two acts sharing one flag, and the
load check attached to the wrong one.** One line separates them.

**Ours does not refuse at all.** `tools/check_bench.py` reads `/proc/loadavg`
nowhere — the only mention of load in the file is a comment recording the
conditions a threshold was once measured under. So a bench run on a loaded box
produces numbers with no provenance and no complaint, and the only thing that
stopped one today was a workflow brief telling a stage not to run it. **A
discipline that lives in a prompt is not a gate**, and it held only because it
was written down three workflows earlier.

The pair is the point: a load guard on the wrong act fails loudly and blocks work
that should proceed; no load guard at all passes quietly and records a number
nobody can use. **The second is worse and looks better.**

What a guard here should do, if one is added: refuse to RECORD, never to COMPARE
— a comparison across two pins is the whole point of an upgrade measurement, as
that host's own Linux baseline argues — and print the load beside every figure it
emits, so a number carries the conditions it was taken under rather than a
person's assurance that they checked.

**Their half is now fixed, and the fix names the distinction rather than moving
the check.** `refuses_a_busy_run(comparing, busy, allow_busy)` — refuse to
RECORD, never to COMPARE — with four unit tests and, separately, an end-to-end
run of both paths with the threshold forced. Their own reason for doing both is
the one worth keeping: **the unit tests say the rule is right, and only the run
says it is wired to anything.** A rule that is correct and unreachable is the
same gate failure as a fixture nobody stands in, arriving from the other
direction.

That leaves ours as the remaining half of the pair, and now it is the only half.

**And it had been hiding the state of their default branch, not only two PRs.**
Main itself was red on Performance for the same refusal-on-the-comparison-path.
Worth adding to the pair: a guard on the wrong act does not merely block work
that should proceed — it conceals whether the branch everyone builds on is
passing, and a red main that everyone has learned to read as "the bench gate
again" is indistinguishable from a red main that means something.

### A fourth way a gate is real and unenforced: an exact assertion about a state nobody reaches

The three recorded above are a gate no change triggers, a gate the wrong version
runs, and a gate compiled but never run. The multires bit-identity gate is none
of them, and it guarantees nothing about the case people will hit.

It is not a weak assertion: it is exact, and re-measured independently at
**0.000000000** difference at three levels. It is not a tautology: its two sides
are genuinely independent, a regional level and a dense one. It runs on every
change, in the right job, at the right version.

**It asserts a state nobody sculpts in.** Bit-identity between a regional level
and a dense hierarchy holds exactly while the boundary detail is zero, and that
is the only case the fixture builds. The moment a coefficient is authored at a
boundary vertex, `P(n) = S(n) + Frame · Detail` reconstructs it against a frame
up to six degrees out, and the guarantee is gone — without the gate moving.

**The question that finds this class is not about the assertion, it is about the
fixture: what state does this test put the system in, and is it the state a user
puts it in?** A test can be precise, independent, executed and current, and still
be measuring a corner of the space nobody stands in. None of §13d, §13e or §13j
reaches it — those ask whether the test could fail, whether its expectation is
independent, and whether the code is executed. This asks whether the SCENARIO is
representative, and only a person who knows what users do can answer it.

Which is why it took a host to find: the consuming session's regional refinement
turned out to be over a VOXEL GRID rather than a hierarchy — the same English
word, a different operation — so it is not exposed today, and it said so with the
condition attached rather than filing the row as not-applicable.

### A threshold between two measurements from ONE run is stable; one against a specification is not

The generalisation of two failures on one PR, and the sharper half is the host's.

`#477`'s `build+test (macos, +metal, parity)` failed a single check out of
15,199,205: `CHECK(moved_from(one_default) == 28)`. `moved_from` counts how many
of 2,000 samples move by more than `1e-4`, so **the integer is decided by however
many samples sit NEAR that threshold** — a thin shell puts a handful there, and a
platform contracting a multiply-add differently moves one across. It read 28
under GCC and something else under AppleClang. The comment beside it said
"asserted exactly" as though that were rigour; it was fragility with a
justification attached.

**The host's own threshold has the identical shape and survives, for a reason
worth stealing.** Its guard asserts *fewer than 40 pixels differ*, where a
correct implementation reads 1 and a too-small region reads 2,363 — so 40 sits
roughly geometrically between signal and noise, with both teeth doing work. But
the property that makes it PLATFORM-STABLE is different: **both captures come
from the same machine in the same run, so a platform that renders differently
cancels rather than accumulates.** Mine compared a count against a number written
down earlier, and the two sides shared only a specification.

**The rule:** a threshold between two measurements taken in one run is
self-relative and travels; a threshold against an absolute recorded elsewhere
does not, however carefully the absolute was measured. Where a number must be
absolute, assert a BAND with both teeth named — what it catches at the low end
and what at the high — and report the value seen, so a failure says by how much
rather than only that a bound was crossed.

**And the fragile surface was the TESTS, three times.** Both CI failures on that
PR were in test code rather than in the change, and both were findable only by a
compiler this machine does not run. A green job is evidence about the change AND
about how much of the test suite that platform's codegen happens to agree with;
those are different claims and a matrix reports them as one.

### Agreement across N paths rules out only what differs between them

The reasoning error that produced the pinhole report, named by the host that
made it, and worth keeping because it reads as rigour: `visual_holes` counted
holes three ways — an incrementally patched mesh, a full rebuild, and
`clay_document_mesh` — and its comment said *"if both show them, they are the
engine's"*. Three pictures. **One rasteriser.**

Agreement across the three ruled out the per-key store and everything else that
differed between them, and said nothing whatsoever about what they shared. The
shared component was the one at fault: a watertight, genus-zero surface with
triangles orders of magnitude below a pixel renders with specks of background
through it, and every path rendered it the same way.

**The check is one question: what do these paths have IN COMMON, and is it in the
set I think I have excluded?** Redundancy across paths that share a stage is not
redundancy over that stage — it is the same measurement taken three times. This
is the same shape as an assertion whose expected value comes from the system
under test (`fold-the-layers-with-an-operator` §13e), one level up: there the two
sides of a comparison share an origin, here the three arms of a corroboration do.

### Three ways a gate is real and unenforced

Found within one day, 2026-09-06, none of them by a gate failing — all three by
someone asking WHICH RUNNER SEES WHAT. Worth keeping as a checklist, because the
common defence ("we have a gate for that") is true in every one of these cases.

1. **A gate no change triggers.** `examples/run_all.py`'s capability-coverage
   check asks whether every living capability has an example or a recorded
   reason. Archiving two changes created two capabilities and nobody adds an
   example for a documentation commit, so it was red on main for a day. The
   check was executable, correct, and unread.
2. **A gate the wrong version runs.** CI pinned `@fission-ai/openspec@1.8.0`
   while a developer's local CLI was newer, so the STRICTER tool was the one
   nobody's CI ran — four capabilities kept the placeholder `## Purpose` that
   `openspec archive` writes, `release_check.py` went red locally, and CI stayed
   green. Pin bumped to 1.12.0. The consuming host had the same class inverted:
   its CI installs `@latest`, so its enforcing version FLOATS and a green tree
   can go red without anyone touching it.
3. **A gate that is compiled but never run.** The host's `agent_end_to_end` is
   built and linted by CI and never executed, under a comment saying "it is
   still compiled and linted here, so it cannot rot unnoticed". It had rotted.
   The worst of the three, because the comment converts an unknown into a false
   known.

4. **An instrument that reports a constant.** Not a gate, but the same
   blindness one layer down, and worth the entry because the two are usually
   built by the same person on the same day: the consuming host's snake-hook
   returns `dirty_bricks: 1`, hard-coded, so the per-phase profiling it shipped
   to find exactly this class of problem records a brush touching 880 bricks as
   touching one. **The profile built to find the cost reported it as free.**
   Same shape as an assertion whose expected value comes from the system under
   test, one level up: a number that cannot vary is not measuring.

The unifying question is not "is there a gate" but **"what would have to happen
for this gate to fail, and does that ever happen here?"**

### A format bump is detected downstream without us saying so

Worth knowing before the next minor moves: ClaySpaceDesktop's own test suite
parses `kClaySpaceMinor` out of `include/clay/io/clayspace.h` and `kSceneMinor`
out of `include/clay/scene/commands.h` **in its vendored copy of this tree** and
asserts both against its own format constant. So a pin move that carries a new
minor fails their build with "the pin moved and the constant did not", before
anyone reads a release note. It also asserts the two minors against each other,
which is this repository's own static assertion that the container and the scene
payload travel together — checked from outside, where a change to one of them
cannot also change the check.

That is the good case of the rule in `fold-the-layers-with-an-operator`'s §13e:
the expected value comes from somewhere the code under test cannot reach. Their
writer and their constant are both theirs; the vendored header is ours.

The practical consequence for a release: **a minor bump does not need to be
announced to be noticed, but it does need to be announced to be UNDERSTOOD.**
Their gate says the number moved; only the notes say what moved with it.

**And the example this paragraph first used was wrong, which is worth keeping.**
It said a subtracting layer written at the older minor comes back as a union.
That is the DEGRADE this repository rejected: `serialize_document` refuses below
minor 18 for a document carrying any non-default composition
(`src/scene/commands.cpp`, `layer_blocking_minor`), so the silent-different-
sculpture case cannot occur. The sentence described the design that was
considered and dropped, four sections after the section that dropped it. A
rejected design is exactly the kind of claim that survives in prose: it was true
when written, nothing re-runs it, and it reads as a fact about the format.

### The practice that catches an inert feature

Their `clay_item_set_gate` was accepted-and-inert for four releases, exactly as
this file's note promised it would be, and what caught the fix was a host test
**written to FAIL the day the engine honoured the call**. It fired on the 0.73.0
pin and is now turned around to hold the protection. The counter-example is worth
as much: a test that asserted only what CAN be read, and never that a reader was
absent, passed happily on both sides of the change that added the readers and
announced nothing. **A tripwire that cannot fail is worth knowing about.**

They have offered a per-pin list of "calls we do not make and why" — 29 entry
points long for v0.84.0 — and a real session trace for `reference/host_loop.py`,
which covers the sequence and not the hours. Both are worth more than another
synthetic fixture, and neither costs this repository anything to accept.

### The host cannot evaluate the fold, because main is untagged

The consuming host is pinned at **v0.84.0**. The fold is on main at 0.87.0 with
no tag, so they cannot pin it, cannot call it, and declined to say whether it
fits — **"arriving, not evaluated"** — on the explicit ground that reading a
header is what produced two of their wrong answers today.

That is the right call and it has a cost we should name: **until a tag exists,
every answer we get back about the fold is a header reading, which is the class
of answer both sides have now been burned by.** The unblock is a release cut, not
a code change, and it is the cheapest open item on this list.

What they could say without calling it is that the SHAPE is right — per-operand
composition, a resolved boolean still first-class for operands nobody converts,
and a refusal carrying a blocking id **and** a count. Those were the three things
they asked for and all three are in. Fit is still unknown.

### The format pre-check should name the layer, not return a boolean

Recorded above: a C-ABI host cannot choose the format minor it writes. The host
has now said what it actually wants from a pre-check, and it is narrower and more
specific than "check before you write".

**Their save is four lines** — build a C string, call `clay_document_save`, check
the result. A refusal comes back as an engine error and reaches the sculptor as
text, which is **correct and late**: they have already chosen a filename and
pressed save. What a `clay_document_writable_at_minor` buys is the ability to say
so **while the document is being built** — to grey the older-format option, or to
name which subtool is the reason, before anyone commits to a path.

So it is **wanted, not needed**: the safety is already ours, in the refusal. It
should not hold anything up, and it should not be sold as a correctness fix.

**But a boolean is the wrong return.** Their argument is the one `_below` already
settled: *"this document needs 18"* sends a sculptor hunting; *"Poros needs 18"*
does not. The blocking layer is already an id inside the refusal path, so
exposing it costs nothing — and a pre-check that returns less than the refusal it
predicts is a worse interface than no pre-check.

### The seed hazard the host cannot reach, and why their type is the reason

Our review found `stamp_coarse` forwarding the bound level's `seed_class` into a
coarse level's class space: an UNREVISIONED seed is trusted verbatim, the coarse
level is larger so the stale index is always in bounds, `geodesic_region` starts
outside its own radius and returns empty, and the coarse dab **silently does
nothing.** pyclay's `MultiresSculptor.stamp` defaults `seed_revision` to `None`,
so the unrevisioned mode is the DEFAULT rather than an edge case.

**It cannot reach the host, and the reason is structural rather than lucky.** On
the hierarchy path they pass no seed at all. Where they do send one, their
`MeshSeed` carries `class` and `revision` **together in one type** — there is no
way to express a bare class in their vocabulary, so the mode we default to is
unreachable from there by construction.

**That is the transferable part.** Our seed is two independent fields and a
sentinel that means "trust me"; theirs is one value that cannot be halved. The
same hazard exists in both codebases and only one of them can express it. Note
this is not an argument for removing the unrevisioned mode here —
`accepted_seed`'s own comment already records why that was rejected, and the
reason still holds: silently refusing an unrevisioned seed turns every shipped
caller's fast path into a full scan, which is a performance regression delivered
as a correctness fix.

They reached it the expensive way and named the artefact: a test module
`the_silent_empty_dab` — a pick, a remesh under the pick, then a dab where the
pick landed, run twice with the revision token kept and struck off. **It lives in
their crate rather than in their tests directory because the broken state cannot
be reached from outside**, which is the honest place for a test whose fixture is
unreachable through the public surface.

**Their proposed sharpening was checked and REJECTED, and the reason is worth
more than the suggestion.** They asked whether forwarding the class with the
BOUND level's revision would turn the silent no-op into something counted rather
than removing the forwarding. The premise holds: every `MeshSculptor` mints its
own `seed_revision_` at construction (`src/mesh/sculpt.cpp:225` and `:228`,
`next_seed_revision()`), so coarse and bound never share one, and the mismatch
would increment `stale_seeds_rejected_` and fall back to the same scan.

**But that counter is not ours to spend.** `stale_seeds_rejected` is public on
both surfaces — `clay_mesh_sculptor_stale_seeds_rejected` and pyclay's
`MeshSculptor.stale_seeds_rejected` — and it means *the host handed us a seed
from an old numbering*. Tripping it internally on every coarse dab by design
would make it fire constantly for a reason no host caused, destroying the one
signal hosts have for their own staleness bugs; `bindings/python/tests/
test_seed_and_peaks.py` asserts it is exactly 0 and exactly 1.

**And the telemetry it promises would be unreadable anyway**: the coarse
sculptor is a throwaway destroyed with the stamp, so nobody can ever call the
accessor on it. So the seed is BLANKED in the copy each coarse sculptor is
given, and the shipped reason is the one that survives inspection: there is no
map between two levels' class numberings to translate a seed with, and the scan
the seed exists to skip is what a coarse walk over a quarter of the vertices
costs regardless.

### Layer across a depth boundary has a live host, which ranks the three majors

The host ships `clay_multires_sculptor_stamp (LAYER)` as their multires verb
today, driven through `hierarchy.surface_mut().sculptor()`. So the coarse ceiling
reset — `bind_coarse` emptying every coarse level's stroke record on a
generation-only rebind, while the bound level's `level_deltas_` correctly
survives — **is a step at the seam in a brush a sculptor can pick today, on a
representation we already offer.**

Of the three majors the review confirmed, that is the one with a user behind it.
The other two are a corrupted display normal and a silent no-op; both are real
and neither is reachable by anyone we know of yet.

### A fixture whose normals all point the same way hides a wrong normal

The clearest instance yet of the fixture class, and it arrived by a route worth
recording: a reviewer's finding that was WRONG AS STATED, with a real defect
underneath it that only reproducing the probe could find.

The claim was that `append_outside_neighbors`'s `want_normals` output is never
executed. It is: deleting it makes `nb_normals_` shorter than `nb_slots_` and
fails an existing case at `CHECK(crossed.dropped == 0)`. **The SLOT is gated.**

**What is ungated is the VALUE.** Substituting a constant `cf3(0, 1, 0)` for the
outside normal, keeping the list the same length, leaves the whole suite
identical at 16,463,873 assertions and 0 failed. The reason is the fixture and
not the code: `bumpy_quads` is a plane cage whose normals all sit within a few
degrees of +Y, and `polish_gate` reads an ANGLE
(`mean_ring_disagreement`) — so a constant normal sits comfortably inside the
gate's own tolerance. **Every value in the fixture is nearly the constant the
bug substitutes.**

The repair was the fixture, not the assertion: the new case runs on a TORUS,
where a wrong outside normal reads as a hard edge, `polish_gate` shuts, and the
rim is silently not polished at all. Proved by substituting the constant again —
`crossed.dropped` reads 3 at radius 0.20 and 1 at radius 0.30 against 0.

**Two things to carry forward.** First, this is the same shape as the
bit-identity gate passing on zero boundary detail, arrived at independently:
*a fixture in which the quantity under test is degenerate proves nothing about
the quantity.* The question to ask a comparison gate is what value would have to
be non-zero — or non-uniform — for the assertion to be a real claim.

Second, the reviewer was wrong and the finding was still worth its cost. The fix
agent reproduced the probe rather than arguing from the code, found the claim
did not hold, and found the real gap one level down. **A wrong finding that is
reproduced rather than dismissed is a cheap way to be right about something
else.**

### A suppression list is evidence about the checker, and the host has nowhere to write one

Our task-symbols gate went from **12 baseline rows to 5** when the resolver was
fixed. Seven of the twelve were never debt: they were the tool's own defect
written down as though somebody owed it. The cause was a single line — a
path-shaped citation answered by `os.path.exists` rather than by `git ls-files`
— and it produced three failures that looked unrelated: gitignored build output
counting as resolved, a `..` citation answered by whatever sat beside the
checkout, and an inversion in which a MORE specific citation fails while a vaguer
one passes.

**The inversion is what made it undetectable from inside.** `scene/bounds`
resolving while `ClayCoreLink/Empty.swift` does not is backwards from the way
anyone would test a path resolver, so it survives the obvious check — and each
failure it caused was parked in the baseline as debt, which is the one place
nobody re-reads.

**The host cannot have this problem, and not by foresight.** They grepped their
gates for an allowlist, a baseline, a waiver, a known-failures file — anything a
violation could be parked in — and there is nothing. The only allowlist in their
`check_layering.py` is `UNSAFE_ALLOWED = {"claycore-sys", "claycore"}`, which is
a RULE (two crates may hold `unsafe`), not a list of tolerated exceptions. So a
gate that is wrong there cannot record its wrongness as debt: it fails, somebody
has to look, and **the only place the pressure can go is into the tool.**

**The rule, and it generalises past gates:** any accumulated list of accepted
exceptions is a record of two things at once — what the code owes, and what the
checker gets wrong — and nobody reads it as both. Twelve rows down to five, seven
of which were never debt, is the number to put in front of anyone proposing a
baseline file as the way to adopt a strict check gradually. **Their property was
obtained by never building the mechanism**, which is the argument for not
building one later rather than for tearing ours out today.

### The host's Camada measurement, and a decision they declined to route around

Their diagnosis of their own per-segment `begin_stroke` held up under
measurement: **mesh Camada converges (1.010 -> 1.036 over six dabs) and hierarchy
Camada is a straight line (0.010 -> 0.059)**, which is our Draw shape, and
`begin_stroke()` appears exactly once in their codebase — per segment, in the
hierarchy path only. Our own gate's ratio (draw past layer by more than three
times) is what they will check the fix against.

**They have not fixed it, deliberately.** It is a confirmed defect on a shipped
brush and the fix changes how a tool behaves on a representation people are
using, so they put it to their user once with the measurements and the one-line
cause, and stopped. Recorded here because the reasoning is worth keeping: *a peer
asking twice is not a reason to schedule someone else's product decision.* We
asked twice; the right answer to the second ask was no.

### A merge resolution is a claim, and mine went in unproved

Resolving an add/add conflict on `tools/check_task_symbols.py`, I took the
branch's 397-line rewrite over main's 134-line original and wrote in the merge
message that the rewrite's `EXTENSIONS` set **"is a superset of main's
`FILE_SUFFIX` tuple"**. I had compared the two lists. I had not compared the two
BEHAVIOURS, and the sentence was false twice over.

main's `claimed_symbols` split a dotted citation and checked each part, so a
struct field and its struct both had to exist. The rewrite recognised only paths,
filenames and identifiers, so every `A.b` reached the "no opinion" branch and
returned `None`. **Twelve dotted citations across the non-archived changes went
silently unverified** — `Document.to_bytes`,
`clay_multires_stamp_report.moved_vertices`, `Document.writable_at_minor` and
nine more. `EXTENSIONS` also dropped main's `.rs` and `.glsl`.

**The aggravating detail is what the same branch was doing at the time**: wiring
that gate into CI. It advertised the checker and weakened it in one change.

**Why the existing habits did not catch it.** A merge is the one edit nobody
diffs against its own parents — the review lens that found it had to be pointed
at the merge commit explicitly, and it was pointed there only because the last
round had already shown that commits written under time pressure go unread. Two
full adversarial rounds had passed over this file. Neither could see it, because
it did not exist until the merge.

**The rule:** a conflict resolution that says "A subsumes B" is a claim of the
same kind as a spec SHALL, and it needs the same evidence — run BOTH sides
against one input and diff the answers. Comparing the two constant tables is
comparing the parts of the behaviour that were easy to see.

Recorded because it is the failure this document names elsewhere as the host's
and as mine — *reading the right code and answering a different question* — this
time in a merge message, where nothing re-runs it. When the fix landed it caught
its own first false positive immediately: three tasks.md lines quoting a linker
error, `GLIBCXX_3.4.31 not found`, read as three symbol claims. A span with
whitespace is prose, which the path and identifier shapes had always known and
the restored member rule had to be told.

### A negative repro that rules out one path, and the ceiling gate that localises it

The host tried to reproduce the coarse ceiling reset and **could not**, and
reported the failure rather than the silence. Their probe: a flat cage converted
to a hierarchy at 0, 1 and 2 levels, six Camada dabs inside ONE gesture at the
same place, peak measured after each.

```
levels 0: [0.01, 0.02, 0.0299, 0.0398, 0.0497, 0.0594]
levels 1: [0.01, 0.02, 0.0299, 0.0398, 0.0497, 0.0594]
levels 2: [0.01, 0.02, 0.0299, 0.0398, 0.0497, 0.0594]
```

**Identical at every level count including ZERO**, where there is no coarse level
to hold a record at all. So whatever those rows show is not level-dependent and
is not the seam: if the defect were reachable this way, 1 and 2 would have to
diverge from 0, and they do not by a digit.

**It rules out one path and nothing else, and #1 is NOT downgraded on it.** The
defect needs something that bumps `cache_generation` MID-STROKE, and their probe
drove segments within a gesture without ever rebinding. An absence of evidence
from a probe that never induces the precondition is not evidence of absence.

**What the rows actually show is on their side, and our own gate is what
localises it.** Six segments, six deposits, no ceiling — the Draw-shaped curve,
not the Layer-shaped one. `test_mesh_sculpt.cpp:1000` already gates the property
on the plain mesh path: twelve stamps against ONE record converge with
`layer_12 <= 0.08f + 1e-4f`, contrasted against `Draw` at identical settings
reading `draw_12 > 0.08f * 2.0f`. So the ceiling works where the record
persists. Their `stroke_into` calls `begin_stroke()` per SEGMENT, and that call's
own comment says it clears the record Layer measures its ceiling against — so
their Camada resets its ceiling once per segment rather than once per gesture,
which makes Layer behave like Draw on a held stroke. Their defect, on a path they
ship, and they flagged it themselves because they had described the ceiling to us
as working without having established it.

**The pattern worth keeping is the shape of the report, not the result.** A probe
that comes back negative is worth publishing WITH the precondition it failed to
induce, because a bare "could not reproduce" would have downgraded a real defect
that a live host can still reach.

### The frame at a region boundary: what it costs to land it unfixed

The host reviewed the regional-multires residual and asked for one thing, on the
row that is theirs: **if the boundary frame is fixable inside the change, fix it
there.** The reason is our own correction to them — nobody meets this through
export, they meet it by sculpting near a boundary — so a change that lands
regional refinement with frames up to six degrees out at boundary vertices ships
a feature whose guarantee holds only where nobody works.

**They checked that it does not reach them rather than assuming it.** Their
`LayerOperation::RefineRegion` refines a **voxel grid**, not a hierarchy — the
same English word, a different operation — and they call
`clay_multires_add_level` for whole levels only, never a regional variant. So
this is not a blocker on their account.

**The condition they attached is the part to honour:** if it lands unfixed, the
row must say what it COSTS, rather than being a follow-up nobody reads. Sections
1.1-1.5 of `finish-regional-multires` are that work — the frame, the halo and
coefficient smoothing — and they are deliberately unticked, with the spec delta
rewritten to describe the tree instead of promising them. The cost, stated: a
coefficient authored at a boundary vertex reconstructs against a frame up to
0.170116 |dnormal| out (124 of 1024 emitted corners on the measured fixture), so
bit-identity with a dense hierarchy holds unconditionally only while boundary
detail is zero.

## Deliberately not doing

Recorded so they are decisions rather than oversights:

- ~~**Topology-CHANGING mesh sculpting** — dyntopo, multires, remeshing,
  subdivision (their LiveClay, ZBrush's dynamic tessellation).~~ **REVERSED
  2026-08-29 — see Phase 5 above.** Kept in full because the reasoning is still
  the right frame for the boundary that remains: *an SDF sidesteps topology
  entirely; competing on dynamic tessellation is not this engine's fight.*
  **Amended 2026-08-14, not dropped**: this row used to say "mesh surface-mode
  sculpting", which was wider than the decision behind it. Moving the vertices
  that already exist is a different claim, and `mesh-fixed-topology-brushes`
  makes it; tessellating new ones stays out.

  What the 2026-08-14 amendment did not answer is what happens AFTER a
  fixed-topology snakehook stretches the triangles past usefulness, and shipping
  those brushes is what made the question live. The audit of the outside review
  had already recorded the half-agreement — *that is a recovery operation, not a
  sculpting mode* — and left it under "decide, do not build". Phase 5 is that
  decision, taken the other way, and bounded: `MeshSculptor`'s byte-identical
  `indices`/`quads` contract is untouched, adaptive topology is a separate
  representation a caller chooses explicitly, and `openspec/specs/meshing`'s
  "SHALL NOT re-tessellate" is scoped to the fixed-topology layer by a delta
  rather than deleted.
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

- ~~**Subdivision multires.** Resolution is an evaluation parameter here, so the
  whole Res+/Resample/multires apparatus has nothing to attach to.~~ **REVERSED
  for MESH layers only, 2026-08-29 — see Phase 5 above.** The reasoning holds
  exactly where it was written and nowhere else: on an SDF layer resolution IS
  an evaluation parameter and Res+ has nothing to attach to, and on a voxel
  layer the level stack (`add-multi-resolution`, `add_level_region`) is the
  apparatus and it landed. A mesh layer is the one representation where
  resolution is neither evaluated nor stacked — it is fixed by the import — so
  the sentence was true of the field and was never tested against the mesh.
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

### The tripwire says the number moved; the handshake is that something acted on it

Recorded earlier: a test written to fail on the other repository's fix is the
cheapest cross-repo handshake. The host has now corrected that, and the
correction is the useful half.

Their side of #472 is **three** things, not one. The tripwire
(`voxel_remesh.rs:271`) is the one that fires. The workaround is
`struct Rebuild { layer, engine_depth }` and the half of
`settle_geometry_revisions` that forgets a mesh sculptor when history stands at
a rebuild's depth. **And the third is a BEHAVIOURAL test** —
`a_stroke_lands_after_the_rebuild_is_undone`: rebuild, undo, stroke, and the
stroke must land on the restored triangles. **It names no mechanism.** It passes
today through the depth record and must still pass tomorrow through the
revision.

**So the handshake is deleting the workaround and finding the BEHAVIOURAL test
still green.** Re-running the tripwire is not the handshake: it says the number
moved and says nothing about whether anything acted on it. If the behavioural
test goes red once the record comes out, the number is moving at a moment their
`settle` is not asked at — and that, rather than the tripwire's colour, is the
result worth reporting.

**The general form:** a tripwire proves the SIGNAL changed. Only a test written
in the vocabulary of the user's action proves the signal was USED. A handshake
between two repositories needs both, and the mechanism-free one is the one that
survives the mechanism being replaced.

### Two documented behaviours, an undocumented composition

From their cut tool, offered as a contract question rather than a bug: **a cut
added to a mirrored layer came back reflected**, because the layer mirror
reflects items and a cut is an item. `clay_item_set_mirror(-1)` is the right
opt-out and it works.

Each half is documented. Neither document mentions the other, and the
composition is where the surprise lives. Worth carrying as a review question for
any new item KIND we add: what does every layer-level operator already do to an
item, and does this one want that? The answer is often yes — a mirrored cut is
defensible — but it should be a decision with a sentence behind it rather than a
default nobody chose.

### A revert proof that did not compile is not a revert proof

The house rule says: revert the fix, check the revert still COMPILES, check the
test fails. **This is what the middle clause is for**, and the iPad session
nearly shipped a false green that names the mechanism exactly.

Its first revert deleted two call sites. That left a helper unreferenced,
`-Werror,-Wunused-function` failed the build, **and the harness ran the STALE
binary** — which reported `2 passed | 0 failed`. It had `build_exit=2` and
`2 passed` on adjacent lines and began reading the second one.

**A test runner will happily give you yesterday's answer.** Nothing about
`2 passed` says which binary produced it, and a revert is precisely the moment
you are least entitled to assume — you have just deliberately broken the code,
so a build failure is the expected outcome and the least surprising thing to
skim past.

**The fix is mechanical: assert the build succeeded before reading the test
result.** Same shape as the vacuity guard above — a cheap check that the thing
you are about to believe was produced by the code you think you changed. (Their
second revert kept the helper referenced under `if (false)`, so the revert
isolated the call sites and the build stayed clean.)

Worth adding to any harness that runs a build and a test in one script: the exit
code of the build is a precondition of the test's output meaning anything, and
scripts that print both put them where a reader chooses.

### A budget that cannot fail until the case is 3.6x slower

The device gate reported `mesh_sustained_grab: no declared budget in the
baseline`. The obvious repair is to declare one. **That would be worse than the
gap**, and the arithmetic says so rather than a judgement:

```
p95                          0.019 ms
NOISE_FLOOR_MS               0.050 ms   (check_device_bench.py)
smallest failing overshoot   0.069 ms   = 3.6x today
```

A budget only fails when the overshoot clears the noise floor. So a budget set at
today's measurement **could not fail until the case was 3.6x slower** — not a
loose budget, not a gate at all, and a row with a number beside it reads as
coverage. The same tool already reports this defect from the other side, when it
says a budget sits 12.8x above its measurement and "could get 13x slower
unnoticed".

**The repair is to declare WHICH GATE, not a ceiling.** `"gate": "drift"` keeps
the rule that must not be weakened — every case declares something, and an
unbudgeted latency number is a measurement rather than a gate — while letting a
case whose claim is about session LENGTH be held to a drift verdict instead of a
millisecond ceiling it was never the right instrument for.

And the declaration itself was made falsifiable in both directions, on doctored
runs:

```
windows removed   -> FAIL "declares the drift gate but records no windows,
                           so nothing gates it"
p95 -> 0.421 ms   -> FAIL "it has a number worth gating, so give it a budget
                           and drop the declaration"
```

The second is the exemption-list rule applied to a budget: **a declaration that
cannot go stale is a permanent escape hatch.** This one expires automatically the
moment the case grows into measurable territory.

### Three checks that could not fire, in one day

The generalisation, from three instances found in a single session — all in the
same session's own tooling, all caught by that session:

| what happened | why the output looked like an answer |
|---|---|
| a revert deleted two call sites, `-Werror,-Wunused-function` failed the build, the harness ran the **stale binary** | `build_exit=2` and `2 passed` printed on adjacent lines |
| `git rebase` piped to `tail`, so `set -e` saw **tail's** exit code | the rebase conflicted and the loop carried on through two more branches |
| `grep -c` returning **exit 1 on a clean build** — no matches is a failure code | a successful check read as a failed one |

Three disguises: a stale artifact, a discarded exit code, and an exit code that
means something other than what the reader assumed. **In every one the check was
structurally unable to fire and the output was well-formed.**

**The common fix is not care.** It is making a check assert its own
preconditions: the build succeeded before the test result is read, the command's
own status rather than a pipeline's, the difference between "no matches" and
"failed to look". That is the same instrument as the vacuity guard below and as
the revert proof itself — *establish that this check could have produced a
different answer, then read the answer.*

### The vacuity guard: the assertion form of the revert proof
### The vacuity guard: the assertion form of the revert proof

The cheapest instrument in this document, and the one that turns a judgement into
a fact.

A comparison gate can carry, as its last line, an assertion that **the two sides
it compares actually differ somewhere** — the iPad session's `any_differed`. It
caught a regression case written on a sphere: a sphere is convex, so its cavity
is zero at the placed point and the unplaced one alike, and **every assertion in
the case held with the fix deleted.** The guard is what failed. Three lines
standing between a green test and a green test of nothing.

**The framing that matters, which is narrower than "check the fixture is
interesting":** it is not a check that the fixture is representative — that is a
judgement, and judgements can be argued into. It is a check that **the comparison
is not vacuous**, which is a fact. *"Is this fixture representative"* invites a
discussion. *"Would this assertion fail if the fix were deleted"* has an answer.

**So it is the assertion form of the revert proof this repository already
requires**, available inside the test rather than as a separate procedure — the
cheap mechanical half of the thing, running on every CI run instead of once at
review. The revert proof is stronger and stays required; the guard is what
survives after the person who ran it moves on.

Worth adding to the comparison gates already in the tree that lack one. Every
fixture failure recorded in this file — the zero-boundary-detail bit-identity
gate, the plane cage whose normals all sat within a few degrees of +Y, the
squashed-operand box that reached past the body, the convex sphere — would have
been caught by a line asserting the two sides differ before asserting how.

### "It would be wrong to" is not "nothing does"

One sentence from the iPad session, offered against its own work, and it names a
substitution that runs under several findings in this file:

> I reasoned from *"copying would be wrong"* to *"nothing copies"*, which are
> different claims and only one of them is checkable.

It was about to delete `ClaySpaceDoc`'s copy assignment. The normative claim is
easy and was correct; the empirical one is the one that decides whether deleting
it breaks a caller, and it takes a grep. (It came back clean — every holder takes
the document by `shared_ptr` or reference, and the single by-value use in the
repository is `ClaySpaceDoc result;` in `load_clayspace`, which is the move path.)

**The substitution is invisible because the two sentences sound like one.** A
design argument for why something SHOULD NOT happen reads as evidence that it
DOES NOT, and only the second licenses removing the thing that would catch it.
The same shape sits under the ABI-minor collision — *"a minor should be unique"*
was true and *"this minor is unclaimed"* was never checked — and under the merge
message that claimed one gate subsumed another after comparing their constant
tables rather than their answers.

**The test for it is mechanical:** the claim you are about to act on, can it be
checked by running something? If not, you are holding the other one.

### An assertion is a gate, so make it fail before trusting it

A corollary that arrived attached to the above. Adding a `static_assert` to name
a confusing compiler error, the session temporarily added a `std::mutex` member
to make it fire, confirmed it named the operation that broke at the definition
that broke it, and removed the member again.

Worth recording because an assertion is the one kind of gate people skip this
step for — a `static_assert` looks like documentation, and documentation does not
get a test. But a `static_assert` on a condition that is unreachable, or worded
for a case that cannot arise, is a safeguard that cannot fail, and it costs one
temporary member to find out.

### A safeguard that cannot fail, and the quantity that cancels the error

Two findings from one attempt, and the first is a category this file did not
have.

**I asked for a flag that turned out to be unfalsifiable.** Reviewing a change
that lets a trim release a cross-level neighbourhood, I required a mark
distinguishing *"regional, neighbourhood released"* from *"self-contained, never
had one"* — on the reasoning that if evaluation re-ran while the pointer was null
and the code read null as self-contained, the boundary normals would silently
fall back to the incomplete ring.

**The second clause does not hold, and the flag therefore cannot fail.**
`cross_level_of` decides self-contained from
`level_is_self_contained(topology, patch_kept)` — a property of the TOPOLOGY,
checked before the pointer is consulted. So a null neighbourhood on a regional
level already means exactly one thing: this level needs one and does not have it,
rebuild. And `release_cross_levels` does `cross.reset()`, so *released* and
*never built* are literally the same state — a null pointer, with no observable
difference for a mark to carry.

The proof was three failed attempts to break the mark: the first broke the
private path while the test went through the public one; the second compared the
wrong quantity (below); and the third revealed there was nothing to break.

**The category: a SAFEGUARD that cannot fail.** This document is a catalogue of
gates that cannot fail. A safeguard that cannot fail is worse in one specific
way — **nobody re-reads a flag.** A test at least gets run and its output looked
at; a defensive flag is read once at review and then trusted forever, so a
mechanism whose removal changes no observable behaviour can sit in a codebase
indefinitely looking like protection. The instrument that finds one is the same:
delete it and see whether anything moves.

**And the invariant was already there** because `level_is_self_contained` made
"never had one" DECIDABLE FROM THE TOPOLOGY. That is the general lesson worth
more than the flag: a state that can be derived from data the code already holds
does not need a bit recording it, and adding the bit creates a second source of
truth that can drift from the first. The redundancy is the hazard, not the
safety.

### Comparing the quantity that cancels the error

The second attempt failed for a reason worth its own line, because it is the
sharpest instance of the wrong-quantity class in this file.

The test compared **positions** across a stamp, to detect a wrong boundary frame.
It could not: within one stamp, the frame that WRITES a coefficient and the frame
that READS it back are the same frame, so `P = S + Frame · Detail` reconstructs
to the same point whatever the frame is. **A wrong frame cancels itself in the
quantity being measured.** Comparing normals — the quantity that is directly
wrong — caught it at once.

This is the same shape as the bit-identity gate that passes on zero boundary
detail, and it is why that one passes: with `Detail = 0` there is nothing for the
frame to be wrong ABOUT. Both are cases where the observable was chosen because
it is the thing users see, and the defect is invisible in it by construction.
**The question is not "is this quantity important" — it is "can this quantity
differ when the thing under test is wrong".**

### Put the bound between the noise floor and the wrong answer, and state both

The most-corrected entry in this file. It was written three times from three
confident measurements, each wrong in a different way, and the arc is the lesson
rather than a preamble to it.

**The question.** A gate for regional-boundary normals against a fully-refined
dense hierarchy as oracle. Area-weighted (`newell`, raw, summed before
normalizing) is right; angle-weighted (`normal_contribution`, over `kQuadTris`)
is the wrong port that looks like the answer.

**The numbers that matter, measured on the built port:**

```
ordering noise on a CORRECT port, worst over both cages:   4.5e-07
the wrong port, the SMALLEST it ever reads:                7.1e-02
```

A factor of about **160,000**. Any tolerance between roughly `1e-6` and `1e-2`
passes the correct port and fails the wrong one — there is no reasonable bound a
reviewer could pick that gets this wrong. **The gate is robust, not delicate**,
and `< 1e-6` was right all along.

**The rule, which is a procedure rather than a slogan:** derive the tolerance
from the MEASURED noise floor, check it against the MEASURED wrong answer, and
state both. A gate is meaningful exactly insofar as those two numbers are far
apart, and **how far apart they are is the thing to report**. A gate whose noise
floor and whose wrong answer sit within an order of magnitude of each other is
not a gate, whatever its bound. (The same discipline the sustained-session gate
already followed and nobody noticed generalising: 0.08% measured residue, 2%
tolerance, wrong fixture at 2.1x — floor stated, wrong answer stated, bound
between them.)

**Why the correct port is not exact, which is our own text.** The residue is
float epsilon from summation ORDER: the dense hierarchy sums one contiguous ring,
the regional one sums its own faces and then appends the derived ones. Same
faces, same values, different order, different last bits — exactly what
`cross_level.h` already says: *"ORDER IS PART OF THE ANSWER, because float
addition is not associative and the readers sum over it."* **A gate demanding
exact equality across two summation orders would fail a correct port.**

**The three wrong turns, kept because the pattern is the point.**

1. *Wrong quantity.* Incident-triangle-area ratio was measured to decide whether
   a fixture could tell area-weighting from angle-weighting. It fell to 1.05x by
   level 3 and the fixture was declared blind. But the two weightings diverge
   where the surface BENDS, not where areas are unequal — the existing cage
   discriminates at 1089 of 1089 vertices, and a planar cage reads exactly zero
   however hard it is graded. *"Graded" sounds like the property and is not.*
2. *Wrong vertex set.* A figure taken over the dense hierarchy at ALL vertices
   was carried across to the BOUNDARY CORNERS as though it were the same
   measurement, producing an argument that the wrong port fails louder than the
   defect. At the corners it reads 0.071 against the defect's 0.103 — **70% of
   it, an improvement that is still wrong.**
3. *Wrong precision.* The correct port was reported as exactly `0.000000`. It was
   `%.6f` printing `4.5e-07`. An argument was then built on the discarded digits
   — and that argument said the gate had to demand exactness, which is the one
   thing it must not do.

**The one-line version, and the reason this entry exists at all:** *the
measurement is rarely the weak step; choosing the quantity, the vertex set and
the precision are.* Every one of the three was a real number, correctly taken,
reasoned from carefully. Nothing downstream of the choice catches the choice.

### The cross-level refresh is on the per-dab path, and it was priced elsewhere at 18x

`MultiresSculptor::stamp` calls `bind()` on every dab, and `bind()` on a binding
that is still good calls `cross_level_at(level)`, which calls
`refresh_cross_level` — a walk over the region rim re-subdividing every outside
vertex from the parent's positions. Every dab, whether or not anything below
moved.

That is deliberate and the reason is written beside it: the outside vertices
belong to the level BELOW, a stroke down there moves them without this level's
cache going stale, and *"tracking them would be a fourth revision counter
guarding a walk over the region rim"*. Handing back what was last read would be
an answer a reader cannot tell from a current one. The behaviour has a gate.

**What the trade was made without is a number, and the iPad session has one from
its own halo**, which cached the outside positions and refreshed only when the
parent moved: refreshing on every evaluation instead cost **18x on a
re-evaluation that had not moved the parent — 0.0002 to 0.0036 ms.** Their
structure is the same shape as ours, so the figure transfers.

Two things keep this a follow-up rather than a defect. The 18x is measured on
the case where **nothing** below moved, which is the best case for caching and
the worst case for us; a crossing stamp writes the coarse side, so on a regional
hierarchy under an actual stroke the parent often HAS moved and the refresh is
work that had to happen. And the correctness argument for re-reading is sound —
the cheap version needs an invalidation signal, which is the fourth counter the
comment declines.

**SETTLED, and the answer is worse than the 18x suggested. Measured against this
branch (issue #493):** the refresh is **7.5% of a dab** on a 4x4 region at level
3 and **18.6%** on an 8x8 at level 4 — 16,641 vertices, an ordinary amount of
sculpting, and roughly one dab in five spent re-deriving a neighbourhood that did
not move.

**The shape is the finding, not the size.** The refresh scales with the RIM and
the dab scales with the FOOTPRINT, so the ratio grows with region size at fixed
brush size. It grows with the thing regional refinement exists to make
affordable.

**And the parent-moved split I asked for turned out to be unnecessary**, for a
reason worth keeping: when the parent has moved, the refresh is work that had to
happen, so the overhead there is zero by construction. The measured column IS the
interior-dab overhead — and interior dabs are the majority of a stroke inside a
refined region, because a crossing stamp is what happens at the boundary, not
what happens while an artist works in the middle of the area they refined. Asking
for a second measurement would have been asking for a number that is zero by
definition.

The "fourth revision counter" objection still stands and this does not overrule
it — it prices it. Tracked as #493 rather than fixed in a reviewed change.

### `openspec validate` cannot see a change directory nobody is implementing

Found by the iPad session while rebasing: its #484 was carrying three files of
#485's openspec change, swept in by a stash cycle. Nothing caught it.
`openspec validate --all --strict` is perfectly happy with a proposal whose
implementation is absent — which is **correct** for a change under review, and is
exactly why a stray one is invisible.

The same shape as the ABI-minor collision recorded above: a check that passes
because the thing it would object to is indistinguishable, in isolation, from
something legitimate. A proposal with no code is the normal state of a proposal;
a version line agreeing with the branch it was merged from is the normal state of
a merge. **Both gates are answering correctly and neither is answering the
question a reviewer has.**

The question in both cases is about the RELATIONSHIP to work outside the diff —
is this change directory one this branch owns, is this minor one another branch
claims — and no gate in this repository asks anything of that kind. Recorded
together rather than separately because the fix, if there is one, is a single
idea: a gate that reads the branch's own manifest of what it claims and checks
it against what everyone else has claimed.

### 51 of our 177 benchmarks are measured and cannot fail

Their finding arrived first and ours is the same effect by a different
mechanism, so both are here.

**Theirs: a baseline that stopped comparing.** Their committed bench baseline was
recorded against engine **0.52.2** and they are pinned at **0.84.0**. It carries
none of the groups added since — no `multires`, no `normals`, no `maintenance`,
no `render`, no `visible`. The gate still fails on the 135 figures it holds, so
it looks alive; but every one of those deltas now folds **thirty-two engine
minors**, and everything added since prints as `new` and **cannot regress by
construction**. Their own skip module exists to stop exactly this shape one level
down: a measurement that quietly stopped being compared looks identical to one
that is fine.

**Ours: no baseline at all, and a rule table that does not cover the binary.**
`tools/check_bench.py` reads one run and compares within it — `MAX_RATIO` and
`FASTER_THAN` are same-run pairs, `MAX_MS` and `MAX_COUNTER` are hand-set
absolutes. Nothing goes stale because nothing is recorded. But counted against
the built binary:

```
registered benchmarks   177
named in some rule      130
IN NO RULE AT ALL        51
```

Those 51 run, print a figure, and nothing can fail on them. (Four names appear in
rules with no benchmark — `BM_MetalStrokePatched`, `BM_MetalTapeResident`,
`BM_MetalTapeReupload`, `BM_VulkanStrokePatched` — and those are correct:
backend-conditional, and both tables skip an absent name deliberately.)

**The shared shape:** a benchmark that is measured and ungated is
indistinguishable, in the output, from one that is measured and passing. Theirs
says `new`; ours just prints a number. Neither says *"nothing is watching this"*.

**What would fix ours is cheap and is not a threshold.** A completeness check —
every registered benchmark appears in at least one rule, or in an explicit
exemption list carrying a reason — is the same instrument
`tools/check_device_coverage.py` already applies to the device table and
`check_test_shards.py` applies to the unit suite. Both were written for this
exact failure. The bench gate is the one place the idea was not applied, and 51
is what that costs. **An exemption with a reason is fine; 51 silent ones are
not.**

### A before-and-after needs a before that exists

The correction to the entry below, made by the host against its own promise
before it delivered on it, and the general point is worth more than the case.

They offered a same-box before-and-after ratio in place of their stale baseline —
record at the old pin, move, record again. Better than the baseline for the
general case. Then they checked the fixture and found **two things that make it
inapplicable to the question I asked**:

1. **Their multires fixture is uniform.** A 16×16 cage with `add_level` called
   four times over the whole surface. So `multires.stamp.mean` is precisely the
   flat number the rim refresh cannot touch — no depth boundary, no outside
   vertices, an empty neighbourhood returned without work. They would have sent
   it, and it would have looked like an answer.
2. **There is no regional "before".** At their pin, `clay.h` has only
   `clay_multires_add_level` over the whole surface; regional multires arrives
   *with* the change being measured. **You cannot take a before-and-after on a
   code path whose "before" is its own absence.**

The second is the transferable one. A ratio between two pins is the right
instrument for *"did this change cost the path that already existed"* and is no
instrument at all for *"what does the path this change ADDS cost"*. Those are
different questions and the same measurement name serves neither honestly.

**What answers the second is a reference taken in the same run**: the new path
against the old path, one box, one binary — regional-over-uniform rather than
regional-at-two-pins. Self-relative again, and available on day one, where the
two-pin ratio needs a past that never existed.

**And the honest third answer is "the path does not run here".** They said
plainly that if their adapter keeps building only uniform hierarchies after the
upgrade, the rim refresh never executes on their side and they will report that
rather than manufacture a figure. A measurement that cannot be taken should be
reported as not taken.

### Re-recording a baseline is its own change, never part of an upgrade

Their reason for refusing to re-record as part of moving their pin, and it
generalises past benchmarks:

> re-recording blesses thirty-two releases of undiscussed drift in one commit,
> and doing it in the same change as an upgrade would make the upgrade's own
> effect unmeasurable.

Both halves are worth keeping. A baseline refresh is a **claim about every
release since the last one**, and burying it in a change that has its own effect
to measure destroys the only run that could have separated them.

**And the measurement they will send instead is the right one anyway:** record
the figures at the old pin, move the pin, record again — same box, same binary,
ratio reported with the load beside it. That answers the question without
depending on the committed baseline at all. It is the self-relative rule applied
to a baseline problem: **two measurements one run apart beat one measurement
against a number from thirty-two releases ago**, and it needs no permission from
anybody to take.

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
