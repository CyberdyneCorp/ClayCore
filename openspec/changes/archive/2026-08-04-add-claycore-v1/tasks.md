# Tasks: add-claycore-v1

Groups 1–10 are Phase 1 (the ClaySpace app's dependency set). Groups 11–14 map to Phases 2–4 (see design.md §Phasing).

## 1. Repository scaffolding & build

- [x] 1.1 Create repo layout (`include/clay/{kernel,math,scene,eval,brick,voxel,mesh,pick,io}`, `src/`, `backends/`, `bindings/`, `tests/`, `tools/`) with CMake presets `cpu-only`/`+metal`/`+cuda`/`+opencl`, warnings-as-errors
- [x] 1.2 Vendor/fetch permissive deps (ufbx, meshoptimizer, xsimd, doctest/Catch2, benchmark) and add the CI license-manifest gate
- [x] 1.3 CI matrix: macOS/Linux/Windows `cpu-only` build + test; ASan/UBSan jobs; module-layering include check; `openspec validate --all --strict` job
- [x] 1.4 Kernel-dialect enforcement check (compile kernel headers against the most restrictive target profile)

## 2. Kernel headers (sdf-kernels)

- [x] 2.1 `shim.h`: fixed-size types (`cfloat3`, `cfloat4x4`, …) and qualifier macros for CPU/Metal/CUDA/OpenCL
- [x] 2.2 `prim3d.h`: all exact 3D primitives + bound primitives with flags; unit tests vs docs/01 reference values
- [x] 2.3 `prim2d.h`: 2D profiles incl. exact polygon and quadratic Bézier; cubic-by-subdivision
- [x] 2.4 `ops.h`: hard booleans; quadratic/cubic/circular smins + chamfer with material-mix `h`; blend-rigidity property tests
- [x] 2.5 `xform.h` + `repeat.h`: transforms, uniform/non-uniform scale, elongate, mirror + Mirror Blend, round/onion; infinite/finite/radial repetition with clamped-cell tests
- [x] 2.6 `lift.h` + `ease.h`: extrude/revolve (exact), extrude-to/loft (bound); easing-curve library (≥30 curves)
- [x] 2.7 `deform.h`: twist/bend/taper/displace, bend_linear/radial, wrap_around, transitions — Lipschitz factors + easing params
- [x] 2.8 Exactness/Lipschitz propagation through the tree + safe-step-scale API; property tests (bounds hold on random compositions)
- [x] 2.9 Stroke item (capsule/round-cone chain, per-point radius/color) + stamp placement + mirror application
- [x] 2.10 `field.h`: tetrahedron normals, sphere tracing (over-relaxation, pixel-proportional eps), AO, Aaltonen soft shadow, raycast refinement

## 3. Host math & scene model (scene-model)

- [x] 3.1 `math/`: AABB, transforms, quaternions, ray, frustum (host-side)
- [x] 3.2 Document model: layers (voxel|sdf), ordered edit lists, groups (≥4 deep, group ops incl. None), instancing, visibility/selection state
- [x] 3.3 Influence bounds per item/group (AABB ⊕ blend ⊕ rounding) + conservativeness property test
- [x] 3.4 Tape compiler: edit list → flat postfix tape (pre-inverted transforms, param blocks); tape-vs-tree equivalence tests
- [x] 3.5 Per-brick tape culling + culled-vs-full bit-identity test
- [x] 3.6 Undo command vocabulary (serializable, invertible, stroke-coalescing) + inverse round-trip tests

## 4. Evaluation backends — CPU & Metal (evaluation-backends)

- [x] 4.1 `clay::eval::Backend` interface (eval_points/eval_bricks/raycast/mesh/capabilities) + runtime registry
- [x] 4.2 CPU scalar reference interpreter (always compiled in)
- [x] 4.3 CPU batch path (block chunks, thread-pool dispatch, compiler-vectorized loops) + batch-vs-scalar parity (1e-6); hand-tuned xsimd lanes deferred to the 10.6 benchmark work
- [x] 4.4 Parity suite harness: per-kernel + composed-scene comparison vs CPU scalar with per-kernel tolerances, CI-gated
- [x] 4.5 Metal backend host (metal-cpp), MSL compilation of kernel headers, argument-buffer tapes; eval_points/eval_bricks/raycast
- [x] 4.6 Metal parity pass on macOS CI (and device smoke test via the app repo)

## 5. Brick cache (brick-cache)

- [x] 5.1 Sparse brick storage (8³/16³, fp16 ±3-voxel band, implicit inside/outside)
- [x] 5.2 Dirty tracking from influence bounds; incremental re-eval; generation counters for stale-result rejection
- [x] 5.3 Locality regression test: distant edit ⇒ bit-identical untouched bricks
- [x] 5.4 LOD mip bricks + consistency test
- [x] 5.5 Memory budget enforcement (query, budget-exceeded results) + mobile-ceiling tests

## 6. Voxel engine (voxel-engine)

- [x] 6.1 Palette-indexed chunked storage to 256³+ with palette+RLE serialization
- [x] 6.2 Edit ops: set/erase/paint (single + N³ footprint), box/line fills, mirror application, build-plane queries, flood select
- [x] 6.3 Greedy meshing with per-face color + losslessness test
- [x] 6.4 Voxel↔SDF bridges (step-function field; SDF rasterization to voxels)

## 7. Meshing (meshing)

- [x] 7.1 Default mesher over surface-crossing bricks: marching tetrahedra (consistent Freudenthal decomposition — watertight/2-manifold by construction, per amended meshing spec); watertight/manifold validation suite
- [x] 7.2 Vertex attributes: colors from color field (blend-faithful), gradient/face normals, box-projection UV utility
- [x] 7.3 Decimation via meshoptimizer (ratio/error targets, color-aware) 
- [x] 7.4 Mesh validation module (watertight, manifold, degenerates, sampled self-intersection) — the CI export gate
- [x] 7.5 Golden-scene meshing gates across the op × blend matrix
- [x] 7.6 Backend::mesh() hybrid GPU meshing on Metal (device field eval + host triangulation) with topology-invariant parity vs CPU; full on-device triangulation left as a perf follow-up

## 8. Picking (picking)

- [x] 8.1 Ray ↔ scene raycast (tape + brick paths) with layer/item attribution
- [x] 8.2 Surface snapping (closest-point gradient descent; position and position+normal modes)
- [x] 8.3 Voxel face/cell picking + build-plane resolution
- [x] 8.4 Bounds/frustum utilities (selection bounds, zoom-to-selection)

## 9. File I/O (file-io)

- [x] 9.1 `.clayspace` chunked container: writer/reader, versioning (backward-open, forward-refuse), round-trip bit-identity tests
- [x] 9.2 OBJ + MTL reader/writer (dependency-free) with vertex-color extension
- [x] 9.3 FBX import via ufbx; minimal binary FBX 7.4 writer; round-trip validated through ufbx (tests) and assimp (CI, independent implementation); Blender-headless validation deferred to the release checklist (too heavy for per-push CI)
- [x] 9.4 PLY reader/writer with vertex colors
- [x] 9.5 Import guardrails: triangle budgets, fuzz corpus, allocation-bomb tests
- [x] 9.6 Platform-consumable mesh buffer API (for app-side USDZ via Model I/O)

## 10. C ABI, SwiftPM, CLI (c-abi, build-packaging)

- [x] 10.1 `clay.h`: opaque handles, error codes, size-query buffer pattern, `clay_version()`; C11 consumer smoke test
- [x] 10.2 Error/memory discipline plumbing (`std::expected`-style internals, thread-local detail messages, `clay_free_*`)
- [x] 10.3 SwiftPM wrapper (Package.swift binaryTarget) + xcframework build script; verified macOS/iOS-device/iOS-simulator slices build and Swift consumes the module; in-app verification pending the ClaySpace Xcode project (repo has no app target yet); Metal wired per-app at integration
- [x] 10.4 FFI hygiene check: header lint (no variadics/bitfields/non-fixed-width ints) + Python ctypes cross-language exercise of the shared library, in CI
- [x] 10.5 `clay-cli`: mesh/validate/eval/convert subcommands over public APIs
- [x] 10.6 Performance benchmarks (points/sec, bricks/sec, mesh time) with CI floor-threshold gates (generous floors catch order-of-magnitude regressions without shared-runner flake; tight deltas need dedicated hardware)

## 11. Phase 2 — Python, extended vocabulary, meshers, glTF

- [x] 11.1 `pyclay` nanobind module: document/layer/edit API, numpy-native eval/gradients (GIL released), meshing, save/load/export
- [x] 11.2 Backend selection + enumeration from Python with clear unavailable-backend errors
- [x] 11.3 Wheels via scikit-build-core + cibuildwheel (macOS arm64/x86-64, manylinux, Windows); pip-install quickstart test
- [x] 11.4 Port golden-scene corpus authoring + property suites to Python; wire into CI
- [x] 11.5 Extended blend vocabulary (groove, tongue/pipe, emboss/deboss, push, avoid, inset, shell, stain/paint, replace) with rigidity tests
- [x] 11.6 Surface nets mesher (preview path) + benchmark (BM_SurfaceNets, CI-gated as strictly faster than the marching mesher: 221 ms vs 331 ms locally)
- [x] 11.7 Dual contouring (QEF/Hermite, regularized normal equations — no SVD dependency) behind an explicit experimental flag + sharp-edge golden test on an off-axis rotated union; the manifold-DC variant remains roadmap-hardening per spec
- [x] 11.8 glTF/GLB writer + glTF-validator CI gate

## 12. Phase 3 — CUDA

- [x] 12.1 CUDA backend host (nvcc, build-time compile of the same kernel headers, -fmad=false for parity); eval_points/eval_grid/raycast. Kernel headers verified under the CUDA shim profile by check_kernel_dialect.py (host-emulated qualifiers); nvcc codegen gated by the CUDA CI job
- [x] 12.2 Parity + pyclay coverage are backend-generic: the C++ parity suite runs every registered backend against CPU scalar, and test_every_registered_backend_matches_cpu does the same from pyclay — both pick up `cuda` automatically. **Not yet executed on NVIDIA hardware** (no CUDA device available here; GitHub runners have none): CI builds CUDA but cannot run device parity. Pending verification on a CUDA machine
- [ ] 12.3 Ship CUDA-enabled wheels/binaries where toolchain permits — deferred to the release checklist (needs a CUDA build host; the wheel currently ships the CPU backend, which per the parity contract changes only speed)

## 13. Phase 4 — OpenCL

- [x] 13.1 OpenCL C-compatible shim branch (macro-mapped builtins, no overloads/templates/namespaces; structs in portable typedef form) + host that JIT-compiles the amalgamated kernel headers (tools/amalgamate_cl.py) with -DCLAY_KERNEL_OPENCL; eval_points + eval_grid (the brick-fill primitive), raycast reports Unsupported
- [x] 13.2 OpenCL parity **verified on real hardware** (Apple M2 Max, OpenCL 1.2): full 157-case suite green with the backend registered, values bit-identical to the CPU reference on the probe scene, pyclay backend="opencl" agreement test passing; CI runs the same suite against a pocl device on Linux. Tier-3 status documented in the evaluation-backends spec and README

## 14. Release

- [x] 14.1 SemVer release checklist automation: tools/release_check.py gates version agreement across CMake/C ABI/wheel, build+tests, backend parity, layering/dialect/licenses, C ABI FFI, openspec validate, benchmark floors, and a real pip-install quickstart; .github/workflows/release.yml runs it on a tag then builds wheels + xcframework and drafts the release. **The v1.0 tag itself is deliberately not cut** — docs/RELEASE.md lists what must be verified first (CUDA device parity above all)
- [x] 14.2 README refreshed to shipped behavior (capability table, backend tiers, spec pointers) + docs/RELEASE.md; change archived into openspec/specs/
