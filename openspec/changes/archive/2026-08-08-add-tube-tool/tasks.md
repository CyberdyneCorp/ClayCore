# Tasks: add-tube-tool

- [x] 1.1 `brush::tube`: points + settings resolve to a stroke item, or a swept
      item when a profile is asked for
- [x] 1.2 A start/middle/end radius, distributed by ARC LENGTH
- [x] 1.3 The B-spline toggle is the point type, not a second curve kind
- [x] 1.4 Closed tubes
- [x] 1.5 Python bindings
- [x] 1.6 Tests: it follows the path; the radius varies where asked; a round tube
      stays EXACT and a profiled one does not; closed joins; the point type
      changes the shape; degenerate input is refused
- [x] 1.7 Docs and an example

Found while building:

- [x] 1.8 A round tube is tessellated by the RESOLVER and its points then marked
      hard, because the radius has to be distributed over the points the field
      will actually see. A sweep is not: it tessellates its own guide at compile
      time and distributes profiles along the result, so tessellating here would
      do it twice.
- [x] 1.9 The example first measured the taper at the B-spline's control points,
      where the curve does not pass — a B-spline APPROXIMATES them, which the
      section above it measures deliberately. The end read 0.000 and the taper
      assertion passed for the wrong reason. Measured on a hard chain instead,
      where the widths come out 0.284 / 0.218 / 0.062 against radii of 0.14 /
      0.09 / 0.03.
- [x] 1.10 Profiles cross the Python boundary as PyProfile, which carries its
      polygon points alongside the engine's Profile — unwrapping only the latter
      loses a polygon profile's shape. And the resolver duplicates a lone profile
      so a sweep has two to interpolate between, so the polygon list has to
      follow it.
