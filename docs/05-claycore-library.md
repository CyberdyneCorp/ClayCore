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
6. **C++20, no exceptions across the ABI.** Errors as `std::expected`-style results internally, error codes across the C API. Modules of the library are usable freestanding (kernels headers are header-only).
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

## 5. Kernel dialect & GPU backends

The kernel dialect is the subset of C++ that all four targets accept: no virtuals, no exceptions, no allocation, no recursion, `constexpr`-friendly, fixed-size types from `shim.h` (`cfloat3`, `cfloat4x4`, …) that map to `simd::float3` (CPU/Apple), MSL vectors, CUDA vectors, or OpenCL vectors under macros (`CLAY_KERNEL_METAL`, `CLAY_KERNEL_CUDA`, …).

Scenes do not become shader code. The `scene` module compiles an edit list into a **flat postfix tape** (opcodes + parameter blocks, transforms pre-inverted). Every backend ships one fixed **tape interpreter kernel** — no per-edit shader recompiles, instant parameter edits (the mzschwartz5 lesson), and the door open to interval-arithmetic tape shortening (Keeter MPR) as the large-scene upgrade.

| Backend | Host layer | Kernel path | Status/notes |
|---|---|---|---|
| **CPU** | thread pool, batch API | same headers, scalar + SIMD (Apple `simd` / SSE-NEON via `xsimd`) | reference; always available |
| **Metal** | `metal-cpp` (pure C++, no ObjC in core) | headers compiled as MSL; argument buffers for tapes | tier-1: the iPad app |
| **CUDA** | CUDA runtime or NVRTC JIT | same headers under `__device__` | tier-2: desktop/pipeline/ML workloads |
| **OpenCL** | OpenCL 3.0 | kernel headers constrained to the C-compatible subset (macro-mapped to OpenCL C) | tier-3, best-effort; Vulkan compute is the likely long-term replacement and slots into the same backend interface |

Backend interface (`clay::eval::Backend`), identical everywhere:

- `eval_points(tape, points[]) -> distances[] / gradients[] / colors[]` — batch field queries
- `eval_bricks(tape, brick_ids[]) -> narrow-band brick data` — incremental cache fill
- `raycast(tape, rays[]) -> hits[]` — picking/rendering support
- `mesh(tape | bricks, params) -> triangles` — GPU meshing where supported
- capability flags (fp16 storage, meshing on device, max tape length)

Backends are runtime-registered; the CPU backend is compiled in unconditionally. Parity suite runs every registered backend against CPU scalar on every kernel and on composed scenes.

## 6. Scene model & evaluation semantics (`clay::scene`)

The document tree the app and specs already define, owned here so every consumer agrees:

- **Layers** (`voxel` | `sdf`), each with transform, visibility, resolution, material; SDF layers hold the **ordered edit list** — items apply to the combined preceding result; groups (nesting ≥ 4) carry group ops incl. None; layer instancing with shared content.
- **Influence bounds** computed per item/group (AABB ⊕ blend ⊕ rounding), used for brick dirtying, culling, and the locality guarantee (distant edits leave existing bricks bit-identical — regression-tested).
- **Tape compiler**: edit list → flat tape; per-brick tape culling (only edits whose influence bound touches a brick appear in its tape — the Dreams design).
- **Undo command vocabulary**: every mutation is a serializable command with an inverse (add/remove/reorder item, set-param, voxel-span edit, layer ops). The in-memory undo stack and the document file share this one vocabulary — one serialization story, tiny undo steps, stroke-level coalescing.

`clay::brick`: sparse virtual grid of 8³/16³ bricks, fp16 narrow band (±3 voxels), dirty-set tracking, async-friendly (evaluation requests are plain data; the app owns threading/queues via the backend), LOD mip bricks for far view.

## 7. Meshing & mesh processing (`clay::mesh`)

- **Marching cubes** (default): consistent ambiguity resolution (asymptotic decider) → watertight, 2-manifold guarantee; runs over surface-crossing bricks only; CPU version is the golden reference, GPU versions (Metal/CUDA) must match topology-invariants (not bit-identical vertices).
- **Surface nets**: cheap smooth preview meshes.
- **Dual contouring** (QEF + Hermite data): sharp-edge export for the chamfer aesthetic; manifold DC variant. Ships behind a flag; roadmap-hardened after v1.
- **Decimation**: quadric edge collapse via `meshoptimizer`, color-attribute aware, target ratio or error.
- **Validation**: watertight/manifold/degenerate checks, self-intersection sampling — the primitive behind CI's export gates and the app's "guaranteed clean booleans" claim.
- **Attributes**: vertex colors sampled from the color field (blend-gradient faithful), normals (field gradient or face), optional UV box-projection utility.

## 8. File I/O (`clay::io`)

- **Document format** (`.clayspace`): binary chunked container (versioned chunks: scene commands, palettes, voxel grids RLE/palette-compressed, thumbnails PNG, camera bookmarks). Forward-version refusal, backward-compat guaranteed; pure claycore so Python/CI can read and write projects.
- **OBJ + MTL**: custom reader/writer (dependency-free), vertex-color extension documented.
- **FBX**: import via **ufbx** (MIT, single-file, battle-tested); export via minimal binary FBX writer (meshes, transforms, vertex colors, units/axis correct for Unity/Unreal/Blender — validated in CI via assimp/Blender-headless round trips).
- **PLY**: reader/writer with vertex colors (interchange with SDF Modeler/MagicaCSG ecosystems).
- **glTF/GLB**: writer (cgltf or custom) — engine-friendly, wheel-friendly.
- **USDZ**: *not* in claycore (Apple Model I/O owns it in the app shell); claycore exposes the mesh+attribute buffers those APIs consume.
- Import guardrails: triangle budgets, malformed-file fuzzing, no allocation bombs.

## 9. Picking & interaction math (`clay::pick`)

CPU-side, latency-critical, called every Pencil event:

- Ray ↔ scene raycast (analytic tape or brick cache, whichever is fresher) with layer/item hit attribution.
- Surface snapping: closest-point-on-surface (gradient descent on the field), position and position+normal modes.
- Build-plane and grid cell resolution for voxel mode; face picking on voxel grids.
- Bounds/frustum utilities for zoom-to-selection and culling.

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
# strokes: a drag becomes stamps, and a stamp becomes an ordinary edit —
# which is what gives a brush undo, coalescing and serialization for free
brush = clay.StrokePreset(radius=0.15, spacing=0.25, pressure_size=1.0,
                          taper_start=0.1, taper_end=0.1, steady=0.4, seed=7)
samples = np.array([[x, y, z, pressure] for ...], np.float32)   # (N,3/4/5)
brush.resolve(samples)                     # pure: positions/radii/strengths
blocks.apply_stroke(samples, brush, blocks.palette_add("#cc7744"))
body.apply_stroke(samples, brush, clay.Sphere(r=1.0), mask=freeze)  # one undo step
clay.StrokePreset.deserialize(brush.serialize())   # versioned: newer is refused

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

# the voxel side moves occupancy through the same map
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
