# Tasks: forty prims are paying for one prim's colour

## 1. The split

- [x] 1.1 `ctape_volume_dist(q, blob, lp, out_color)` — the volume opcode's
      body, moved verbatim out of `ctape_prim_dist`. Verbatim on purpose: it is
      the only path that ever wrote through the pointer, so identical
      behaviour is a property of the edit rather than a claim about it.
- [x] 1.2 `ctape_prim_dist` loses the out-parameter and keeps the analytic
      prims.
- [x] 1.3 `ctape_prim_local` dispatches: `ctape_volume` to the new entry point,
      everything else to the old one.
- [x] 1.4 `tests/unit/scene_utils.h`'s reference walker drops the argument it
      was passing as `ignored_color`. It was already a distance-only caller
      and already said so in a comment.

## 2. It still compiles as five dialects

- [x] 2.1 `check_kernel_dialect.py` passes: cpu, cuda and metal profiles plus
      the OpenCL and Vulkan amalgamations.
- [x] 2.2 The parity corpus still compares colour as well as distance for a
      coloured volume, on every backend registered in the build.
- [x] 2.3 Metal specifically, since `CLAY_OUT`/`CLAY_INOUT` differ per dialect
      and the volume path is now reached through a second function: a coloured
      volume evaluated on the Metal backend agrees with the CPU reference.

## 3. Tests

- [x] 3.1 A coloured volume reports its samples' colours — existing coverage in
      `test_volume_color.cpp`, which must not move.
- [x] 3.2 A volume with no colour section reports the item's colour — existing,
      must not move.
- [x] 3.3 The dispatch is exercised by a tape that holds BOTH a coloured volume
      and analytic prims, so a wrong branch shows up as a wrong colour rather
      than as nothing. VERIFIED to fail against a dispatch that never takes the
      volume path (2 of 5 assertions). Its unique coverage is the mixed tape —
      under a broken dispatch it fails ALONGSIDE the single-prim tests above
      rather than alone, which is worth saying rather than implying it catches
      something nothing else does.

## 4. The measurement

- [x] 4.1 Mac, C ABI harness, medians of three at 1000 stamps: `mask_extrude`
      **3829.1 -> 3288.6 ms**, below its pre-colour 3348.8 ms.
      `sdf_consolidate` 370.1 -> 324.4 ms, which does not return to its 243.4
      ms pre-colour figure because the rest of that one is the colour PASS —
      `fix-consolidation-colour-cost`, complementary and already open.
- [x] 4.2 Device gate on the reference iPad, from a clean tree, thermal
      `nominal` at both ends. **`mask_extrude` 3786.6 -> 2711.1 ms** — from
      1.51x over the v0.30.0 baseline to 1.08x, well inside its 3751 ms budget.
      The device recovered MORE than the Mac did (1.40x against 1.16x), which
      is the same asymmetry the bisect found in the other direction: this cost
      is bigger on the tablet, both to pay and to remove.
- [ ] 4.3 Whether the two changes together return `sdf_consolidate` to its
      v0.30.0 baseline. On device, each alone: 916.4 -> 678.8 ms with the
      colour-pass skip, 916.4 -> 772.5 ms with this. Neither is enough on its
      own — this change's gate run still fails `sdf_consolidate` at 1.47x, and
      it is the ONLY failure left in the 59. Predicted ~243-250 ms on the Mac
      for the pair; recorded either way.
- [x] 4.4 `sdf_stamp_cpu` was NOT device noise. **6.43 -> 4.51 ms**, against a
      4.32 ms baseline: 1.04x, effectively recovered. `sdf_stamp_bricks` came
      back with it, 5.40 -> 4.47 ms, below its own 4.86 ms baseline.

      Worth recording as a wrong call rather than a lucky one. The last gate
      report reasoned that the two stamp cases moving in OPPOSITE directions
      across two runs looked like noise. The run-to-run scatter was real — 5.39
      then 6.43 on near-identical code — but the level underneath it was this
      defect, and the reasoning would have closed the question if anything had
      been concluded from it. It was left open because "looks like noise" is
      not a measurement; that is the only reason it got measured.

## 5. Not in this change

- [ ] 5.1 The colour pass in the consolidation bake. Different defect, already
      open as `fix-consolidation-colour-cost`. This change must not be reviewed
      as covering it.
