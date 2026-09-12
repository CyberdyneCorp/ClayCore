## 1. The defect

- [x] 1.1 `continues_gesture` matched type, centre and radius by raw float `==`
- [x] 1.2 MEASURED on a 60-frame drag: anchored 2 warps / 0.121 ms; recomputed
      anchor 58 warps / 47.6x; pressure-driven radius 120 warps / 101x;
      finger-following centre 120 warps / 101x
- [x] 1.3 Linear in frames — 30/60/120/240 give 60/120/240/480 when following,
      and 2 at every count when anchored

## 2. Why not a tolerance

- [x] 2.1 An epsilon folds two genuinely distinct gestures — a re-grab a hair
      from the last — into one, which changes the document
- [x] 2.2 Quantising makes the fold depend on where the gesture sits relative
      to the grid

## 3. The change

- [x] 3.1 `gesture_id` on the deformer, stamped at emission on EVERY image so a
      mirrored drag's copies belong to the gesture that produced them
- [x] 3.2 `continues_gesture` matches on it when either side is non-zero
- [x] 3.3 `clay_move_params.gesture_id`, appended behind `struct_size`
- [x] 3.4 NOT serialised: field-by-field deformer IO, so the format is unchanged

## 4. What building it found

- [x] 4.1 The field first landed on `Transition`, not `Deformer` — the anchor
      `std::uint8_t ease = 0;` matches the former first, since both carry an
      easing. Found by checking which struct the line was inside rather than
      that the edit applied

## 5. Tests

- [x] 5.1 Unnamed + moving radius STACKS, and unnamed + moving centre stacks —
      asserted so the fix cannot read as a no-op
- [x] 5.2 Named + moving radius, and named + moving centre, collapse to the
      anchored cost
- [x] 5.3 Two identifiers do not fold; a named gesture does not continue an
      unnamed one
- [x] 5.4 A zero id and a TRUNCATED struct_size predating the field give the
      same warp count and the same field

## 6. Still open

- [ ] 6.1 Device gate before any tag carries this
- [ ] 6.2 pyclay does not expose the field yet
