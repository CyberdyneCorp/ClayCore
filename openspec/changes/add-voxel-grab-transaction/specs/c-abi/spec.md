## ADDED Requirements

### Requirement: A voxel drag is reachable from C as a transaction
The C ABI SHALL expose the voxel grab gesture as `clay_voxel_grab_begin`, `_update`, `_written_box`, `_commit`, `_cancel` and `_destroy`, on the shape `clay_sdf_move_begin` already has on the field side.

`_update` SHALL take the TOTAL displacement from the anchor. `_written_box` SHALL report the brush's footprint, fixed for the whole gesture whatever the displacement grows to, so a host has its invalidation region from the first frame. `_destroy` SHALL cancel an uncommitted transaction.

Every write SHALL raise the same undo step and dirty-region bookkeeping a stateless verb does.

`clay_voxel_sculpt_grab`'s documentation SHALL state that it does not compose and point at the transaction.

#### Scenario: A drag delivered in pieces lands where one delivered whole does
- **WHEN** the same total drag is delivered through the transaction as one, two, four and eight updates
- **THEN** the grid is identical in every case, and differs from the untouched grid

#### Scenario: The written box does not grow with the drag
- **WHEN** the box is read at the start of a gesture and after a long update
- **THEN** it is the same box

#### Scenario: A spent transaction is refused, not silently accepted
- **WHEN** update or commit is called after a commit or a cancel
- **THEN** it is refused
