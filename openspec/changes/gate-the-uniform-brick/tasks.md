# Tasks

## 1. The uniform-brick gate

- [x] 1.1 `prove_uniform` — one evaluation of the brick's own culled tape at the lattice centre, `|f(c)| > (band + L·hd)(1 + 1/64)` with the lattice's own half-diagonal, never the cull region's
- [x] 1.2 `uniform_stub_tape` — a one-instruction constant tape that keeps the brick's batch slot and carries the colour submit reads at sample *n/2*, so cpu, metal, the device destination and submit see an ordinary brick
- [x] 1.3 Only the whole-document evaluation is gated; per-layer halves always walk
- [x] 1.4 A `uniform` `ResumeEntry` kind holding the proof (centre and *n/2* value and colour, *L*), charged against the budget so it can be evicted; `resume_bricks` carries it through `compile_layer_suffix` at two seeded points and re-proves under `max(stored, suffix)` when every appended item folds by a max rule
- [x] 1.5 The seedless design measured (warm dab 4.5× and 19× slower) and recorded as the reason the proof is stored
- [x] 1.6 A proof counts as refilled where made and resumed where carried, so `clay_resume_stats` keeps its meaning; `clay_internal_set_uniform_gate` / `clay_internal_gated_bricks` as test seams
- [x] 1.7 `test_c_uniform_gate.cpp`: stored bit-identity with the gate on and off, the proof-as-seed path through an append and a carve against from-scratch fills, the proof-rate floor, the multi-layer refusal, the metal backend's classification
- [x] 1.8 Three existing cases that read raw refill floats beyond the band run with the gate off, with the reason in a comment
- [x] 1.9 `BM_DabRefillSculpted400` / `BM_DabRefillSculpted1500` — the cold dab window on the helix fixture
- [x] 1.10 `clay.h` states the stand-in values at the entry point; docs/05 explains the proof, the ball and the bound

## 2. The pick marches the cached tape

- [x] 2.1 `raycast_scene(doc, tape, index, ray, options)`; the original signature forwards with `pickable_tape(doc)` and no index
- [x] 2.2 `clay_raycast_attributed` / `clay_raycast_bounded` pass the per-revision tape, and the cull index only when the tape's scale is below 1
- [x] 2.3 `march_tape` compiles a tape culled to the clipped segment (dilated by `kRayCullDilation`, through the index's plan) and uses it only when its scale is strictly larger
- [x] 2.4 `pickable_tape` takes the index and plan, dropped on the ghost-layer copy path
- [x] 2.5 `SceneHit::steps` and `RaycastOptions::local_tape`, so the claim is testable
- [x] 2.6 Test: a far high-Lipschitz item does not slow a ray that never nears it — same hit, fewer steps, culled scale 1, cached overload bit-for-bit, a ray through the item ties

## 3. Attribution without a document

- [x] 3.1 `scene::compile_item(layer, node)` — the item as an Add, alone, under the layer's placement and symmetry; a group yields an empty tape
- [x] 3.2 `item_field_distance` evaluates `compile_item` over a probe layer built once per call (placement, `scale_axes`, `mirror_axes`, `mirror_k`; radial stays out, as before)
- [x] 3.3 `attribute(doc, tape, position, layer, item)` reads a single candidate layer's winner off the pickable tape; several candidates compile per layer as before
- [x] 3.4 Buffer reuse across candidates measured (<1%) and not done
- [x] 3.5 Tests: `compile_item` byte-identical to the single-item layer's compile over the gnarly corpus; both entry points held against the old implementation kept verbatim as the reference

## 4. The cache keeps its surface bound

- [x] 4.1 `BrickCache::surface_bounds()`, maintained by submit, evict and both trims; a removal marks the box stale only when the brick touched a face; one refold per mutating call
- [x] 4.2 The refold on the mutating side, never in a const query, because `clay_brick_cache_raycast_many` reads the cache from many threads
- [x] 4.3 `raycast_bricks` reads the bound instead of folding `surface_bricks()`
- [x] 4.4 Test: the bound equals the fold after every mutation, to exact float equality

## 5. The analytic brick walk

- [x] 5.1 Brick DDA over the cache skipping uniform bricks whole; an Outside brick beside a differing neighbour walks only its face-layer cells
- [x] 5.2 Cell DDA under Surface bricks; a per-brick corner slot table carried along the ray, four corners carried from the cell before
- [x] 5.3 The JCGT cubic per cell, Marmitt's split and bracketed Newton–Raphson for the first root of `f = eps·max(t, 1)`; the analytic gradient as the normal
- [x] 5.4 The sphere trace kept as `pick::detail::raycast_bricks_sphere_traced`, test-only
- [x] 5.5 Test: 760 rays agree with the sphere trace — hit/miss, *t* within `voxel_size·0.05`, normals within 26° and facing the ray
- [x] 5.6 `float_to_half` stores subnormal halves correctly (`126 − e`), with the IEEE bit patterns pinned; docs/05 records the fp16 contract

## 6. The record

- [x] 6.1 docs/05: the proof, the seed, the pick path, the surface bound and the analytic walk; docs/06: the GPU route walks the same reconstruction
- [x] 6.2 docs/09: a dated section with every before/after number, stated as Mac numbers
- [x] 6.3 Spec deltas for c-abi, brick-cache and picking
