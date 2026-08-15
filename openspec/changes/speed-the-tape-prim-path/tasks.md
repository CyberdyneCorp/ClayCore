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
- [ ] 4.2 Device gate on the reference iPad, from a clean tree. `mask_extrude`
      back inside its 3751 ms budget. The Mac understates this cost — x1.18
      here against the device's x1.51 — so this is measured, not predicted.
- [ ] 4.3 Whether the two changes together return `sdf_consolidate` to its
      v0.30.0 baseline. Predicted ~243-250 ms on the Mac; recorded either way.
- [ ] 4.4 `sdf_stamp_cpu`, still unexplained from the last gate. It evaluates
      the same prim path, so it is a candidate to be fixed here — or to be the
      device noise it looked like. This is where that gets settled.

## 5. Not in this change

- [ ] 5.1 The colour pass in the consolidation bake. Different defect, already
      open as `fix-consolidation-colour-cost`. This change must not be reviewed
      as covering it.
