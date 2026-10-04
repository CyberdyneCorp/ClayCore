## ADDED Requirements

### Requirement: A stroke session is reachable from Python
pyclay SHALL expose `StrokeTransaction(preset)` with `append(samples)`, which takes the `(N, 3..8)` sample arrays `StrokePreset.resolve` takes and returns `(new_stamps, revised_from)`, along with `end()`, `stamps()` in `StrokePreset.resolve`'s shape, and `status()` carrying `ended`, `samples`, `stamps`, `settled` and `revised_from`. `append` after `end()` SHALL raise `ValueError`.

#### Scenario: The parity harness runs on pyclay
- **WHEN** a path is appended in batches of 1, 5, 40 and an uneven schedule and ended
- **THEN** `stamps()` equals `StrokePreset.resolve` of the whole path, array for array
