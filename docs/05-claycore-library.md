# claycore — C++20 SDF + Voxel Engine Library

Complete description of `claycore`, the portable C++20 core that owns all SDF/voxel mathematics, scene semantics, evaluation, meshing, and file I/O for ClaySpace — and stands alone as a reusable engine for tools, pipelines, and research. The iPad app is claycore's first client, not its boundary.

Math and algorithms are the ones catalogued in [01-sdf-math-foundations.md](01-sdf-math-foundations.md); architecture follows the decisions in the `add-clayspace-v1` OpenSpec change (Dreams-style brick cache, ordered edit lists, rigid blends).

---

## 1. Purpose & positioning

claycore is **the single source of truth for everything that is not UI or platform shell**:

- One implementation of every distance function, blend, and operator — compiled into CPU code *and* every GPU backend from the same headers. No Swift/MSL/CUDA copies drifting apart.
- One implementation of scene semantics (ordered edit lists, groups, influence bounds, blend locality) — so a document evaluates identically in the iPad app, a Linux CI job, a Python script, or a future desktop port.
- Headless by construction: no UI, no windowing, no Apple framework dependencies in the core. The library builds and its full test suite runs on macOS, Linux, and Windows.

Consumers, in priority order:

1. **ClaySpace iPad app** (Swift shell → C API / Swift-C++ interop; Metal backend).
2. **CI** — headless watertightness, parity, and round-trip export tests on Mac/Linux runners.
3. **Python users** — procedural generation, batch export, dataset generation, test authoring, DCC scripting (Blender/Houdini can load the wheel).
4. **Future ports/plugins** — desktop editors, engine importers, command-line converters.

## 2. Design principles

1. **Single-source kernels.** Every distance function/operator is written once in a restricted C++ "kernel dialect" header, compiled into: CPU (scalar reference + SIMD), Metal (MSL is C++14-based), CUDA, and OpenCL. Backend differences live in a thin shim header, never in the math.
2. **CPU scalar is the reference.** Bit-exactness across GPUs is impossible (FMA contraction, fast-math); the CPU scalar build defines correctness, and every backend must match it within documented tolerances (default: 1e-4 relative on distances, verified per-kernel in the parity suite).
3. **Conservative fields.** The library tracks exactness per node (exact / bound / Lipschitz-L) through the expression tree (per 01 §2.7) and exposes the resulting safe step scale — sphere tracing and sparse meshing never assume `|∇f| = 1` unless the tree proves it.
4. **Blend locality by construction.** Only rigid (locally supported) smooth blends in the core vocabulary; every edit item exposes an influence bound (AABB ⊕ blend radius ⊕ rounding). This is what makes brick culling, incremental re-eval, and the scene-model locality guarantee possible.
5. **Data-oriented, allocation-disciplined.** Evaluation paths take flat buffers (edit tapes, point batches, brick lists); no per-sample allocation; deterministic memory ceilings for mobile.
6. **C++20, no exceptions across the ABI.** Errors as `std::expected`-style results internally, error codes across the C API. Modules of the library are usable freestanding (kernels headers are header-only). The oldest toolchain CI gates is AppleClang 15 (Xcode 15.4, the `macos-14` runner) — C++20 features that toolchain lacks, such as parenthesized aggregate initialisation (P0960), stay out of the tree so consumers on older Xcode keep building.
7. **Permissive licensing throughout** (library MIT/Apache-2; deps MIT/BSD/zlib only) so it can ship inside a commercial app and a public wheel.

## 3. Architecture & module map

```
claycore/
├── include/clay/
│   ├── kernel/          # single-source kernel dialect (header-only, backend-portable)
│   │   ├── shim.h       #   vec/scalar types, address-space & qualifier macros per backend
│   │   ├── prim3d.h     #   3D primitive SDFs          (01 §1.1–1.2)
│   │   ├── prim2d.h     #   2D profile SDFs            (01 §1.3)
│   │   ├── ops.h        #   booleans, smins, chamfer & extended blends (01 §2.1–2.2)
│   │   ├── xform.h      #   transforms, elongate, symmetry, scale     (01 §2.3)
│   │   ├── repeat.h     #   grid/finite/radial repetition             (01 §2.4)
│   │   ├── deform.h     #   twist/bend/taper/displace, eased variants (01 §2.5)
│   │   ├── lift.h       #   extrude, revolve, shell/onion, round      (01 §2.6)
│   │   ├── ease.h       #   easing-curve library (fogleman-style)
│   │   └── field.h      #   normals (tetrahedron), AO, soft-shadow, raycast steppers (01 §3)
│   ├── parallel/        # the one data-parallel primitive: a batch thread pool, below everything
│   ├── memory/          # what a host will spend and may take back: the budget
│   │                    #   profile, the category ledger and its three roll-ups,
│   │                    #   the trim vocabulary and pin, the checked capacity
│   │                    #   estimator, the bounded per-stamp scratch arena
│   ├── math/            # host-side geometry: AABB, transforms, quats, ray, frustum
│   ├── scene/           # document model: layers, groups, edit items, tape compiler, undo commands
│   ├── eval/            # backend-agnostic evaluation API + backend registry
│   ├── brick/           # sparse brick cache: narrow band, dirty tracking, per-brick tape culling
│   ├── voxel/           # colored voxel grids: storage (palette+RLE), edits, mirror, greedy meshing
│   ├── mesh/            # marching cubes / surface nets / dual contouring, decimation, validation
│   ├── pick/            # ray picking, surface snapping, closest-point queries
│   ├── session/         # what lives for a sitting, never for a save: the one undo
│   │                    #   history across representations, the transient SDF
│   │                    #   sculpt transactions live Smooth and Move run inside,
│   │                    #   and the derived prefix field cache that keeps a long
│   │                    #   history from being re-evaluated on every dab
│   └── io/              # document format, OBJ/MTL, FBX (ufbx), PLY, glTF; USD hooks
├── src/                 # CPU implementations, backend hosts
├── backends/
│   ├── cpu/             # scalar reference + SIMD batch (dispatch via clay/parallel)
│   ├── metal/           # metal-cpp host; kernels compiled from include/clay/kernel via MSL
│   ├── cuda/            # NVRTC/nvcc host; same headers
│   └── opencl/          # OpenCL 3.0 host; kernels via C-compatible subset (see §5)
├── bindings/
│   ├── c/               # flat stable C ABI (clay.h) — Swift, C#, anything FFI
│   └── python/          # nanobind module `pyclay`, numpy-native
├── tests/               # unit, parity, property, golden-mesh, fuzz
└── tools/               # clay-cli: eval/mesh/convert/validate from the command line
```

Dependency rule: `kernel`, `parallel` and `memory` depend on nothing; `scene`/`brick`/`mesh` depend on `kernel`+`math`; backends depend on `eval`; `io` and bindings sit on top. **No module depends on a backend**, and `tools/check_layering.py` gates it.

That last rule is why `parallel` is its own module rather than living where it
started. The thread pool was a private header of the CPU backend, so the rule
locked the core library out of the only pool in the tree: every mesher, every
voxel verb, redistance, the mask distance transform and the per-brick tape cull
were single-threaded not because they resist parallelism but because they could
not legally reach it. It sits below everything now — depending on nothing but
the standard library — so a core module can use it and the gate can see that it
does.

`memory` is a leaf for the same reason, arrived at from the other direction.
`mesh` has to consult a budget on every stamp — the scratch hard bound, what may
be deferred, which levels are resident, what an operation is allowed to peak at
— and `io` is the top of the table, so a budget living beside `io::MemoryReport`
would have meant either a `mesh -> io` edge that makes the table cyclic or a
byte count threaded through every call signature from the host down, which puts
residency policy in the host. `scene` was the other candidate and is worse in a
subtler way: `scene` is what gets serialized, so a device budget landing there
would drift into the file format and travel with a document to another machine.

### Memory under pressure: who decides what, and in what order

The engine owns the MECHANISM, the ORDER and the reconstruction guarantee. The
host owns the MOMENT. Nothing in the library evicts on its own high-water mark:
an engine that did would mutate a document behind a host that may be mid-save or
holding a readback, and `MultiresSurface::cache_generation` exists precisely
because a released cache is a use-after-free waiting for pressure to find it.
Eviction happens at exactly three moments — an explicit `trim(pressure)`, a
residency change the host itself caused, and the scratch arena settling back to
its soft bound at a STROKE BOUNDARY, never inside a pointer event.

`trim` releases in this order, and a host implementer should read it as prose
rather than infer it from a header:

1. transient scratch beyond its steady capacity;
2. preview staging;
3. evaluated caches (subdivided positions, frames, normals);
4. spatial indices for inactive levels;
5. derived positions for inactive levels;
6. other rebuildable caches — chunk tables, per-level runtime caches;
7. history, and only to the host's own policy.

It NEVER releases unsaved authoritative content: base geometry, topology,
multires detail, sculpt layers or their masks. After a trim at any pressure the
authoritative checksum is unchanged and every released cache reconstructs to an
identical surface — asserted by `detail_checksum`, not by inspection.

A `memory::MemoryPin` held by a serializer or a readback turns a trim into a
no-op that reports what it WOULD have released, so a memory warning arriving
mid-save gets an honest answer instead of a document mutating under the writer.

**A stroke is the other thing worth pinning, and for a different reason.** A trim
mid-drag is CORRECT — the stroke commits the same surface bit for bit whether or
not one arrives, which `tests/unit/test_extreme_poly_exactness.cpp` asserts by
taking the same stroke twice and comparing every level — but at
`Pressure::Critical` it is not free: the next dab pays a full re-evaluation of
every level under the one being sculpted, measured at 13x to 182x an ordinary dab
and growing with the model (docs/09, "What a memory warning costs the dab after
it"). `Pressure::Warning` leaves the sculpt level resident and costs the next dab
nothing. So a host answering a warning mid-drag should prefer `Warning`, or hold
a pin and answer at the stroke boundary; the choice is a latency one and not a
correctness one.

Every sentence above is a test rather than an intention.
`tests/unit/test_memory_trim.cpp` asserts the checksum across a critical trim,
that every dropped cache reconstructs BIT for bit, that the partition comes back
naming the same faces under the same chunk ids, that the four pressures form a
monotone chain with `None` releasing nothing, and that a pinned trim reports and
releases nothing. `tests/unit/test_scratch_arena.cpp` asserts the arena's own
half — including that a release actually releases, which it did not: `trim` was
written with `std::vector::shrink_to_fit`, which libstdc++ implements through
`__shrink_to_fit_aux` and which does NOTHING when exceptions are off, and this
library compiles its core with `-fno-exceptions`. A critical trim of a 160 KB
arena returned 0 and kept every byte, and it is invisible to anything derived
from the vector's SIZE. Four other files in this tree still call it.

The profile a host fills carries fields for DERIVABLE work only — exact normals
during a drag, index quality, display level, cache residency, preview drain
rate. There is deliberately no field that reaches a topology decision, a detail
coefficient or a brush setting: a deferred split would make the committed mesh a
function of machine speed, and "a memory-saving mode changed my sculpt" is
unrepresentable rather than merely forbidden.

**The library therefore spawns threads**, which the "the caller owns threading
and queues" principle in §2 does not say and should: one process-wide pool,
`hardware_concurrency() - 1` workers, created lazily the first time anything
dispatches a batch. Making the worker COUNT host-configurable is
`add-mobile-thread-scheduling` and is not done.

**What the work is for, it now says.** Every dispatch carries a
`parallel::WorkClass` — `Interactive`, `UserInitiated`, `Utility`,
`Background` — and the pool applies it to each worker for the duration of a job
and restores it afterwards, because the workers are persistent and a class left
behind would schedule the next job wrong. On Apple that maps onto the pthread
QoS classes; everywhere else it currently records the class and does nothing
else, which is a written branch in `src/parallel/thread_policy.cpp` rather than
an absent file. `Interactive` maps to `QOS_CLASS_USER_INITIATED` and not to
`USER_INTERACTIVE`, which Apple reserves for a main thread's event handling; a
pool of workers claiming it would compete with the UI thread it exists to feed.

A call site that says nothing runs as whatever the thread is already running
as, which is `UserInitiated` until something says otherwise — so nothing
changed meaning by being ported. A nested dispatch inherits the class of the
job it is already inside rather than its own argument: it runs inline on that
thread, and re-scheduling it would re-schedule the outer work with it.

**THE CLASS TRAVELS DOWN, IT IS NOT WRITTEN AT THE BOTTOM.** Almost every
dispatch in the library is in a leaf — `eval_points` in the CPU backend, a
marching wave, a volume bake, a brick refill — and every one of those is
reached from an interactive dab and from a background rebuild alike. A class
written at the leaf would have to pick one caller and be wrong for the other,
and since the leaves are the hot paths, picking "interactive" marks the whole
library interactive, which is the same as having no classes at all. The
operation at the top declares; everything beneath inherits and needs to know
nothing about it.

**THE HOST OWNS THE OTHER HALF, and the library cannot do it for you.** A QoS
class propagates from the thread that makes a call, so a dab issued from a
background queue is background work no matter what the pool does with its own
workers — the pool can only classify the threads it created. Call the
interactive entry points from a thread the OS already considers interactive:

```swift
// Swift host: the class the pool inherits is the one you dispatch from.
DispatchQueue.global(qos: .userInitiated).async {
    clay_dynamic_sculptor_stamp(...)
}
```

Long or deferrable work — a mesh export, cache maintenance, a rebuild between
strokes — belongs on `.utility` for the same reason.

This is not a workaround for the library being unclassified; it is where the
decision genuinely lives for the host-facing queries.
`clay_brick_cache_eval_requests` refills bricks for a live preview AND for an
offline bake, and `clay_brick_cache_raycast_many` answers a pointer AND a batch
of rays with nobody waiting. Nothing inside those functions can tell which, so
they do NOT declare a class: they inherit the thread the host called them on,
which is the host saying what this particular call is for. Calling them from a
`.utility` queue makes them utility work all the way down to the last chunk. Per-operation QoS is
deliberately NOT exposed through the C ABI: an operation knows what it is for
better than its caller does, and a knob there invites a host to mark everything
interactive.

**What a host may call from its own worker.** Three contracts, all stated in
`clay.h` where the calls are, and all of the same shape — free-threaded against
a *const* document, never concurrent with a mutating `clay_document_*` /
`clay_layer_*` call, with `clay_last_error` per-thread so a worker reads its
own:

- `clay_brick_cache_eval_requests`, which takes no cache and is the original of
  the form. A cache *handle*'s calls are still the host's to serialize.
- `clay_mesh_sculptor_create` / `_refresh` / `_refit` (issue #368). Arming a
  mesh subtool costs the weld plus the ray tree — around 116 ms and 89 ms at
  296k triangles — and the tree is built lazily, so a host that moves only
  `create` to a worker still pays the tree on whichever thread picks first.
  Call `_refresh` there too. **Since 0.89.0 the weld half is paid once per
  layer, not once per session**: the document holds the adjacency and hands it
  to every sculptor over that layer, so a second `create` over unchanged
  triangles is 0.25 ms rather than 121 ms. See the topology cache below.

**And what never reaches a device at all.** Every bake in the ABI —
`clay_layer_consolidate` and its `_cost` / `_cancellable` / `_region` forms,
`clay_sdf_smooth_begin`, `clay_sdf_move_begin` — evaluates through the CPU
backend's reference arithmetic on that pool, and takes no backend argument
(issue #379). It is an *injected* evaluator rather than a chosen one, because a
bake lives in `clay::scene` and the layering runs `eval -> scene`: a bake
cannot name a backend. That is deliberate and worth keeping — a baked volume is
content the document then carries, so it must not depend on which GPU wrote it,
and byte-identity with the serial walk is the contract the pool is held to.
Which backend a host *draws* the result with is the separate question, and the
one `clay_eval_points` and the parity suite answer.

**What a bake does change is the march.** A consolidated layer is one *sampled
volume* where it was a list of parametric items, and a sampled volume declares a
Lipschitz of `sqrt(3)` times its samples' (`kernel::cfi_volume`) — because a
lattice is only a *bound* on the distance to its own surface, and the sphere
trace may not overstep a bound. So `clay_safe_step_scale` for that layer falls
from 1.0 to at most 0.577 and the trace takes correspondingly more steps.
Measured on a roughened ball: 7.1 steps a ray parametric, 22.9 once
consolidated *at the same shape*, and 33.8 after a Smooth stroke was committed
over it — so most of the rise is the lattice, and the rest is the relax, which
flattens the field and shortens every step. A renderer with a *fixed*
step budget therefore draws a consolidated layer worse than it drew the
parametric one, and two renderers with different budgets disagree about the same
document — which is a rendering-budget difference, not an evaluation one (issue
#379). `clay_sculpt_budget.safe_step_scale` and `clay_layer_safe_step_scale` are
what say by how much. It also puts a floor under
`clay_sculpt_policy.min_safe_step_scale`: that threshold is what *triggers* a
consolidation, so setting it above `1/sqrt(3)` marks every already-consolidated
layer permanently over budget — the collapse is refused (there is nothing left
to collapse) but the report never comes back under.

### What to bake at, and whether to bake at all (ABI 0.86.0)

`clay_layer_field_report` sets `advises_consolidation` when a layer's march has
degraded and consolidation is the cure. The next call a host needs takes a
`clay_consolidation_params` whose `cell_size` is **required and > 0**, and that
field's own comment says why nothing here will guess it. So the engine was
telling a host to bake and then making it invent the one number it has no basis
for — a constant compiled into the app, or a slider put in front of a sculptor
who cannot be expected to know what consolidation means, let alone at what
resolution. `clay_layer_consolidation_advice` fills the struct:

```c
clay_consolidation_params params = {sizeof params};
clay_consolidation_cost cost = {sizeof cost};   /* may be NULL */
int32_t advises = 0;
clay_layer_consolidation_advice(doc, layer, 0.4f, &params, &cost, &advises);
if (advises && cost.bytes < my_budget)
    clay_layer_consolidate(doc, layer, &params, NULL, NULL, NULL);
```

`*out_advises` is 1 only when **both** hold: the field report advises at this
same threshold, *and* the **projected** `safe_step_scale` in `out_cost` reaches
it. The second half is why this is not merely a params helper. A sampled volume
declares `sqrt(3)` times its samples' Lipschitz, so a consolidated layer's step
scale is at best `1/sqrt(3) = 0.577`; a host whose frame budget wants 0.8 is
asking for something no bake can deliver, and handing it params would trade a
parametric layer for a dense volume and still miss the budget. It is told 0.

**Not advised means zeroed.** `out_params` and `out_cost` are cleared to the
`struct_size` the caller declared, with that `struct_size` preserved — unlike
`clay_layer_consolidation_state`, which leaves its cost untouched. The
difference is what the caller does next: this hands over something it will feed
to a *destructive* call, so the failure has to be loud. `cell_size == 0` is
exactly the value `clay_layer_consolidation_cost` and `clay_layer_consolidate`
already refuse, so a host that never reads `*out_advises` gets
`CLAY_ERROR_INVALID_ARGUMENT` and an unchanged document. There is no reading of
the zeroed struct under which anything bakes.

**Where `cell_size` comes from.** The layer's own extent gives the scale and its
own contents give the feature, both in the **local** frame the bake samples in —
not the world-space bounds, which compose the layer transform and would be wrong
by the layer's scale, invisibly so at identity:

```
E    = the longest side of that box
f_i  = 4 * cell_size_i                              for a node carrying samples
     = the smallest axis of the node's own shape box            otherwise
       (both carried into the bake's frame by the node's scale)
cell = clamp(min(f_i) / 4, E / 512, E / 32)
band = 3 * cell ; padding = band ; redistancing ON
```

Four cells across the smallest feature: `kBrickDim` is 8, so that is half a
brick. Measured on a 0.06 dab blended onto a unit form — the surface moves 27%
of the dab's radius at 2 cells, 7.0% at 4 and 1.8% at 8, for 0.32, 1.42 and
5.51 MB. Four is where the curve turns. **The volume arm is the load-bearing
one**: `(4 * c) / 4 = c`, so a layer whose finest content is a volume at cell
size `c` is advised `c` unchanged. That is the whole answer to "a number nobody
chose" — the only degradation ever advised is `CLAY_DEGRADATION_VOLUMES`, and
such a layer carries volumes whose resolutions somebody chose at an earlier
bake. The advice hands the finest of them back, so a re-bake loses no detail
already stored and gains none that was never there.

**A stepping-bound Lipschitz was rejected as the source of the resolution**, and
this is the design decision most worth knowing. `clay_field_report.lipschitz`
bounds the step a marcher may take; it does **not** bound `|grad f|`. An
ellipsoid declares 1 and measures 1.09 near its tips, 3.6 for a needle, and
`taper`, `wrap_around` and `bend_curve` exceed their declared factors outright.
A sampling rate derived from it would look principled and be unsound exactly on
the shapes that motivate a bake. The Lipschitz enters the *advice* instead — as
`out_cost.sample_lipschitz`, measured on the samples a real bake produced — and
never the resolution.

**What it does not promise.**

- **Not optimal, and not better than your own number.** It knows the layer's
  extent and contents; it does not know your viewport, your zoom, your device's
  memory or what the artist is about to do next. `out_params` is a struct you
  own so that you can override it.
- **Not stable.** Adding one small item changes the smallest feature and
  therefore the cell size. A host that means to re-bake the same box later must
  *store* the params rather than re-ask.
- **Not a memory bound.** The `E/512`–`E/32` clamp bounds the *grid*. A banded
  volume stores only surface bricks, so the bytes follow surface area:
  `out_cost.bytes` is the memory, and it is the number to refuse on.
- **Not a fidelity claim.** A bake at the advised cell size still discards every
  parameter of every item it absorbs and every colour but the first. The
  analytic arm reads a node's own box, so a fillet finer than either shape that
  made it is not seen: heavy blends are advised coarse.
- **No pinned region.** Derived from the layer's *current* bounds, so re-asking
  after an advised bake advises a slightly larger box each time. A host
  consolidating the same region repeatedly must pin the region itself.
- **Not cheap, and a NULL `out_cost` is not a fast path.** `*out_advises` is
  *defined* by the projection, so the sampling pass happens either way. This is
  not a per-frame call.

It bakes nothing and changes nothing, and that includes an instance layer's
sharing: `clay_layer_consolidate` severs a shared edit list before it bakes and
this does not, for the same reason `clay_layer_consolidation_cost` does not.
Asking whether a bake is *advisable* must never be the thing that unlinks a
subtool.

Refused before any sampling: `CLAY_ERROR_NOT_FOUND` for a layer that is not
there — "no such layer" and "not advised" are different answers — and
`CLAY_ERROR_INVALID_ARGUMENT` for a null `doc`, `out_params` or `out_advises`,
for a `struct_size` below either original layout, and for
`advise_below_step_scale <= 0`. The last deviates from
`clay_layer_field_report`, where zero legitimately means "measure without asking
for advice": every output here is defined against a threshold, so a zero would
buy a full sampling pass to be told nothing. A non-SDF, protected or empty layer
is **not** an error — it succeeds, advises nothing and zeroes, following
`clay_layer_warp_cost_get`'s rule that a host walking a stack of mixed kinds
should not have to special-case them.

**The engine still never bakes on its own**, and that is the settled answer to
the standing question of automatic background consolidation. Consolidation is
destructive; an engine firing it on a background thread would be mutating a
document behind a host that may be mid-undo-group or mid-save, and deciding on
an artist's behalf that a sphere's radius is no longer editable. This is the
recommendation, not the action.

## 4. Operation inventory (the complete SDF vocabulary)

Everything below ships in `clay::kernel` with CPU reference + per-backend parity tests. Items marked *(bound)* propagate non-exactness through the tree per principle 3.

**3D primitives (exact):** sphere, box, rounded box, box frame, torus, capped torus, link, capsule, infinite & capped & rounded cylinder (incl. arbitrary axis), cone (exact) & capped & round cone, plane, hex prism, octahedron, pyramid, cut sphere / cut hollow sphere, solid angle, tetrahedron, platonic solids via plane folds. **(bound):** ellipsoid, tri-prism, cheap octahedron, superellipsoid / L-norm sphere — these downgrade the node's tracked field info, which lowers the safe sphere-tracing step scale.

Plane and infinite cylinder have no finite geometry bound: they report an infinite influence bound, so per-brick culling never drops them and `Document.bounds()` stays finite by falling back to the finite part of the scene.

**2D profiles (for extrude/revolve):** circle, box, segment, hexagon, equilateral triangle, trapezoid, vesica, arbitrary polygon (exact, even-odd sign), quadratic Bézier. Cubic Bézier by adaptive quadratic subdivision (quintic roots ruled out, 01 §1.3).

**Booleans & blends:** union/subtract/intersect/xor (hard); smooth variants of all three via **quadratic smin** (default), cubic (C2), circular; **chamfer** (linear) profile; extended vocabulary as algebraic smin variants — **groove, tongue/pipe, emboss, deboss, push, avoid/repel, inset, shell, stain/paint (color-only), replace**. All blends rigid (finite support); every blend carries the material-mix falloff `h` for color blending (01 §2.2).

**Transforms & structure:** rigid transform (inverse-applied), uniform scale (exact), non-uniform scale *(bound, tracked)*, elongation, mirror/symmetry planes with Mirror Blend, rounding/dilate/erode, onion/shell.

**Repetition:** infinite grid (round-based), finite grid with clamped cell index + neighbor-cell padding, radial/circular array in O(2) sector evaluations, per-element transform overrides.

**Deformers *(bound, Lipschitz-tracked)*:** twist, bend, taper, displacement by callable/noise, `bend_linear`, `bend_radial`, `wrap_around` (bend interval around a cylinder), `transition_linear/radial` (spatial morph between two subtrees) — each accepting an **easing curve** parameter from `ease.h` (30+ curves; every falloff/array/transition takes one).

**Lifts:** extrude (exact), extrude-to/loft *(bound)*, revolve (exact).

**Sculpting constructs:** stroke item = capsule/round-cone **polyline chain** with per-point radius/color (Pencil smear, Dreams-style); stamp placement; per-item op+blend+color; symmetric application through active mirrors.

**Field utilities:** tetrahedron-trick normals, analytic-gradient smin variant, sphere tracing with over-relaxation and pixel-proportional epsilon, safe step scaling from tracked Lipschitz bounds, AO and Aaltonen soft-shadow queries, raycast with last-two-sample refinement.

**Voxel operations (`clay::voxel`):** palette-indexed dense chunked grids to 256³+; set/erase/paint single and N×N footprints; box/line fills; per-axis mirror application; build-plane queries; flood select; greedy meshing with per-face color; voxel↔SDF bridges (voxel grid as step-function field for compositing; SDF rasterized into voxels at a chosen resolution).

**Resolution levels.** A grid holds a stack: level 0 is the coarsest, level *k* has half the cell size of level *k-1*, and one level is *active* — every cell-addressed call acts on it, so the level is grid state rather than an argument and no signature had to change to gain one. Discrete levels rather than an adaptive octree, and the reason is the dither: a falloff brush resolves sub-unit strength by hashing the *cell coordinate*, which is what makes a soft stroke land on the same cells on every platform and backend, and a stack of uniform lattices keeps that untouched while an adaptive structure would change what a cell coordinate means. Adding a level subdivides every occupied cell into its eight children, so the solid does not move; each level above the coarsest also carries the *offsets* that distinguish it from the level below, which is what lets a broad stroke at a coarse level reach the finest one without flattening the detail already sculpted there. `.clayspace` stores the coarsest level in the layout it always had and the offsets as a tail, so a one-level grid's bytes are unchanged and a reader that predates the tail opens the document at the coarsest level (`39_multi_resolution.py`).

## 5. Kernel dialect & GPU backends

The kernel dialect is the subset of C++ that all four targets accept: no virtuals, no exceptions, no allocation, no recursion, `constexpr`-friendly, fixed-size types from `shim.h` (`cfloat3`, `cfloat4x4`, …) that map to `simd::float3` (CPU/Apple), MSL vectors, CUDA vectors, or OpenCL vectors under macros (`CLAY_KERNEL_METAL`, `CLAY_KERNEL_CUDA`, …).

Scenes do not become shader code. The `scene` module compiles an edit list into a **flat postfix tape** (opcodes + parameter blocks, transforms pre-inverted). Every backend ships one fixed **tape interpreter kernel** — no per-edit shader recompiles, instant parameter edits (the mzschwartz5 lesson), and the door open to interval-arithmetic tape shortening (Keeter MPR) as the large-scene upgrade.

A document remembers its compiled tape and rebuilds it when the document changes. **Appending an item — which is what a brush stamp is — reuses the compiled prefix** instead of re-emitting the document: the compiler emits items left to right and never moves what it has written, so the prefix's parameter offsets and blob handles are still correct with the new item's bytes after them. At 50 000 items that rebuild is 0.54 ms against 10.3 ms for a full compile. Every other edit — an insert anywhere but the tail, a removal, a move, a parameter change, undo, redo — recompiles in full, and so does an append the compiler cannot prove is one.

The reused tape has different bytes and so its own `compile_id`, but it also **names the tape it grew from** and the offset in each section where the two stop agreeing. A backend holding the ancestor resident transfers only that suffix instead of the whole tape. **Both GPU backends do.** Over a 300-dab stroke on a 20,000-item document, 3.25 MiB of tape:

| | host CPU per dab | buffer reallocations | wall |
|---|---:|---:|---:|
| Vulkan, patched | 0.333 ms | **0** | 48.7 ms |
| Vulkan, re-uploaded | 9.44 ms | 300 | 57.8 ms |
| Metal, patched | 2.02 ms | **0** | 49.0 ms |
| Metal, re-uploaded | 2.70 ms | 300 | 49.2 ms |

**Read the reallocation count, not the wall clock.** Both rows of each pair evaluate the same 40,000-instruction tape with the same dispatch, so they pay the same GPU latency and differ only in what the host copied first — 1.19x apart on a discrete GPU and 1.02x on unified memory. What the change removes is host CPU and allocator churn, and on Metal the allocator is the whole of it: 300 `newBuffer(StorageModeShared)` calls a stroke become none, on the platform where memory pressure ends sessions.

Metal's layout needed less work than Vulkan's — it already kept `instrs`, `params` and `blob` in three separate buffers, so an append is a tail write into each with no gap to reserve between them. What it did need is slack inside each buffer, because an `MTL::Buffer` cannot be resized: an exact fit means the next dab does not fit and the patch declines. Both backends reserve the same `n + n/2 + 1024`, which makes the re-packs over a stroke geometric rather than per-dab.

| Backend | Host layer | Kernel path | Status/notes |
|---|---|---|---|
| **CPU** | thread pool, batch API | same headers, scalar + SIMD (Apple `simd` / SSE-NEON via `xsimd`) | reference; always available |

The CPU pool over-decomposes a batch into several chunks per worker rather than
one, so its atomic claim counter load-balances: a core that finishes early takes
more work instead of idling. That matters most where the cores are not
interchangeable — a mobile SoC pairing performance and efficiency cores — and it
halves a preview `raycast_many`. Batches below the caller's minimum chunk size
are untouched, so a single pick or a small brush batch still runs inline.
| **Metal** | `metal-cpp` (pure C++, no ObjC in core) | headers compiled as MSL; argument buffers for tapes | tier-1: the iPad app |
| **CUDA** | CUDA runtime or NVRTC JIT | same headers under `__device__` | tier-2: desktop/pipeline/ML workloads |
| **OpenCL** | OpenCL 3.0 | kernel headers constrained to the C-compatible subset (macro-mapped to OpenCL C) | tier-3, best-effort; the Vulkan backend below is its long-term replacement, and slots into the same interface |
| **Vulkan** | Vulkan 1.1, no optional features | same headers as GLSL: buffer cursors are indices into a storage buffer (`CLAY_AT`), casts are functional, enums become `const int` when the shader is generated | tier-3; `eval_points` + `eval_grid`, raycast `Unsupported`. **Not an Apple path** — there it is MoltenVK over Metal, which cannot beat the Metal backend it translates into |

Backend interface (`clay::eval::Backend`), identical everywhere:

- `eval_points(tape, points[]) -> distances[] / gradients[] / colors[]` — batch field queries
- `eval_points_batch(tapes[], runs[]) -> distances[] / gradients[] / colors[]` — many point runs, each against its own (typically per-brick culled) tape, in one call; the CPU backend dispatches the flattened batch across its thread pool, and the brick-mesh attribute pass (gradient normals + colors) evaluates through it
- `eval_bricks(tape, brick_ids[]) -> narrow-band brick data` — incremental cache fill
- `raycast(tape, rays[]) -> hits[]` — picking/rendering support
- `mesh(tape | bricks, params) -> triangles` — GPU meshing where supported
- capability flags (fp16 storage, meshing on device, max tape length)

Backends are runtime-registered; the CPU backend is compiled in unconditionally. Parity suite runs every registered backend against CPU scalar on every kernel and on composed scenes.

### Devices a caller lends us

A registered backend creates and owns its device, which is right for a headless
library and wrong for a host that was going to draw on a GPU anyway: results are
computed on our device, copied to host memory, and uploaded again to theirs.
`eval::make_backend(name, DeviceHandles)` returns a backend bound to the
caller's device — as an **instance the caller holds**, never a registry entry,
because two hosts with two devices cannot share one process-wide slot under one
name. Registration means exactly what it meant before and is unaffected.

Vulkan and Metal adopt; CPU, OpenCL and CUDA report that they cannot, and a
caller whose adoption is refused falls back to the registered backend and gets
**identical values**. Adoption changes where work runs, never what it computes.

The ownership contract, which is what keeps this from becoming a source of
crashes inside someone else's driver:

- The library **retains nothing it did not create and destroys nothing it did
  not make**. A `DeviceHandles` holds borrowed handles; the backend's
  destructor releases its own pipelines and buffers and leaves the caller's
  device, queue and instance alone.
- The library **creates, destroys and waits on no synchronization primitive
  belonging to the caller**, and submits to a supplied queue only inside a call
  the caller made.
- Work issued during a call has **completed when that call returns**. Nothing is
  left in flight with no way for the caller to know when it lands.
- **Calls on one device are the caller's to serialize**, exactly as they are for
  `brick::BrickCache`. A GPU queue is not free-threaded, and adding a lock here
  would be a threading policy the consumer did not ask for.

`Backend::eval_grid_device` writes results into a caller-owned buffer slice.
Values stay `float32` and are **not** quantized on the device even though the
brick cache stores fp16: quantization and band classification are
`BrickCache::submit`'s, and a device path that did them would be a second
implementation of the step most able to drift. That one implementation is a
software round-to-nearest-even conversion, bit-identical on every platform,
and it is exact through the subnormal halves too: a distance below 2⁻¹⁴
(6.1e-5) is stored as the nearest multiple of 2⁻²⁴, with values under 2⁻²⁵
rounding to zero. It used to shift those by the wrong amount and store a
huge value or a NaN, so a lattice sample that landed within 6e-5 of the
surface poisoned the eight cells around it — a brick raycast through one of
them fell off the ray and missed the whole model.

`Backend::eval_grid_batch_device` is `eval_grid_batch` for that caller-owned
destination — the brick-refill shape, with grid *i* landing at the same fixed
slot the host-memory batch uses. The default loops `eval_grid_device`, so any
adopting backend answers it with identical values; the Metal override runs the
whole batch as **one dispatch** through the same concatenated-tape kernel as
the host-memory batch, which is what `clay_brick_cache_eval_requests_device`
rides: without it the zero-copy path paid a command buffer and a wait per
brick and sat 25-165x behind the host-memory route it exists to beat.

**The device destination resumes too** (#345). The host-memory refill has kept
each brick's float32 result as a seed since #306, so a dab costs what the dab
adds; its device sibling had none of that and walked the whole surviving edit
list over every sample, every dab — which put the callers most likely to be
latency-bound, a renderer evaluating into the buffer it will draw from, on the
slowest route in the library. `clay_brick_cache_eval_requests_device` now shares
the document's seed store with the host-memory form: a brick that can be resumed
is answered on the host by the same code the host-memory refill runs and
**written into its own slot**; a brick that cannot is evaluated on the device
into that slot as before and **read back** so it becomes the next dab's seed.
On an RTX 5060, a 12-brick window over a 20,000-item sculpt went **59.3 ms to
0.27 ms** per dab, and 5,000 items **15.2 ms to 0.08 ms**.

The two new primitives that join the halves are `Backend::write_device_buffer`
and `Backend::read_device_buffer` — host memory into a caller-owned slice and
back, `caps().device_copy` saying which backends have them. **Default false and
Unsupported**, so a backend without them takes the whole-batch full walk it took
before: correct, silent, and exactly as fast as it always was. Vulkan implements
them with a **compute shader**, not `vkCmdCopyBuffer`, because nothing obliges a
caller to have created its buffer with `VK_BUFFER_USAGE_TRANSFER_SRC/DST` —
the storage binding the evaluation path already needs is the one usage that can
be assumed. Metal implements them with an **`MTLBlitCommandEncoder`** (#350),
which the Vulkan restriction does not reach: an `MTLBuffer` carries no transfer
usage flag to be missing, so every buffer is a legal blit source and
destination, `MTLStorageModePrivate` included. Staging is one of *our* shared
buffers rather than the caller's pointer, for the same reason — a private
buffer has no `contents()` to memcpy through, and assuming otherwise works on
every unified-memory Mac and faults on the first host that allocated private.

Both are served **only by an adopted backend**. `device_copy` names a buffer the
caller lent us, and one from the device the registered backend made for itself
is not that; the registered `"metal"` and `"vulkan"` entries report false and
refuse. CUDA and OpenCL have no adoption path at all — `eval::make_backend`
serves Metal and Vulkan, and OpenCL is not in `eval::DeviceApi` — so a caller
cannot obtain a `clay_device` for them and the question does not arise.

The seed is kept only where this path can say what it means: with **more than
one visible SDF layer** a seed is two values — the active layer's and the
accumulated field of everything beneath it — and this path evaluates the
document whole, so what it could store is neither half. It stores nothing rather than something
mislabelled, and multi-layer documents take the full walk here. The host-memory
form keeps the two halves apart and resumes them fine.

The shape this **did not** take, and why, is measured rather than argued
(`openspec/changes/resume-the-device-refill/design.md`): making the seed
device-resident so the suffix evaluates on the GPU cannot win on the windows a
sculpt actually submits. A seeded kernel still has to dispatch, and one dispatch
of the emptiest possible tape costs **23 µs a brick** on that GPU, where the
whole host-side resumed refill of 12 bricks — including the per-brick suffix
compile a device path would also have to pay — is **18 µs** at 200 items and
**155 µs** at 20,000. The copy it would save is 24 KiB, under a microsecond.

The limit worth stating plainly: this makes evaluation **output**
device-resident, **not** brick **storage**. Generations, staleness,
classification, quantization and the memory budget are host code over host
memory, and that is where a submitted brick becomes a stored brick. A host on
the device path owns the conversion, and the cache's guarantees are not in play.

**A backend registers when its core operations work, not when all of them do**
(#63). A GPU runtime builds one pipeline per kernel and they fail
independently: on Apple Paravirtual GPUs — which is what macOS VMs and
GitHub's macOS runners are — `clay_raycast` would not compile while the
evaluation kernels did. Requiring all of them threw the whole backend away and
left `clay_list_backends` answering `cpu`, which is exactly what a build with
no Metal compiled in answers. Three release attempts were spent on that.

So point and grid evaluation are the bar. An operation whose pipeline is
missing reports `false` from `caps()` and returns `Status::Unsupported` when
called — the refusal a caller already handles for `mesh()` — and the backend
keeps accelerating everything else. Batched operations are deliberately not
part of the bar: the base class loops over the single-item form with identical
values, so an unavailable batch kernel costs submission overhead rather than
capability.

Two calls make this visible to a host, because a registry that answers `cpu`
for both "no GPU in this build" and "this machine's GPU was rejected" gives a
host no way to tell a missing feature from a broken one:

- `clay_backend_supports(name, op, &supported)` — per operation. Branch on
  this.
- `clay_backend_diagnostic(name, buffer, &size)` — why a backend is missing or
  partial, carrying the runtime's own words: a pipeline that failed to compile
  contributes the compiler's log verbatim, since that is the text that
  identifies the cause and a host filing a bug cannot recover what we already
  discarded. Diagnostic prose for a human, not a parseable status.

A partial backend keeps its ordinary name. A host asks for `metal` to get
acceleration, and per-operation refusal is how this interface already says "not
from me"; a second name would force every host to match a string it has never
seen, which fails harder than the `Unsupported` it must already handle.

## 6. Scene model & evaluation semantics (`clay::scene`)

The document tree the app and specs already define, owned here so every consumer agrees:

- **Layers** (`voxel` | `sdf`), each with transform, visibility, resolution, material; SDF layers hold the **ordered edit list** — items apply to the combined preceding result; groups (nesting ≥ 4) carry group ops incl. None; layer instancing with shared content. An SDF layer also carries a **composition** — op, blend profile, blend radius, rounding, the same four an item carries — and the visible SDF layers FOLD under it rather than unioning (see *A layer that cuts the layers below it*).
- **Influence bounds** computed per item/group (AABB ⊕ blend ⊕ rounding), used for brick dirtying, culling, and the locality guarantee (distant edits leave existing bricks bit-identical — regression-tested).
- **Tape compiler**: edit list → flat tape; per-brick tape culling (only edits whose influence bound touches a brick appear in its tape — the Dreams design).
- **Cull index** (`scene/cull_index.h`): per-revision cache of everything the per-brick cull consults — item geometry bounds (splines tessellated once, not per brick), group influence bounds, the cull pad — the feathered replace's band plus the DRAG a smooth chain needs beyond it (#282): per profile `min(support, k · envelope(N))`, the envelope rising with `log2` of the layer's effective contributor count (nodes times their mirror and radial copies, each a leaf the tape folds through a seam blend) and the layer's seam `k` folded as its own term capped at what the item maxima alone would set, so the pad reaches the support only past ~800 quadratic contributors and is never wider than the pre-#335 `max(support, k)` (#335). Because it depends on the counts, the index keeps RAW per-profile maxima and resolves the envelope at read time — folding per node would freeze each node's `N` and leave an appended index below a fresh build. A **hard** blend drags nothing however its `k` reads, since its smin is a step and the running value is a plain `min()`, so it contributes nothing to that pad (#335); `Paint` and the extended modes do drag, because their colour fade and their channel both read `k`. A node's OWN bound keeps its `max(support, k)` dilation either way — in a mixed chain that is margin for the drag its smooth neighbours apply, and taking it away measured 540 → 10,105 band-clamped disagreements where narrowing the pad alone measures 540 → 540 — plus a per-batch **coarse cull plan**: one test of every chain against the union of a batch's brick regions yields survivor lists carrying the cached bounds, so each per-brick compile walks only the items near the batch instead of the whole document. Non-local items always survive; chains holding a feathered volume replace are never pruned (the feathered/hard replace choice reads what the cull dropped from that chain). Pure acceleration: the emitted tapes are byte-identical to the unindexed compile, regression-tested on an adversarial corpus (mirrors, groups, deformers, feathered volumes, non-local layers). The C ABI keys the index on the document revision, exactly as the cached whole-document tape — and, exactly as that tape, an APPEND extends the cached index rather than rebuilding it. A **brick refill resumes** on the same terms: `clay_brick_cache_eval_requests` keeps its own float32 results as per-brick seeds, and a brick carrying one evaluates the appended items onto it instead of the whole surviving edit list over every sample — **7.13 ms to 0.13 ms at 20,000 items**, bit-for-bit identical, carrying **colour** too — the seed keeps the colour the prefix reached, because a coloured walk folds a `CTapeValue` and continuing from the distance alone would fold every combine against black (8.12 ms to 0.15 ms at 20,000 items with colour requested) — and falling back in full wherever continuing would not be exact (the cull pad moved, the brick's own prefix produced no accumulator, the seed kept no colour when one is asked for, or the edit was not an append to the layer an append extends). **More than one visible SDF layer is no longer a refusal**: the seed keeps the half BENEATH the active layer as its own value — static while that layer is sculpted — and the refill applies the fold itself, through the kernel's own combine. That split is available while the SEAM it is taken at — the last visible SDF layer — folds with a plain hard Add, which is every document written before layer composition existed and every document whose top layer unions; a composed seam refuses the split and stores no seed, because the refill holds six floats and no document and cannot be told which operator to rejoin them with (`scene::layer_join_is_hard_union`). 16.41 ms to 0.17 ms at 40,000 items across two layers. `scene::compile_document_part` emits one side of that split, both sides culling under the whole document's pad, since a part compiled under a smaller pad drops items the whole compile keeps.. Bounded at 64 MB — bytes rather than bricks, since a coloured brick carries four times the floats — and evicted **least recently USED** (#346). The order is refreshed wherever a seed is used: handed out to answer a refill, rewritten by the resumed path, re-stored by the full one. Evicting by FIRST STORAGE, which is what it did, is exactly backwards for a stroke — the working set is stored at the first dab and rewritten by every dab after, so it sat nearest the front and a store under pressure dropped precisely the bricks the next dab was about to ask for, keeping ground the brush crossed once and left. Not reachable at 64 MB for a dim-8 distance-only cache (2,048 B a brick, so 32,768 of them), but a dim-16 coloured one is 64 KiB a brick, or about 1,000, which a large model does reach. The order holds ONE place per stored seed and no others: a region invalidation removes an entry's place with the entry, where it used to leave it behind — the order's own memory is not counted against the budget, so those grew outside the ceiling without bound, and a brick discarded and stored again was given a second place. The bytes counted are what the entries HOLD rather than what they are using, since a refill carrying no colour empties an entry's colour buffer without releasing it. Two carve-outs on the ceiling: the most recently used seed is kept whatever the budget says, since a budget below one brick would otherwise drop what the caller just stored, and the resumed path's rewrite is the one store that does not evict, so a stroke adding colour to entries that had none can sit above the budget until the next full refill trims it. An edit that is not an append no longer drops the lot: a seed is the value of that brick's CULLED tape, so an item whose influence misses the brick's cull region was dropped from it anyway and the seed survives, carried to the new revision. `command_influence_bound` supplies the reach, taken on BOTH sides of the apply and unioned — one side is not an answer, since an add's node is not there before and a removal's is not there after. Undoing an item no brick can reach went **20.79 ms to 2.64 ms** at 50,000 items. An edit whose reach is not known still drops everything, which stays the default. **A GESTURE STATES ITS REACH ONCE** (#358). Deriving the region per command is right for a single edit and wrong for a gesture that issues one command per node it touches: `clay_layer_move_surface` issues 257 over a 1,000-item document, and paying two `command_influence_bound` calls and a seed-store walk for each of them cost that path **1.34x** for four releases — over the device gate's tolerance, under its noise floor, so nothing failed. A drag can state its region exactly and in O(1): the warp's weight is zero outside `radius` of the centre, and a point with zero weight is not moved, so no sample outside that ball can evaluate differently. That is cheaper AND tighter than the derived union of 257 whole item bounds. `GestureRegion` invalidates once for the bracket, on the failure path too, since a gesture that applied three commands and then refused has still changed the document; a caller that cannot state its reach keeps the derived one, which is always correct. 0.134 ms → 0.097 ms, back to the pre-regression figure. **PER BRICK, not per batch** (#342). The gate used to require every brick of a call to carry a seed at one shared revision, and nothing re-stamps a seed but the refill that writes it — `touch_appended` leaves the store alone and a refill re-stamps only the bricks it filled — so a dirty window that MOVES always mixes the ground the last dab covered with ground it had not, and one disagreeing brick sent all of them down the full walk. The fast path fired only while the brush stood still. Each brick is now carried forward from its own revision, with one plan memoized per distinct revision (a moving window holds one or two) and unservable bricks falling into the miss gather alone: a four-brick window sliding one brick every third dab went **4.60 ms to 0.15 ms** per dab at 20,000 items, and its worst dab — the hitch as the brush crosses a brick plane — **19.50 ms to 1.36 ms**. A plan is also refused when the appends went to a layer OTHER than the one the suffix would extend: NodeIds are per-layer, every layer numbering from 1, so `compile_layer_suffix`'s tail check could agree by coincidence and fold the active layer's own last node on twice while the dab actually made was never evaluated (0.49 in distance, ten cells, silently). **The resumed loop no longer holds the document cache mutex while it evaluates** (#348). It used to compile AND evaluate every brick under the one lock it takes to read the seeds — serial, where the full path it replaces hands the batch to `eval_grid_batch` and spreads every row of every brick over the pool, and blocking `clay_eval_points` on another thread for the duration, since `cache_mutex_` guards the tape cache, the cull index and the append log too. The lock was held because `seed_for` hands out a raw pointer into the seed store and a concurrent refill may rewrite or evict the entry under it, so the seed is COPIED OUT under the lock instead — into the buffer the evaluation writes its answer to, which in the single-layer case is the caller's own output slot and so costs one `memcpy` and no second pass: `eval_points_seeded` reads a block's seed into its stack before it writes that block's result, so a seeded walk may run in place (#306's open question 7, answered). Compile and evaluate then run with the lock down; it is retaken to store what the bricks reached, with the revision re-checked exactly as `store_seeds` re-checks it. On a 24-thread desktop at 5,000 items, the lock beyond the cull-index copy it has to pay under it either way went **65.8 µs to 10.0 µs** for 48 bricks at a sixteen-dab suffix, **19.4 µs to 2.4 µs** at 12 bricks, and **22.4 µs to 6.8 µs** for 48 bricks at a one-dab suffix; the copy costs about **0.05 µs a brick**, inside the noise even at one brick. The deferred phase goes over `clay::parallel::ThreadPool` only when it is worth a dispatch, which is a narrower window than it looks: an empty `parallel_for` over 48 units costs 16–19 µs there, so 48 bricks at a sixteen-dab suffix is 66 µs of work and goes **66 µs serial to 31 µs pooled**, while 12 bricks of the same suffix is 21 µs and would go the wrong way, to 25. The gate is therefore a count of SAMPLE-INSTRUCTIONS — samples times appended items, so it needs no restating for another lattice or a longer suffix — set at 262,144, about three times the dispatch; below it the walk stays serial, off the lock either way. The gate is calibrated CONCURRENTLY as well, since letting several host threads refill at once is the point of coming off the lock and `ThreadPool` holds one job slot — a dispatch from a second thread replaces the advertised job, degrading toward serial rather than deadlocking. Measured at 48 bricks and a sixteen-dab suffix with 1 to 12 threads each refilling the same window, pooled stays **0.77–0.92x** the serial branch throughout, so the constant holds under concurrency; and the fan-out clay.h actually recommends — one batch of requests split across threads — puts every slice under the gate by itself, so nothing dispatches and the collision cannot arise. What is left under that lock is mostly the per-revision cull index, which the call copies to extend and which stays there, because the cache is what the lock is for. Nothing about a refill's answer depends on any of it: a new case races three refill threads and three `clay_eval_points` threads over one document and checks every window bit-for-bit against a document that never resumed, clean under `asan-ubsan` and under ThreadSanitizer — and with the copy reverted so the raw pointer stays live across the unlock, TSan reports the race the copy exists to prevent while the values still agree — the mutant's assertions pass and doctest reports SUCCESS. A sanitizer rather than a value check is therefore the only thing that can guard it, so ThreadSanitizer is a `tsan` preset and a per-pull-request CI job rather than something someone remembers to run; the whole suite is clean under it. The run needs ASLR off (`setarch -R`), or TSan aborts on an unexpected mapping before a test executes. Because the two paths are bit-identical by contract, nothing about a refill's output can say which produced it — `clay_document_resume_stats` reports cumulative `resumed_bricks` / `refilled_bricks` and the seed store's occupancy, which is what makes a fast path that stops firing visible rather than merely slow. **A seed is keyed by what it describes**, not merely by where it sits (#349): the brick coordinate, the LATTICE (dims and voxel size) and the BAND. A brick's tape is culled against `request_brick_box(req).dilated(band)`, so a narrower band drops items a wider one keeps and a seed taken under one band continued from a different field — a 0.15-band seed serving a 0.6-band request measured 9 of 512 samples wrong, worst 0.105 (two voxels), at a true distance of 0.354 that is well inside the band asked for and so a sample a submit stores rather than clamps. The cull PAD is the other term of that dilation and stays a gate rather than a key: it is a document-global maximum that moves under a single cache, where the band and the lattice are fixed properties of the cache that asked. Keying on the lattice also stops two caches over one document — a viewport and a mesher sharing a brick coordinate — from evicting each other's seeds on every call: `shaped_entry` refused the mismatched entry so the answers stayed right, but `store_seed` overwrote it and `resumed_bricks` sat at zero for both. All three caches read ONE append log, each taking its own tail of it. A stroke bumps the revision per stamp, and a rebuild walks every node recomputing bounds that did not move: **2.42 ms at 50,000 items against 0.13 ms to extend**, of which 94% was bounds. `CullIndex::append` refuses wherever it cannot be sure the document changed only by that append, so being wrong costs a rebuild rather than a wrong cull, and the index it produces is the one a rebuild gives — per-brick tapes byte-identical, held in `test_cull_index.cpp` over the adversarial corpus. **Extending it costs the dab, not the document** (#347). What was left once the rebuild was gone was an append that still walked the document twice: `CullIndex::append` re-walked the touched layer's flat node map to recompute the cull pad, and the ABI deep-copied the cached index to protect a reader that might be holding it against a plan — 0.054 ms and 0.043 ms at 20,000 items, together 79% of a 0.123 ms resumed dab. The pad is now kept as its two terms PER LAYER and raised from the appended subtree alone, including the children of a group the build never descends into: per layer a maximum of sums and a sum of maxima are the same number, which is what makes that exact rather than merely safe — one global pair of maxima would report a pad larger than a fresh build's. An append that cannot see the layer's node map grow by exactly the subtree it names refuses, so the pad can never be left BELOW what a rebuild reports, which is the direction that loses items a brick needed. And the copy is taken only when someone IS holding the index: every reader takes its handle under the cache mutex and holds it while it reads, so a use count of one under that mutex means no snapshot exists and the append extends the cached index in place. That decision is `scene::append_cached`, a free function rather than a branch inside the ABI's cache, so both halves of it are asserted directly — a stroke's consecutive appends extend one index with no copy taken, and an append made while a holder exists leaves the holder's pad and entries exactly as they were. An append at 20,000 items went **0.0542 ms → 0.000258 ms** and stopped scaling with the document — 1.00x against the same append on a document a tenth the size, where it was 11.9x, gated as a ratio in `tools/check_bench.py` — and the whole resumed dab behind it went **0.1233 ms → 0.00299 ms** with the same bricks resumed (medians of 7, load average 1.07 → 1.02 across both runs). The index carries its own append log rather than sharing the tape's, which is single-consumer by construction, and `clay_brick_cache_eval_requests` / `_device`, `clay_eval_grid`, `clay_tape_export` and the brick-mesh attribute pass all compile through it.
- **Undo command vocabulary**: every mutation is a serializable command with an inverse (add/remove/reorder item, set-param, voxel-span edit, layer ops). The in-memory undo stack and the document file share this one vocabulary — one serialization story, tiny undo steps, stroke-level coalescing.

`clay::brick`: sparse virtual grid of 8³/16³ bricks, fp16 narrow band (±3 voxels), dirty-set tracking, async-friendly (evaluation requests are plain data; the app owns threading/queues via the backend), LOD mip bricks for far view.

### Warming a cold brick window off the interaction thread

The resumable refill made a dab flat in document size — but only for bricks that
already carry a seed. **A brick with no seed still walks the whole surviving
edit list**, and that cost follows the size of the sculpt. That is what is left
of #306, and it is a *tail* problem rather than a median one. A moving stroke on
a worked sphere, 24 dabs, dabs spread evenly:

| items | resumed / refilled | median dab | **worst dab** |
|---:|---|---:|---:|
| 1,000 | 888 / 96 | 0.290 ms | 3.96 ms |
| 5,000 | 888 / 96 | 0.170 ms | 12.02 ms |
| 20,000 | 888 / 96 | 0.157 ms | 45.15 ms |
| 50,000 | 888 / 96 | **0.155 ms** | **113.31 ms** |

90% of bricks resume, the median is flat across 50x the document, and the worst
dab scales linearly — because a moving brush keeps reaching ground it has not
covered. Where a cold window's time goes, at 50,000 items: the cull index 2.9
ms, compiling twelve culled tapes 18.7 ms, and **evaluating them 68.4 ms**,
already spread over ~11 of 12 cores. There is no parallelism left to win and no
constant to shave: a brick dilated by its band and the chain pad genuinely
overlaps 8,126 of the 50,000 dabs, and an exact answer has to evaluate them.

**So move it, rather than make it cheaper.** The seed store belongs to the
*document*, not to a cache or a thread, and
`clay_brick_cache_eval_requests` is free-threaded against a const document. So a
worker can pay the cold walk and leave seeds the interaction thread resumes
from. Measured on the same 50,000-item document and the same twelve bricks:

| | interaction thread pays |
|---|---:|
| no warming | **107.99 ms** |
| the window warmed on a worker first | **0.004 ms** |

with the values **bit-identical** and `resumed_bricks` confirming the fast path
fired. That last part is the half worth testing rather than assuming: a prefetch
returning "close enough" values would put a silent correctness bug inside a
performance feature.

**What it costs is an edit.** The warming refill is only safe while the document
is not being mutated, so it belongs at pointer-down, between strokes, or on a
camera move — not between the dabs of a live stroke, where every dab is a
mutating call. A host that warms the neighbourhood a brush is about to enter
pays for it once, before the stroke that needs it.

### A brick proven uniform is not walked

The cold walk above is mostly spent on bricks that store nothing. A dab dirties
the solid box its influence bound covers, and on a worked model most of that
box is clay: on the sculpted-sphere fixture (`BM_DabRefillSculpted`), **57 of
the 80 bricks** one dab dirties are uniformly inside at 400 dabs and 64 of 80
at 1,500 — and `BrickCache::submit` is where a brick is classified, from its
samples, so each of them paid 512 walks of its culled tape to be told it holds
no lattice.

One evaluation says the same thing. The compiler tracks every tape's Lipschitz
bound *L* (the number the raycaster steps by), so `|f(x) − f(c)| ≤ L·|x − c|`;
every lattice sample sits within the lattice's half-diagonal *hd* of its centre
*c*; so when

```
|f(c)| > band + L · hd
```

no sample can be within the band, let alone cross zero, and every one carries
`f(c)`'s sign. The brick is uniform, its class is what submit would have found,
and it is a **proof rather than a heuristic** — a sample inside the band would
contradict the bound. The ball is the **lattice's own**, not the band-dilated
cull region (that puts the band term in twice and collapses the proof rate to
~21%), and the bound read is the brick's **own culled tape's**, never the whole
document's (the cull is what keeps *L* small — a deformer far from the brick is
not in this tape). Measured on the fixture, **91% of the uniform bricks pass at
400 dabs and 97% at 1,500, none falsely**; the rest walk as before.

**The bound has to be a slope bound.** `exactness.h` keeps *L* = 1 for the
underestimating fields — an ellipsoid, a tri prism, an overflowing repeat, a
loft, a sweep, a sampled volume — because `|f| ≤ distance` makes stepping by
*f* safe, and that says nothing about `|∇f|`: an ellipsoid's bound field
measures a slope of 1.09 near its tips and 3.6 for a needle-shaped one, and
three deformer factors are exceeded outright by the same 400k-pair probe (taper
1.03–2.5×, `wrap_around` 1.05×, `bend_curve` 7–9× at the guide's ends). Under
such an *L* the inequality is false by more than its margin: a needle ellipsoid
on the lattice diagonal proved 852 of 1,000 bricks and stored one the walk
finds SURFACE as OUTSIDE — the mesh was unchanged, the stored state and halves
were not. The compiler folds `Tape::lipschitz_bounds_gradient` beside `info`,
false once any item in the tape brought one of those fields or deformers and
copied with the prefix on an append, and the gate refuses a tape without it —
on the full path and on the proof-carrying suffix alike. The refusal is per
brick, through the cull: a brick whose region the ellipsoid does not reach
compiles a tape without it and keeps the gate.

A proven brick keeps its place in the batch: a **stub tape** — one
`ctape_empty`, the far field for an outside brick and, through a zero scale
and a rounding of `band + spacing`, the constant `−(band + spacing)` for an
inside one, carrying the colour submit reads at sample *n/2* — takes its
tape's slot, so the device
copy, the fixed-stride destination and submit all see an ordinary brick, and
what the cache **stores** is bit-identical to the walk's. What the refill
**returns** for such a brick is the stub's samples, every one beyond the band
with the brick's sign, which is the only property of a uniform brick's values
that anything downstream reads (`clay.h` says so at the entry point).

**A gated brick's seed is its proof**, not the stub. Storing the stub would
poison the store — the next dab's suffix would fold onto numbers the field never
produced, and a seeded answer is bit-identical to a walked one by contract, so
nothing could tell — and storing nothing measured worse than the walk it
replaced: a seedless brick pays its culled compile again on every dab, and the
warm dab went **0.55 → 2.5 ms at 400 items and 0.44 → 8.4 ms at 1,500**, for a
cold-dab win of 2x. So the entry holds the field's unclamped value and colour at
the centre and at sample *n/2*, plus the tape's bound; the next dab folds its
suffix onto those two points — the walk's own arithmetic there — and re-proves
the brick with `max(stored, suffix)` as the new bound. That bound is exact when
every appended item folds by a max rule (Add, Subtract, Intersect, hard or
smooth); anything else — extended modes, transitions, gates, a group — takes the
full path, where the gate reads the bound the compiler declared. A dab that
reaches a proven brick fails the re-proof and walks it once, as a cold brick
would. A proof counts as **resumed** when it is carried forward and as
**refilled** when it is made on the full path (it neither walks nor resumes
there; `clay_internal_gated_bricks` counts those), so the ratio `clay.h`
documents keeps its meaning.

Cold dab through the C ABI, medians of 7 on an M2 Max: **9.22 → 5.58 ms at 400
dabs (1.65x), 33.4 → 14.1 ms at 1,500 (2.4x)**; a whole-model fill of 9,240
bricks **580 → 335 ms and 2,213 → 1,180 ms**, with the same brick counts in
every class. The warm dab — the same window, one appended dab later — goes
**0.52 → 0.25 ms at 400 and 0.44 → 0.17 ms at 1,500**, because a brick carried
forward as a proof skips the 512-point seeded walk a lattice seed still runs.

### A layer that cuts the layers below it

A layer used to be organisation and nothing else. Visible SDF layers hard-unioned
into the ones beneath them, unconditionally, so an artist who wanted one shape to
cut another had to put both in the SAME layer — and then could no longer hide the
cutter, reorder it, transform it or lock it as a thing. `scene::LayerComposition`
gives an SDF layer the four values an item already carries — **op, blend profile,
blend radius, rounding** — and the document compile stops unioning and **folds**:
the first visible SDF layer initialises the accumulator, and every later one
combines with what is below it. `A − B + C` is a stack, hiding a subtractive
layer restores the uncut geometry, and layer order is geometry.

**A layer boolean IS the item boolean.** The same `scene::Op` and
`scene::BlendProfile`, the same `Compiler::emit_chain_combine` a group's tail
already emitted, the same `kernel::ctape_combine_values` at run time. No second
vocabulary, no second evaluator, no second copy of the kernel math — and no
second cost: the same 2,000 dabs folded as 1,000 layers and as 1,000 groups
inside one layer compile to **3,999 instructions each**, at **0.376 ms against
0.488 ms**, and 1,000 layers costs 1.02x what 10 does over the same items
(`BM_LayerFoldStack*` / `BM_ItemFoldStack*`, gated as ratios and as an
instruction count in `tools/check_bench.py`). A document expressing a shape as
two layers and a document expressing it as one layer of the same items agree in
distance, colour, bounds and safe step, sample for sample.

**The first visible layer's operator is not applied.** `Subtract(empty, A)` and
`Intersect(empty, A)` are the two ways a stack opens with nothing on screen and
no error, and an artist who drags their base layer to the top would meet both.
The item rule's OTHER half does **not** lift for that first layer: an item that
opens a chain with a carve is SKIPPED, and a layer that opens a stack with one
INITIALISES, because skipping is the empty frame this rule exists to prevent.

**Which layer is first is a property of the DOCUMENT.** It is read off the
visible SDF layer list and never off the accumulator a particular compile
happens to hold. A per-brick tape can drop every item of every layer beneath a
composed one — that is what culling is for — and a compiler that decided
first-ness from its own accumulator would then stop applying that layer's
operator for that brick alone: an intersecting cutter returning a solid sphere
where the document has nothing, a subtracting one rendering as a lump, in one
brick and not its neighbour, with no error and no counter. For a layer that is
not the document's first the item rule DOES lift, in full: with the accumulator
absent a carving operator drops the layer (over nothing, a subtract and an
intersect *are* nothing), `Shell` and `Replace` fold against an explicit empty,
and a union takes the layer as it is — verbatim what `compile_list` and
`compile_group` do with an item that opens a chain, which is what keeps a
subtracting LAYER and a subtracting ITEM the same document.

**An empty layer is not always a no-op.** A layer whose chain produced nothing —
empty, all hidden, or culled out of one brick — is skipped where its operator
reads an absent operand as no change, and folded against the far field where it
does not. Union and subtract are identity there; **intersect is not**, and an
intersecting layer that was skipped leaves material the whole-document tape
removes. That disagreement is per-brick, so it appears only inside bricks the
intersecting layer does not reach and only in the culled path. The predicate
probes `ctape_combine_dist` rather than tabulating the answer, so a mode whose
math moves cannot leave a stale table behind.

**Symmetry resolves first.** Mirror and radial copies are emitted per item and
folded into the layer's own chain long before the layer folds, so a layer
combines **once**, with the shape its symmetry made. Combining each copy
separately would change the result wherever the blend is smooth, because a
smooth combine does not associate.

**Bounds and the cull pad follow the fold.** A smooth or extended fold bulges
past the union of both operands, so the layer's extent enters `tape.bounds`
dilated by the fold's OWN support — `scene::chain_blend_support`, the single
expression `group_blend_support` and the item path already use. Without it 11,618
lattice samples in the fixture carry material outside the box the tape reports,
which is a dropped brick and a lost ray hit rather than an error. The same
support enters `cull_pad_terms` as a per-layer constant, so `document_pad` and
`CullIndex::refresh_pad` — a maximum over layers of that, each — pick up the
inter-layer term they had no slot for; without it 11 of 21 samples in the
fixture's region differ between a per-brick compile and the whole-document one,
inside the band where nothing is looking. Exactness and the Lipschitz bound fold
as the item combine folds them, `cfi_relief` included.

**What a composition costs, and where.** A composition change is an ordinary
layer-property command: one undo step, no structural bump, no retired prefix
seeds. Its dirty region is the layer's own extent dilated by the fold's support
— which is right for a subtract, because a subtract cannot create material
outside its left operand — and for an **intersect** it is unioned with the extent
of the visible SDF layers BENEATH, because `max(acc, item)` takes material away
everywhere the accumulator has any. The same widening covers hiding an
intersecting layer and reordering one, both of which the naive box gets wrong in
the direction that leaves stale bricks.

An edit made INSIDE a layer is not widened by the OPERATORS above it — a combine
is pointwise, so it changes the folded result exactly where it changed the
accumulator — but it IS dilated by their **supports**: a smooth or extended fold
moves its result up to its own support away from where its operands moved, so an
item edit's reach is its own bound dilated by the fold its layer enters through
and by every fold above that, summed (`folds_from_layer_support`). It is the
dilation `node_reach_bound` already applies once per enclosing GROUP, one level
up, and it has to live in `node_command_bound` because that is the function
holding the Document. Without it an ordinary dab into a document with one soft
fold leaves band-relevant samples changed outside the box the command reported.

And a command that changes **which layers are visible SDF layers** — add,
remove, hide, show, and the remove-and-add pair `clay_document_move_layer` is —
can move the first-visible rule onto the layer above it, which is a change over
that layer's OWN whole extent rather than over the edited layer's. Hiding the
base under a subtractive cutter promotes the cutter to the initialiser and it
comes back as material everywhere its shape is. `first_visible_flip_bound`
covers that, and only for a composed layer above, since promoting a hard union
from `min(acc, M)` to `M` differs only where `acc` had material — which is the
edited layer's own box, already in the region.

That intersect box is the conservative answer and it is expensive: the host
measured an intersecting item's live drag refilling **26.2x** the surface bricks
of the geometry it produces at reference size and **241.2x** at ten times the
extent — 45.5 ms and 7.5 s a frame. The intersect walks a BOX and produces a
BAND, and the narrowing that fixes it is a refill-REGION change rather than a
bounds one; it is not specific to layer composition, since an intersect item pays
the same numbers today.

**What it refuses.** `CLAY_OP_INLINE` is a group's children-apply-outward mode
and a layer has no outer chain to apply into; the two transitions read their
endpoints from a node, and a composition carries none. Both are refused at the
setter rather than compiled against defaults nobody wrote. A **non-SDF layer**
refuses at both ends — set and read — rather than storing a control that does
nothing or answering `CLAY_OP_ADD`, which a host would read as a fold the layer
cannot carry.

**Old documents are unchanged, by construction rather than by migration.** The
default composition is `Add` / `Hard` / `k = 0` / `rounding = 0`, which is the
hard union every layer has always folded with, and `CLAY_OP_ADD` and
`CLAY_BLEND_HARD` are both zero — so an all-zero composition IS the old
behaviour. A document written before the feature loads with every layer unioning
and compiles to the same tape, byte for byte. Going the other way is a
**refusal**, not a degrade: scene minor 18 is the first layout that can say a
composition, and writing a composed document at 17 would bring a cutter back as a
lump welded on, in a file that opens cleanly and looks deliberate.
`clay_document_writable_at_minor` is how a host asks before it saves, and it
names the layer that blocks. pyclay has the same question as
`Document.writable_at_minor(minor)`, which answers `(ok, blocking_layer)` — the
id rather than only a flag, so a script can say which subtool to change instead
of leaving a person to find it.

### An intersect is bounded by its layer

`item_influence_bound` reported `Everything` for any op that is not local, and
"not local" covered two things that behave differently. One of them has a finite
answer, and the difference is 1.8x on a drag: the reporter of #319 measured the
same object, the same drag, the same scene, differing only in the operation —
**19.39 ms subtracting, 35.52 ms intersecting**, none of it the intersect being
harder to evaluate.

- **An INTERSECT is bounded by its LAYER.** `max(acc, item)` can only take
  material away, and what it takes away is inside what the layer already
  occupies — it cannot put material where the layer has none.
- **A SPATIAL MORPH is not.** Its weight *saturates*:
  `ctransition_radial_weight` is `clamp((length(p.xz) − r0)/(r1 − r0), 0, 1)`
  about the **world Y axis**, so past `r1` the weight is exactly 1 and the result
  *is* the item's own field, arbitrarily far from anything the layer occupies.
  These keep `Everything`, and #319's report — which lumps "intersect, the
  spatial morphs" together — would have been unsound taken literally. That claim
  is *mechanical*: it is a statement about the kernel's weight, not about a
  fixture, and morph behaviour is unchanged here in any case. A probe of the
  radial morph leaks 4 points in 200,000 on arm64 and none on x86_64, which is
  reported and not asserted — the leaking points sit a rounding error either
  side of the band edge.

**What made this shippable was a sample count, not a fixture.** #326 held it
back on a real objection: nothing in the suite could tell a correct bound from a
wrong one, because the item's *own* geometry box is ~3x tighter than the layer's
and measured drift 0 as well. Four candidate fixture designs were proposed to
break the tie and **none was needed**. Moving the intersect of #319's own
sphere+box document leaves 34 drifting points in 400,000 — about 1 in 11,700 —
so the property test's 4,000 samples miss it roughly seven times in ten. At
200,000 it shows every run:

| candidate bound | worst band-clamped drift |
|---|---:|
| the item's own geometry | **0.100** (146 points) |
| the layer's extent | **0** |

against a 0.15 band, by exact equality, boxes dilated by `band + cull_pad`. So
#319 asked for the tightest bound that holds, not one 3.4x too loose. The
sample count is a property of how *rare* a violation is rather than of how hard
the document is — a local item's bound is its own geometry and a violation there
is dense, which is what the gnarly corpus finds at 4,000.

**The cull gate does not change.** `item_influence_is_local` still refuses every
non-local op, so an intersect still appears in every brick's tape. "May this be
omitted from a brick's tape" and "which bricks does moving it dirty" were always
different questions and only the second has the finite answer; keeping them
separate is what makes this safe. `item_own_influence_bound` also keeps the
infinite answer — it asks how far the item's own body reaches, which is what a
brush that has already reflected itself tests a drag against.

Measured on the reporter's case, bricks dirtied per drag frame over 20 frames,
1,000 tracked: subtract **64 → 64**, intersect **1,000 → 216**. 216 is exactly
the figure the triage on #319 predicted.

**The extent is walked, so share it (#451).** An intersect's bound is the
layer's extent, and taking that extent walks every visible node for its geometry
bound — a stroke or a sweep re-tessellates its curve, a mirrored item bounds
every copy, a deformer chain runs its slope probes. Before #319 an intersect
answered `Everything` in constant time, so nothing cared how many of them a
layer held; afterwards every caller that meets an intersect meets it in a loop,
and each of those went from O(items) to O(intersects × items). Measured on a
layer of 1,000 spline strokes holding 20 intersects: a layer bound **0.082 ms on
v0.73.0 against 1.61 ms on v0.78.0**, and an attributed raycast **1.51 ms
against 3.09 ms**, with a subtracting control flat throughout. The per-brick
refill did NOT move — it is the bound QUERY that regressed, which is why a host
saw it on an operation rather than on a code path.

`scene::LayerExtent` is the answer: a scratch memo taken once and threaded
through `item_influence_bound`, `node_influence_bound`, `node_reach_bound`,
`node_influence_bound_in_document` and `layer_influence_bound`, and held across
the loops in `layer_influence_bound`, `pick`'s attribution and
`clay_brick_cache_mark_dirty_nodes`. It is keyed on the (content, layer) PAIR,
because an instanced layer shares one `SdfContent` under its own transform and
therefore has its own extent. Passing nothing is the old behaviour, and a layer
with no intersect never asks it for anything. It is valid only for one query:
the extent it holds is the geometry of the nodes as they were when it was taken,
so it is a local, never a member.

What remains is one walk per query rather than none: `apply_edit` takes
`command_influence_bound` on both sides of every command, so a drag still pays
two. Removing that needs a revision-scoped cache on the ABI's document, which
`clay_document::cached()` is already shaped for.

### ...and a MOVE of one is bounded by its sweep

The layer-wide answer above is right for the question it answers and ruinous as
a refill region. The reporter of #471 measured the same drag, the same document,
the same 97 items, differing only in the operator: **3.8–4.2 ms subtracting
against 41.5–44.0 ms intersecting**, and **12.8 ms against 6.8–10.1 SECONDS** on
a fixture with ten times the extent. Nothing about an intersect is harder to
evaluate; what differs is how much gets re-evaluated.

The fix is not to weaken the influence bound — every consumer of it needs the
conservative answer, and per-brick culling may never drop an intersect. It is
that remeshing asks a NARROWER question. Not "where can this node change the
field" but "where can THIS EDIT have moved the surface", and for an in-place
MOVE the two have different answers. Outside the swept union of where the
operand was and where it went, the operand's own field is a positive beyond the
band on both sides, so `max(acc, item)` returns a beyond-band positive on both
sides and the band-clamped value a brick stores cannot have moved.

`scene::command_surface_delta_bound` is that second answer, for exactly one edit
kind: a `SetTransformCmd` on an existing, visible Intersect item with finite
support. It is `node_influence_bound_in_document` with the intersect arm taken
out — the operand's geometry bound, dilated by its layer's chain pad, by each
enclosing group's blend support and by the folds above, unioned over every
instancing layer — taken on BOTH sides of the apply and unioned, because one
side is not an answer. Everything else reports nothing and keeps the
conservative union, which is what makes the change narrow: no other edit's
region moves at all. It refuses a deformed operand (a warped field
underestimates distance by a factor the bound carries no dilation for), a
NON-UNIFORM per-axis scale on the operand or on its layer (the same
underestimate, by a factor of `max(s)/min(s)` — `cscale_nu_dist` multiplies the
local distance by the smallest component, so the field beyond the box is not the
distance the box was drawn against), a sampled volume, an unbounded or
infinitely repeated primitive, a GATE on the operand or anywhere in the layer's
chain, and a spatial morph in the chain or in a fold above — the cases where
"band-clamped equal" stops implying "equal", and a lerp downstream can carry a
beyond-band difference back into the band.

THE CHAIN PAD IS A TERM HERE AND IS NOT ONE IN A LOCAL OP'S BOUND, which is the
one place the two bounds are not the same expression. A local combine outside
its support is the identity bit for bit, so nothing downstream can see the edit;
an intersect's is not — the raw value out there is the moved operand's own
distance — so a smooth combine further down the chain can drag the difference
back toward the band. `cull_pad` is the measured distance over which it can and
is reused rather than re-derived.

`clay_layer_set_transform_bound` (ABI 0.90.0) is the host's half: the edit
`clay_layer_set_transform` applies plus the box it changed, in
`clay_layer_node_influence_bound`'s three-state shape, ready for
`clay_brick_cache_mark_dirty`. The generic queries are untouched — they answer
for an arbitrary edit and stay conservative — and a host cannot tell whether it
received the proved box or the fallback, because both are safe to dirty.

Measured on the issue's fixture, one drag frame, 98 items, one voxel size at
both extents (a 24-thread Linux desktop at load 3.5–4.1; the counts are
deterministic, the milliseconds are what that machine did):

| row | dirty bricks | AABB / layer | refill | total |
|---|---|---|---|---|
| subtract, reference | 63 | 2.9% | 2.81 ms | 5.04 ms |
| intersect, layer bound, reference | 576 | 100% | 22.63 ms | 27.9 ms |
| intersect, swept delta, reference | 121 | 7.9% | 5.66 ms | 7.51 ms |
| intersect, layer bound, ten times | 9,680 | 100% | 328.90 ms | 397 ms |
| intersect, swept delta, ten times | 286 | 1.2% | 12.19 ms | 15.7 ms |

The claim held is the COUNT, not the clock: going from the reference fixture to
one with ten times the cross-section, the same cutter making the same drag
dirties **540 → 1,152 bricks** where the layer bound dirties **900 → 15,600**.
The residual growth is the chain pad following the fixture's blend radii, which
scale with the form; it is not the extent.

### Drawing a preview beside the rest of the document

A live sculpt transaction previews **one layer** — that is what
`clay_sdf_smooth_preview_delta_take` hands over, and it is why a dab costs what
it touches rather than what the artist has already made. But
`clay_brick_cache_eval_requests` evaluates the hard union of every visible SDF
layer and attributes no brick to the layer it came from, so a host drawing only
the preview was drawing that layer **alone**: every other visible field layer
vanished for the length of the gesture. ClaySpaceDesktop's answer was to refuse
— open a live gesture only when the sculpted layer is the only visible one —
which took the feature away from exactly the documents subtools exist for
(#378).

The missing question was the third one. There was "the whole document"
(`clay_eval_points`) and "one layer" (`clay_layer_eval_points`), and no way to
ask for **every visible SDF layer except one**. `clay_eval_points_excluding`,
`clay_eval_gradients_excluding` and `clay_brick_cache_eval_requests_excluding`
are that question; `Document.eval_excluding` and `.gradients_excluding` are the
pyclay half.

**Composing is a minimum, and it is exact — while every layer unions.** The
union of two fields IS the smaller of the two distances, so
`min(excluding(L), your own preview of L)` is the field the whole document would
evaluate to — not an approximation of it. There is no blend parameter to match
and no seam to hide. A host takes the excluded evaluation **once at
pointer-down**, because the layers it excluded do not move while the artist
drags, and composes it with the live preview per frame.

That identity is a property of the UNION and it does not survive a layer
composition. With A, B(subtract) and C, and B excluded, the rest is `A + C` and
the part is `B`, and no combine of those two values is `(A − B) + C`: removing a
layer from the middle of a fold changes what every layer above it folds onto.
So the three excluding entry points and pyclay's `eval_excluding` /
`gradients_excluding` **refuse** with `CLAY_ERROR_INVALID_ARGUMENT` as soon as
any applied fold is not a hard Add, naming the layer that composes. The compile
underneath (`scene::compile_document_except`) still means what it always meant —
the document without that layer — and is unchanged; what is deleted is the
promise that it sums back.

**The refusal names an alternative, not a wall.** The pairing that survives a
fold is **below + the previewed layer**, and
`clay_brick_cache_eval_requests_below` (ABI 0.86.0) is the half that was
missing. It answers every visible SDF layer *beneath* the one you name, folded
exactly as the document folds them — the layers below may compose however they
like, because that half is compiled with their own compositions rather than
unioned. Combining it with the host's live preview through the op, blend
profile, blend radius and rounding `clay_document_layer_composition` reports for
the previewed layer **is** the whole document's field, not an approximation of
it; the `min` above is that same composition for the one case where the layer
unions.

It refuses only when the layer named is **not the last visible SDF layer**, and
that refusal hands back the id of the lowest visible SDF layer above it **and
how many are above it in all**, so a host can offer *"hide or move `Poros` to
smooth `Forma_principal` live"* rather than reporting the tool unavailable — and
can say so once rather than after each hide, which the id alone cannot: on a
stack with two field layers above the target, acting on the one named gets the
sculptor refused again naming the next. The id is the row to act on first; the
count is what decides the sentence, and a host wanting every id enumerates the
visible SDF layers above the named one itself. Hidden layers and mesh or voxel
layers above do not block it: they are not in the fold.

That position restriction belongs to this call and is **not** a narrowing of the
excluding form. `clay_brick_cache_eval_requests_excluding` refuses on the
document rather than on a position, so in a document where every layer unions —
every document written before ABI 0.86.0 — it still works at any stack position,
exactly as it always did. Excluding a layer from the
*middle* of a stack still has no repair — the layers above it fold onto an
accumulator that included it, so the two halves are not two operands of one
combine — and reconstructing that case would need a three-way split and two
host-side combines, which this ABI does not offer.

**Neither call edits the document**, which is the other half of why they exist.
The route a host would otherwise take — hide the layer, sample the rest, show it
again — is three edits, and an edit taken between `clay_sdf_smooth_begin` and its
commit is one the commit correctly refuses.

The engine half is `scene::compile_document_except`, a third case in the
predicate `compile_document_part` already had. That function's `below` **stops**
at the named layer, so it is "before" rather than "except" and drops everything
above it too; the new case skips the layer and keeps walking. Both cull under
the **whole document's** pad, for the reason the split already documented: a
part compiled under its own smaller pad drops items the whole compile keeps, and
the parts then stop summing to the whole.

Two deliberate refusals. An **unknown layer** is `CLAY_ERROR_NOT_FOUND` rather
than "exclude nothing" — a host whose id went stale would otherwise be handed the
whole document and would draw the excluded layer twice, once from here and once
from its own preview, which looks like a shading artefact rather than a bug. A
**hidden or empty** layer succeeds, because it contributes nothing to the union
already and refusing would make a host branch on state it has no reason to
track.

And the excluded refill **takes no seed and leaves none**. A seed is a brick's
value for *this* document, which a later refill continues with the items the
document has gained since; a value computed without one of the layers is not
that, and storing it would hand the next whole-document refill a seed with a
layer missing — silently, because a seeded answer is bit-identical to a walked
one by contract. So it is a plain batched walk, priced like a stroke's first dab
rather than its tenth, which is the right price for something taken once a
gesture. `clay_document_resume_stats` does not move for it.

### Scaling a subtool per axis

A ZBrush-style gizmo scales per axis — the three boxes on the arms — and users
expect it on a placed object *and* on a whole subtool. `scale-an-item-per-axis`
(#320) gave every NODE placement three factors in 0.54.0 and deferred the layer
deliberately, because `layer.xform * node.xform` is consumed as a rigid frame by
`brush::move` and `brush::lattice_gizmo`. A host therefore had to hide the axis
boxes in scale mode for a subtool (#373).

`Layer::scale_axes` closes it, composed innermost in the layer's own frame
exactly as a node's is in its own:

```
world_from_local = layer.xform · diag(layer.scale_axes)
                 · node.xform  · diag(node.scale_axes)
```

`xform.scale` stays the similarity factor at each level and the axes modulate
it, so a triple of ones is the identity and **three equal factors compile to
byte-identical tape** — the case a test pins, because it is what makes every
document written before the field unaffected by it.

**What a non-uniform scale costs, and what it does not.** The inverse goes into
the tape's matrix and the distance is multiplied back by the product of the
smallest component of each per-axis scale in the composition. That is
conservative rather than exact, and provably so: the composed linear part is
`L · D_l · R · D_n` with `L` and `R` rotations, and the smallest singular value
of a product is at least the product of the smallest singular values, which for
a rotation is 1. So the value never overestimates the true distance. The field
stays **1-Lipschitz** — dividing by `s` and multiplying back by `min(s)` can
only shorten — so `safe_step_scale` does not move and no marcher slows down.
What goes is `is_exact`. A world **radius** mapped inward is divided by the
**largest** component instead, the dual: `brush::move` takes the layer's factor
into that rule, so a drag never reaches outside what the artist circled.

Bounds, influence bounds, picking and a mesh layer's placed geometry all read
the three factors; a mesh layer's **normals** go through the inverse transpose,
as `clay_mesh_transform_nonuniform` already documents, because rotating a normal
is right for a similarity and tilts every one of them off the surface under a
squash.

`kSceneMinor` and `kClaySpaceMinor` move to 16. An older stream loads with
(1, 1, 1), which is what those files always meant; a stream written at an older
minor does not carry the field, so that minor's reader does not desynchronise.
The placement and the squash are **one command**, so one undo restores a frame
that actually existed rather than the rotation from one step and the squash from
another.

**One verb refuses rather than approximating.** `brush::lattice_gizmo` returns
no warps for a per-axis-scaled layer. A cage records its item-to-cage placement
as a `math::Transform`, and on a squashed layer the map it needs is

```
cage.placement⁻¹ · layer.xform · diag(L) · node.xform · diag(N)
```

a general affine map — the layer's diagonal sits *between* the two placements,
so it is not a similarity and not `Transform ∘ diag` either, which is the shape
`drag-a-squashed-item` predicted when it deferred the widening here. Placing a
cage through the narrower record would warp every item in a space it does not
occupy, silently and with no error. Refusing is not a fix for that widening and
does not pretend to be — the same record still drops a *node's* per-axis scale,
which predates this change — but it stops the layer scale from adding a second
silent case to it. `scale-a-layer-per-axis` task 5.1 carries the widening.

### Duplicating a subtool costs a layer record

An **instance layer** is a second layer over the same edit list. It is what a
ZBrush-style *duplicate subtool* is for — ten bolts from one bolt, an earring on
the other ear, a blockout repeated along a belt — and the point is that it costs
a layer record rather than a copy of everything already sculpted.

```c
clay_layer_id bolt = 0;
clay_document_instance_layer(doc, blockout, "bolt 2", &bolt);
float where[3] = {5, 0, 0}, up[3] = {0, 1, 0};
clay_document_set_layer_transform(doc, bolt, where, up, 0.0f, 1.0f);
```

**Shared: the edit list, and only the edit list.** An edit through either layer
is an edit to one list and appears through both — which is why
`clay_layer_node_influence_bound` reports the union over every layer sharing the
node, so a host that dirties by what it was told refills every instance rather
than leaving nine of them stale.

**Not shared: everything else the layer carries** — transform, name, visibility,
protection, mirror, radial. Those are copied from the source at creation and
diverge from there; placing the instance somewhere else is what turns one edit
list into two bolts.

Five consequences worth stating, because each is a question a host will ask:

| | |
|---|---|
| **Consolidate** | severs first. A bake replaces an edit list, and a bake means *this subtool is finished*, so the layer gets a private copy and the other instances stay parametric. One undo restores both the items and the sharing. |
| **Save and load** | keeps the sharing. From `.clayspace` minor 15 a layer record can name the layer whose edit list it shares, so ten instances are one edit list in the file. Written at minor 14 or below they come back as ten independent layers — the shapes are right, the link is gone. |
| **Removing the source** | is legal and unremarkable. The content is held by every layer sharing it, so this removes a placement. `clay_document_layer_info` re-homes the link: the first survivor in stack order reports `content_source == 0` and the rest name it. |
| **Reordering** | keeps the sharing across a crash too. A reorder is a remove and an add, and the add names a surviving sharer, so a journal replay restores instances rather than deep copies. Without the name the recovery is silent and wrong: the shapes are right and the subtools are no longer linked. |
| **Dragging** | dirties every placement. `clay_layer_move_surface` states its reach as the drag's ball — one box per image the layer's symmetry makes of it, since the mirror and radial copies move where the reflected and rotated balls are, and separate boxes rather than their union because the union of two balls is the slab between them — instead of deriving a region per item, and those balls sit in the dragged layer's placement, so on shared content it also invalidates each sharer's influence bound. Only a shared edit list pays that. |

`clay_document_layer_info` is also how a subtool panel draws the link at all:
`content_source` is the following end and `share_count` is how many layers hold
the list, so the source of a link is distinguishable from an ordinary layer.

### Dropping a subtool on the floor (ABI 0.86.0)

Three placements a host cannot write itself correctly:

```c
clay_layer_snap_to_ground(doc, layer, 0.0f);  /* low face of the box -> y = 0 */
clay_layer_centre_bounds(doc, layer);         /* centre of the box -> origin  */
clay_layer_zero_to_origin(doc, layer);        /* translation -> (0, 0, 0)     */
```

Each writes the placement's **translation and nothing else**, as one
`clay_document_set_layer_transform_nonuniform`'s worth of change: one command,
one undo step, one invalidation.

**Why they are in the library rather than in the host.** Written outside, the
read-modify-write goes through a pair that traps twice.
`clay_document_layer_transform` *refuses* a layer carrying three different
per-axis factors, so the read half cannot begin on a subtool a gizmo squashed;
and `clay_document_set_layer_transform` *clears* the per-axis scale, so a host
that got past the first trap by reading the uniform factor elsewhere silently
unsquashes the model — the artist presses "snap to floor" and the shape changes.
The correct composition is the per-axis pair with the rotation and all three
factors carried through: four calls, one refusal and one clearing rule, for a
menu item.

**Which box they read.** `clay_layer_bounds`: the tight world-space box over all
three representations, with no blend or chain-pad dilation — the influence bound
would leave the model hovering by exactly that dilation. It is still not the
silhouette, in three ways that will each produce a bug report:

- a **SUBTRACT** item contributes its own box, so a layer whose lowest visible
  item subtracts lands *that* box on the plane and the material stops higher up;
- a **smooth blend** can bulge past it — rounding is dilated into the box, a
  smooth union's bulge is not;
- **hidden items are excluded**, which is the intended reading rather than a
  limitation (the placement follows the silhouette the artist can see) and means
  hiding the lowest item moves where the next press lands.

Marching the surface for its true lowest point was the alternative: a bake or a
raycast sweep per press, no exact answer for a smooth field either, and the
slowest call in a transform panel. Rejected.

**An instance is placed, never severed.** What instancing shares is the edit
list; a placement is not shared. Unlike `clay_layer_consolidate`, which severs
because a bake *replaces* an edit list, there is nothing here to sever — placing
one instance is the gesture instancing exists for. Two instances of one edit
list snap to the same floor independently and both land.

**What they refuse**, and it differs per call:

| condition | snap | centre | zero |
|---|---|---|---|
| no layer carries the id | `NOT_FOUND` for all three | | |
| ghosted or locked | `INVALID_ARGUMENT`, before any bound is read | | |
| a placement gesture is open | `INVALID_ARGUMENT`, as for every other edit | | |
| layer holds no material | `INVALID` | `INVALID` | **accepted** |
| layer carries a radial mode | `INVALID` | `INVALID` | **accepted** |
| layer is unbounded | `INVALID` | `INVALID` | **accepted** |
| `ground_y` not finite | `INVALID` | — | — |
| box degenerate in any axis | accepted for all three | | |
| layer hidden | accepted for all three | | |

Empty is **refused rather than silently doing nothing**: a host greying the menu
item out wants to know, and "already in place" and "there is nothing here" are
the two states an artist most needs told apart. The code is
`INVALID_ARGUMENT` so that `NOT_FOUND` keeps meaning "no layer carries this id"
alone. A **radial** layer is refused because `clay_layer_bounds` carries a
layer's mirror copies and not its radial ones, so a placement computed from that
box would drop the original onto the plane with its copies already through it —
a wrong answer rather than a loose one, and invisibly so. An **unbounded** layer
is refused for the arithmetic: a plane reports faces at ±`FLT_MAX` and every
placement derived from one overflows to an infinite position and a tape of NaNs.
A **degenerate** box is accepted, because nothing here divides by an extent and a
flat layer is exactly the one "drop it on the floor" is pressed on.

**What they do not promise.**

- **Not bit-idempotent.** Each states a *total* placement, so pressing twice is
  the same gesture as pressing once — but the second press recomputes the box
  from an already-moved layer, and `f + (p + (g - (f + p)))` is not `g` in
  float. The second press lands within one rounding at the coordinate's
  magnitude; asserting bit equality at large coordinates will be flaky.
- **A press that moves nothing still costs an undo step.** Suppressing it needs
  a tolerance in world units this ABI would have to name and defend, and a host
  that cannot predict how many undos its own button cost is worse off.
- **No delta is returned.** The change is rigid, so a host may transform its
  drawn mesh instead of refilling — but it works the matrix out itself, by
  reading `clay_document_layer_transform_nonuniform` before and after and
  subtracting the two positions, which is exact because the change is a pure
  translation.

**`centre_bounds` is named for the box, and that name is deliberate.** The
roadmap row that asked for this spelled it *centre-mass*. The engine holds no
density, so the call's answer is identical for a hollow shell and a solid of the
same extent. The occupancy-weighted centroid that would earn the other name is
not merely more expensive, it is **not expressible by this signature**: a
sampled centroid has no answer until someone names a cell size — the one
`clay_consolidation_params` carries — and `(doc, layer)` has nowhere to put it,
so an implementation would have to invent a resolution the answer silently
depends on. It would also be a CPU bake per press, have no field at all for a
mesh layer until rasterized, and move when the artist changed the layer's voxel
size, which changes nothing about where the shape is.

A **third** reading of "centre" is not this call either: moving the *pivot* to
the content's centre without moving the content, so the gizmo stops hanging off
the subtool. That cannot be done at layer level — keeping the content still
while the layer's origin moves means shifting every item's local placement by
the inverse, which is item-level work on the shared edit list and on an instance
would move every other instance.

`zero_to_origin` **reads no bounds**, which is what makes it a different call
rather than a special case of the centring one: it is the only one an empty, a
radial or an unbounded layer can take. It leaves the rotation and both scale
factors exactly as they were — the full reset already exists as one
`clay_document_set_layer_transform_nonuniform` with an identity, and a
convenience call that quietly threw away an authored rotation would be the most
expensive undo in the set.

Layer scope is the only scope: it is what every other whole-subtool property
already has. There is no item-level or whole-document form, and adding one later
re-lays out nothing — but an item-level snap needs an answer this does not have,
namely what a snap means for a selection spanning a group whose children's
placements are relative to it. A whole-document snap is N layers inside
`clay_document_begin_undo_group`.

In pyclay: `layer.snap_to_ground(y)`, `layer.centre_bounds()`,
`layer.zero_to_origin()`, raising where the C ABI refuses.

### History: one undo, and exactly what it covers

`correct-the-undo-scope` found that the library had **three unrelated history
mechanisms and no single step spanning two of them**, and that nothing said so
— a host learned by shipping. `unify-the-undo-history` closed that. This
section is the thing that was missing.

**One order across three representations.** `clay_document_undo` /
`Document.undo()` reverse the most recent edit *whatever made it*. An SDF stamp,
then a voxel smooth, then a mesh grab undo as mesh, voxel, SDF. The entry points
did not change shape; since ABI 0.43.0 they reverse more than they did.

| representation | what a step holds | where the inverse comes from |
|---|---|---|
| SDF edit list, layer state | one `UndoStack` entry | the command's inverse |
| voxel grid | the cell writes the edit made, in order | replayed backwards to their `before` |
| mask | the cells the edit changed | snapshot on the first `touch()`, diffed when the step closes |
| mesh layer | sparse vertex deltas | `VertexDeltas::revert` |

It is an **index over** them, not a merge. **Masks were the fourth
representation with no mechanism at all** — twenty mutating entry points and
zero command variants, which the audit that counted three did not count — and
they record now, so `undo` no longer stops at one. Each stores what it
always stored, for the reasons it always had: a voxel edit has no compact
inverse — the inverse of "carve here" is the cells that were there — and a
vertex displacement is not an edit item.

**A verb is one step, not one per cell.** A `sculpt_smooth` that touches four
hundred cells is a single undo. So is a `fill_box`.

**Creating a layer is a step, of any kind.** `clay_add_sdf_layer`,
`clay_document_add_voxel_layer` and `clay_document_add_mesh_layer` each record
one command. Through 0.55.0 the voxel one did not, and the seam showed at a
*crossing* — make a voxel layer, rasterize a starting form into it — because
the fill recorded and the layer did not, so one undo emptied the new layer and
left it standing (#341). Undoing a voxel or mesh layer's creation removes the
layer and **keeps its payload**: an `AddLayerCmd` carries a layer by value and
could not carry a sculpt, so retaining the cells is what lets a redo restore
content rather than an empty layer. While the layer is absent the payload is
not reachable and is not saved.

**A group is one step across representations.** `begin_undo_group` /
`end_undo_group` bundle every step the bracket produced — edit list, voxel,
mask, mesh — not just the commands. Through 0.55.0 they bracketed the edit list
alone, so a bracketed crossing still undid in two, layer first and fill second.
A bracket that produced one step is unchanged, and an operation nothing records
stays its own step rather than being folded in.

**An edit that changed nothing is not a step.** A dab that misses every cell, a
flatten meeting a flat region, erasing an already-empty cell — all ordinary
here, and none of them produce an undo that does nothing. `undo_depth` counts
steps that will actually reverse something, so a menu built from it is honest.

**Undo is a DOCUMENT concept.** A grid made with `clay_voxel_grid_create` /
`clay.VoxelGrid(...)` is not in a document, records nothing, and still edits.

#### What is NOT covered, stated plainly

The obvious guesses are wrong, so they are worth naming:

- **Consolidate IS undoable.** It takes an `UndoStack` and records through the
  command vocabulary. It is the operation most often assumed otherwise.
- **Rasterizing into a grid IS undoable.** It writes through the same cell
  choke point every verb uses.

What genuinely is not:

- **VOXEL sculpt-layer property changes** — strength, visibility, order,
  merge-down. Their effect on cells replays, but the property value does not, so
  an undo would restore the pixels and not the setting. A partial undo is worse
  than none, so they are not steps yet. Since 0.76.0 the **mesh** stack over a
  multiresolution hierarchy does record them — rename, strength, visibility,
  reorder, lock, add, remove, merge and bake are all `MultiresLayerProperty`
  steps — because an additive displacement can restore its own coefficients
  exactly and a dialled-back pass is a thing an artist means to undo. That is
  the shape the voxel side would have to reach, not an inconsistency to
  preserve.
- **Operations that destroy history itself** — dropping a resolution level,
  removing a VOXEL sculpt layer. Removing a *mesh* sculpt layer is a step: the
  property record carries a whole-stack snapshot on each side, so it comes back
  with its id, its name and its coefficients.
- **Creating a mask.** Mask *edits* record; the mask's existence does not. It is
  the same shape of gap that layer creation had until #341 closed it.
- Anything a **host** does that the engine never sees.

A host that needs a boundary can read it: the history records unreversible
operations as barriers, `undo_depth` stops counting at the nearest one, and the
barrier names what is in the way. That is how a UI says "you cannot go further
back than this" instead of silently skipping it.

#### Memory

```python
b = doc.history_bytes            # undo, redo, journal, total, steps, dropped_steps
doc.set_history_budget(64 << 20) # 0 = unbounded, which is the default
doc.trim_history(16 << 20)       # on demand, e.g. on a memory warning
```

**What is expensive is not what you expect**, which is why measuring it beats
guessing:

- The command stack stores **inverses**, so *removing* an item records a whole
  node — 440 bytes plus its deformer chain and stroke points — while *adding*
  one records an id. A session of deletes and a session of adds cost very
  differently.
- An **undone layer creation** still costs what its payload costs. The cells are
  retained for the redo, so they are counted document-wide until the creation is
  redone or the document is saved.
- A voxel or mask step is proportional to the cells it **changed**, so one big
  fill can outweigh a thousand dabs.
- A mesh step holds its deltas **by value**, which is what makes it
  self-contained and also doubles a mesh stroke.
- The **journal keeps its own copy** of every payload, so a session with crash
  recovery on holds roughly twice what one without it does.

**The budget bounds undo and redo only.** It deliberately does *not* evict from
the journal: those bytes are the host's crash recovery, and dropping them
silently would lose exactly what that feature exists to keep. The journal is
reported instead, and the host trims it with `journal_trim` once its bytes are
durable.

**Redo is spent before undo**, because redo is transient — the next edit
discards it anyway. **The newest undo step is never dropped**: a budget that
could make the next undo fail would be worse than no budget, because a host
cannot tell that from a bug.

**Truncation is observable, not an error.** `dropped_steps` is how far the
horizon has moved, so a host presents it rather than letting a user hunt for a
step that is gone.

Runnable: [`examples/59_undo_across_representations.py`](../examples/59_undo_across_representations.py).

### Cancelling a long operation

Three budget classes, and until ABI 0.45.0 the third had no exit. From the
device gate: `mask_extrude` measures 4403 ms and `sdf_consolidate` 661 ms on the
reference iPad, and every one of those was a synchronous call a host entered and
could not leave. The threading contract closed the obvious workaround — calls on
one handle must be serialized, *const readers included* — so a host could not
even read the document from another thread to drive a progress bar.

```python
token = clay.CancelToken()
# on another thread, when the user presses Stop:
token.cancel()
# and meanwhile, to draw a bar:
p = token.progress   # running, phase, phase_count, fraction, done, total
```

```c
clay_cancel_token* token = clay_cancel_token_create();
clay_layer_consolidate_cancellable(doc, layer, &params, NULL, NULL, NULL, token);
/* ... from the UI thread ... */
clay_cancel_token_cancel(token);
```

**A token, not a callback.** `clay.h` contains no function pointers. A progress
callback would be the first one every FFI consumer has to marshal, and it would
fire on a *pool worker* rather than the caller's thread — so the header would
need a rule about what it may touch, and the honest answer is "almost nothing".

**`cancel()` is the one call in this ABI that is safe from another thread.**
Everything else requires the host to serialize. It is safe before an operation
starts, during one, after one has returned, and twice.

**A cancelled operation leaves everything exactly as it found it.** These
operations build a result and install it at the end, so a cancel is a discard —
the document comes back byte-identical and the undo stack gains no entry. You
never have to undo a cancellation.

**A cancellable entry point is a second one**, not a parameter added to the
first: `clay_layer_consolidate_cancellable` beside `clay_layer_consolidate`,
the same shape `clay_mesh_validation_report` has to `clay_mesh_validate`. The
older call is sugar with a null token, so a host that does not want one is
unaffected — which is the whole point.

**No time estimate.** A multi-phase operation's phases differ in per-unit cost
by more than an order — redistancing and a colour fill are not the same work per
brick — so a figure derived from the fraction would be wrong in the direction
that annoys users most. You have the wall clock.

**`CLAY_ERROR_CANCELLED` is not a failure.** It is distinct from every fault
code and from `CLAY_ERROR_BUDGET_EXCEEDED`, which means a limit the *host*
declared before the call. One means "too big"; the other means "stopped".

### Surviving a crash

A recovery is **a snapshot plus the steps since it**. The library owns the
bytes; the host owns the file.

```python
snapshot = doc.to_bytes()                    # write once
...edits...
journal, now_at = doc.journal_since(0)       # append; remember now_at
doc.journal_trim(now_at)                     # once those bytes are durable

# after the crash
recovered = clay.load_bytes(snapshot)
recovered.enable_undo()
result = recovered.replay_journal(journal)
```

**Why not just autosave the document.** `clay_document_save` is whole-document
and synchronous, and the device gate prices that class of work at hundreds of
milliseconds to seconds. A host autosaving on a timer stalls its UI for however
long the whole sculpt takes — and the cost grows with the sculpt, so the safer
it tries to be, the worse the stall. A journal costs what CHANGED: measured in
`examples/60_surviving_a_crash.py`, three ordinary edits journal 507 bytes
against a 3595-byte re-save — **7.1× cheaper per autosave**.

**And the crossover, because "always cheaper" is false.** A journal entry is
raw — a voxel step is 14 bytes per changed cell — while the document stores
that grid palette- and RLE-compressed. So a single edit that rewrites a large
fraction of the model journals *larger than the whole document*: the same
example measures one big `fill_box` at 7189 journal bytes against a 590-byte
document. The rule is not "journal instead of saving", it is:

> **Re-snapshot when the journal grows past the snapshot.**

which is a size comparison a host already has both sides of.

**What the host owns**, because the right answer differs between iOS, a desktop
filesystem and a database: where the file lives, when it is flushed (`fsync`,
atomic rename), how often to re-snapshot, and what to do with a leftover
recovery file on the next launch.

**Peek, not drain.** Indices are absolute for the life of the session and do not
shift when you trim, so a failed write is retried by asking again, and a host
that asks below the trimmed floor gets nothing rather than a silently shorter
history — `journal_range()` is how it finds out.

#### The three rules that keep a recovery honest

**A journal this build does not understand is refused**, not partly read. A
recovery that silently drops what it could not parse is worse than none, because
the user cannot see the gap. Events applied before a bad one stand — replay is
not a transaction — so replay onto a copy if you want all-or-nothing.

**A journal replayed onto the wrong snapshot is refused too**, with
`CLAY_ERROR_SNAPSHOT_MISMATCH` (a `ValueError` naming the pair, in pyclay) and
*nothing applied*. A journal carries a hash of the bytes it continues from,
stamped by `to_bytes` / `save` on one side and `load_bytes` / `load` on the
other, so you get the check by writing the ordinary recovery path. The two
refusals mean opposite things: unreadable says discard the file, mismatched says
the file is fine and you handed it the wrong snapshot.

> It names the snapshot **current at the index you asked from**, not the last
> one you took — so serializing again to compare sizes does not repoint a
> journal you already have. What it does *not* catch is replaying the same
> journal twice onto the same snapshot: the indices are yours to keep. It is
> also not a checksum, and journals written before 0.86.0 name no snapshot and
> are never refused for the pair.

**Replay stops at a barrier rather than skipping it.** A barrier is an operation
nothing can reproduce — dropping a voxel resolution level, or anything a host
does that the engine never sees. (Mask edits *were* one, and are not any more.)
Replay returns success with the flag set, and a host that sees it needs a
*fresher snapshot*, not a longer journal. Continuing past it would hand back a
document quietly missing that operation's effect.

**Ask before you need it**: `journal_barrier(from)` (`clay_document_journal_barrier`)
reports the first such operation in the log and where, so you re-snapshot while
you still can. Learning it from a replay means learning it during the recovery,
which is the one moment the answer is useless.

A journal therefore carries an ordinary sculpting session end to end, mask
edits included.

#### What a journal is not

Not a document. It is a crash artifact paired with **one** snapshot, versioned
separately from `.clayspace` precisely so nobody mistakes it for a portable
format. Not durability, not multi-process, and not a way to survive a crash
*during* a long operation — that work simply will not be in the journal.

Runnable: [`examples/60_surviving_a_crash.py`](../examples/60_surviving_a_crash.py).

## 7. Meshing & mesh processing (`clay::mesh`)

- **Marching cubes** (default): consistent ambiguity resolution (asymptotic decider) → watertight, 2-manifold guarantee; runs over surface-crossing bricks only; CPU version is the golden reference, GPU versions (Metal/CUDA) must match topology-invariants (not bit-identical vertices).
- **Surface nets**: cheap smooth preview meshes — cheap in both senses, and they are different claims. It emits about a **third** of marching's vertices and triangles at equal resolution, which is what makes it a preview: less to upload, less to draw. It is also cheaper to BUILD, at **86.8 ms against 120.1 ms** over the same precomputed lattice. That second half was false for a while and nothing noticed, because the gate that meant to assert it compared two calls whose cost was 80–96% attribute evaluation and therefore ranked them by vertex count (#304). The gate is now on the two meshers over one lattice, with no field evaluation and no attributes on either side.
- **Dual contouring** (QEF + Hermite data): sharp-edge export for the chamfer aesthetic; manifold DC variant. Ships behind a flag; roadmap-hardened after v1.
- **Quad meshing** (`mesh/quad_mesh.h`, `VoxelGrid::mesh_quads`): the lattice dual with the quads it already builds KEPT, four indices per face in `Mesh::quads` beside — never instead of — the triangles, so every existing consumer is byte-identical and unmodified. A voxel sculpt gets a second mode: one planar quad per exposed voxel face, welded within a palette colour. **A lattice-derived quad grid, NOT field-aligned retopology** — the quads follow the lattice and not the form, so there are no edge loops, no feature poles, and nothing animation-ready; it is the input a retopology pass replaces. A requested quad COUNT is a short search over the cell size that reports what it produced: approached, never hit, because the count goes as `cell⁻²` and is not even monotonic in it. Faces mode has no cell size, so there the search WALKS the grid's resolution levels coarsest first and stops at the first one that reaches the target — monotonic, at most one mesh per level, and one for EVERY level up to the one that stops it, so a target met at level k costs k+1 meshes and that is what `iterations` reports; budget the stack's length, not the bracketing pair. `clamped` there means the level stack ran out rather than that a limit was hit — the target is below what the coarsest level THAT YIELDS ANYTHING gives, or above what the finest gives, since a stack is not a strict mip and a sculpt made only at a fine level leaves the coarse levels empty. OBJ, PLY and FBX carry the quads; **GLB does not**, because glTF 2.0 defines no quad primitive mode. See `docs/08-mesh-readback.md` and `examples/44_quad_export.py`.
- **Decimation**: quadric edge collapse via `meshoptimizer`, color-attribute aware, target ratio or error. Quads do not survive it: edge collapse is a triangle operation and breaks the pairing on its first collapse, so a decimated quad mesh comes back as triangles.
- **Validation**: watertight/manifold/degenerate checks, self-intersection sampling — the primitive behind CI's export gates and the app's "guaranteed clean booleans" claim.
- **Global voxel remesh** (`mesh/voxel_remesh.h`): one semantic operation that rebuilds a WHOLE surface through a signed narrow-band field at an explicit world voxel size — DynaMesh, in artist terms. It composes what is already above it (the BVH and its generalized winding sign, the sparse sampled field, the watertight marcher, the validator, the attribute transfer) and adds no second implementation of any of them; what it owns is the decisions BETWEEN them, which are the parts a host would otherwise have to invent: what resolution means, what an open surface becomes, what a request costs before it is made, what is guaranteed about the result, and what "the same input twice" produces. Overlaps fuse, self-intersections resolve, density comes out uniform; vertex and polygon identity are destroyed, UVs are DROPPED (not reprojected — a resampled UV across a seam is a stretched layout that looks like a preserved one), and detail finer than the voxel size may go. Colour survives by closest-point transfer, and a caller-owned per-vertex mask survives through `mesh::transfer_vertex_scalar`. Its one piece of new engineering is the sampling domain: the existing converter evaluates every brick of the bounding box, which is 24M winding-number queries at longest-axis 256, so the remesh supplies its own brick fill to the same `sample_blocks` entry point and evaluates only the bricks near a triangle — with the stored samples BIT-IDENTICAL to the dense path's, which is a test rather than a claim. Preflighted (`voxel_remesh_estimate`), budget-refused before it allocates, cancellable inside every stage, deterministic to the bit, and validated before it returns. See `docs/07-brushes-and-features.md` § 8c and `examples/67_voxel_remesh.py`.
- **A rebuild through the document** (`clay_document_voxel_remesh_layer`, `Document.voxel_remesh_layer`): the same operation landing on a LAYER, as one undo step, transactional — a refusal, a validation failure or a cancel leaves the layer byte-identical and adds no step. Its undo record is `session::Step::Kind::MeshReplace`, holding the mesh on each side, because a sparse `VertexDeltas` records no indices by design and cannot express a change that replaces them. A per-layer GEOMETRY REVISION moves for a wholesale replacement and never for a sculpt: that asymmetry is what lets a cached adjacency, BVH or sculptor survive a brush stroke and be invalidated by a rebuild, and it is what makes a late-arriving commit refusable rather than silently winning. It also closes a trap neither of the older checks could see — a replacement landing on the same vertex and index counts passes both the mesh-pointer comparison (a `std::map` node's address is stable) and the sculptor's own count check.
- **Welding** (`mesh/weld.h`): merging coincident vertices and removing the triangles that collapses. **The default mesher emits ZERO-AREA TRIANGLES** — 1458 of 70,140 on a plain analytic sphere, two per cent, with two corners at bit-identical positions — and nothing had noticed, for two reasons worth stating: everything downstream tolerates them (an exporter writes them, a BVH holds them, the decimator drops them itself), and `validate` counts them as SLIVERS rather than degenerates, because its `degenerate_triangles` means *repeated indices* and these have distinct indices at identical positions. So a marched mesh reports `clean()` while carrying them. `mesh::DynamicSurface` cannot hold one — a half-edge face with two of the same vertex does not exist — so it refuses, which meant **no mesh this library marched could become an adaptive surface at all** until this shipped. Not the same verb as `Adjacency`'s weld: that groups vertices into classes and leaves the triangle list byte-identical (a seam's duplicates move together under a brush); this merges them and rewrites the triangles. Watertightness survives by construction — a triangle whose corners coincide bounds nothing, so removing it cannot open a hole — a UV seam is preserved by default (duplicated positions with different UVs is how a flat mesh *represents* a seam), quads are dropped on rewrite, every index is in range afterwards, and a mesh with nothing to merge comes back byte-identical.
- **Spatial scalar transfer** (`mesh/transfer.h`): resampling a caller-owned per-vertex array — a mask, a weight — from one mesh onto another by closest point. Mask is not a `Mesh` field and does not become one to suit an operation; this is what carries it across a topology replacement.
- **The dual meshers walk the cell range once.** Surface nets, dual contouring and the quad mesher share `dual_grid_mesh`, which used to place every cell's vertex in one pass and then walk the whole range again to emit quads — re-reading four lattice corners it already had, for every cell including the ones that own no vertex and can contribute no quad. It now emits a cell's quads as it places its vertex, which is sound because a quad only ever references cells reached by stepping BACK along the two axes that are not the edge's: the owning cell and three already placed. The sampler is also templated rather than a `std::function`, so the tape meshers' lambda inlines — eight corners a cell is 33M calls on a 0.02 lattice, and none of them was inlining. Together 113 ms → 75 ms serial, output byte-identical across all five entry points (#304).
- **Brick meshing runs the march across bricks in parallel and welds serially.** Marching a brick is pure — it reads the cache, which is a const lookup — so bricks fan out over `clay/parallel`. The WELDING cannot: one `Builder` serves every brick precisely so a lattice edge shared by two of them yields one vertex, which is what keeps the sparse set watertight at brick seams. So the march records what each brick would emit and a serial pass replays those recordings through the single builder in key order, calling `edge_vertex` in the same sequence the serial loop did — the result is byte-identical to the serial path by construction, not by tolerance. Measured on a 24-core desktop: 276 bricks 30.9 ms → 7.5 ms.
- **Attributes**: vertex colors sampled from the color field (blend-gradient faithful), normals (field gradient or face), optional UV box-projection utility. For a brick mesh the gradient/color taps go through per-brick culled tapes — one tape per involved brick, shared by normals and colors — and are evaluated as ONE flattened batch on the CPU backend's thread pool (`eval_points_batch`), so a dense re-mesh costs about what refilling the same bricks does instead of one core's worth of serial taps; the results are byte-identical to the serial evaluation. The non-brick meshers — `mesh_tape`, `mesh_tape_dc`, the dual-grid path — share the batching but not the culling: `apply_tape_attributes` is handed a tape rather than a document, so it hands every vertex to `eval_points` in one call and gets the pool and the blocked walk without a per-region tape. That was worth **58x** on the pass and 18-20x on the whole meshing call, and it was 96% of a coloured `mesh_tape` before it (#302). Byte-identical there too, and it falls back to the serial walk when no CPU backend is registered.

## 8. File I/O (`clay::io`)

- **Document format** (`.clayspace`): binary chunked container (versioned chunks: scene commands, palettes, voxel grids RLE/palette-compressed, imported meshes, multiresolution hierarchies, thumbnails PNG, camera bookmarks). Forward-version refusal, backward-compat guaranteed; pure claycore so Python/CI can read and write projects.
- **A mesh layer's multiresolution hierarchy is carried by the document**
  (`'MRES'`, container minor 19, ABI 0.88.0). Before this a hierarchy was a
  standalone handle no document held, so saving a sculpt wrote the base cage as
  an ordinary mesh layer and dropped every level above it — and a host's own
  side-car file was the only record that a row had ever been a hierarchy.
  - `clay_layer_multires_present` says which rows carry one, without decoding
    anything, and tells "not a mesh layer" apart from "a mesh layer with
    nothing on it".
  - `clay_layer_take_multires` MOVES a hierarchy into a layer and repoints the
    source handle at the document's copy. A move rather than a copy because
    `MultiresSurface` is move-only in C++ and a copying form would round trip
    through encode/decode — hundreds of megabytes on the largest thing an
    artist is holding.
  - `clay_layer_multires` hands back a BORROWED handle; the document owns it,
    destroying the handle leaves the hierarchy, and removing the hierarchy makes
    every handle onto it answer `CLAY_ERROR_NOT_FOUND` rather than dangle.
  - `clay_layer_remove_multires` drops it. Replacing is refused rather than
    silent: a mask can be repainted and sculpted levels cannot be recovered from
    the cage, so the destruction is a call a host has to mean.
  - **The cage exists twice and is not reconciled.** The mesh layer holds
    triangles and the hierarchy holds its own copy of the base level, because a
    hierarchy is built from a MESH and keeps no link back. The two could always
    drift; carrying both in a document does not change that, and nothing edits
    either to agree with the other. `clay_layer_multires_matches_cage` lets a
    host see it — computed on demand, never stored, and exact rather than
    tolerant. A `0` is the ordinary state after the cage has been edited, not an
    error.
  - **What it costs, measured**, a 289-vertex cage sculpted at its finest level:

    | levels | vertices at top | document bytes | save |
    |---:|---:|---:|---:|
    | none | — | 9,750 | 0.012 ms |
    | 1 | 1,601 | 44,090 | 0.053 ms |
    | 2 | 6,273 | 105,582 | 0.132 ms |
    | 3 | 24,833 | 326,870 | 0.402 ms |
    | 4 | 98,817 | 1,211,926 | 1.546 ms |

    **The cost follows AUTHORED DETAIL, not level count.** The same hierarchy at
    four levels with nothing sculpted into it adds 128 bytes and no measurable
    time, because the detail field is sparse. So a host cannot price a document
    from its level count, and a deep hierarchy an artist has not touched is
    nearly free to carry.

    A hierarchy is routinely the largest payload in a document, and
    `clay_document_save` is whole-document and synchronous — so an autosave on a
    timer stalls for the whole of the figure above. That is not fixed here; the
    journal (`survive-a-crash`) is where incremental saving belongs.
- **OBJ + MTL**: custom reader/writer (dependency-free), vertex-color extension documented.
- **FBX**: import via **ufbx** (MIT, single-file, battle-tested); export via minimal binary FBX writer (meshes, transforms, vertex colors, units/axis correct for Unity/Unreal/Blender — validated in CI via assimp/Blender-headless round trips).
- **PLY**: reader/writer with vertex colors (interchange with SDF Modeler/MagicaCSG ecosystems).
- **glTF/GLB**: writer (cgltf or custom) — engine-friendly, wheel-friendly.
- **USDZ**: *not* in claycore (Apple Model I/O owns it in the app shell); claycore exposes the mesh+attribute buffers those APIs consume.
- Import guardrails: triangle budgets, malformed-file fuzzing, no allocation
  bombs. A `*_file` loader prices the file's length against
  `ImportBudget::max_file_bytes` before it sizes a buffer, so a path that is not
  a readable regular file is an `IoStatus` rather than a termination — a
  directory opens for reading and reports its length as `LONG_MAX`. A loader
  never returns a mesh whose normals, colors or uvs are non-empty and a
  different length than its positions; an attribute a file supplies for only
  some of its objects is dropped rather than returned short.

## 9. Picking & interaction math (`clay::pick`)

CPU-side, latency-critical, called every Pencil event:

- Ray ↔ scene raycast (analytic tape or brick cache, whichever is fresher) with layer/item hit attribution. The brick raycast starts from `BrickCache::surface_bounds()`, the union of the surface bricks' boxes that the cache keeps current across submit, evict and trim — it used to fold that box from `surface_bricks()` on every ray, a walk of the whole map per Pencil event that cost more than the march it preceded on a whole-model cache. Inside that box the walk is **analytic on the cache's own reconstruction** rather than a sphere trace of it: the field a brick cache answers is the trilinear interpolation of its lattice, so along a ray inside one cell it is a cubic in *t* (Hansson Söderlund, Evans and Akenine-Möller, *Ray Tracing of Signed Distance Function Grids*, JCGT 2022), and the first crossing in a cell is that cubic's first root — found by splitting at the derivative's zeros and refining with Newton–Raphson inside the bracket — rather than something a march creeps up on at eight hash lookups per sample. A brick-level DDA skips uniform bricks whole (Inside, Outside and never-evaluated alike; an Outside brick beside a Surface one walks only its face-layer cells, since only those have a neighbour's samples for corners), a cell-level DDA under a Surface brick gathers eight corners per cell — four of them carried over from the cell before — and the cubic is solved only where the corners straddle the threshold. The hit is where the field falls below `eps` scaled by the distance, the same crossing the sphere trace stopped at, so a host that picked with the old march picks the same point; the normal is the field's own gradient in the cell that was hit (the paper's analytic normal), which is the same field the old six-tap central difference sampled and agrees with it everywhere but across a crease. The sphere trace is kept as `pick::detail::raycast_bricks_sphere_traced`, the reference the agreement test holds the walk to on hundreds of rays: every hit and miss the same, every t within a twentieth of a voxel. Measured on the 1,500-dab whole-model cache of the `bench_unspent` harness, a pick went from 1.4 µs to 0.6 µs; random rays from a sphere around the model from 1.9 µs to 1.4 µs.
- Surface snapping: closest-point-on-surface (gradient descent on the field), position and position+normal modes.
- Build-plane and grid cell resolution for voxel mode; face picking on voxel grids.
- Bounds/frustum utilities for zoom-to-selection and culling.

**A pick marches the cached tape, and pays only the step scale its own ray
needs.** Two things the pick path used to do per Pencil event, neither of them
visible from the hit. It compiled the document: `pick::raycast_scene(doc, ray)`
built a fresh pickable tape every call, while the C ABI already held one per
revision for every other pick query — at 1,500 items that compile was 0.3 ms
of a 1.0 ms pick. `clay_raycast_attributed` and `clay_raycast_bounded` now hand
the cached tape (and the document's cull index) to an overload of
`raycast_scene` that takes them; the library entry point keeps its signature
and compiles as before. And it marched at the document's worst step scale: the
tape's `safe_step_scale` is ONE Lipschitz bound folded over every visible node,
so a twisted box two units from the model dropped it from 1 to 0.28 and a ray
nowhere near the box took 2.4× the steps it needed, because the bound cannot
say *where* the field is steep. When that scale is below 1, the march now
compiles a tape culled to the ray's own segment — a box around the clipped ray,
through the same per-brick cull the refill uses, so the culled field is exact
wherever the march evaluates — and steps by the culled tape's scale when it is
larger. An item whose influence misses the segment is dropped, and its
Lipschitz contribution with it; a ray straight through the steep item keeps it,
the scales tie, and the whole tape marches as it did (no compile is used
unless it wins). The hit is the same surface either way — the surface did not
move, only the step length did — and `SceneHit::steps` reports the march so a
test can hold the claim. With both, the twisted case measured 1.20 → 0.95 ms
at 1,500 items and 0.33 → 0.26 ms at 400, the plain case 1.02 → 0.80 ms and
0.28 → 0.22 ms; a culled compile still emits nearly every item of a long
blended chain (the blend envelope pads the region), which is why the local
tape is gated on the step scale rather than compiled for every ray.

**And attribution no longer builds a document per candidate.** Once the march
has a hit, `pick::attribute` names the layer and item under it: the layer by
|field| of each visible, unghosted SDF layer's own tape at the hit, the item by
|field| of each candidate item's own tape — the item as an Add, alone, under
the layer's placement and mirror — over the items whose influence bound
reaches the hit. It used to answer the second question by constructing a
`Document`, a `Layer` and an `SdfContent`, inserting a copy of the node into
the hash map and running the whole-document compile, once per candidate per
pick, and the first by compiling every layer per call; on a stroke whose dabs
overlap that is dozens of document constructions a Pencil event, and at 1,500
items attribution was 0.42 ms of a 0.80 ms pick whose march was 0.38.
`scene::compile_item(layer, node)` now emits the one item and nothing else —
byte-identical to `compile_layer` over a layer holding only that item, held as
a test over the gnarly corpus — and the tape overload of `attribute`, which
`raycast_scene` calls with the pickable tape it marched, reads a single-layer
document's winner off that tape instead of compiling the layer again (one
candidate layer means the pickable tape *is* its tape; with several the
per-layer compile stays, since their union cannot say which is nearest). The
ids a hit attributes to are unchanged: the old implementation is kept verbatim
in `test_pick.cpp` as the reference, and both entry points are held against it
over mirrored, squashed, ghosted and radial layers, groups, subtracts, paints
and strokes. The plain case measured 0.80 → 0.51 ms at 1,500 items and
0.22 → 0.14 ms at 400, the twisted case 0.91 → 0.65 and 0.25 → 0.18. What is
left over the bare march is mostly the influence-bound walk over every item of
the winning layer, which the cull index caches per revision and attribution
does not yet read.

### Asking the shape what it is: surface measures

Curvature, cavity, convexity, "facing up", ambient occlusion and thickness — at
a point, or over a region as a mask.

**Why this is cheap on a field and expensive in a mesh engine.** Curvature here
is the field's **Laplacian**, and its sign is unambiguous: for `f = |p| - R` the
Laplacian at the surface is `2/R`, *positive* for convex. So cavity and
convexity are one subtraction apart. A mesh has to estimate curvature from a
vertex ring, which is a discrete approximation with a valence-dependent error.

The same argument runs for occlusion. A mesh traces rays against triangles
through an acceleration structure that must be rebuilt when the mesh changes; a
field is marched directly, at any resolution, with nothing to build and nothing
to invalidate — and it measures the **actual** surface rather than a
tessellation of it.

```c
clay_measure_params p = { .struct_size = sizeof p };
clay_measure_defaults(&p);
p.ray_length = 0.02f;              /* what "occluded" means here */
clay_measure_points(doc, CLAY_MEASURE_OCCLUSION, points, n, &p, out, NULL);
```

**One implementation, two shapes.** `clay_mask_from_surface` walks the same
measure over a region and bands it to the surface. Until v0.51.0 the lattice
form was the *only* form and carried its own copy of the stencil; there is one
now, which is what makes "a cavity mask and a baked map agree about this
surface" a construction rather than a claim.

**`ray_length` is the parameter that decides what the number means.** Occlusion
measured over 1 cm and over 1 m describe different things about the same point,
and neither is more correct. There is no good default; leaving it at 0 takes
20× `scale`, and that is a guess.

**Occlusion is occlusion, not lighting** — 0 is open sky, 1 is fully enclosed.
Tools disagree about which way this runs, so the name says which.

**Determinism is not negotiable.** Every other query here returns the same bits
on every backend and every run, and a hemisphere sample is the first thing that
could quietly break that. The pattern is a fixed low-discrepancy sequence
rotated by a hash of the *point* and an explicit `seed` — not a random number
generator, not thread-dependent, not order-dependent. Same seed, same bits.

### Bounded rays, and projecting onto the surface

`clay_raycast` searches to infinity, which cannot express *"look 5 mm along this
normal"* — the query a bake cage and a snap tool both are. Worse, it makes a
miss indistinguishable from a hit on the far side of the model, which is exactly
what puts garbage in the seams of a baked texture. `clay_raycast_bounded` takes
`tmin`/`tmax`; a second entry point, because the original's signature has
shipped.

`clay_project_to_surface` is the cage query: from a point and a direction, find
the nearest surface within a distance and report **how far it was, signed**.

**Both ways, and both sides.** A cage point built from a low-polygon mesh may
sit *outside* the high-polygon surface or *inside* it, depending on whether the
low-poly bulges or pinches there, and the caller cannot know which. Two things
follow, and a first implementation usually misses both: the search runs in both
directions along the axis, and the march steps by `|f|` while detecting the
crossing by **sign change** — because started inside, the signed distance is
negative and an ordinary sphere-march cannot take a step at all.

The signed distance comes back from the call that found the point rather than
being recomputed from it, because it *is* the height-map value and deriving it
again is a second chance to disagree about the sign.

Runnable: [`examples/64_measuring_the_surface.py`](../examples/64_measuring_the_surface.py),
which projects 2000 points onto a creased surface, measures all six, and checks
that the mask and the per-point form agree.

### Naming a region: surface groups

ZBrush's PolyGroups, Blender's Face Sets. Until v0.50.0 this library had no such
concept on any representation. Visibility was per **layer**, so "isolate the
head" meant the head had been authored as its own layer — a decision taken
before the artist knew they would want it. A layer holds exactly one mask, so N
named regions could not be faked with N masks. And scene groups group
*edit-list nodes*, which says how three items combine and nothing about which
part of the resulting surface is the head.

**One world-space lattice**, asked "which group is this surface point in"
identically whatever the surface is made of. The obvious alternative — a
per-face id on a mesh, a palette channel on a grid, something else for SDF — is
three mechanisms, three sets of semantics for hide/isolate/grow/border, and they
will disagree.

The free answer for SDF, mapping a surface point to the **item** that produced
it, fails the two cases that matter and fails them for one reason: *an artist's
groups do not respect the edit list*, because the edit list is how the shape was
**built** and a group is about what it **is**. An armour panel spanning two
items is not an item. A face that is part of one sphere is not one either.

```c
clay_groups* g = NULL;
clay_document_groups(doc, 0.05f, &g);      /* created on first ask */
clay_groups_fill(g, head_min, head_max, HEAD);
clay_groups_isolate(g, HEAD);              /* show one, hide the rest */
```

**Hiding is not deleting**, and the implementation is what makes that true
rather than a promise. Nothing is cut and no hole is closed: the field is
untouched and the *produced mesh* is filtered, so showing a group again brings
back the same triangles rather than a re-meshed approximation of them. Making
hidden regions evaluate as empty would have been the obvious implementation and
is wrong twice — it would change what the document **means** (the invariant
`tools/check_layering.py` protects by withholding `clay/voxel` from
`clay::scene`), and it would carve a boundary into the surface that showing
could not undo.

Both the ids and what was hidden survive a save, in a `'GRUP'` chunk at
`.clayspace` minor 13.

**What respects the hidden set, and what does not** — listed rather than left to
be discovered, because a brush that silently reaches hidden surface is worse
than one that refuses:

| | |
|---|---|
| `clay_document_mesh`, `_mesh_quads` | **respects it** — hidden faces dropped; a quad export is filtered *by quad*, so it keeps its quads |
| `clay_raycast`, `_many`, `_attributed` | **respects it** — the march steps *over* hidden surface and picks what is behind, since hiding the front of a head is how you reach the inside of it |
| `clay_document_eval`, every field query | **does not** — the field is what the document *means*, and a group must not change it |
| every brush and voxel verb | **does not** — a brush is bounded by its footprint and by a **mask**, which is the existing mechanism for "do not edit here". Gating on visibility too would be two mechanisms for one intent that can disagree. Isolate to see; mask to protect |
| `clay_document_save` | **does not** — the whole document is written, hidden included. Anything else would make hiding a form of deleting |
| `clay_mesh_save` and the exporters | **does not** — they take a mesh you already have; mesh the document first and the filter has already run |

Two limits worth knowing before you build on it. The group boundary is quantised
to the lattice rather than to the representation, so a mesh that could have
carried an exact per-face border does not — that is a visible edge at the group
border and it is the price of one mechanism instead of three. And **grow is
volumetric, not geodesic**: ZBrush grows a face set *along* the surface, this
dilates in 3D, so where a surface folds back within `steps` cells of itself the
growth crosses the gap and claims the other side.

Runnable: [`examples/63_surface_groups.py`](../examples/63_surface_groups.py),
which names a band spanning two items — the case no item-derived rule could
express — isolates a region, sculpts on what is left, and shows that hide-then-
show restores the triangle count exactly.

### Answering a platform memory warning

The brick cache is likely the largest single allocation in a sculpting app's
process, and iOS does not ask politely: a memory warning ignored is how an app
gets killed. Until v0.34.0 the cache had a budget and no way to give anything
back — a submit past it was **refused**, the surface stopped updating exactly
where the artist was working, and the only recourse was to destroy the cache and
rebuild from nothing, which is the most expensive operation available taken at
the moment the device can least afford it.

The sequence a host runs on a warning, end to end:

```c
/* 1. What is actually held, in both pools. memory_usage bounds the fp16
      payloads; bookkeeping_bytes is the per-key map, which grows with how much
      space has ever been dirtied and is OUTSIDE the configured budget. */
clay_brick_stats s = { .struct_size = sizeof s };
clay_brick_cache_stats(cache, &s);

/* 2. Reclaim payloads first — the big pool. `focus` is where the artist is
      working, or where the camera points; the cache cannot know either. */
uint64_t dropped = 0;
clay_brick_cache_trim(cache, s.memory_usage / 2, focus_xyz, &dropped);

/* 3. Then the bookkeeping, which trim does not touch. */
uint64_t forgotten = 0;
clay_brick_cache_forget_empty(cache, &forgotten);
```

Nothing is lost. An evicted brick is one that must be re-evaluated if it is
looked at again, which is exactly what the mark-dirty / take-dirty / submit
cycle already does — so a host that trims and later pans back simply pays for
those bricks again, and gets **bit-identical** data for them.

Three properties worth relying on:

- **Dirty bricks are never dropped.** They are already scheduled to be
  rewritten, so evicting one would trade memory for the thing the host is
  waiting on. The target may therefore be unreachable; the stats say where it
  got to.
- **The level-1 mip survives eviction.** *Dirtying* a child invalidates it,
  because the shape changed; eviction does not change the shape, it drops a
  cached copy. So a host that has built level 1 keeps a coarse silhouette to
  draw at an eighth of the memory — which is most of the value of responding to
  a warning at all.
- **Work in flight stays safe.** Eviction does not reset generations, so a
  request issued before the trim is still refused as `Stale`, and a key removed
  by `forget_empty` answers `Stale` too. Late work is discarded rather than
  landing in a slot it no longer owns.

**Which bricks go is a spatial decision, not a temporal one.** "Least recently
used" is the reflex answer and the wrong shape: a sculptor works in a
neighbourhood, so what they return to is near where they are working rather than
what they touched last. Measured on a walking-stroke fixture that trims to 60%
after every dab, dropping furthest-from-focus re-requested **21%** of what it
evicted against **53%** for dropping arbitrarily — and reached the same target
having evicted **40% fewer bricks**, because it was not dropping things that
came straight back. Pass `NULL` for the focus only when there genuinely is none:
an offline bake, or a document that is not on screen.

**There is no automatic eviction loop**, and there will not be. The cache
publishes no refill loop, thread pool or timer for the same reason: the consumer
owns scheduling. Eviction is something a host asks for.

### What the document itself costs

The cache is not the document, and until v0.49.0 the document could not be
measured at all. Every subsystem accounted for itself and **nothing rolled up**:
the history reported its bytes, one grid reported its sculpt layers, the brick
cache reported a cache the document does not own — and the edit list, the voxel
chunk storage those sculpt layers sit beside, masks, mesh layers and the
passthrough blobs reported nothing. A rasterized voxel layer is the largest
thing most documents hold and it was invisible.

```c
clay_memory_report m = { .struct_size = sizeof m };
clay_document_memory(doc, &m);
```

**The breakdown is the feature, and the total is not.** Under pressure you do
not need to know how big the document is; you need to know *which part*, because
that is what decides what you may release:

| | may you release it? | what it costs you |
|---|---|---|
| `history` | **yes** | undo depth — `clay_document_set_history_budget` is the lever |
| `voxel_sculpt_layers` | **yes** | voxel undo depth |
| `passthrough` | **yes** | a thumbnail; regenerable |
| the brick cache | **yes** | a stall — *not counted here; not owned here* |
| `edit_list`, `voxel_content`, `mesh_layers`, `masks` | **no** | it is the user's work |

So the order on a warning is: trim the brick cache first (a stall), then the
history (undo depth), and never the rest. `voxel_content` and
`voxel_sculpt_layers` live inside the same grids and are reported separately for
exactly that reason — a combined figure would hide the only voxel bytes you are
allowed to touch.

`clay_layer_memory` gives the same breakdown for one layer, so a large document
can be attributed to the abandoned blockout that is 200 MB of it rather than
merely called large.

**A `MultiresSurface` a HOST holds is not in this report, and that is a
consequence of what it is rather than an omission.** A surface the host built
and keeps beside the document is a standalone handle like `DynamicSurface`, so
the document cannot walk to it. Its accounting is per surface, through
`clay_multires_memory`, and it makes the same authoritative/rebuildable split
this table does:

**A hierarchy the DOCUMENT carries IS in this report** (ABI 0.88.0). Once
`clay_layer_take_multires` has moved one into a layer, the document holds it and
walks it: `base` and `topology` land in `surface_content`, `detail` in
`multires_detail`, `sculpt_layers` in `sculpt_layers`, and the rebuildable rows
in `surface_caches`. The two cannot double-count, because a document only ever
reports what it holds and the ledger only what the host passed in.

| | may you release it? | what it costs you |
|---|---|---|
| `base`, `topology`, `detail` | **no** | it is the user's work |
| `sculpt_layers` | **no** | it is the user's work — every layer's coefficients and masks |
| `evaluated`, `composed`, `runtime_index` | **yes** | a rebuild, and nothing else |

`sculpt_layers` is reported apart from `detail` even though the two hold the
same quantity in the same field type, because they hold it under different
owners: a host deciding what to merge, bake or delete needs to see which of the
two is costing it, and a combined figure would hide the only number an artist
can act on. `composed` is the materialized `B + SUM(s*m*L)` — derived from the
two rows above it and droppable, even though it is the array evaluation reads.

There is deliberately **no cap on a layer stack**, unlike the history's budget.
A cap that silently stopped recording would leave the pass on the surface and
un-dialable, which is a correctness bug wearing a memory limit's clothes. The
levers are `compact_sculpt_layers()` — the cheapest, and the one to reach for
first, because it releases every all-zero block a gesture that undid itself left
behind — then merge, bake and delete. Only the first is free of a decision: a
compaction is asserted to leave the **evaluated surface bit for bit**, both
immediately and after every covered block has been recomposed, so a host under
pressure may run it without asking the artist. Merge, bake and delete each
change what is addressable afterwards. See
[07 §8b](07-brushes-and-features.md#memory-is-reported-and-never-capped).

**Instance layers are counted once**, document-wide, and in full per layer — ten
instances of one blockout are one allocation, and saying otherwise would invite
you to free memory that does not exist, while reporting zero for an instance
would call displaying it free. Since 0.58.0 that survives a **save**: a shared
edit list is written once and named from the other instances, so reading the
report back after a reload gives the figure it gave before. Through 0.57.0 every
layer's content went out inline, so a document of ten instances reloaded ten
times heavier and the layers were quietly no longer linked.

**Three things to expect, none of them a defect:**

- **It is a floor, not an equality.** These are container walks. Allocator block
  headers, size-class rounding and arena fragmentation are invisible from here,
  as are this library's own code and static data. The OS will charge the process
  more; do not read the gap as a leak.
- **It exceeds the same document's file, often several times over.** A
  `.clayspace` is RLE- and palette-compressed; live storage is not.
- **`voxel_content` follows chunks, not cells.** A chunk is 32³ cells allocated
  whole, so **one voxel costs 32 KiB** and 32 768 voxels filling that same chunk
  cost the same 32 KiB. Two layers whose occupancy differs by three orders of
  magnitude report an identical figure when they touch the same chunks. Present
  it beside `clay_voxel_occupied_count` if you like, but expect the two to move
  independently: what grows this number is the *region* an artist has worked in,
  not how solidly they filled it.

`transient` reports memory held only while an operation is in flight — a mask
copies its chunks on the first touch inside a recorded step, roughly doubling
for that step's duration. **Through the C ABI it always reads zero**, and that
is a statement about the ABI rather than the mechanism: every mask entry point
opens its step and closes it before returning, and calls on one document must be
serialized, so there is no moment at which you could hold a handle, have a step
open, and ask. It is reported anyway so the total stays the sum of the fields if
an entry point spanning a step is ever added. Do not build a response around it.

### Which space a mesh-sculpting call speaks

A mesh layer's vertex arrays are **layer-local** and its transform places them.
That is the contract, and it is the right one — baking a transform into vertices
every time a layer moves is expensive, lossy and hostile to history.

What was wrong until 0.91.0 is that the calls crossing that boundary did not
agree, and the header did not say which was which:

| call | takes | returns |
|---|---|---|
| the layer's `positions` / `indices` | — | **local** |
| `clay_layer_bounds` | — | **world** |
| `clay_document_mesh_combined` | — | **world** |
| `clay_mesh_sculptor_raycast` | **world** ray + a frame | **world** hit |
| `clay_mesh_sculptor_stamp` | **local** centre and radius | local |
| `clay_mesh_sculptor_apply_stroke` / `_preset` | **local** samples | local |
| `clay_mesh_sculptor_lattice` / `_deformer` | **local** cage and axes | local |

Rows four and five are the two calls a host makes back to back to sculpt where
the finger is. On a layer translated by 3 and scaled by 2, feeding the first
into the second **moved 0 vertices and returned `CLAY_OK`** — and `moved == 0`
is documented to mean "reached nothing, fully masked, or no displacement", so
the failure was indistinguishable from three ordinary outcomes.

**A session now declares its space once:**

```c
clay_mesh_sculptor_use_layer_transform(sculptor);   /* the call you want */
/* or, for a mesh held outside a document: */
clay_mesh_sculptor_set_world_frame(sculptor, &frame);
```

With a frame declared, every position, radius, direction and normal crossing
that handle is world — the stamp, both stroke calls, the raycast, and the
**mask**, which is world-addressed by design and was being sampled at a local
vertex position by the single-stamp path. Declaring nothing is the identity,
which is exactly the behaviour that came before; a host that has not heard of
this is not opted into it. Passing a per-call frame *and* declaring a session
one is refused rather than resolved by precedence.

`clay_mesh_sculptor_use_layer_transform` **refuses a layer carrying a per-axis
scale** (`CLAY_ERROR_UNSUPPORTED`). Under one, a round brush in world is an
ellipsoid on the model, so `radius` names nothing a spherical surface walk can
honour and a normal stops being carried by the rotation alone. Reading the scale
as uniform would put the dab in a plausible wrong place and report success.
Closing it needs an anisotropic brush footprint. Bounds and the combined export
honour the per-axis scale either way.

The lattice and the deformer stay **local-only** and say so: a cage is authored
against the model, not against the world.

**The topology cache IS in this report, and it is the one sculptor-adjacent
figure a document can account for by itself** (ABI 0.89.0). Building the weld
classes and the neighbourhood CSR a brush walks is the whole of what a sculptor
costs to construct — 120.8 ms on a 296k-triangle mesh, against a sculptor whose
other members are empty vectors — and it used to be paid again by every session
opened on the same triangles. The document now holds one per mesh layer:

```c
clay_topology_cache_stats s = { .struct_size = sizeof s };
clay_document_topology_cache_stats(doc, &s);   /* entries, bytes, hits, misses */

uint64_t released = 0;
clay_document_trim_topology_cache(doc, &released);
```

Read `hits` and `misses` first, not `bytes`. A cache that never hits and a
cache that is not there are indistinguishable from outside, so those two are the
only way to tell whether your session pattern is defeating it.

- **Sculpting does not invalidate an entry.** A weld partition is pinned when it
  is built and positions move under it freely — the fixed-topology contract, and
  what a sculptor live across a stroke has always held. A sculptor created after
  a stroke therefore gets what the live one had. The one visible consequence: a
  vertex dragged out of a coincidence it was welded into stays in its class.
- **Replacing a layer's triangles does.** `clay_document_replace_mesh_layer`,
  `clay_document_voxel_remesh_layer` and `clay_mesh_weld` on a layer all drop
  the entry.
- **An entry is verified, not trusted.** Every lookup fingerprints the entry
  against the mesh it is about to be served for — counts, weld epsilon and a
  hash of the index buffer, 0.25 ms — so two meshes with identical counts and
  different connectivity cannot be served each other's data, and a replacement
  path that failed to invalidate is a slow miss rather than a wrong answer.
- **A trim releases only what no live sculptor holds**, which is a fact rather
  than a best effort: entries are reference-counted. It is safe from a memory
  warning arriving mid-stroke — it releases the layers nobody is working on and
  leaves the one under the finger alone.
- **A standalone `clay_mesh` builds its own.** It belongs to no layer, so there
  is no identity to key an entry on, and keying on the pointer would key a cache
  on an address the next allocation can reuse.

**A sculptor's scratch is not in this report, and it is not an omission.**
`clay_document_memory` measures the document; a mesh sculptor is a handle held
BESIDE one, on the same footing as `MultiresSurface::memory()`, and a document
does not know how many are open on it. Since 0.77.0 each sculptor owns one bump
arena for the buffers a stamp needs and cannot hold as members — the automask's
frontiers, the adaptive region's sort permutation — reset rather than freed
between stamps, so a warm dab on a stable surface allocates nothing. Ask the
sculptor:

```c
clay_brush_arena_stats a = { .struct_size = sizeof a };
clay_mesh_sculptor_arena_stats(sculptor, &a);   /* also _dynamic_ and _multires_ */
```

Read it as **`growths`, then `high_water_bytes`, then `capacity_bytes`** — the
order is deliberate. There is no current-usage field because a current usage
reads zero between stamps whatever the arena is doing, and `growths` is the only
one of the three that distinguishes an arena that has converged from scratch
that is never released: the second allocates nothing after warm-up and consumes
memory without bound, which no allocation count can see. The table above asks
"may you release it?" and the answer here is **yes, by destroying the
sculptor** — the arena is rebuildable state, in the brick cache's category
rather than the user's work, and it costs a warm-up rather than a stall. There
is deliberately nothing to tune: no reserve, no cap, no growth factor, because
each would be a number a host sets from one device and is wrong about after the
brush radius changes, and the arena already sizes itself from the largest
footprint it has been given.

Runnable: [`examples/62_what_this_document_costs.py`](../examples/62_what_this_document_costs.py),
which builds a document the way an artist would and attributes it. On that
fixture a 60-item SDF blockout is 29 KiB and the layer rasterized from it is
514 KiB — **18× the entire edit list**, and the largest term that used to be
unreported.

### What "latency-critical" costs, measured

"Latency-critical" was an adjective here until v0.25.0; it is now a number.
`tools/run_device_bench.sh` measures one brush stamp on an attached iPad at
p50/p95 across a 10/100/1000-stamp document axis, and
`tests/device/baseline.json` is the committed reference. See `docs/RELEASE.md`
for how to run it and how to read a result.

From the first baseline — **iPad Air 13-inch (M3), iOS 26.5.2** — worst-point
p95 per operation:

| | p95 | grows as |
|---|---|---|
| every voxel verb (11 of them) | **< 0.03 ms** | flat |
| one SDF stamp, edit + evaluate, CPU | **4.41 ms** | `N^0.88` |
| one SDF stamp, edit + evaluate, Metal | **1.77 ms** | `N^0.30` |
| one SDF stamp, through the brick cache | 5.60 ms | `N^0.64` |
| Move drag (`layer_move_surface`) | 0.10 ms | `N^1.02` |
| consolidate | 1.57 s | `N^0.84` |
| mask extrude | 2.53 s | `N^0.91` |

Four things a host should design around, none of them obvious from the API:

1. **The voxel path is effectively free and flat; the SDF path is neither.**
   At 1000 stamps one SDF stamp already exceeds the engine's half of a 120 Hz
   frame (4.17 ms), and a real sculpt is far more than 1000 stamps.
2. **Metal is not always the fast choice.** It *loses* to the CPU at ten stamps
   (0.44 ms vs 0.08 ms p95) and wins by 2.5x at a thousand (1.77 vs 4.41),
   because dispatch overhead dominates until the work amortises it. Select by
   document size and measure the crossover on the hardware you ship to; a host
   that picks Metal unconditionally is slower through the whole blockout phase.
3. **The incremental path is not the cheap one.** Driving the brick cache the
   way a host does — dirty the new node, drain, evaluate, submit — costs *more*
   than re-evaluating the whole working volume at these sizes (5.60 ms against
   4.41 ms at 1000 stamps). Bricks refreshed per stamp is constant at ~13
   across the axis, so the cost is the culled tape compiled per brick, not the
   number of bricks. That is what `add-item-spatial-index` addresses.
4. **`consolidate` and `mask extrude` are seconds, and both scale with the
   document** — `N^0.84` and `N^0.91`. They need progress UI, and neither is
   something to trigger from an advisory threshold without telling the artist.
   (Both have moved since this baseline, and both were re-measured on the
   reference iPad on 2026-08-25: `consolidate` is **315 ms** p95 at 1000
   stamps and `mask extrude` **4.29 s**. `consolidate`'s bake batches its
   lattice samples through the CPU backend's thread pool, and every verb that
   bakes stopped measuring its sample Lipschitz over the dense lattice. They
   still scale with the document — `N^0.59` and `N^0.95` on that run — and
   still deserve the progress UI. Note `mask extrude` is *slower* than this
   table says rather than faster: the first-baseline figure had drifted from
   the committed baseline, which records 4.40 s.)

## 10. Python bindings (`pyclay`)

nanobind module, numpy-native, shipped as wheels (macOS arm64/x86-64, Linux, Windows) with the CPU backend always included and GPU backends when present.

```python
import pyclay as clay
import numpy as np

doc = clay.Document()
body = doc.add_sdf_layer("body", resolution=512)
body.add(clay.Sphere(r=1.0), blend=clay.Smooth(0.2), color="#38a6cf")
body.add(clay.Capsule(a=(0,0.8,0), b=(0,1.6,0), r=0.35),
         op=clay.Op.ADD, blend=clay.Chamfer(0.1))
body.add(clay.Box(size=(0.4,0.4,0.4)).twist(1.2), op=clay.Op.SUBTRACT)
body.mirror(axis="x", blend=0.15)

d = body.eval(points)                    # (N,3) float32 -> (N,) distances
g = body.gradients(points)               # tetrahedron-trick normals
mesh = doc.mesh(resolution=512, decimate=0.5, backend="cpu")
assert mesh.is_watertight()
mesh.save("body.fbx"); mesh.save("body.obj")
doc.save("body.clayspace")               # opens in the iPad app
```

The snippets in this section are illustrative. For code that is executed on
every CI run — and rendered — see `examples/`, which covers the primitive set,
every blend and combine mode, deformers, repetition, lifts, transitions, voxel
sculpting, meshing and I/O.

The C ABI mirrors this surface: an item builder composes the same edits, and
voxel grids, brushes, sculpting verbs, picking and evaluation are all reachable
from C and therefore from Swift. `tools/check_binding_parity.py` fails CI when a
`pyclay` capability has no C counterpart and no recorded exemption, so the two
cannot drift apart again.

The builders are write-only on purpose — the host that filled one already knows
what it put there — so the ABI has no `clay_item_*` getter. Reading a PLACED
node is a different question, asked by a host that reloaded a document and has
only ids: `clay_layer_stroke_points` returns a curve's control points as
authored, taking the arguments `clay_layer_set_stroke_points` takes, so what
comes out goes straight back in. An armature answers the same call for its
nodes — the same x, y, z, radius list — `clay_layer_armature_parents` for its
topology and `clay_layer_armature_signs` for which nodes carve (#99, one +1/-1
per node, positive-padded exactly as short parents read as roots), mirroring
the split on the setter side, so a reloaded rig can be re-posed and
un-negatived through `clay_layer_armature_edit` with indices the host actually
holds. `clay_layer_node_prim` reports which primitive a placed node carries, so
a host finds the armature (or the curve) by asking rather than by probing
readers until one stops refusing — and `clay_layer_node_count` /
`clay_layer_node_at` say which nodes there are to ask about, since node ids are
not dense and a rig placed after a run of removed ones is invisible to a probe.

Every branch of that walk except one now ends in a reader. The one that did not
was the plain item — `clay_layer_node_prim` said which primitive it carried and
nothing else — so a host that placed a primitive, moved it with a manipulator
and edited its operation afterwards kept those values in a table of its own,
saved beside the document and keyed by node id, and kept it correct across undo
and redo by following the engine's history by depth (#317).
`clay_layer_node_transform`, `clay_layer_node_params` and
`clay_layer_node_op_blend` close it, each taking what its setter takes so what
comes out goes straight back in: position, rotation axis/angle and scale; the
primitive's parameter block by the size-query pattern, counted in floats and
sized off the primitive rather than off a table the caller keeps; and op, blend,
blend radius and rounding. `clay_layer_node_influence_bound` is NOT the
positional answer and never was — it is dilated by rounding and blend support
and, under a layer mirror, covers the reflection too, so an item at x = 0.9 in
a mirrored layer reports a bound centred on the origin.

Documents can be edited after they are built, not only appended to. Every
editing entry point applies one command from `scene/commands.h` — the same
vocabulary the `.clayspace` format records — so a binding edit means exactly
what a saved document means, and becomes undoable for free once the undo stack
is exposed:

```python
node = layer.add(clay.Sphere(r=0.5))       # ids come back from add()
layer.set_transform(node, position=(2, 0, 0))   # and survive every edit
layer.set_prim(node, clay.Box(size=(1, 1, 1)))  # modifiers on the node stay
layer.set_op_blend(node, op=clay.Op.SUBTRACT, blend=clay.Smooth(0.2))
layer.set_color(node, "#ff8800")
layer.append_stroke(stroke_node, [(1.0, 0.0, 0.0, 0.3)])   # a drag gesture
layer.remove(node)

doc.set_layer_visible(layer.id, False)     # exact and reversible
doc.set_layer_transform(layer.id, position=(0, 4, 0))
doc.move_layer(layer.id, 0)
doc.remove_layer(layer.id)

# three placements written once, on the inside: each writes the TRANSLATION and
# nothing else, as one command and one undo step. Done outside, the read half
# refuses a per-axis-scaled layer and the write half CLEARS the per-axis scale,
# so "snap to floor" silently unsquashes the subtool.
layer.snap_to_ground(0.0)   # low face of the tight world box -> y = 0
layer.centre_bounds()       # centre of that box -> the origin
layer.zero_to_origin()      # translation -> (0, 0, 0); rotation and scale kept
# They raise where the C ABI refuses: an empty, radial or unbounded layer for
# the first two — empty is refused rather than a silent no-op, because "already
# in place" and "there is nothing here" are the two states an artist most needs
# told apart. `zero_to_origin` reads no bounds and takes all three.
# `centre_bounds` is named for the BOX: the engine holds no density, so the
# occupancy-weighted centroid the name "centre-mass" implies has no answer until
# someone names a cell size, and this signature has nowhere to put one.
```

A **group** is a node whose children compile as one sub-expression, so an op
inside it reaches its own subtree and nothing else — which is what makes
"intersect A with B, then union that into C" sayable at all. Without it a plate
needs a layer of its own; see `examples/37_groups.py`.

```python
plate = layer.add_group(op=clay.Op.ADD)             # a node id, like any other
layer.add(clay.CutHollowSphere(r=0.6, h=0.17, t=0.05), parent=plate)
layer.add(clay.Box(size=(0.6, 2, 2)), op=clay.Op.INTERSECT, parent=plate)
layer.children(plate)                               # -> [shell_id, cutter_id]

inline = layer.add_group(op=clay.Op.INLINE)         # children join the OUTER
layer.add(clay.Sphere(r=0.2), parent=inline)        # chain: naming, not a field
```

A group has no transform of its own — the compiler composes `layer * item` and
nothing else — and it carries no transition op, so both are refused rather than
silently ignored. Reparenting is `layer.move(node, parent=group)`, whose inverse
is the parent and index it captured before moving; a move into a node's own
subtree is refused, because it would detach the subtree from the roots and the
next save would drop it.

Undo is opt-in per document and rides the same commands, so no reachable edit
escapes it and the app does not reimplement a history over a second vocabulary
that could disagree with what a saved document records:

```python
doc.enable_undo()                   # off by default; costs nothing unused
layer.set_transform(node, position=(2, 0, 0))
doc.undo()                          # -> True; the document is byte-identical
doc.redo()

doc.begin_undo_group()              # a compound gesture undoes as one step
layer.set_color(node, "#ff8800")
layer.set_op_blend(node, op=clay.Op.SUBTRACT)
doc.end_undo_group()

doc.undo_depth, doc.redo_depth      # what a UI needs to label its buttons
```

Consecutive appends to one stroke coalesce automatically, so a drag gesture is
a single undo step rather than one per sample.

### Widened surface (implemented)

Beyond the sample above, `pyclay` reaches the rest of the C++ vocabulary:

```python
# sculpt strokes — one edit item, not one per segment
body.add(clay.Stroke(points=[(-1, 0, 0, 0.3), (0, 0.5, 0, 0.22), (1, 0, 0, 0.3)],
                     blend_k=0.05))

# every primitive is a class; the unbounded ones work like the rest
body.add(clay.Link(length=0.3, r1=0.5, r2=0.15))
body.add(clay.SolidAngle(angle=0.7, ra=0.8), op=clay.Op.SUBTRACT)
body.add(clay.Plane(normal=(0, 1, 0), offset=-0.5), op=clay.Op.SUBTRACT)  # flat cut
body.add(clay.Icosahedron(r=0.4), blend=clay.Smooth(0.1))

# extended combine modes
body.add(clay.Box(size=(0.6, 0.6, 3.0), position=(0.7, 0, 0)),
         op=clay.Op.GROOVE, blend=clay.Smooth(0.12), rounding=0.05)

# a whole LAYER's boolean: how it folds into the visible SDF layers beneath it.
# Omitted arguments keep their value, and the reader hands back what the setter
# takes, so a host can show the current fold rather than guess it.
cutter = doc.add_sdf_layer("cutter")
cutter.add(clay.Sphere(r=0.6, position=(0.7, 0, 0)))
doc.set_layer_composition(cutter.id, op=clay.Op.SUBTRACT, blend=clay.Smooth(0.1))
op, blend, rounding = doc.layer_composition(cutter.id)
doc.set_layer_visible(cutter.id, False)   # gives the uncut geometry back exactly
# ask BEFORE saving for an older build: minor 17 cannot say a composition, and
# writing one there would bring the cutter back as a lump welded on
ok, blocking = doc.writable_at_minor(17)  # (False, cutter.id) once it composes

# voxels: standalone grids or document layers
blocks = doc.add_voxel_layer("blocks", voxel_size=0.1)
blocks.set_many(np.array([[0, 0, 0], [1, 0, 0]], np.int32), blocks.palette_add("#ff8800"))
blocks.set_brush((0, 0, 0), 5, blocks.palette_add("#4488ff"), shape="sphere")
blocks.paint_brush((0, 0, 0), 7, blocks.palette_add("#ff4444"))  # occupied only
# soft brushes: falloff dithers coverage across the footprint, deterministically
blocks.set_brush((0, 0, 0), 15, 1, shape="sphere", falloff="smooth", strength=0.6)
# sculpting verbs reshape what is already there
blocks.sculpt_smooth((0, 0, 0), 9)
blocks.sculpt_inflate((0, 0, 0), 9, amount=-1)          # negative = erode
blocks.sculpt_flatten((0, 0, 0), 9, normal=(0, 1, 0))
blocks.sculpt_pinch((0, 0, 0), 9)
# fill-cavities is NOT a morphological closing: a ball of radius r fits INTO a
# dent wider than r, so a bigger element fills less. The rule is "an empty cell
# with 4+ occupied face neighbours is inside a pocket".
blocks.sculpt_fill_cavities((0, 0, 0), 9, passes=2)
blocks.sculpt_scrape((0, 0, 0), 9, normal=(0, 1, 0))     # flatten + smooth, ONE snapshot
blocks.sculpt_smudge((0, 0, 0), 9, displacement=(0.2, 0, 0))  # skin, not lump
blocks.sculpt_carve_alpha((0, 0, 0), 9, alpha=stamp, direction=(0, 1, 0))  # (H,W) floats

# pre-bake repair. The report is non-destructive: a destructive operation whose
# input is somebody's sculpt should be askable before it is answerable.
blocks.repair_report()                     # enclosed_voids, void_cells, largest, airtight
blocks.repair_close_holes(passes=1)        # only ever adds cells
blocks.repair_fill_voids()                 # enclosure decided by a flood from outside
# masks freeze a region against any edit: effective strength is
# strength * (1 - mask). Addressed in WORLD units on their own lattice, so a
# mask survives a resolution change or a move between representations — the
# failure mode 3DCoat is best known for.
freeze = doc.add_mask("blocks", cell_size=0.1)
freeze.paint((0.5, 0, 0), size=9, falloff="smooth")     # target=1 masks, 0 erases
freeze.expand(1); freeze.smooth(1); freeze.invert()     # region operations
blocks.erase_brush((0, 0, 0), 15, mask=freeze)          # masked cells untouched
freeze.sample_many(points)                              # (N,3) -> (N,) in [0,1]
# masking is a GESTURE: the same stroke engine resolves it, so spacing,
# pressure, taper and steady stroke reach a mask stroke. The footprint comes
# from each stamp's WORLD radius, so the stroke's width does not track the
# mask's resolution.
freeze.apply_stroke(samples, brush, target=1.0)         # target=0 erases
# invert() flips only what has been PAINTED — an unbounded sparse lattice has
# no finite complement — so "mask this, edit everything else" takes the region
# from the caller, who always has one.
freeze.invert_within(((-1, -1, -1), (1, 1, 1)))
freeze.fill(((-1, -1, -1), (1, 1, 1)), 0.0)             # 0 releases the region
# and the freeze reaches the field verbs, not only the voxel ones
volume.relaxed(centre=(0.5, 0, 0), region_radius=0.4, mask=freeze)
# strokes: a drag becomes stamps, and a stamp becomes an ordinary edit —
# which is what gives a brush undo, coalescing and serialization for free
brush = clay.StrokePreset(radius=0.15, spacing=0.25, pressure_size=1.0,
                          taper_start=0.1, taper_end=0.1, steady=0.4, seed=7)
samples = np.array([[x, y, z, pressure] for ...], np.float32)   # (N,3/4/5)
brush.resolve(samples)                     # pure: positions/radii/strengths
blocks.apply_stroke(samples, brush, blocks.palette_add("#cc7744"))
body.apply_stroke(samples, brush, clay.Sphere(r=1.0), mask=freeze)  # one undo step
clay.StrokePreset.deserialize(brush.serialize())   # versioned: newer is refused

# the Move brush: drag the assembled SURFACE, which is not what putting a grab
# on one item does. A deformer is per ITEM and its centre is in that item's own
# frame, so a grab pulls one item's share of a blended form and leaves the rest.
# This maps the drag into every contributing item's frame and puts it at the
# FRONT of each chain — deformers apply in authoring order, so the first is the
# outermost warp on the geometry. One undo step however many items it touches.
layer.move_surface((0, 0, 0), (0, 0.4, 0), radius=0.8)   # -> the nodes warped
# The surface moves LESS than you ask for: grab weights at the sample point
# rather than at its preimage. Monotonic, so a UI can calibrate.
# mask extrude: mask a patch of a surface and pull it off as a solid — ZBrush's
# Extract. THE MASK IS THE REGION, so unlike relax and flatten there is no
# region_radius: the painted region bounds itself. The one new mechanism is
# measuring the mask — a [0,1] scalar on a lattice is not a distance field, so
# composing one directly would put a step in the result and the Lipschitz bound
# would stop meaning anything. After that it is ordinary op composition.
freeze.to_field(band=0.2, pad=0.3)                # the measured mask, as a Volume
plate = doc.mask_extrude(freeze, thickness=0.12,  # 'outward' | 'inward' | 'centred'
                         side="outward", border_round=0.05, border_smooth=2)
shell.add(plate)                                  # an ordinary item
blocks.mask_extrude(freeze, thickness=0.12)       # the voxel path: cell space,
                                                  # keeps the palette, agrees to
                                                  # within a voxel
# It BAKES, and it refuses rather than returning something empty when the mask
# misses the surface — the common mistake, and the one an empty result hides.

# snakehook: pull a horn or a tendril out of a form — the brush that turns a
# sphere into a creature. NOT a new kind of geometry: the stroke opcode already
# sweeps a sphere along a chain with a radius per point, and that IS a tendril
# once the radii taper, so the field stays EXACT and a tendril costs the
# raymarcher nothing. What this adds is the step that turns a drag into the item.
# The anchor is prepended, so the tendril starts where the user touched rather
# than where the first drag sample landed a frame later. The taper follows ARC
# LENGTH, so gesture speed does not decide thickness. taper_curve above 1 thins
# away fast (a whip); below 1 holds and then drops (a horn).
# It ADDS material rather than moving it: ZBrush pulls existing surface, so its
# body dimples at the base and ours does not.
body.add(clay.snakehook(anchor=pick_point, inward=-pick_normal, path=drag_pts,
                        base_radius=0.2, tip_fraction=0.15, taper_curve=0.6),
         blend=clay.Smooth(0.08))

# magnify and pinch: ONE deformation, one signed strength — positive swells the
# surface away from the centre, negative gathers it toward. Maxon's own page
# says so ("Magnify: ... inverse of Pinch"), so there is no separate pinch.
# The centre of the scale is its FIXED POINT: a scale about a point on the
# surface bulges the form around it and leaves that point exactly where it was.
# Support is finite, which is what keeps influence bounds tight; scaling space
# is not distance preserving, so the safe step scale drops with the strength.
clay.RoundBox(size=..., r=...).magnify(center=(0, 0, 0), radius=0.95, strength=0.45)
grid.sculpt_magnify(cell, size=3)   # the voxel inverse of sculpt_pinch

# noise: irregularity — the irregular sibling of `displace`, whose sine is
# regular by construction and gives an even corrugation instead. The HASH IS
# INTEGER, and that is not an implementation detail: cross-backend parity is
# tolerance-based (1e-6 CPU / 1e-4 GPU), and a float hash multiplies each
# backend's own `sin` by ~43758 and takes a fractional part, turning a 1e-7
# difference into an O(1) one. Integer ops give the same bits everywhere, which
# is why the shim now carries `cuint`.
# The SEED is a plain parameter, not global state. The fractal is normalized, so
# `octaves` adds detail and `amplitude` stays the one control over how far the
# surface moves. Offsetting the distance costs the marcher: the step scale drops
# with amplitude x frequency, summed over the octaves.
clay.Sphere(r=0.7).noise(amplitude=0.09, frequency=5.0, octaves=4, gain=0.5, seed=17)

# loft: two or more profiles interpolated along Z, evenly spaced. Three or more
# are BRACKETED, so wide-narrow-wide gives a waist rather than a straight
# taper. A loft is a BOUND, and its Lipschitz is not 1 — interpolating very
# different profiles over a shallow depth makes the field change fast along the
# axis, so the document's safe step scale drops and the raymarcher slows down.
body.add(clay.Loft([clay.Profile.circle(r=1.0), clay.Profile.polygon(pts)],
                   half_depth=1.0, ease=3))

# swept: the same profiles carried along a GUIDE curve. The frame is
# parallel-transported when the item compiles — a Frenet frame flips at an
# inflection and is undefined where the guide is straight. Profiles are
# distributed by ARC LENGTH; the ends are the profile itself, flat. BOUND, and
# its Lipschitz has two terms: curvature R/(R-r) and the profile lerp. A
# profile wider than the guide's tightest bend folds through itself — not
# refused, since a guide is editable, but the step scale collapses.
body.add(clay.Swept(guide_pts, [clay.Profile.circle(r=0.3), clay.Profile.circle(r=0.1)],
                    types="spline", tolerance=0.01))

# volume: a field SAMPLED onto a sparse narrow-band grid, then used as an item.
# Storage follows the SURFACE, not the region — only bricks straddling the band
# hold samples, the rest carry a signed lower bound — so it is O(area) and
# rides in the tape's blob like any other payload rather than behind a resource
# handle. BOUND, in two halves: where it has samples the value interpolates and
# OVERSHOOTS (accurate to the sampling, not a lower bound); where it has none it
# is a genuine lower bound. `cell` is a real dial on accuracy. Not constructible
# through the C ABI until mesh import gives it a source; documents containing
# one load, evaluate and mesh everywhere.
baked = clay.Volume.from_document(other_doc, cell=0.04)   # band defaults to 3 cells
body.add(baked, op=clay.Op.SUBTRACT)
baked.cell_size, baked.band, baked.brick_count, baked.megabytes, baked.bounds

# mesh import: the same sampling, fed by a triangle soup — which is what makes
# an imported model something you can WORK on rather than only display. The
# SIGN is the hard half and comes from the generalized winding number, not a
# ray cast or the nearest triangle's normal: real assets are not watertight,
# one hole flips a parity test for a whole half-space, and the nearest triangle
# to a point inside a model may face away when the wall is missing. A winding
# number passes smoothly through 1/2 across an opening instead.
# `cell` defaults to a fraction of the mesh's own longest side, since a default
# in world units suits neither a building nor a bolt.
body.add(clay.Volume.from_mesh(clay.load_mesh("scan.ply"), cell=0.02))
# the budget is checked against the file's DECLARED counts before anything is
# allocated — a malformed file can claim a billion triangles. 0 = the default.
clay.load_mesh("untrusted.obj", max_vertices=2_000_000, max_triangles=4_000_000)

# and the microscope, when you want to see the sign behave rather than trust it
q = clay.MeshQuery(mesh)          # the BVH is built HERE, once, not per call
q.distance(pts), q.signed_distance(pts), q.contains(pts)
q.winding_number(pts, beta=2.0)   # beta=0 sums every triangle; slow, exact
clay.Mesh.from_triangles(positions, indices)   # hand back a mesh you edited

# relax: the last of the core sculpting brushes — voxels always had smoothing,
# SDF layers had none. RELAX BAKES: what comes back is a volume, not the edit
# list that went in, and that is inherent rather than a shortcut, because a
# general relax must smooth a bump in the middle of ONE item and no reweighting
# of an edit list expresses that.
# Smoothing destroys EXACTNESS but cannot break the Lipschitz bound — an average
# cannot vary faster than what it averages — and a field whose slope is bounded
# by 1 is automatically a conservative bound on the distance to its own zero
# set, so the raymarcher stays correct.
smooth = baked.relaxed(strength=1.0, radius_cells=3, iterations=4)
smooth = baked.relaxed(radius_cells=3, centre=(0, 0.7, 0),   # a brush, not a
                       region_radius=0.35, falloff=0.2)      # filter

# flatten: put a facet on part of a shape, which a Cut cannot — a cut is global
# to its prism and has no falloff. Two-sided, matching the voxel sculpt_flatten:
# material on the normal's side goes AND hollows on the other side fill.
# It RESAMPLES rather than rewriting samples in place, because flatten moves the
# surface by many band widths and a fixed set of bricks cannot follow one that
# walks out of it — the facet needs bricks that held nothing. Sample from the
# DOCUMENT where you can: a volume reports a bound, not a distance, outside its
# own band. Flattening a volume costs what the brush touches, not what the
# volume holds (#300): only the bricks its ball reaches are re-evaluated and
# reclassified, and the rest keep their bytes.
# A REGION IS REQUIRED. Where flatten's weight is 1 the result IS the plane, so
# with no region it would replace the shape with a half-space — a ball comes
# back as a box. Not ZBrush's Clip either: as a solid, Clip is exactly Trim.
facet = clay.Volume.flattened_from(doc, plane_point=(0, 0.55, 0),
                                   plane_normal=(0, 1, 0), cell=0.025,
                                   centre=(0, 0.62, 0), region_radius=0.3,
                                   falloff=0.25, strength=1.0)
facet.sample_lipschitz          # measured, not assumed: a tight taper is steeper
imported.flattened(...)         # a volume source, for a mesh with no document

# consolidation: what lets any of the above be used TWICE. Each region verb
# samples a document and hands back a volume, so the second call samples a
# volume — and outside its band a volume reports a bound, not a distance. Move
# fails from the other side: each drag appends a grab and those multiply.
layer.field_report(advise_below_step_scale=0.25)
# -> {'lipschitz', 'safe_step_scale', 'steepest_volume',
#     'longest_deformer_chain', 'item_count', 'advises_consolidation'}
# The two mechanisms are named SEPARATELY because they have different cures and
# the aggregate cannot tell them apart. ADVISORY: nothing here bakes, and the
# threshold is yours, because a tolerance for marching cost belongs to a
# viewport and a frame budget rather than to the artwork.

layer.consolidation_advice(advise_below_step_scale=0.4)   # AT WHAT, and what it costs
# -> {'advises': bool, 'params': {...} or None, 'cost': {...} or None}
# The recommendation `advises_consolidation` above leaves open: `field_report`
# says a bake is the cure, and `consolidate` then requires a `cell` nothing will
# guess. `params` is keyed to `consolidate`'s own arguments, so
# `layer.consolidate(**advice["params"])` is the whole round trip; both `params`
# and `cost` are None when nothing is advised, which raises at the next call
# rather than baking at a resolution nobody chose.
# `advises` is True only when the report advises AND the PROJECTED step scale
# reaches the threshold — a sampled volume declares sqrt(3) times its samples'
# Lipschitz, so 0.577 is the ceiling and asking for 0.8 gets False.
# `cell` is four cells across the layer's smallest feature, clamped to
# [E/512, E/32] of its longest side. The DECLARED Lipschitz is deliberately not
# the source: it bounds a marcher's STEP, not |grad f|, so a rate derived from
# it is unsound exactly on the shapes that motivate a bake.
# NOT optimal, NOT stable across edits, NOT a memory bound (cost['megabytes']
# is), and NOT cheap — a full sampling pass, so not a per-frame call.

layer.consolidation_cost(cell=0.03, band=0.12)   # what it WOULD cost, no change
# -> {'megabytes', 'brick_count', 'sample_count', 'cell_size', 'band',
#     'sample_lipschitz', 'lipschitz', 'safe_step_scale', 'bounds'}
layer.consolidate(cell=0.03, band=0.12)          # ONE undo step, same report
layer.consolidation_state                        # the same dict, or None

# BAKING ALONE DOES NOT BOUND THE LIPSCHITZ. Steepness is a property of the
# FIELD; resampling it onto a lattice reproduces it, and a finer cell makes it
# worse. What removes it is redistancing — replacing the samples with the
# distance to their own zero set — which `consolidate` does by default.
layer.consolidate(cell=0.03, redistance=False)   # measure the difference
layer.consolidate(cell=0.03, region=((-1, -1, -1), (1, 1, 1)))  # pin the box
#     when consolidating the same area repeatedly: a volume's geometric bound
#     is its whole sampled box, so each bake would pad the previous padding.

# The scope is a LAYER, because an arbitrary run of siblings has no field of its
# own — an edit list is ordered and its operators relative — where a layer does.
# What survives is the surface at `cell`; what does not is every parameter of
# every item absorbed. Hidden items are left alone. Protected layers refuse.
# Undo hands the parametric form back with ids and parameters intact.

# curves: a stroke point carries a type saying how it joins the next, so a
# stroke is a curve whose points are all hard corners. Typed points tessellate
# into the same segment chain at compile time, so a curve costs nothing to
# evaluate and no backend knows it exists. Handles are LOCAL space.
body.add(clay.Stroke(points=pts,                       # (N,4) x,y,z,radius
                     types="spline",                   # or one per point
                     closed=True, tolerance=0.005))    # tolerance is a
                                                       # document property
body.add(clay.Stroke(points=pts, types="bezier",
                     in_handles=handles, out_handles=handles))
body.set_points(node_id, pts, types="hard")            # undoable whole-list edit
                                                       # also reshapes a placed
                                                       # Swept's guide, which is
                                                       # the same point list —
                                                       # but never closed, and
                                                       # never under two points

# the cut tool: a shape drawn over the model, in WORLD units on the frame the
# viewport already has. The cut is a PRISM, not a frustum — a converging cut
# would have a non-flat face and depend on where the camera stood. Which side
# survives is the OP, not a flag.
doc.add_sdf_layer("body").add(
    clay.Cut(origin=(0, 0, -4), right=(1, 0, 0), up=(0, 1, 0), forward=(0, 0, 1),
             shape=clay.CutShape.circle(0.4),   # or .rect / .polygon / .curve
             region=doc,                        # sizes the sweep to cut through
             rounding=0.05),                    # bevelled cut walls
    op=clay.Op.SUBTRACT)                        # INTERSECT keeps only the inside
clay.CutShape.curve(control_pts, types="spline")  # a spline lasso

# protection: ghost is "show me this but stay out of my way" (still evaluated,
# never picked, never edited); lock is "this is finished" (still picked).
# Neither changes what the document evaluates to, and an edit to a protected
# layer raises rather than being silently dropped.
doc.set_layer_protection(reference.id, ghost=True)
doc.set_layer_protection(body.id, locked=True)
doc.layer_protection(body.id)              # -> (ghost, locked)

blocks.rasterize(doc)                      # SDF -> voxels
vox_mesh = blocks.mesh()                   # greedy meshing

# mesher selection (dual contouring is experimental, per the meshing spec)
preview = doc.mesh(resolution=128, mesher="nets")
sharp = doc.mesh(resolution=128, mesher="dual_contouring", experimental=True)

# quads: a lattice grid, NOT retopology. The target is approached, not hit.
quads = doc.mesh_quads(target=20000)       # -> Mesh with (Q, 4) quads
quads.quad_report                          # what it actually produced
boxes = blocks.mesh_quads(mode="faces")    # one quad per exposed voxel face
quads.save("out.obj")                      # OBJ/PLY/FBX carry quads; GLB does not

# picking, scalar and batch
hit = doc.raycast((0, 0, -5), (0, 0, 1))   # position, normal, layer, item
hits = doc.raycast_many(rays)              # (N,6) float32 -> arrays
snapped = doc.snap_to_surface(points)      # (N,3) -> positions + normals
cell = blocks.raycast((5, 0.75, 0.75), (-1, 0, 0))   # cell, face, adjacent
lo, hi = body.selection_bounds([node_id])  # tight bounds for zoom-to-selection
```

```python
# arrays — grid and radial repetition, chainable like deformers
body.add(clay.Sphere(r=0.2).repeat_grid(spacing=1.0, counts=(2, 0, 0)))  # finite
body.add(clay.Box(size=(0.3, 0.8, 0.3)).twist(0.9).repeat_radial(count=5, offset=1.2))
body.add(clay.Sphere(r=0.3).repeat_grid(spacing=2.0))    # infinite: never culled
```

Repetition preserves exactness only while the item plus its rounding and
blend influence fits inside its half-cell (docs/01 2.4); the compiler checks
that and downgrades the tracked field to a bound when it does not.

```python
# profile-driven modelling: extrude and revolve exact 2D profiles
outline = np.array([[-1, -1], [1, -1], [1, 0], [0, 0], [0, 1], [-1, 1]], np.float32)
body.add(clay.Extrude(clay.Profile.polygon(outline), half_depth=0.3))
body.add(clay.Revolve(clay.Profile.circle(0.3), offset=1.1))   # == a torus
# built-in profiles: circle, box, hexagon, triangle, trapezoid, vesica, polygon
```

Curved outlines reach documents by flattening to a polygon host-side
(`math/bezier.h` converts cubics to quadratic chains); open curves are
unsigned distances rather than regions, so they are not profiles.

```python
# deformers — chainable, applied in call order
body.add(clay.Box(size=(0.4, 0.4, 0.4)).twist(1.2), op=clay.Op.SUBTRACT)
body.add(clay.Cylinder(r=0.6, h=1.0).taper(-1.0, 1.0, 1.0, 0.35))
body.add(clay.Sphere(r=1.0).displace(amplitude=0.08, frequency=6.0))

# wrap_around bends a flat interval around a cylinder about Z — a relief or a
# line of text around a column. The interval fixes the radius: r = (x1-x0)/2pi.
body.add(clay.Box(size=(6.283, 0.3, 1.0)).wrap_around(-3.1416, 3.1416))

# elongate inserts flat sections without distorting the ends — a sphere becomes
# a capsule. Alone among the deformers it is exact (on an origin-symmetric
# primitive), so it costs nothing in step scale.
body.add(clay.Sphere(r=0.5).elongate((1.0, 0.0, 0.0)))

# the ramped bends displace by an amount that eases across a region: tilt the
# top of a form, or slump the rim of a disc, without touching the rest
body.add(clay.Box(size=(0.6, 2.0, 0.6))
         .bend_linear(a=(0, -1, 0), b=(0, 1, 0), v=(1.0, 0, 0), ease=3))
body.add(clay.Cylinder(r=1.2, h=0.15).bend_radial(r0=0.2, r1=1.2, dz=0.6))

# elongate_axis is the companion to elongate: it stretches any primitive,
# symmetric or not, at the cost of a flat interior plateau — so it is a bound
# where elongate would be exact.
body.add(clay.Cone(h=0.6, r1=0.5, r2=0.1).elongate_axis((0.8, 0.0, 0.0)))

# grab and pose are the region deformers: unlike every other one they have
# FINITE SUPPORT, so outside the radius the field is untouched and per-brick
# culling still holds. That is what lets them act like a sculpting brush.
# front_only stops the far side of a form travelling with the near side.
body.add(clay.Sphere(r=1.0).grab(center=(1, 0, 0), radius=0.8,
                                 displacement=(0.5, 0, 0), front_only=True))
body.add(clay.Cylinder(r=0.3, h=1.0).pose(center=(0, 0.8, 0), radius=1.0,
                                          axis=(0, 0, 1), angle=0.7))

# pose_line ramps the rotation ALONG a segment instead of radially, which is how
# a limb tapers: the anchor stays put and the rotation grows toward the tip. It
# is a bend rather than a rigid swing, and unlike grab it has no finite support
# — everything past b turns with the tip.
body.add(clay.Capsule(a=(0, -1, 0), b=(0, 1, 0), r=0.25).pose_line(
    a=(0, -1, 0), b=(0, 1, 0), axis=(0, 0, 1), angle=0.8))

# the voxel side moves occupancy through the same map. Occupancy is binary, so
# the resample is nearest-cell and rounds PER AXIS: accumulate a drag in the
# host until it clears half a cell (blocks.voxel_size) or the call is a legal
# no-op. blocks.change_count, read either side, says which it was.
blocks.sculpt_grab((0, 0, 0), 15, displacement=(0.3, 0.0, 0.0), shape="sphere")
```

```python
# spatial morphs — combine modes, not point warps: they blend TWO fields
body.add(clay.Box(size=(1.2, 1.2, 1.2)), op=clay.Op.TRANSITION_LINEAR,
         transition=clay.TransitionLinear(a=(0, -2, 0), b=(0, 2, 0), ease=1))
body.add(clay.Torus(R=1.0, r=0.3), op=clay.Op.TRANSITION_RADIAL,
         transition=clay.TransitionRadial(r0=0.5, r1=2.0))
```

Transitions are deliberately NON-LOCAL: their weight reaches arbitrarily far
from both operands, so such items report infinite influence and are never
culled. Only `wrap_around` remains header-only (its inverse-mapping
semantics need their own design pass); the Python call raises rather than
silently doing nothing.

Use cases the bindings are designed for: authoring the spec's golden-scene test corpus, procedural/batch asset generation, ML dataset generation (SDF samples, mesh/CSG pairs — the PrimFusion-style direction), Blender/Houdini scripting, and quick math experiments against the same kernels the app ships.

## 11. C ABI (`bindings/c/clay.h`)

Flat, stable, versioned C API over documents, evaluation, meshing, and I/O — the boundary the Swift app links against (alongside or instead of direct Swift-C++ interop), and the FFI story for C#/Rust/anything. Opaque handles, error codes, caller-owned buffers; no C++ types cross it.

Authoring reaches what `pyclay` reaches: an opaque **item builder** (`clay_item_create` → setters → `clay_layer_add_item`) carries the chained modifiers and variable-length payloads no fixed struct can express, and the flat `clay_item_desc` path is sugar over it. Every descriptor struct starts with a `uint32_t struct_size` the caller sets and the library reads only up to, so fields are appended without a major bump; setting it is mandatory, and a value that is not a declared layout is rejected rather than misread.

**Groups are reachable too**: `clay_layer_add_group` creates one and returns a node id like any other node, `clay_add_item_in_group` / `clay_layer_add_item_in_group` are the two add paths with somewhere to put the edit (the originals append to the layer root and have no argument that could say otherwise), and `clay_layer_children` enumerates a group by the size-query pattern — which doubles as the "is this a group?" question a host that reloaded a document has no other way to ask. `CLAY_OP_INLINE` is declared because the value appears in saved documents, and is accepted on a group and refused on an item.

**A loaded document is discoverable** (#69): `clay_document_layer_count` / `clay_document_layer_at` enumerate the layers in **stack order** — the order evaluation uses and `clay_document_move_layer` sets — so a host that reopens a document recovers the set and the order in one pass instead of probing ids against `clay_layer_bounds` with a guessed gap constant. `clay_document_layer_info` fills a versioned `clay_layer_info` descriptor (id, representation `CLAY_LAYER_SDF`/`VOXEL`/`MESH`, stack index, visibility, ghost, locked) and `clay_layer_name` returns the layer's name by the size-query pattern. Everything settable about a layer is readable back; before this surface existed, a reloaded document lost its names, treated voxel layers as SDF and — the silent one — could *evaluate* differently, because stack order was unrecoverable. **And renameable** (#92): `clay_document_set_layer_name` is the setter that getter implied — a layer used to be named once, by whichever call created it, so a host's rename lived only in the host and was lost on the next save, which `clay_layer_name` turned from invisible into a reopened document confidently showing the stale name. It is a command like every other layer edit, so a rename is one undo step and a ghosted or locked layer refuses it. Names are not unique — the create calls never required it — so `clay_document_voxel_layer` and `clay_document_mesh_layer`, which look a layer up *by name*, answer with the first layer in stack order carrying it: hold the id when a lookup has to survive a rename. **And an id is now something a host can spend** (#365): `clay_document_voxel_layer_by_id` and `clay_document_mesh_layer_by_id` reach a layer's grid or its geometry from the id alone. Until 0.57.0 that advice could not be followed — the name was the ONLY route back to a payload, so two layers sharing one name shadowed each other *silently*, the lookup succeeding on the first of them, and a host's only defence was to forbid duplicate names on voxel layers: a uniqueness rule the ABI asks for nowhere else. The by-id pair refuses on the same terms as the by-name pair — `CLAY_ERROR_NOT_FOUND` for an id no layer carries, for a layer of another representation, and for a layer whose payload is not held beside the document — and it resolves the id in the *document* rather than in the payloads, because a grid deliberately outlives its layer across an undo and reaching one that way would be a new hole. Its out-parameter is required, unlike the by-name form's: the caller already supplied the id, so a NULL asks nothing.

**So are the nodes inside one** (#91): `clay_layer_node_count` / `clay_layer_node_at` enumerate a layer's **top-level** nodes in evaluation order, the node-level sibling of the pair above. `clay_layer_children` answers for a *group*, and a layer's root is not a group — it has no node id at all — so before this the only way to find, say, the armature `clay_layer_stroke_points` had just been taught to read was to probe node ids from 1 upward and tolerate a run of misses; ids are not dense, nothing bounds a gap, and a node placed after a long run of removed ones was simply invisible. Top level only is deliberate: the whole tree is walked by pairing the two calls — enumerate the roots, ask `clay_layer_node_prim` what each one is, recurse with `clay_layer_children` through the ones it refuses as groups — which keeps the nesting a host needs to redraw an outliner. A voxel or mesh layer holds no nodes and counts 0 rather than failing.

**And what a node HOLDS reads back** (#317): `clay_layer_node_transform`, `clay_layer_node_params` and `clay_layer_node_op_blend` are the reading half of the four setters that write a placed node — the surface a host otherwise reimplements in a side-car file beside the `.clay`, keyed by node id, and has to keep correct across undo, redo and reload on its own. Each takes what its setter takes, every out-pointer is optional, and a call passing none of them still validates the layer and the node, which is how a host asks whether an id is still a node of that layer. The transform reads a quaternion back as an axis and angle, so what comes out is a *canonical representative* of the rotation — angle in `[0, pi]`, axis flipped where that is what naming it inside the range takes, and never zero, because `clay_layer_set_transform` refuses a zero axis and a reader its own setter rejects would not be a round trip. **A NULL axis is refused for the same reason** (#327): every C transform takes the WHOLE value — position, axis, angle and scale — so there is no argument meaning "leave this one alone", and NULL read as "no rotation" would have to discard the position beside it. Say it the way the readback answers it: any non-zero axis with an angle of 0. The refusal names *which* array was missing rather than reporting the pair, since a host that reads the axis as optional otherwise learns only that one of two was null. The parameter count is the *current* primitive's arity, so replacing a primitive changes the count as well as the values, and the kinds whose payload is out of line (stroke, armature, sampled volume) count 0 rather than refusing — their payload is read by the typed reader that applies. A group is refused by the first two for the reason its own setters refuse it, and answered by the third, because a group carries an op and a blend like any other node. Colour is the one setter still without a reader.

**And an item can be scaled PER AXIS** (#320, ABI 0.54.0): `clay_layer_set_transform_nonuniform` and `clay_item_set_scale_nonuniform` take a `float scale[3]`, `clay_layer_node_transform_nonuniform` reads it back, and `clay_mesh_transform_nonuniform` is the mesh-side sibling. Every transform in the interface took ONE factor before this, and the shapes a hard-surface boolean workflow cuts with are mostly not uniform — a slot is a squashed capsule, an oval bolt hole a squashed cylinder, a chamfer a box stretched on one axis. The primitives that carry their own extents could say it at creation and never afterwards; the ones that do not (a capsule, a cylinder, a torus) could not say it at all, so an artist who placed a cylinder could not make it an oval without deleting it and losing where it stood.

**The cost is not the one it looks like.** A non-uniform scale is not a similarity, so the field stops being a true distance — the engine evaluates at `p / s` and multiplies back by the *smallest* component, which never overestimates. But that can only shorten a distance, so the field stays 1-Lipschitz: the Lipschitz bound and `clay_layer_safe_step_scale` are **unchanged** and nothing gets slower. What goes is `clay_tape_info`'s `out_is_exact` — the value becomes a bound on the distance rather than the distance, short by at most the ratio of the largest axis to the smallest. A uniform value, the default `(1, 1, 1)` included, keeps the field exact and compiles to identical tape. The operator was already in the kernel (`cscale_nu_point` / `cscale_nu_dist`) and already classified (`cfi_scale_nonuniform`); this change is the plumbing, and it needed no new tape opcode because the primitive record is already `[inv affine 12][scale][rounding]` — the matrix takes `S^-1` and the scale slot takes `min(s)`.

Two consequences worth knowing at the call. Both C transform setters write the WHOLE transform, so `clay_layer_set_transform` means "this node's scale is uniform s" and **collapses** a per-axis one; and `clay_layer_node_transform`, the single-factor reader from 0.53.0, **refuses** a node carrying a non-uniform scale rather than reporting one of the three — the same rule #317 was filed over, that a reader which cannot express what is there must not answer. pyclay needs neither: `scale=` takes one number or three in the same argument, and its partial updates leave a squash alone unless you name it. A LAYER's scale stayed uniform at 0.54.0 — `layer.xform * node.xform` is consumed as a rigid frame by the move brush and the lattice gizmo, and what those should do with a per-axis one was deferred as its own question. **#373 answered it in 0.78.0**: `Layer::scale_axes` composes innermost in the layer's own frame, `brush::lattice_gizmo` refuses rather than approximating on a squashed layer, and the scene and `.clayspace` formats move to minor 16. See [Scaling a subtool per axis](#scaling-a-subtool-per-axis).

**The incremental path is reachable too** (`clay_brick_cache_*`, ABI minor 24): the brick cache of §8 is an opaque handle created from a `clay_brick_config` and never bound to a document, with exactly one refill loop — `mark_dirty` → `take_dirty` → `eval_requests` → `submit` — plus brick readback in the stored fp16 bits at a fixed `dim³` stride, statistics, LOD mips, `mesh_bricks` and `raycast_bricks`. The mip is meshable as well as readable (`clay_brick_cache_mesh_lod`): a coarse brick is the cache's own lattice at twice the spacing, so a level changes the spacing and the meaning of the key list and nothing else — and because an empty mesh already means "no surface bricks", a level nobody built is `CLAY_ERROR_NOT_FOUND` rather than an empty one. It mirrors `brick::BrickCache` member for member and adds no policy: no owned thread, no lock, no refill driver, no ordering or eviction knob, because the consumer owns queues and scheduling. (One batched const query, `clay_brick_cache_raycast_many`, fans its rays out across the engine's shared worker pool — the same pool the CPU backend evaluates points with — and returns only when every ray is done; each slot is byte-identical to the single-ray call's answer, so this is a latency detail, not a policy.) A request is a plain value whose layout *is* `brick::BrickRequest` (asserted with `offsetof`), so a drain is a `memcpy` and a request may be queued, copied and evaluated on any thread. It carries its lattice *and its band*, because the tape must be culled against the brick dilated by the band — a sample keeps its true distance whenever that distance is within the band, so an item a band outside the brick still decides samples inside it, and a band passed separately is a band that will one day arrive as zero. Three supporting calls come with it, none of which existed before: `clay_eval_grid` (dense lattice evaluation with an optional cull region — the per-brick fill primitive), and `clay_layer_node_influence_bound` / `clay_layer_influence_bound`, which report the box an edit must *dirty* (wider than `clay_layer_bounds`, and flagged when unbounded). Undo joins them at ABI minor 40: `clay_document_undo_bound` / `clay_document_redo_bound` are the plain undo and redo plus the influence bound of what the step applied, in the same three-state shape, because the engine holds the list of commands it replayed and a host outside it can only guess — the layer, or a node diff that misses every in-place edit. Marking is validated in 64-bit before the engine converts the region: a span above `CLAY_MAX_BATCH` or a brick coordinate outside `int32` is refused with the cache untouched, since three caller floats can otherwise name 10<sup>19</sup> bricks.

**And the live preview is incremental** (ABI 0.60.0): `clay_sdf_smooth_preview_item` copies the whole working volume, which is the right shape for a host joining mid-gesture and the wrong one for a per-frame loop — a dab moves a ball of bricks and the host re-uploads the model. `clay_sdf_smooth_preview_delta_info` and `clay_sdf_smooth_preview_delta_take` hand over exactly the bricks whose bytes are new: the ones a dab materialized and the ones its relax moved. Ask, size the buffers, take — the caller owns the memory it uploads from, which is the convention `clay_brick_cache_take_dirty` and `clay_voxel_take_dirty_chunks` already set. A short buffer takes **nothing** and reports what it needs, because taking is what clears the delta and a partial drain would strand bricks no later call reports. The delta ACCUMULATES until taken and is deduplicated by brick — a dab materializes a brick and then relaxes it, so the same coordinate arrives twice by construction — so a host that skips a frame loses nothing. `generation` is bumped by every update that changed the preview and by nothing else, which is how a host tells a duplicate read from a skipped frame and drops an upload begun against an older one; taking clears the delta and not the generation, because the generation names the state the caller now holds. `clay_sdf_preview_brick` carries no `struct_size`: it is an array element like `clay_brick_request`, received in hundreds and read rather than filled in.

**And a live field brush is a transaction** (ABI 0.59.0): `clay_sdf_smooth_*` and `clay_sdf_move_*` are two opaque handles with the same four verbs — `begin`, `update`, `commit`, `cancel` — plus a `destroy` that IS a cancel, so an error path which simply drops the handle leaves no half a stroke behind. They exist because two brushes cannot be spelled as edit-list stamps and so had no live form at all. Smooth BAKES (`field::relax` returns a volume, not an edit list), and a host with nowhere to keep that volume between pointer events must bake the layer again per dab — `sdf_consolidate`, 313 ms on the reference iPad — so the only affordable implementation ran once at pointer-up and the artist smoothed blind. Move warps every item it reaches, and written into the document per pointer event a drag churns revisions, tapes, caches and picking sixty times a second to produce one edit. A transaction holds the transient state instead: `clay_sdf_smooth_begin` samples the layer ONCE, each `clay_sdf_smooth_update` relaxes that retained volume in place, and `clay_sdf_smooth_commit` installs the volume the dabs were applied to rather than baking again; `clay_sdf_move_begin` walks the edit list once to prepare the items the drag reaches — through the ball or, under a layer mirror or radial count, through any image of it — each `clay_sdf_move_update` resolves the grabs of every *affected* item (one per image that reaches it) with no traversal at all, and the commit writes every final chain as one step. **Between begin and commit the document does not change** — no nodes, no deformers, no undo entries, and `clay_document_save_memory` taken mid-gesture returns the bytes it returned before it, which is what the ABI tests assert first. Each update fills a `clay_sculpt_dirty`: a conservative world box, the brick count, and `changed`, with the box and the count *geometric* (what the brush selected, not what happened to move) so they are reproducible however much unrelated model surrounds the brush, and `has_bounds` carrying emptiness as the third state the rest of this ABI already uses. Move's update takes the TOTAL displacement from the anchor, never an increment — 0.1, 0.2, 0.5 must end where a single fresh 0.5 lands, not past it — and `clay_sdf_move_preview_grab_count` / `clay_sdf_move_preview_grab` report the resolved grabs as the parameters `clay_item_add_deformer(CLAY_DEFORM_GRAB, ...)` already takes — one grab without symmetry, one per reaching image with it — so a host draws the preview through machinery it has. A commit **refuses a source that moved underneath it**: the layer is fingerprinted at begin and re-checked at commit, and an external edit stands rather than being written over by a preview computed against a document that no longer exists. `clay_sculpt_policy` carries the sampling (`clay_consolidation_params`' three numbers, with its meanings) and the budget — `min_safe_step_scale`, `max_deformer_chain`, `max_item_count`, where zero disables a criterion so an all-zero policy authorises nothing — plus `allow_consolidation`, which is the whole opt-in for the destructive half: over budget without it fills `clay_sculpt_budget.over_budget` and changes nothing, and with it the collapse happens inside the stroke's own undo step so one undo puts back both. The check runs only between completed strokes, never while the pointer is down.

**And a deep layer's history has a cache a host can hold** (ABI 0.79.0):
`clay_sdf_prefix_cache_*`. A Smooth gesture on a layer with a long edit list pays
the whole surviving history on every window the artist has not touched yet — 242
ms a dab at 20,000 items — because a window with no seed walks everything that
reaches it. `session::SdfPrefixCache` has folded that history at a boundary since
#371 and made the same dab 2.32 ms; what shipped with it was reachable from C++
and from no host, because `clay_sdf_smooth_begin` let the cache argument default
to null. The handle closes that. It follows `clay_brick_cache`'s ownership rule to
the word — **it belongs to whoever made it, never to a document**, takes no
document, destroys with `void`, and outlives any document it was built against —
because a host should not learn two lifetimes for two caches. Nothing in it is
authoritative: every entry is rebuildable, nothing is serialized, and dropping the
lot changes no output and only the next cold window's latency.

The recipe is four calls and the order matters:

1. `clay_sdf_prefix_boundary_for` — is this layer deep enough to be worth
   caching? It builds and evaluates nothing, so asking is free.
2. `clay_sdf_prefix_cache_build` — **between gestures**, never on the
   pointer-down path. It is the whole layer's cost (2,170 ms at 20,000 items) and
   it is a separate call for exactly that reason: `SdfSourceField::open`
   deliberately uses a prefix and never makes one.
3. `clay_sdf_smooth_begin_cached` — the same begin, plus the cache to look in. A
   null cache, or one holding nothing for this layer, is exactly
   `clay_sdf_smooth_begin`, and neither builds anything.
4. `clay_sdf_prefix_cache_stats` — `seeded_windows` against `fallback_windows` is
   how a host tells "the cache served this gesture" from "this window happened to
   be warm". The gesture's OUTPUT is identical either way by design, so nothing in
   the result can say.

The sampling is deliberately NOT settable twice. A prefix is keyed on its
resolution, so one built at a cell size the gesture does not use is a miss — no
error, no wrong answer, no acceleration — which is the one failure shape that
looks like the feature not working. So the three cache knobs live on
`clay_sculpt_policy` beside the one `cell_size` the gesture already has, and there
is nowhere to put a second. `docs/09` carries the break-even: **under ten cold
windows at every size measured**, and the build follows the history while the
cache's SIZE follows the model — 1.43 MiB at 5,000 items and at 20,000 alike — so
a budget is sized from one and a build scheduled from the other.

**And the voxel side has one now too** (#86): `clay_voxel_mesh` is the *whole* grid, always, and it costs every occupied chunk on every call — so displaying a sculpt used to cost the model while editing it cost the dab. Two calls close that, in the brick cache's own vocabulary: `clay_voxel_take_dirty_chunks` drains the chunks a mutation could have changed (capacity in, count out, remainder reported — the `clay_brick_cache_take_dirty` shape, not a size query), and `clay_voxel_mesh_chunks` meshes exactly those keys and reports a `clay_voxel_chunk_mesh_range` per key so a host patches GPU sub-ranges instead of rebuilding the buffer. Every mutation feeds the set through the one cell-write choke point, a write that changes nothing dirties nothing, a write on a chunk *face* also dirties the chunk across it — the exposure test reads that cell — and a chunk emptied to nothing is dropped from the grid and *still* reported, because that is the key whose quads a host must remove. Unlike `clay_brick_mesh_range`, these ranges **partition** the mesh with no shared vertex: a voxel face belongs to one cell in one chunk, so clamping the greedy merge to a chunk boundary emits more, smaller quads over the identical surface — never a crack, and no straddler to attribute the way `mesh_bricks` must (#66). The cost is triangle count at chunk seams (+3.7% measured on a realistic sculpt), which is why `clay_voxel_mesh` stays whole-grid for export. Measured on Linux desktop at the device gate's 0.02 cell size — *not* comparable to the device baseline, per `docs/RELEASE.md`; the ratio transfers, the absolute does not — one dab dirties ~2 of 81 occupied chunks and re-meshes in **0.65 ms against 23.3 ms** for the whole grid.

## 12. Dependencies (all permissive, all C/C++)

| Dep | Role | License |
|---|---|---|
| ufbx | FBX import | MIT |
| meshoptimizer | decimation, mesh optimization | MIT |
| metal-cpp | Metal host (Apple platforms) | Apache-2.0 |
| nanobind | Python bindings | BSD-3 |
| xsimd (or hand SIMD) | CPU vectorization | BSD-3 |
| cgltf / tinyply (optional) | glTF/PLY | MIT |
| doctest or Catch2, benchmark | tests/bench | MIT/BSL/Apache |

assimp is kept out of the shipping library (heavy, licensing surface) but used **in CI** as an independent validator of exported files. No Boost, no exceptions-across-ABI, no GPL/LGPL.

## 13. Build, packaging, testing

- **CMake** presets: `cpu-only` (any platform), `+metal` (Apple), `+cuda`, `+opencl`; warnings-as-errors, sanitizers in CI.
- **SwiftPM wrapper** target so the Xcode app consumes claycore as a package (prebuilt xcframework or source).
- **Wheels** via scikit-build-core + cibuildwheel.
- **clay-cli**: `clay mesh in.clayspace --res 512 -o out.fbx`, `clay validate out.fbx`, `clay eval --points pts.npy` — CI's workhorse and a user-facing converter.
- **Test pyramid**: kernel unit tests vs. reference values from docs/01 → property tests (Lipschitz bounds hold, blends rigid, locality bit-identity) → backend parity suite → golden-scene meshing tests (watertight/manifold across the op matrix) → I/O round-trip + fuzz → performance benchmarks with regression gates (points/sec, bricks/sec, mesh time on fixed scenes).

## 14. Versioning & compatibility

SemVer on the C ABI and Python API; kernel headers may evolve freely inside a major. Below 1.0 the 0.x rule applies — the ABI may break on a minor bump, and **0.2.0 did**: `clay_item_desc` and `clay_mesh_params` grew the leading `struct_size`, which shifts every other field, so a 0.1.0 binary is rejected with `CLAY_ERROR_INVALID_ARGUMENT` (never silently misread) and consumers recompile. Consumers therefore compare majors at init, and the minor too while the major is 0. Document format version is independent and governed by the `project-documents` spec (backward-open, forward-refuse). GPU backend availability never changes results — only speed — enforced by the parity suite.

## 15. Phasing (maps to OpenSpec changes)

1. **Phase 1 (inside `add-clayspace-v1`)**: kernel headers (primitives, core ops, smooth/chamfer, transforms, repetition, strokes), scene/tape/undo, brick cache, CPU + Metal backends, MC meshing + decimation + validation, OBJ/FBX/PLY + document I/O, C ABI, clay-cli, full test pyramid. This is exactly the app's dependency set.
2. **Phase 2**: pyclay wheels; extended blend vocabulary surfaced in the app; surface nets; dual contouring hardening; glTF writer.
3. **Phase 3**: CUDA backend (pipeline/ML workloads); deformer family exposed in-app; interval-arithmetic tape culling for huge scenes.
4. **Phase 4**: OpenCL (or Vulkan-compute successor) backend; SYCL evaluation if demand appears.

Each post-v1 phase enters as its own OpenSpec change with spec deltas against the capabilities this library serves.
