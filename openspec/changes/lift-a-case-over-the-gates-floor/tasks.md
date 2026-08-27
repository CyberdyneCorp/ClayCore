# Tasks

- [x] Add `Measurement.batch`, defaulting to 1, and record it.
- [x] Add a `batch:` argument to `measureAxis` and apply it to the 21
      `devicemeasure` cases below the floor.
- [x] Give `pose_region` a reset, since a batched body places a node per
      iteration and would otherwise grow the document past the size its axis
      point names.
- [x] Time one DRAG of 64 dabs in the voxel session cases and the mask freeze,
      keeping every drag on the existing path so the `fill_cavities` and
      `mask_freeze` fixtures still work.
- [x] Move `mask_freeze` from `interactive` to `gesture`.
- [x] Deposit several items per stroke in `cut_passes`, `magnify_pinch` and
      `snakehook_tendrils`, sized so the growth exponent stays under 1.25.
- [x] Deliver `move_drags` in four sub-steps, and say why not eight.
- [x] Extend `sdf_stamp_bricks`' axis to 10,000 rather than batching it.
- [x] Record `sdf_stroke_bricks` as a batch of 24 rather than a per-dab
      quotient.
- [x] Re-derive `tests/device/baseline.json` for every changed case from a
      valid run on the reference iPad.
- [x] Update `docs/09-brush-latency-and-coverage.md` and `docs/RELEASE.md`,
      whose worked example quotes the old 22/39 split.
