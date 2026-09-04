## 1. The asset

- [ ] 1.1 `field::FieldStamp`: the captured `FieldVolume`, its local bounds, the
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
- [ ] 1.3 Standalone encode/decode, so a host library can keep a stamp on disk
- [ ] 1.4 Memory accounting separates asset payload from placements

## 2. Oriented capture

- [ ] 2.1 A caller-supplied capture frame; +Z outward, X/Y the tangent plane
- [ ] 2.2 A surface-oriented helper from hit + normal + azimuth, through
      `kernel::calpha_frame` — the SAME resolution the scalar alpha uses,
      including its fallback axis, so a stamp and an alpha cannot disagree
- [ ] 2.3 NO inferred orientation. Stated in the header with the reason: a frame
      derived from the samples moves when the region does, so re-capturing the
      same detail yields an asset that disagrees with placements already made

## 3. Placement

- [ ] 3.1 A placed stamp is an ordinary `Volume` item under a transform
- [ ] 3.2 Transform order pinned BY A TEST: world -> inverse translation ->
      inverse orientation -> inverse scale -> local sample
- [ ] 3.3 Uniform scale only, and a non-uniform one REFUSED rather than accepted
      with a bound the marcher will step through
- [ ] 3.4 No `strength` multiplier, and the reason in the header

## 4. Stroke

- [ ] 4.1 Resolved stamps become placements: spacing, pressure, jitter, taper,
      azimuth
- [ ] 4.2 One gesture is one undo step

## 5. Gates

- [ ] 5.1 Identity-placement parity with the source, within the declared tolerance
- [ ] 5.2 N placements hold ONE payload, across a save and reload
- [ ] 5.3 Deterministic capture encoding: the same region captured twice is the
      same bytes
- [ ] 5.4 Safe-step / Lipschitz correctness for a placed and scaled stamp
- [ ] 5.5 Capture and placement benchmarks at 32^3 / 64^3 / 128^3 / 256^3; the
      claim is that a new placement costs a reference and not a payload
- [ ] 5.6 C ABI, pyclay, a numbered example that renders and asserts
- [ ] 5.7 Version lines together; `check_*` gates green
