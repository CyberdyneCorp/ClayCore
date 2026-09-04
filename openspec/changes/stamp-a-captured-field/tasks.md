## 1. The asset

- [x] 1.1 `field::FieldStamp`: the captured `FieldVolume`, its local bounds, the
      capture frame, and a content id. Reuses the volume capture already shipped
      (`clay_item_volume_from_document`) rather than adding a second sampler
- [x] 1.2 An asset table on the document, so a placement REFERENCES an asset and
      two placements of one stamp are one payload.
      SPLIT OUT and DONE as `write-a-shared-payload-once` (format minor 17),
      because the defect was never stamp-specific: `Node::volume` and
      `Node::gate` are the same member type with the same per-node
      serialization, so calling the fix "field stamps" would have mislabelled
      it. 1,499,457 -> 189,117 bytes for 8 placements of one capture
- [x] 1.2a BOTH HALVES TOGETHER. A writer that deduplicates against a reader that
      still calls `make_shared` per node loads N unrelated volumes and the next
      save writes N payloads again.
      DONE there, and asserted as object identity rather than as size alone
- [x] 1.3 Standalone encode/decode, so a host library can keep a stamp on disk
- [x] 1.4 Memory accounting separates asset payload from placements

## 2. Oriented capture

- [x] 2.1 A caller-supplied capture frame; +Z outward, X/Y the tangent plane
- [x] 2.2 A surface-oriented helper from hit + normal + azimuth, through
      `kernel::calpha_frame` — the SAME resolution the scalar alpha uses,
      including its fallback axis, so a stamp and an alpha cannot disagree
- [x] 2.3 NO inferred orientation. Stated in the header with the reason: a frame
      derived from the samples moves when the region does, so re-capturing the
      same detail yields an asset that disagrees with placements already made

## 3. Placement

- [x] 3.1 A placed stamp is an ordinary `Volume` item under a transform
- [x] 3.2 Transform order pinned BY A TEST: world -> inverse translation ->
      inverse orientation -> inverse scale -> local sample
- [x] 3.3 ~~Uniform scale only, and a non-uniform one REFUSED~~ — **the premise
      is wrong and the refusal was not written.** Checked against the tree:
      `cfi_scale_nonuniform` keeps the Lipschitz constant and drops only
      EXACTNESS, and `clay.h` states the distance is divided by the SMALLEST
      component of the scale, "which never overestimates the true distance — so
      the field stays a conservative bound and stays 1-Lipschitz, and
      clay_safe_step_scale does not move". Refusing would have removed a working
      capability. The property the refusal was for is gated instead (5.4)
- [x] 3.4 No `strength` multiplier, and the reason in the header — a resolved
      stroke's strength reaches `clay_layer_place_stamps` and is deliberately
      dropped there, with the reason beside it

## 4. Stroke

- [x] 4.1 Resolved stamps become placements: spacing, pressure, jitter, taper,
      azimuth
- [x] 4.2 One gesture is one undo step

## 5. Gates

- [x] 5.1 Identity-placement parity with the source, within the declared tolerance
- [x] 5.2 N placements hold ONE payload, across a save and reload
- [x] 5.3 Deterministic capture encoding: the same region captured twice is the
      same bytes
- [x] 5.4 Safe-step / Lipschitz correctness for a placed and scaled stamp —
      3990 steps from outside a hard-squashed placement, 0 crossings
- [x] 5.5 Capture and placement benchmarks at 32^3 / 64^3 / 128^3 / 256^3; the
      claim is that a new placement costs a reference and not a payload
- [x] 5.6 C ABI, pyclay, a numbered example that renders and asserts
- [x] 5.7 Version lines together; `check_*` gates green

## What the plan got wrong, found by checking it

- [x] 6.1 **1.2 and 1.2a were already done** by `write-a-shared-payload-once`
      (format minor 17), which the task list already recorded. The proposal's
      "measured 1,499,457 bytes for 8 placements" is the defect BEFORE that
      landed and reads as current; it is history now.
- [x] 6.2 **3.3's refusal is not needed** — see above. The tree is more careful
      than the plan assumed.
- [x] 6.3 **The frame must round-trip as a QUATERNION.** Storing the normal and
      tangent and re-resolving them on load runs the frame back through
      `calpha_frame` and a basis-to-quaternion conversion; the rounding in that
      made every sample of a reloaded stamp differ from the original in the last
      ulp. Caught by the round-trip gate asserting field equality rather than
      "it loaded".
