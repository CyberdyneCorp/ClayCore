# Proposal: sphere brush footprint, and the paint brush in Python

## Why

Voxel sculpting ships exactly one brush: a solid N×N×N cube. Every organic
form — anything rounded, anything that is not architecture — is built by
stamping cubes and cleaning up the corners by hand. A sphere footprint is the
other half of the basic toolkit and is a handful of lines, since the footprint
loop already exists.

Separately, `paint_brush` and `paint_mirrored` are implemented in C++ but were
never bound to Python, while `set_brush`, `erase_brush` and `set_mirrored` all
were. The python-bindings spec already claims Python exposes "single/brush/
box/line edits, mirrored edits", so the spec currently overstates what ships.
The examples had to work around it with a loop of per-cell `paint` calls.

## What Changes

- **A brush shape**, selected per call rather than as a parallel family of
  functions: `set_brush`, `erase_brush` and `paint_brush` take a `BrushShape`
  (`Cube` or `Sphere`), defaulting to `Cube` so existing calls are unchanged.
  A sphere of size `n` is the sphere inscribed in the cube of size `n` — cells
  whose centre lies within radius `(n-1)/2` of the brush centre.
- **`paint_brush` and `paint_mirrored` reach Python**, closing the gap between
  the binding surface and what the spec says it exposes. Python selects the
  shape with a string (`shape="sphere"`), matching the convention already used
  for `axes=` and `mesher=`.
- **The even-size trap is documented, not changed.** `n=2` and `n=4` silently
  behave as 1 and 3 because the radius is `(n-1)/2`. That is existing
  behaviour; this change writes it down and tests it rather than altering
  sizes people may already depend on.

## Capabilities

### Modified Capabilities

- `voxel-engine`: brush footprints gain a shape; the paint brush is specified
  alongside set and erase.
- `python-bindings`: `paint_brush` and `paint_mirrored` are exposed, and the
  brush ops take a shape argument.

### New Capabilities

_None._

## Impact

- `include/clay/voxel/grid.h`, `src/voxel/grid.cpp`, `bindings/python/pyclay_module.cpp`, tests, `examples/07_voxel_sculpting.py`, docs.
- No serialization or undo impact: brushes are grid ops, not stored state, and
  are not part of the command layer or the `.clayspace` format.
- Non-goals: falloff or soft-edged brushes (voxel occupancy is binary, so
  there is no partial coverage for a falloff to act on), and the mesh-sculptor
  verbs (smooth, inflate, flatten, pinch) which need a different data model.
