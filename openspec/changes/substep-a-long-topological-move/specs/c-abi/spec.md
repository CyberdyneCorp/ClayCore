## ADDED Requirements

### Requirement: A topological move can be sampled from a document
The ABI SHALL expose a topological move sampled from a document, `clay_item_volume_move_topological_from`, returning a new item that carries the moved volume and leaving the document unmodified. It SHALL take the move descriptor the in-place form takes, validated the same way, and the volume sampling and optional region that `clay_item_volume_flatten_from` takes, with the same rules. When no region is passed, the sampled region SHALL be the document's padded bounds grown by the displacement's length, so that material pulled outward is not clipped.

Both topological move entry points SHALL refuse a non-finite anchor or displacement with `CLAY_ERROR_INVALID_ARGUMENT`.

#### Scenario: A short drag matches bake-then-move
- **WHEN** a host moves a ball's crown a short way through the document-sourced call, and separately bakes the same region with a band covering the drag and moves that volume in place
- **THEN** the two fields agree within half a cell near the surface

#### Scenario: A long drag needs no band and no region
- **WHEN** a host drags a unit ball's crown by (0.5, 0, 0.4) at radius 0.3 through the document-sourced call with the default band and no region
- **THEN** the pulled material is present at (0.35, 0, 1.25), the point (0, 0, 1.25) above the anchor is outside, and the crown under the grip is still inside at (0, 0, 0.99)

#### Scenario: Malformed calls are refused
- **WHEN** the call is given a zero cell size, half a region, a null document, descriptor or sampling, a radius that is not positive, or a NaN displacement
- **THEN** it is refused and no item is returned
