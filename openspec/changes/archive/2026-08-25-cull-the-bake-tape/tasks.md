# Tasks: cull the bake's tape per brick, exactly

## 1. Establish the shape before building

- [x] 1.1 Where a bake's time goes after #270/#271: essentially pure tape
      evaluation, linear in tape length.
- [x] 1.2 What a per-brick cull buys: 5.4 instructions of 1,199 on a hard union.
- [x] 1.3 The sign question #277 raised — can a brick with an EMPTY culled tape
      hold material? Three adversarial documents (a solid box, a sphere with a
      subtracted void, two separated spheres), 751 bricks, 48 empty tapes, none
      holding material. And it follows from the cull rule: an item that could
      make a point inside is within a band of it and so is kept.
- [x] 1.4 That per-WINDOW culling is not the cheaper middle ground it looks
      like. A window is 512 consecutive slots, which is a slab spanning the
      region rather than a compact box: measured 30x WORSE than not culling.

## 2. Exactness

- [x] 2.1 The two facts the design rests on, stated in the header as an
      argument rather than a claim: culled >= true, and culled <= band implies
      equal.
- [x] 2.2 Bricks with nothing in the band are finished by the culled tape:
      they store no samples, and their sign is right.
- [x] 2.3 A kept brick's out-of-band samples are refined with the whole tape.
- [x] 2.4 VERIFIED why that refinement cannot be skipped: without it the volume
      oversteps its own distance by 0.033 against the plain bake's 0.002 — 1.65
      cells where the interpolation overshoot is 0.1. Measured over 262k
      exterior probes, not argued.
- [x] 2.5 With it, the overstep is identical to the plain bake's to the digit.

## 3. Speed

- [x] 3.1 The refinement is BATCHED across the window. Scattered scalar calls
      measured 1.78x where the batch measures 3.48x — the scalar path gives up
      the vectorised evaluator.
- [x] 3.2 One CullPlan per window, one compiled tape per brick, fanned out over
      the pool.

## 4. The guard

- [x] 4.1 Culling is refused when a brick's tape is not a third of the
      document's or less. Measured at both ends: a fifth left the k=0.06 case
      (1.49x) on the table, a half reaches into the losses.
- [x] 4.2 The probe samples the WHOLE lattice, not the first window. A window
      is a slab at the bottom of the region where every tape culls to nothing;
      probing it chose culling for documents that then ran 1.7x SLOWER. This
      was a real mistake, caught by measurement, and the comment says so.
- [x] 4.3 Verified across k from 0 to 0.16 at two document sizes: never below
      0.99x, up to 3.44x.

## 5. Where it does NOT apply

- [x] 5.1 `clay_item_volume_flatten_from` keeps the whole tape. Flatten
      transforms the block after the fill returns, so a brick the fill saw as
      empty can come back holding the surface and store values that were never
      refined. Found by `test_c_volume.cpp`'s march-cost assertion failing, not
      by inspection.
- [x] 5.2 The precondition is stated on `document_block_fill` itself, with the
      symptom, so the next caller does not have to rediscover it.
- [x] 5.3 Relax is unaffected and does use it: it samples first and relaxes the
      volume afterwards.

## 6. Tests and gates

- [x] 6.1 Byte-identity across five documents: hard union, two blends (one
      wide enough that culling is refused), a single sphere, and one with a
      subtracted void. Brick count, declared Lipschitz and `serialize()`.
- [x] 6.2 Full unit suite: 1419 cases, no failures. `clay_c_smoke` OK.
- [x] 6.3 `BM_VolumeBakeCulledDoc` against `BM_VolumeBakeWholeTapeDoc` in
      FASTER_THAN — the only thing that would catch the guard quietly deciding
      never to cull. 66.3 ms against 122 ms.
- [x] 6.4 `check_bench.py` passes with no gate moved.

## 7. Not in this change

- [ ] 7.1 Metal and the other backends; this is the CPU bake path.
- [ ] 7.2 The per-pass volume copy in relax (#278).
