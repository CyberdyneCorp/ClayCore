## 1. The defect

- [x] 1.1 `Compiler::fold_info` folds `deformer_lipschitz(item)` — the item's
      whole chain — into a tape that `Compiler::cull_deformers` has already
      shortened. Two independent calls on the same item; neither knows about
      the other
- [x] 1.2 MEASURED with `benchmarks/cull_vs_bound_probe.cpp`: at 48 Move dabs a
      brick emits 9 grabs and declares 253.61, which is `1.125^48` — the whole
      chain's product. At 1 dab it emits NOTHING and declares 1.12
- [x] 1.3 Established that it is a correctness defect and not only a slow one:
      `craycast` scales the distance its acceptance test sees by the step
      scale, so a degraded bound manufactures hits — 4090 against a truth of
      3960 on a 64x64 fan

## 2. Two designs that were wrong

- [x] 2.1 REFUTED: price the grab against the `local` AABB in scope at the
      pricing site. That is `prim_local_bounds(item)`, the primitive's own
      extent, not a query region
- [x] 2.2 REFUTED: narrow each easing's slope to the sub-interval the brick
      spans. Sound, but Move sets `ease = 0` and linear's derivative is the
      constant 1, so it wins nothing for the brush in question

## 3. The change

- [x] 3.1 `deformer_lipschitz(item, deformers)` — the same bound over an
      explicit chain; the existing one-argument form delegates to it
- [x] 3.2 `chain_links` takes the chain explicitly for the same reason
- [x] 3.3 `Compiler` records the chain `emit_prim` emitted, keyed on the source
      vector's ADDRESS so a stale entry cannot be read as a shorter chain
- [x] 3.4 `fold_info` bounds that chain when the address proves it belongs to
      the item in hand, and the item's full chain otherwise
- [x] 3.5 `consolidate.cpp`'s `steepest_deformer_chain` and the influence bound
      keep the one-argument form: both ask a whole-item question

## 4. Tests

- [x] 4.1 A region no warp reaches declares exactly 1.0 — fails at 3.138 without
      the change
- [x] 4.2 Unreachable warps do not raise the bound — fails at 97.017 against
      1.4641 without the change
- [x] 4.3 A culled march agrees with a dense sign-change truth — fails at 360
      against 332 without the change
- [x] 4.4 The fixture's own guards refuse a fan that hits everything or nothing;
      the first version tripped them and was widened past the silhouette
- [x] 4.5 Full suite 11/11, parity 46/46 over 1,406,731 assertions

## 5. Measurement

- [x] 5.1 A/B with #542 present on both sides: 48 dabs, 759.2 ms -> 48.0 ms
      (15.8x), steps 3,511,660 -> 226,458, false hits 130 -> 0
- [x] 5.2 RECORDED NEGATIVE: meshing measures nothing here. `mesh_tape`
      evaluates a dense grid and never marches — 7.666 ms against 7.213 ms
      while the bound moved 43x
- [x] 5.3 RECORDED NEGATIVE: the whole-document tape is unchanged, by design.
      `declared_vs_actual_probe` reads identically on both sides

## 6. Still open

- [ ] 6.1 Device gate on the reference iPad, with #542, before the tag that
      carries either
- [ ] 6.2 The whole-document tape still compounds over the full chain, which is
      correct for it and is what #541's headline figure measures. A host that
      picks through an unculled tape sees none of this
