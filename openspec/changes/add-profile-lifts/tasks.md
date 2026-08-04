# Tasks: add-profile-lifts

- [x] 1.1 Rename the tape's out-of-line float pool `strokes` -> `blob` (tape, compiler, three GPU kernel entry points, reference evaluator)
- [x] 1.2 Tape: `CProfileType` + `ctape_profile_dist`; `ctape_extrude` / `ctape_revolve` opcodes evaluating a profile in the lifted frame
- [x] 1.3 Scene: `Profile` descriptor with out-of-line polygon vertices; `Prim::extrude` / `Prim::revolve`; compiler emission
- [x] 1.4 Bounds: local bounds from the profile (extrusion slab, revolution annulus)
- [x] 1.5 Serialization + reference tree evaluator carry profiles
- [x] 1.6 Tests: extrude(circle) == capped cylinder, revolve(circle) == torus, concave polygon sign/shape, exactness preserved, bound conservativeness + culled-tape identity, round trip
- [x] 1.7 Python: profile objects, Extrude/Revolve, numpy polygon input, error for open curves, tests
- [x] 1.8 Docs + full verification (all presets, python ON/OFF, release checklist)
