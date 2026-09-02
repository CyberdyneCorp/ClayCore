# Tasks

- [x] Split `edit_guard` and `perform_edit` out of `apply_edit`, so a gesture
      path shares the guard and the apply without the invalidation, and so a
      refused edit pays nothing for a bound it will not use.
- [x] `GestureRegion`: one invalidation for a whole gesture, from a region the
      caller states, applied on the failure path too.
- [x] `clay_layer_move_surface` states the drag's ball, from the radius as read
      through the versioned descriptor.
- [x] `BM_MoveDrag1000` / `BM_MoveDrag10000`, with a ceiling that is honest
      about what it can and cannot catch.
- [x] Tests: a dragged brick reads as a full refill of the dragged document,
      across three drag shapes; a far drag leaves the seeds alone.
- [x] Prove the tests fail when the region is made too tight.
- [x] Revert `move_drags` to one application per stroke, and record why.
- [x] Re-derive `sdf_move` and `move_drags` from a device run, and drop the
      held exemption once `sdf_move` matches its baseline again.
