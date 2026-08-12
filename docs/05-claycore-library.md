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
│   ├── math/            # host-side geometry: AABB, transforms, quats, ray, frustum
│   ├── scene/           # document model: layers, groups, edit items, tape compiler, undo commands
│   ├── eval/            # backend-agnostic evaluation API + backend registry
│   ├── brick/           # sparse brick cache: narrow band, dirty tracking, per-brick tape culling
│   ├── voxel/           # colored voxel grids: storage (palette+RLE), edits, mirror, greedy meshing
│   ├── mesh/            # marching cubes / surface nets / dual contouring, decimation, validation
│   ├── pick/            # ray picking, surface snapping, closest-point queries
│   └── io/              # document format, OBJ/MTL, FBX (ufbx), PLY, glTF; USD hooks
├── src/                 # CPU implementations, backend hosts
├── backends/
│   ├── cpu/             # scalar reference + SIMD batch + thread-pool dispatch
│   ├── metal/           # metal-cpp host; kernels compiled from include/clay/kernel via MSL
│   ├── cuda/            # NVRTC/nvcc host; same headers
│   └── opencl/          # OpenCL 3.0 host; kernels via C-compatible subset (see §5)
├── bindings/
│   ├── c/               # flat stable C ABI (clay.h) — Swift, C#, anything FFI
│   └── python/          # nanobind module `pyclay`, numpy-native
├── tests/               # unit, parity, property, golden-mesh, fuzz
└── tools/               # clay-cli: eval/mesh/convert/validate from the command line
```

Dependency rule: `kernel` depends on nothing; `scene`/`brick`/`mesh` depend on `kernel`+`math`; backends depend on `eval`; `io` and bindings sit on top. No module depends on a backend.

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
implementation of the step most able to drift.

`Backend::eval_grid_batch_device` is `eval_grid_batch` for that caller-owned
destination — the brick-refill shape, with grid *i* landing at the same fixed
slot the host-memory batch uses. The default loops `eval_grid_device`, so any
adopting backend answers it with identical values; the Metal override runs the
whole batch as **one dispatch** through the same concatenated-tape kernel as
the host-memory batch, which is what `clay_brick_cache_eval_requests_device`
rides: without it the zero-copy path paid a command buffer and a wait per
brick and sat 25-165x behind the host-memory route it exists to beat.

The limit worth stating plainly: this makes evaluation **output**
device-resident, **not** brick **storage**. Generations, staleness,
classification, quantization and the memory budget are host code over host
memory, and that is where a submitted brick becomes a stored brick. A host on
the device path owns the conversion, and the cache's guarantees are not in play.

## 6. Scene model & evaluation semantics (`clay::scene`)

The document tree the app and specs already define, owned here so every consumer agrees:

- **Layers** (`voxel` | `sdf`), each with transform, visibility, resolution, material; SDF layers hold the **ordered edit list** — items apply to the combined preceding result; groups (nesting ≥ 4) carry group ops incl. None; layer instancing with shared content.
- **Influence bounds** computed per item/group (AABB ⊕ blend ⊕ rounding), used for brick dirtying, culling, and the locality guarantee (distant edits leave existing bricks bit-identical — regression-tested).
- **Tape compiler**: edit list → flat tape; per-brick tape culling (only edits whose influence bound touches a brick appear in its tape — the Dreams design).
- **Cull index** (`scene/cull_index.h`): per-revision cache of everything the per-brick cull consults — item geometry bounds (splines tessellated once, not per brick), group influence bounds, the feather cull pad — plus a per-batch **coarse cull plan**: one test of every chain against the union of a batch's brick regions yields survivor lists carrying the cached bounds, so each per-brick compile walks only the items near the batch instead of the whole document. Non-local items always survive; chains holding a feathered volume replace are never pruned (the feathered/hard replace choice reads what the cull dropped from that chain). Pure acceleration: the emitted tapes are byte-identical to the unindexed compile, regression-tested on an adversarial corpus (mirrors, groups, deformers, feathered volumes, non-local layers). The C ABI keys the index on the document revision, exactly as the cached whole-document tape, and `clay_brick_cache_eval_requests` / `_device`, `clay_eval_grid`, `clay_tape_export` and the brick-mesh attribute pass all compile through it.
- **Undo command vocabulary**: every mutation is a serializable command with an inverse (add/remove/reorder item, set-param, voxel-span edit, layer ops). The in-memory undo stack and the document file share this one vocabulary — one serialization story, tiny undo steps, stroke-level coalescing.

`clay::brick`: sparse virtual grid of 8³/16³ bricks, fp16 narrow band (±3 voxels), dirty-set tracking, async-friendly (evaluation requests are plain data; the app owns threading/queues via the backend), LOD mip bricks for far view.

## 7. Meshing & mesh processing (`clay::mesh`)

- **Marching cubes** (default): consistent ambiguity resolution (asymptotic decider) → watertight, 2-manifold guarantee; runs over surface-crossing bricks only; CPU version is the golden reference, GPU versions (Metal/CUDA) must match topology-invariants (not bit-identical vertices).
- **Surface nets**: cheap smooth preview meshes.
- **Dual contouring** (QEF + Hermite data): sharp-edge export for the chamfer aesthetic; manifold DC variant. Ships behind a flag; roadmap-hardened after v1.
- **Decimation**: quadric edge collapse via `meshoptimizer`, color-attribute aware, target ratio or error.
- **Validation**: watertight/manifold/degenerate checks, self-intersection sampling — the primitive behind CI's export gates and the app's "guaranteed clean booleans" claim.
- **Attributes**: vertex colors sampled from the color field (blend-gradient faithful), normals (field gradient or face), optional UV box-projection utility. For a brick mesh the gradient/color taps go through per-brick culled tapes — one tape per involved brick, shared by normals and colors — and are evaluated as ONE flattened batch on the CPU backend's thread pool (`eval_points_batch`), so a dense re-mesh costs about what refilling the same bricks does instead of one core's worth of serial taps; the results are byte-identical to the serial evaluation.

## 8. File I/O (`clay::io`)

- **Document format** (`.clayspace`): binary chunked container (versioned chunks: scene commands, palettes, voxel grids RLE/palette-compressed, thumbnails PNG, camera bookmarks). Forward-version refusal, backward-compat guaranteed; pure claycore so Python/CI can read and write projects.
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

- Ray ↔ scene raycast (analytic tape or brick cache, whichever is fresher) with layer/item hit attribution.
- Surface snapping: closest-point-on-surface (gradient descent on the field), position and position+normal modes.
- Build-plane and grid cell resolution for voxel mode; face picking on voxel grids.
- Bounds/frustum utilities for zoom-to-selection and culling.

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
   (Since this baseline, `consolidate`'s bake batches its lattice samples
   through the CPU backend's thread pool — ~6x faster at these sizes on an
   M2 Max, byte-identical output. It still scales with the document and still
   deserves the progress UI; re-measure on device before relaxing either.)

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
nodes — the same x, y, z, radius list — and `clay_layer_armature_parents` for
its topology, mirroring the split on the setter side, so a reloaded rig can be
re-posed through `clay_layer_armature_edit` with indices the host actually
holds. `clay_layer_node_prim` reports which primitive a placed node carries, so
a host finds the armature (or the curve) by asking rather than by probing
readers until one stops refusing.

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
```

A **group** is a node whose children compile as one sub-expression, so an op
inside it reaches its own subtree and nothing else — which is what makes
"intersect A with B, then union that into C" sayable at all. Without it a plate
needs a layer of its own; see `examples/36_groups.py`.

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
# It SAMPLES rather than editing a volume, because flatten moves the surface by
# many band widths and a band cannot follow one that walks out of it. Sample
# from the DOCUMENT where you can: a volume reports a bound, not a distance,
# outside its own band.
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

**A loaded document is discoverable** (#69): `clay_document_layer_count` / `clay_document_layer_at` enumerate the layers in **stack order** — the order evaluation uses and `clay_document_move_layer` sets — so a host that reopens a document recovers the set and the order in one pass instead of probing ids against `clay_layer_bounds` with a guessed gap constant. `clay_document_layer_info` fills a versioned `clay_layer_info` descriptor (id, representation `CLAY_LAYER_SDF`/`VOXEL`/`MESH`, stack index, visibility, ghost, locked) and `clay_layer_name` returns the layer's name by the size-query pattern. Everything settable about a layer is readable back; before this surface existed, a reloaded document lost its names, treated voxel layers as SDF and — the silent one — could *evaluate* differently, because stack order was unrecoverable.

**The incremental path is reachable too** (`clay_brick_cache_*`, ABI minor 24): the brick cache of §8 is an opaque handle created from a `clay_brick_config` and never bound to a document, with exactly one refill loop — `mark_dirty` → `take_dirty` → `eval_requests` → `submit` — plus brick readback in the stored fp16 bits at a fixed `dim³` stride, statistics, LOD mips, `mesh_bricks` and `raycast_bricks`. The mip is meshable as well as readable (`clay_brick_cache_mesh_lod`): a coarse brick is the cache's own lattice at twice the spacing, so a level changes the spacing and the meaning of the key list and nothing else — and because an empty mesh already means "no surface bricks", a level nobody built is `CLAY_ERROR_NOT_FOUND` rather than an empty one. It mirrors `brick::BrickCache` member for member and adds no policy: no owned thread, no lock, no refill driver, no ordering or eviction knob, because the consumer owns queues and scheduling. (One batched const query, `clay_brick_cache_raycast_many`, fans its rays out across the engine's shared worker pool — the same pool the CPU backend evaluates points with — and returns only when every ray is done; each slot is byte-identical to the single-ray call's answer, so this is a latency detail, not a policy.) A request is a plain value whose layout *is* `brick::BrickRequest` (asserted with `offsetof`), so a drain is a `memcpy` and a request may be queued, copied and evaluated on any thread. It carries its lattice *and its band*, because the tape must be culled against the brick dilated by the band — a sample keeps its true distance whenever that distance is within the band, so an item a band outside the brick still decides samples inside it, and a band passed separately is a band that will one day arrive as zero. Three supporting calls come with it, none of which existed before: `clay_eval_grid` (dense lattice evaluation with an optional cull region — the per-brick fill primitive), and `clay_layer_node_influence_bound` / `clay_layer_influence_bound`, which report the box an edit must *dirty* (wider than `clay_layer_bounds`, and flagged when unbounded). Marking is validated in 64-bit before the engine converts the region: a span above `CLAY_MAX_BATCH` or a brick coordinate outside `int32` is refused with the cache untouched, since three caller floats can otherwise name 10<sup>19</sup> bricks.

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
