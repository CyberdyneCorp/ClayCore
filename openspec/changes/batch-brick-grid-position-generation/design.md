## Design

BrickGrid::sample_positions(first, count, out_xyz) writes count consecutive bricks in existing slot order, with x-fastest local samples and three floats per point. The caller supplies count * kBrickSamples * 3 floats; count zero writes nothing and permits null output. For each brick, call sample_cell(slot, 0) once, then walk z/y/x and add local integer offsets before float conversion. Compute origin + global_cell * cell_size exactly as sample_position does. Do not increment floating coordinates or derive positions from a pre-rounded world brick origin.

SdfSourceField::block_fill retains its existing point buffer and evaluation/coverage batching, replacing only coordinate generation with the bulk writer. Other bake/placement paths remain unchanged in this change.

## Verification

Compare every coordinate bit against the scalar reference across nonzero first slots, row/plane crossings, partial runs, several grid shapes, negative/mixed/large origins, signed zero and non-binary cell sizes. Guard output bounds and zero-count behavior. Run field/prefix-cache/Smooth tests, sanitizer coverage and unchanged transaction-output comparison. Measure coordinate generation and actual Smooth pointer-down separately, and run complexity, layering, strict OpenSpec and platform CI.
