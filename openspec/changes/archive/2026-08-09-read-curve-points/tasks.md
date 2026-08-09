# Tasks: read-curve-points

- [x] 1.1 `clay_layer_stroke_points`: the setter's arguments with `size_t*` for the
      count, following the size-query convention `clay_cut_polygon_from_curve` uses
- [x] 1.2 `write_curve_points`, beside `read_curve_points`, so the readback and the
      setters cannot drift apart in what a point array means
- [x] 1.3 Reading answers on a ghosted, locked or hidden layer: protection refuses
      edits, and this is not one
- [x] 1.4 `SetStrokePointsCmd` accepts a swept guide, with the closed-guide refusal
      kept at both binding boundaries — the rule is an ABI/binding one, not a
      scene-model invariant, and refusing in the command would have reported it
      as a missing node
- [x] 1.5 Tests: a round trip through the ABI and through `.clayspace`; the readback
      is the current state, not the authored one; a profiled tube's guide reads back
      and edits; a short buffer reports what it needed and writes nothing; the error
      table; a protected layer still reads; an empty list is a count of zero
- [x] 1.6 Swift smoke: size query, fill, and a short buffer, so the symbol is proven
      exported and callable from a real host. The pure-C smoke reads the placed
      chain back too, which also shows the builder really copied its payload
- [x] 1.7 pyclay: `Layer.set_points` reaches a swept guide and refuses a closed one,
      so the widening does not open a door in one binding and not the other

Found while building:

- [x] 1.8 A group node cannot be created through the C ABI, so the getter's
      `is_group` guard is unreachable from the C tests. Kept anyway — the check
      costs nothing and a group's prim block is a sphere by default, which would
      otherwise read as a curve-less item by luck rather than by rule.
- [x] 1.9 The size query refuses a stray `out_types` rather than ignoring it:
      nothing sizes the parallel arrays when the point buffer is absent, so a
      caller that passed one meant to read and got the buffer wrong.
- [x] 1.10 Widening the command opened a SECOND back door, not just the closed one:
      `validate_item` refuses a swept item whose guide is under two points, and
      the placed-node path could then cut one down to one point. Nothing failed —
      `emit_swept` writes no record below two points, so the tube just vanished.
      Refused at both boundaries alongside the closed guard, since it is the same
      rule from the same place.
- [x] 1.11 `prim_carries_curve` on `PrimType`, beside `prim_carries_profiles`, so
      the ABI and the command ask the same question rather than each spelling out
      `Stroke || Swept`. The `Node::stroke` comment said "PrimType::Stroke only",
      which stopped being true when sweeps started using the field.
