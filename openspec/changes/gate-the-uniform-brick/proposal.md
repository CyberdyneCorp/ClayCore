# Proposal: a dab should not pay to learn that a brick is clay

## Why

Two things run on every Pencil event of an SDF sculpt: the brick refill that
shows the dab, and the pick that aims the next one. Both were spending most of
their time on work whose answer was already known.

**The refill walked bricks that store nothing.** A dab dirties the solid box
its influence bound covers, and on a worked model most of that box is clay: on
the sculpted-sphere fixture — a sphere plus dabs on a three-turn helix, a dim-8
cache at a 0.01 voxel and a 3-voxel band — **57 of the 80 bricks** one dab
dirties are uniformly inside at 400 dabs and 64 of 80 at 1,500.
`BrickCache::submit` is where a brick is classified, from its samples, so each
of those paid 512 walks of its culled tape to be told it holds no lattice. At
1,500 dabs the cold dab cost 33.4 ms, eight times the 4.17 ms frame share.

**The pick recompiled a document that had not changed, and marched at the
document's worst step.** `pick::raycast_scene(doc, ray)` built a fresh pickable
tape on every call while the C ABI already held one per revision for every
other pick query — 0.3 ms of a 1.0 ms pick at 1,500 items. The march stepped
by the tape's `safe_step_scale`, ONE Lipschitz bound folded over every visible
node, so a twisted box two units from the model dropped it from 1 to 0.28 and a
ray nowhere near the box took 2.4× the steps it needed. Attribution then
answered "how far is the hit from this item" by constructing a `Document`, a
`Layer` and an `SdfContent`, inserting a copy of the node and running the
whole-document compile, once per candidate item per pick — 0.42 ms of the 0.80
ms that were left.

**The brick raycast walked the whole map before its first step.** It began
every ray with `surface_bricks()` — an allocation and a walk of every tracked
brick — to fold the box it marches inside, and on a whole-model cache (9,240
tracked, 2,303 surface) that fold cost more than the march. The march itself
was a 256-step sphere trace with eight hash lookups per sample and six more
samples for a central-difference normal, over a field that is a cubic in *t*
inside each cell.

## What changes

**A brick is proven uniform before it is walked** (`bindings/c/clay_c.cpp`,
`prove_uniform`). The compiler tracks every tape's Lipschitz bound *L* — the
number the raycaster already steps by — so `|f(x) − f(c)| ≤ L·|x − c|`, and
every lattice sample sits within the lattice's half-diagonal *hd* of its centre
*c*. When `|f(c)| > band + L·hd` no sample can be within the band and every one
carries `f(c)`'s sign: the brick is uniform, its class is what submit would have
found, and a sample inside the band would contradict the bound. It is a proof
rather than a heuristic. The ball is the lattice's own (the band-dilated cull
region puts the band term in twice and collapses the rate to ~21%) and the
bound is the brick's own culled tape's, never the document's. A proven brick
keeps its batch slot with a one-instruction constant stub tape carrying the
colour submit reads at sample *n/2*, so every backend, the device destination
and submit see an ordinary brick and what the cache STORES is bit-identical.
Only the whole-document compile is gated; per-layer halves always walk.

**A gated brick's seed is its proof.** Storing the stub would poison the store
— the next dab's suffix would fold onto numbers the field never produced, and
nothing could tell. Storing nothing was implemented first and measured the warm
dab **4.5× slower at 400 dabs and 19× slower at 1,500** (0.55 → 2.5 ms, 0.44 →
8.4 ms): a seedless brick pays its culled compile on every dab, which is the
expensive half. So the entry holds the field's unclamped value and colour at the
centre and at sample *n/2* plus *L*; the next dab folds its suffix onto those
two points with the seeded walk's own arithmetic and re-proves under
`max(stored, suffix)`, exact when every appended item folds by a max rule (Add,
Subtract, Intersect). Anything else, or a dab that reaches the brick, takes the
full path. A proof counts as refilled where it is made and resumed where it is
carried, so `clay_resume_stats` keeps its documented meaning.

**The pick marches the cached tape, at the step scale its own ray needs.**
`raycast_scene` gains an overload taking the pickable tape and the cull index;
`clay_raycast_attributed` and `clay_raycast_bounded` pass the cached ones. When
the tape's scale is below 1 the march compiles a tape culled to the ray's
clipped segment — through the index's plan, without which the culled compile
recomputes every bound and costs what the march saves — and steps by its scale
when that is strictly larger. An item whose influence misses the segment is
dropped and its Lipschitz contribution with it; the culled field is exact
wherever the march evaluates, so the hit is the same surface. `SceneHit::steps`
reports the march so the claim can be held.

**Attribution compiles the item, not a document.** `scene::compile_item(layer,
node)` emits the one item as an Add, alone, under the layer's placement and
symmetry — the three steps `compile_list` takes for a first Add item, so it is
byte-identical to `compile_layer` over a layer holding only that item. An
overload of `attribute` takes the pickable tape and, with one candidate layer,
reads the winner off it instead of compiling the layer again. The probe layer
carries the same four fields the per-item `Document` copied (placement,
`scale_axes`, `mirror_axes`, `mirror_k`; radial stays out, as before), so no
hit attributes differently. The old implementation is kept verbatim in
`test_pick.cpp` as the reference.

**The cache keeps its surface bound.** `BrickCache::surface_bounds()` is the
union of `brick_bounds()` over every Surface brick, maintained by the
mutations: a submit producing a Surface brick expands it; a Surface brick
leaving that state marks it stale only if it touched a face, and the box is
refolded once at the end of that mutation — one fold per `trim_to` however many
bricks it drops. The refold is on the mutating side rather than lazily in the
query because `clay_brick_cache_raycast_many` fans rays across the worker pool
against a const cache, and a const query that writes a cached field is a data
race.

**The brick raycast is analytic on the cache's reconstruction.** A brick-level
DDA skips uniform bricks whole, a cell-level DDA runs under the Surface bricks,
and per cell the first root of the cubic the trilinear field is along the ray
(Hansson Söderlund, Evans and Akenine-Möller, *Ray Tracing of Signed Distance
Function Grids*, JCGT 2022) is found by Marmitt's split at the derivative's
zeros and Newton–Raphson inside the bracket; the normal is the field's own
gradient in the cell. The hit is the crossing of `eps` scaled by distance, the
same point the sphere trace stopped at. The sphere trace stays as
`pick::detail::raycast_bricks_sphere_traced`, the reference the walk is tested
against.

**And a bug the agreement test found, verified on `main`:** `float_to_half`
shifted a float below 2⁻¹⁴ by `113 − e` instead of `126 − e`, so every distance
under 6.1e-5 was stored as a huge value or a NaN. A lattice sample that close to
the surface poisoned the eight cells around it, and a brick raycast through one
of them fell off the ray and missed the model; four samples of the 120-dab test
cache were affected. Fixed in its own commit with the IEEE round-to-nearest-even
bit patterns pinned.

## What it is held to

**Stored values are bit-identical with and without the gate**, on a whole
model, colours off and on: state, halves and rgba of every brick. The gate
proves at least half the uniform bricks and no surface brick, and a proof-carrying
window refilled through an append and a carve equals a from-scratch fill with
the gate on and off. The per-layer half is never gated; the metal backend
classifies a gated brick as the cpu does.

**`surface_bounds()` equals the fold over `surface_bricks()`** after every
mutation — fill, interior and face evictions, both trims, `forget_empty`,
reclassifying refills in both directions, eviction one brick at a time down to
the empty box, a whole-cache refill — to exact float equality on all six
fields.

**The pick lands on the same surface.** A far steep item leaves the hit within
1e-3 of the whole-tape march with strictly fewer steps; the cached-tape overload
reproduces *t* and steps bit-for-bit; a ray through the steep item ties and
marches the whole tape, bit-identical. Attribution is held against the old
implementation over mirrored, squashed, ghosted, hidden and radial layers,
groups, subtracts, paints and strokes, on lattice and scattered points and on
hits from eight directions. The analytic walk agrees with the sphere trace on
760 rays: every hit and miss the same, every *t* within a twentieth of a voxel,
every crossing normal within 26° of the six-tap one and facing the ray.

## Impact

CPU backend on an Apple M2 Max through the C ABI, medians (7 for the refill
rows, 41 for the pick rows), against `main` at 512c8c5d. Mac numbers, not
device numbers; the device gate has not been run on this change.

**The refill**, the dab window (80 dirty bricks) and the whole model (9,240):

| | 400 dabs | | 1,500 dabs | |
|---|---:|---:|---:|---:|
| cold dab | 9.22 → **5.58 ms** | 1.65× | 33.4 → **14.1 ms** | 2.37× |
| whole-model fill | 580 → **335 ms** | 1.73× | 2,213 → **1,180 ms** | 1.88× |
| warm dab, one dab later | 0.52 → **0.25 ms** | 2.1× | 0.44 → **0.17 ms** | 2.5× |
| uniform bricks proven, the window | 52 of 57 (91%) | | 62 of 64 (97%) | |
| uniform bricks proven, the model | 6,669 of 7,132 (93.5%) | | 6,584 of 6,937 (94.9%) | |

None falsely: the class counts of the whole model are identical to the walk's
(2,108 / 2,303 surface, 1,669 / 1,997 inside).

**The pick**, `clay_raycast_attributed`, one ray per Pencil event, each change
measured against the branch before it:

| | plain, 1,500 | plain, 400 | twisted, 1,500 | twisted, 400 |
|---|---:|---:|---:|---:|
| `main` | 1.006 ms | 0.274 ms | 1.234 ms | 0.345 ms |
| the cached tape and the ray-local step | 0.805 | 0.219 | 0.953 | 0.259 |
| attribution without a document | **0.518** | **0.143** | **0.666** | **0.182** |
| against `main` | 0.51× | 0.52× | 0.54× | 0.53× |

The bare march (`clay_raycast`, untouched) reads 0.38–0.40 ms plain and
0.61–0.63 ms twisted at 1,500 either side, so the pick is now the march plus
~0.12 ms; what is left of that is the influence-bound walk over every item of
the winning layer, which the cull index caches per revision and attribution
does not yet read.

**The brick raycast**, one ray at the 1,500-dab whole-model cache:

| | ms |
|---|---:|
| `main` | 0.0639 |
| the surface bound kept in the cache | 0.0014 |
| the analytic walk | **0.0006** |

Together 110×. In-process, the same ray 1.45 → 0.56 µs; 4,000 random rays
from a sphere around the model 1.95 → 1.37 µs per ray.

## What it is not

**Not a change to any stored brick or mesh.** The gate's stand-in values are
what the refill RETURNS for a proven brick — every one beyond the band with the
brick's sign, the colour at sample *n/2* the field's — and `clay.h` says so at
the entry point; what submit stores is bit-identical, which is the contract the
parity fixture and the locality tests hold. The one stored change is the half
fix, for samples that used to be garbage.

**The proof is as sound as the declared bound.** A false `tape.info.lipschitz`
would already be a raycast bug; here it would store a hole in the model with
nothing in the values to notice, so any feature that changes a field without
folding into `info` is the thing to probe. The `max(stored, suffix)` shortcut
assumes Add, Subtract and Intersect fold by a max rule, which they do.

**The hit's normal and the hit's slack change by design.** The analytic normal
replaces the six-tap central difference and differs across creases of the
trilinear reconstruction; the twisted pick's *t* moves by 1.2e-3, which is the
whole-scale march's own `eps·t / 0.28` stopping slack removed, and lands exactly
where the same ray lands without the box. `RaycastOptions::max_steps` is no
longer consulted by the brick raycast: the walk is bounded by the surface
bricks' box, not by a budget.

**Not measured on hardware.** Every number above is the M2 Max; the device
gate is the next thing to run.
