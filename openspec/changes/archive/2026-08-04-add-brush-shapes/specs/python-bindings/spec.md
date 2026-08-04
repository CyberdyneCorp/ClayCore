# python-bindings — paint brush and brush shapes

Delta for `add-brush-shapes`.

## ADDED Requirements

### Requirement: Paint brush and mirrored paint in Python
The module SHALL expose `paint_brush(cell, size, index, shape=...)` and `paint_mirrored(cell, index, axes=...)`, matching the C++ surface so that every brush and mirrored edit implemented by the engine is reachable from Python.

#### Scenario: Recolouring a region without adding voxels
- **WHEN** a script calls `paint_brush` over a region that is partly empty
- **THEN** the occupied cells change palette index and the occupied count is unchanged

#### Scenario: Mirrored paint
- **WHEN** a script calls `paint_mirrored` with `axes="x"` on a symmetric model
- **THEN** both the authored cell and its mirror are recoloured

### Requirement: Brush shape selection from Python
Brush operations SHALL accept a `shape` argument taking `"cube"` (default) or `"sphere"`, matching the string-argument convention used elsewhere in the bindings. An unrecognised shape SHALL raise rather than silently falling back.

#### Scenario: Sphere brush from Python
- **WHEN** a script calls `set_brush(cell, 5, index, shape="sphere")`
- **THEN** fewer cells are set than the cube of the same size, and every set cell lies within the sphere radius

#### Scenario: Unknown shape is rejected
- **WHEN** a script passes `shape="blob"`
- **THEN** the call raises a `ValueError` naming the accepted shapes
