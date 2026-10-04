## Context
Three constructors take caller arrays: `from_triangles`, `from_quads`, and now `from_arrays`. The issue sketches a descriptor rather than a ten-argument call, which is also what lets the struct grow (a tangent channel, a second uv set) by appending behind `struct_size`.

## Decisions
- **One copy path.** `start_mesh` (positions, non-null, non-zero), `take_triangles`, `take_quads` (validation plus the derived triangulation) and `take_attributes`. `from_triangles` / `from_quads` are `start_mesh` plus one take; `from_arrays` is `read_desc`, `start_mesh`, `take_index_kind` and `take_attributes`. Each function stays well under the backend complexity target.
- **Exactly one index kind, decided by pointer OR count.** Treating a count without its pointer as "absent" would turn a host's NULL-buffer bug into a silent mesh of the other kind, or into a refusal with the wrong message. Both kinds is refused rather than preferring one: triangles passed alongside quads either equal the derived ones and are redundant, or differ and would break the invariant `mesh_data.h` states.
- **The original layout is the whole struct** (`read_desc(in, sizeof(clay_mesh_arrays), ...)`). The struct is new, so no caller can have compiled against a shorter layout.
- **No length validation for attributes.** It is impossible through a pointer, and the header says so next to the call.

## Rejected
- Setters on a built mesh (`clay_mesh_set_uvs`). A mutating setter on a handle that may be BORROWED from a document would need the layer-revision and undo story the constructor avoids. A host-owned mesh built in one call has none.
