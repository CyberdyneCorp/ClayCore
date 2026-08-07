# python-bindings — move drag continuity

Delta for `add-move-drag-continuity`.

## ADDED Requirements

### Requirement: Previewing a move from Python
The module SHALL expose previewing a drag, returning the nodes it would warp without touching the document.

#### Scenario: A script previews before committing
- **WHEN** a script previews a drag and then applies it
- **THEN** the preview leaves the document unchanged and names the same nodes the move reports
