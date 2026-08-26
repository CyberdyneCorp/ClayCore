# Tasks

- [x] 1.1 Fuse `dual_grid_mesh`'s vertex and quad passes into one walk, taking the quad pass's four corner reads from the eight the vertex pass already has
- [x] 1.2 Template it on the sampler, so the tape-level meshers' lambda inlines; leave the public `mesh_lattice_*` signatures taking a `std::function`
- [x] 1.3 Prove byte-identity across all five entry points — `mesh_tape_nets`, `mesh_tape_dc`, `mesh_tape_quads`, `mesh_lattice_nets`, `mesh_lattice_dc` — by hashing positions, indices, quads and normals before and after
- [x] 1.4 Regression test for the ordering property the fusion rests on: the largest index in successive quads never goes backwards
- [x] 1.5 `BM_MeshLatticeMarch` / `BM_MeshLatticeNets` over one precomputed lattice, and restore the `check_bench.py` pair on those rather than on the tape meshers
- [x] 1.6 Record in `check_bench.py` why the old tape pair is not the one restored
- [x] 1.7 Update the spec: separate "cheap to build" from "cheap to carry", and say what a gate must exclude to measure the first
- [x] 1.8 Update `docs/` where the preview mesher's cost is described
