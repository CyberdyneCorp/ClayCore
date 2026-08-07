# Tasks: add-snakehook

- [x] 1.1 `brush::snakehook`: anchor + normal + drag path resolve to a stroke item
- [x] 1.2 ~~Anchor inside the surface~~ — the premise was wrong, see 1.8
- [x] 1.3 Taper by ARC LENGTH, not sample index, so gesture speed does not shape it
- [x] 1.4 A tip radius floor, so the tendril ends rather than vanishing
- [x] 1.5 Python bindings
- [x] 1.6 Tests: it attaches, it tapers, an uneven drag gives the same tendril as an
      even one, a short drag still leaves a mark, degenerate input is refused, and
      the field stays exact
- [x] 1.7 Docs, example, full verification

Found while building:

- [x] 1.8 The anchor was to be pushed INSIDE the surface, on the theory that one
      sitting exactly on it would leave a neck where the two fields meet. It
      does not: the sweep from a surface point already overlaps the body by its
      own radius, so a deeper anchor only adds material where the body is solid
      anyway — the field around the base measured identical at depths 0.0, 0.5
      and 1.0. The parameter was removed rather than kept as a knob that does
      nothing, and what the anchor really buys was written down instead: the
      tendril begins where the user TOUCHED, not at the first drag sample, which
      arrives a frame later with the finger already moving.
- [x] 1.9 `taper_curve` was documented backwards. The radius goes as (1 - t)
      raised to it, so above 1 thins away quickly and below 1 holds the
      thickness — the opposite of what the comment said.
