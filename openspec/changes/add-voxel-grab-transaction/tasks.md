## 1. The gesture

- [x] 1.1 `voxel::GrabTransaction` captures the material at `begin` and
      resamples every `update` from that capture
- [x] 1.2 `update` takes the TOTAL from the anchor, never an increment
- [x] 1.3 The capture widens lazily as the drag reaches, which is sound because
      only the footprint is ever written
- [x] 1.4 `commit` keeps the last update and releases the capture; `cancel`
      restores the footprint from it
- [x] 1.5 The resample is the SAME walk and the same map `sculpt_grab` uses, so
      the two cannot drift on a single emission

## 2. The bindings

- [x] 2.1 `clay_voxel_grab_begin` / `_update` / `_written_box` / `_commit` /
      `_cancel` / `_destroy`, with `_destroy` cancelling an uncommitted drag
- [x] 2.2 Every write raises the same undo step and dirty bookkeeping a
      stateless verb does
- [x] 2.3 `VoxelGrid.grab` returning a context manager: leaving commits, an
      exception cancels

## 3. Say that the plain call does not compose

- [x] 3.1 `clay_voxel_sculpt_grab`'s note, with the measurement
- [x] 3.2 `VoxelGrid.sculpt_grab`'s docstring

## 4. Proof

- [x] 4.1 The split does not change the result, at all three footprints
- [x] 4.2 ...and the drag actually moves material, so 4.1 is not satisfied by
      doing nothing
- [x] 4.3 An update is idempotent
- [x] 4.4 A drag can go back on itself
- [x] 4.5 One update equals the stateless call, cell for cell
- [x] 4.6 Cancel restores exactly; destroy cancels an uncommitted drag
- [x] 4.7 The capture grows and the written box does not
- [x] 4.8 A brush that is not a footprint is refused
- [x] 4.9 The same claims at the C boundary and from pyclay
- [x] 4.10 4.1, 4.3 and 4.4 fail when `update` reads the grid instead of the
      capture
