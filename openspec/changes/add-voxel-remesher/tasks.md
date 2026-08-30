# Tasks: add-voxel-remesher

## 1. Decide first

- [x] 1.1 DECIDE the cancellation shape. `parallel::CancelToken`, not the
      callbacks the source guide proposed: `clay.h` carries no function
      pointers, a callback would fire on a pool worker, and `sample_blocks`
      already takes a token
- [x] 1.2 DECIDE where mask lives. It is a caller-owned per-vertex gate and
      stays one; the remesh gets `mesh::transfer_vertex_scalar` beside the
      transfer it belongs with, rather than mask moving into `mesh::Mesh`
- [x] 1.3 DECIDE whether the sparse domain may differ from a dense evaluation.
      It may not, for a closed source — the stored samples are bit-identical
      and a test asserts it; for an open source the far-brick sign may differ
      and the requirement says so
- [x] 1.4 DECIDE that the pure operation ships without the document. No layer,
      no history, no revision token; those are a later `scene-model` change

## 2. Core API

- [x] 2.1 `include/clay/mesh/voxel_remesh.h` — params, estimate, report,
      status, result; the resolution modes, surface modes, open-surface and
      small-component policies; band and padding as named internal constants
- [x] 2.2 Parameter validation: non-finite or non-positive voxel size, zero
      longest-axis resolution, negative thresholds, out-of-range strengths,
      and `build_multires_levels != 0` refused as unsupported
- [x] 2.3 `mesh::voxel_remesh_estimate` — resolved voxel size, grid dims,
      active samples, memory, triangle range, boundary edges, components,
      thin-feature warning, budget verdict; no field, no BVH, no allocation
      proportional to the request
- [x] 2.4 Library ceilings on active samples and lattice cells, independent of
      the caller's budget

## 3. Sampling

- [x] 3.1 Active-brick marking from source triangle AABBs dilated by the band,
      over the same brick lattice `FieldVolume::sample_blocks` builds
- [x] 3.2 Sign for the complement: connected components of inactive bricks, one
      generalized-winding query per component, deterministic representative
- [x] 3.3 The `BrickBlockFill` — BVH signed distance across the pool for active
      bricks, a constant fill for the rest — through `sample_blocks`, so the
      converter is reused and not forked
- [x] 3.4 Cancellation at the window boundary and inside the fill; progress
      through the token
- [x] 3.5 Prove the equivalence: stored bricks and stored samples bit-identical
      to `mesh::to_field` at the same region, cell and band

## 4. Extraction

- [x] 4.1 Promote `mesh_lattice_parallel` to `mesh/marching.h` with its
      concurrency precondition stated
- [x] 4.2 March the volume's own lattice, reading stored samples exactly and
      the far bound elsewhere, with one ring of positive samples around it
- [x] 4.3 Sharp mode through `mesh_lattice_dc`, marked experimental, excluded
      from the watertight contract
- [x] 4.4 Connected components of the result; the small-component policy

## 5. Projection, attributes, volume

- [x] 5.1 Constrained projection: distance clamp, normal-compatibility
      rejection, strength as a lerp, parallel per vertex
- [x] 5.2 Colour through `transfer_attributes`; UVs dropped; malformed source
      arrays treated as absent
- [x] 5.3 `mesh::transfer_vertex_scalar` in `mesh/transfer.h`
- [x] 5.4 Volume measurement, the clamped optional correction, and the cases
      that skip it

## 6. Validation and reporting

- [x] 6.1 Validate the result; enforce the contract per policy and surface mode
- [x] 6.2 Fill the report; make every failure a distinct status

## 7. Bindings

- [x] 7.1 C ABI: `clay_voxel_remesh_params` / `_estimate` / `_report`,
      `clay_mesh_voxel_remesh_defaults`, `_estimate`, `clay_mesh_voxel_remesh`,
      `clay_mesh_transfer_vertex_scalar`; bounded descriptor fills
- [x] 7.2 Distinct result codes for the distinct failures
- [x] 7.3 pyclay: `voxel_remesh`, `voxel_remesh_estimate`,
      `transfer_vertex_scalar`; named results, exceptions naming their cause
- [x] 7.4 Binding parity and the C ABI check pass

## 8. Tests

- [x] 8.1 Fixtures: sphere, cube, chamfered cube, torus, thin plate, long thin
      cylinder, overlapping spheres, cube+sphere overlap, nested shells,
      self-intersecting fold, open sphere, inconsistent winding, disconnected
      islands, tiny floating component, stretched sculpt, coloured mesh
- [x] 8.2 Geometry: fusion, watertightness, manifoldness, orientation, no NaN,
      resolution monotonicity of surface error
- [x] 8.3 Resources: invalid resolutions refused, oversized request refused
      before allocation, long-thin fixture proves the sparse scaling
- [x] 8.4 Sparse/dense equivalence on a closed source
- [x] 8.5 Cancellation non-destructive, mid-stage, per stage
- [x] 8.6 Determinism: repeated runs byte-identical
- [x] 8.7 Attributes: colour transferred, absent stays absent, malformed safe,
      mask resampled
- [x] 8.8 Projection: recovers detail, honours the clamp, refuses the far sheet
- [x] 8.9 Components and volume policy
- [x] 8.10 C ABI tests, including the short-descriptor fill
- [x] 8.11 pyclay tests
- [x] 8.12 `mesh_lattice_parallel` equals `mesh_lattice` byte for byte

## 9. Example

- [x] 9.1 `examples/67_voxel_remesh.py` — stretched source, intersecting
      shells, two resolutions, the estimate printed, the losses stated
- [x] 9.2 Committed renders; registered in `examples/run_all.py`

## 10. Performance

- [x] 10.1 Benchmark rows: sphere at 128 and 256, intersections at 256, the
      long-thin fixture at 256, projection, attribute transfer
- [x] 10.2 Baseline recorded; scaling with source triangles at fixed shape and
      resolution

## 11. Repository

- [x] 11.1 `CMakeLists.txt` sources; `tests/CMakeLists.txt` entries
- [x] 11.2 Version lines moved together: `CMakeLists.txt`, `bindings/c/clay.h`,
      `pyproject.toml`, and whatever `tools/release_check.py` reads
- [x] 11.3 Docs: `README.md`, `docs/05-claycore-library.md`,
      `docs/07-brushes-and-features.md`, `docs/sculpt_comparison.md`,
      `openspec/ROADMAP.md`
- [x] 11.4 `openspec validate --strict` clean
