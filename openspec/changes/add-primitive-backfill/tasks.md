# Tasks: add-primitive-backfill

- [x] 1.1 Tape: 14 opcodes + dispatch (plane, tetrahedron, dodecahedron, icosahedron, link, capped torus, solid angle, cut sphere, cut hollow sphere, exact cone, infinite cylinder, tri prism, cheap octahedron, L-norm sphere); the _ab endpoint variants stay kernel-only (8 params, no new capability)
- [x] 1.2 Scene: `Prim` constructors, `prim_is_unbounded` / `prim_is_bound_field` predicates in one place
- [x] 1.3 Bounds: local bounds per primitive; unbounded ones report infinite influence
- [x] 1.4 Exactness: bound-only primitives downgrade tracked field info (generalize the ellipsoid special case)
- [x] 1.5 Tests: tape-vs-kernel for every primitive, unbounded never culled, bound downgrade, meshing sanity, round trip
- [x] 1.6 Python classes + a coverage test that enumerates them
- [x] 1.7 Docs + full verification
