# Tasks: add-tape-deformers

## 1. Tape

- [x] 1.1 `CDeformType` enum + fixed-size deformer record; primitive param blocks become fixed-width so the deformer block sits at a known offset
- [x] 1.2 `ctape_apply_deformers`: warp the local point in order, apply per-deformer distance corrections; wire into `ctape_eval`
- [x] 1.3 Kernel test: tape-with-deformer equals direct `deform.h` application; chain order matters

## 2. Scene

- [x] 2.1 `scene::Deformer` struct + `Node::deformers`; helper constructors (twist/bend/taper/displace)
- [x] 2.2 Compiler emits the chain and folds each deformer's Lipschitz factor via `cfi_*` into the tape's field info
- [x] 2.3 Conservative bound widening per deformer in `prim_local_bounds` (rotational hull, taper scale, displacement amplitude)
- [x] 2.4 Command serialization carries the chain (Node has a new vector field); reference tree evaluator mirrors it
- [x] 2.5 Tests: bound conservativeness on deformed items, per-brick culled-tape band-clamp identity, safe-step-scale drop, document round trip

## 3. Python

- [x] 3.1 `.twist(k)` / `.bend(k)` / `.taper(...)` / `.displace(...)` chainable modifiers + `.deformers` inspection; clear error for unsupported constructs
- [x] 3.2 Tests: the docs/05 sample line works, order matters, round trip

## 4. Integration

- [x] 4.1 Update docs/05 and README (deformers are no longer the documented gap)
- [x] 4.2 Verify every preset (cpu-only, metal, opencl, asan) + python ON/OFF + release checklist
