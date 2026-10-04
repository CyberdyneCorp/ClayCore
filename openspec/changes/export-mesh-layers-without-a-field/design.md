## Context

`clay_document_mesh_combined` is documented as "mesh the field, then append every visible mesh layer". It was implemented literally: field first, and any failure of the field returned. The field's failures are right for `clay_document_mesh`, which was asked for a surface and has none to give, and wrong for the combined call when the mesh layers are the surface.

## Decisions

### Gather the mesh layers first

Whether any mesh layer is visible decides what an empty field means, so the layers are placed before the field is meshed. With none visible the call delegates to `clay_document_mesh` unchanged, which keeps the header's "returns exactly what clay_document_mesh would" literally true, refusals included.

### An `EmptyField` mode on `mesh_tape`, not a second mesher path

The field can come out empty in two places inside `mesh_tape`: an empty tape (refused before any grid is priced) and a mesh that `drop_hidden` filtered to no triangles (every surface group hidden). Both must read as "nothing" for the combined call, and both must stay refusals for `clay_document_mesh` and `clay_document_mesh_sdf_layer`. A mode argument, defaulting to `Refuse`, keeps the one shared mesher -- resolution ceiling, hidden-group drop, decimation -- rather than duplicating its front half.

Rejected: calling `clay_document_mesh` and treating its errors as "empty". `CLAY_ERROR_INVALID_ARGUMENT` also means a malformed `clay_mesh_params` or an over-fine voxel size, and `CLAY_ERROR_BACKEND` also means a mesher failure; swallowing either would turn a real error into a silent export of the mesh layers alone.

### Parameters are still validated

`read_desc` runs before the field is considered, so a malformed parameter block is refused even when the field is empty and would not have used it. An unbounded but non-empty tape still refuses: that is a field that cannot be meshed, not an empty one.

## What building it found

- The two empty checks took `mesh_tape` from cognitive complexity 15 to 19. Its voxel-size pricing (caller's size or extent over resolution, the finite check, the grid-sample ceiling) moved verbatim into `price_mesh_voxel`; `mesh_tape` is 14 and `clay_document_mesh_combined` drops from 22 to 7 with the layer placement pulled out.

- The plan named the empty tape only. The hidden-group case (`clay_groups_set_visible` over the whole surface) reaches a different refusal, `CLAY_ERROR_BACKEND`, from a non-empty tape; the regression test for it failed on the pre-fix code with code 8 rather than 1, which is how it was confirmed to be the second path rather than the first one again.
