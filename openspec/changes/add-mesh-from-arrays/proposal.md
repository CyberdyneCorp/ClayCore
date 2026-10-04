## Why
`clay_mesh_from_triangles` and `clay_mesh_from_quads` copy positions and indices and nothing else. A host that computed its own per-vertex attributes had no constructor that keeps them: ClaySpaceDesktop's retopology comes back from CyberRemesher with a UV layout (#661, ClaySpaceDesktop #213). The only entry point that attached uvs was the OBJ reader, so the host wrote vertex-aligned `v`/`vn`/`vt` text into memory and read it back through `clay_mesh_load_memory(..., "obj", ...)`. That works, since the floats round-trip and the reader keeps them vertex-aligned, but it is a text encode and parse standing in for a copy.

`mesh::Mesh` already stores `normals`, `colors` and `uvs`, each empty or vertex-aligned. `clay_mesh_normals` / `_colors` / `_uvs` already read them, and `mesh_stream` already writes and reads all three in a document. The gap is the constructor only; no data model or format changes.

## What Changes
- C ABI 0.123.0 -> 0.124.0: `clay_mesh_arrays`, a descriptor with a leading `struct_size`: `positions`/`vertex_count`, `normals`, `colors`, `uvs` (each NULL or vertex-aligned), `indices`/`index_count`, `quad_indices`/`quad_index_count`. And `clay_mesh_from_arrays(const clay_mesh_arrays*, clay_mesh**)`.
- Exactly one index kind. Triangles are taken as given; quads derive the triangles by the `(a,b,c),(a,c,d)` rule `clay_mesh_from_quads` already uses, so `quads_consistent` holds by construction. A kind is supplied when its pointer is non-NULL or its count non-zero.
- A NULL attribute leaves that attribute absent. A non-NULL one is copied for `vertex_count` entries, bit-exactly.
- Refused with `CLAY_ERROR_INVALID_ARGUMENT`, `out_mesh` left NULL: NULL descriptor or out pointer, a `struct_size` below the layout, NULL positions, zero vertices, neither or both index kinds, a count without its pointer, a count that is not whole triangles / quads, an index past the vertices.
- `clay_mesh_from_triangles` and `clay_mesh_from_quads` now go through the same position, triangle and quad helpers, so the three constructors cannot drift. Their accepted inputs and results are unchanged; only the `clay_last_error` text for a NULL pointer now names the one argument that was NULL.

## What building it found
- The helpers live inside the file's `extern "C"` block, where returning a `std::vector` trips `-Wreturn-type-c-linkage` under `-Werror`. They take an out-parameter instead.
- No attribute is length-checked, and none can be: a pointer does not say how long it is. That is the contract `positions` already had. The header states it beside the call, together with the values the call does not touch (no renormalising, clamping, wrapping, or NaN rejection).
- Undoing the mesh-layer creation keeps the payload, and the redo brings the attributes back unchanged. The attach copies the whole `mesh::Mesh`, so nothing in the layer path needed to learn about the new constructor.
- pyclay is not extended here. `check_binding_parity.py` runs pyclay -> C, so a C-only constructor passes it. pyclay has the same gap, though: `Mesh.normals` / `colors` / `uvs` are read-only views, and `Mesh.from_triangles` / `from_quads` take positions and indices only. The host that asked is a C host; a `Mesh.from_arrays` for Python is a follow-up, and it would be checked against this entry point by the gate's `clay_mesh_` prefix rule with no alias.

## Impact
Additive: a new descriptor and entry point, and the two existing constructors behave as before. No format minor change, since the attributes were already serialized for any mesh layer that carried them.
