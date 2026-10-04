## Implementation
- [x] `clay_mesh_arrays` and `clay_mesh_from_arrays` in `bindings/c/clay.h`, with the vertex-aligned-or-absent invariant and what the call does not validate stated beside it.
- [x] `bindings/c/clay_c.cpp`: shared `start_mesh` / `take_triangles` / `take_quads` / `take_attributes`; `clay_mesh_from_triangles` and `clay_mesh_from_quads` rebuilt on them.
- [x] C ABI 0.123.0 -> 0.124.0 in `CMakeLists.txt`, `bindings/c/clay.h` and `pyproject.toml`.
- [x] `tests/unit/test_c_mesh_from_arrays.cpp`: attributes back bit-exactly; NULL attributes absent; quads derive the triangles `from_quads` derives; attach, undo/redo and save/load keep them; every refusal leaves `out_mesh` NULL.
- [x] Mutation check: dropping the attribute copy fails 4 of 5 cases (16 assertions); accepting both index kinds fails the refusal case.
- [x] `docs/08-mesh-readback.md` lists the constructor.
