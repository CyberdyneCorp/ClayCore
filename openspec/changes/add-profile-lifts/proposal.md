# Proposal: profile-driven modelling — 2D profiles with extrude and revolve

## Why

`prim2d.h` ships nine exact 2D profiles (including the arbitrary even-odd polygon and a closed-form quadratic Bézier) and `lift.h` ships exact extrusion and revolution. None of it is reachable from a document: the tape has no profile or lift opcodes, so the whole profile-driven branch of SDF modelling — text, logos, imported SVG outlines, turned/lathed shapes — is unavailable to the app, to `pyclay`, and to every backend.

This is the largest capability still missing per unit of work, and it is the one the ClaySpace app will want first: extruded text and lathed profiles are staple authoring operations that no combination of the current 14 primitives can express.

## What Changes

- **Profiles in the tape**: a profile descriptor (type + parameters) covering circle, box, hexagon, equilateral triangle, trapezoid, vesica, and the arbitrary polygon. Polygon vertices live out-of-line, as stroke points already do.
- **Two lift opcodes**: `extrude` (exact, half-depth along Z) and `revolve` (exact, profile offset from the Y axis), each evaluating its profile in the lifted frame.
- **Out-of-line pool renamed**: the tape's `strokes` buffer becomes `blob`, since it now carries stroke points *and* polygon vertices. A pure rename across the tape, the compiler, and the three GPU kernel entry points — the name was accurate when strokes were its only payload and is misleading now.
- **Influence bounds** for lifted items: the profile's 2D bound extruded along the depth axis, or swept into an annulus for revolution.
- **Scene + Python**: `Prim::extrude(profile, half_depth)` / `Prim::revolve(profile, offset)` with `clay.Extrude(...)` / `clay.Revolve(...)` and `clay.Polygon(points)` taking an `(N,2)` array; profiles round-trip through `.clayspace`.

## Capabilities

### Modified Capabilities

- `sdf-kernels`: the 2D-profile and lift requirements gain the tape-reachability guarantee they currently lack.
- `scene-model`: influence bounds cover lifted items.
- `python-bindings`: profiles and lifts join the module's API surface.

### New Capabilities

_None._

## Impact

- `include/clay/kernel/tape.h`, `include/clay/scene/types.h`, `src/scene/{bounds,tape_build,commands}.cpp`, the three GPU kernel entry points (rename only), `bindings/python/pyclay_module.cpp`, tests, and the reference tree evaluator.
- Backends inherit lifts through the shared tape header; parity is the check.
- Non-goals, each with a reason rather than an omission:
  - **Segment and Bézier profiles** stay header-only: they are *unsigned* curve distances, not closed regions, so extruding one needs a thickness/rounding semantic that deserves its own design pass. Curves reach documents today by flattening to a polygon host-side (`math/bezier.h` already converts cubics to quadratic chains).
  - **`extrude_to` (loft)** needs two profiles per item and a second profile slot; deferred.
  - Text layout and SVG parsing are out of scope — they produce polygons, which this change consumes.
