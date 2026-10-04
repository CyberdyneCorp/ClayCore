## Why

`clay_document_mesh_combined` meshed the field first, through `clay_document_mesh`, and returned that call's error before it looked at a mesh layer. `clay_document_mesh` refuses an empty tape with `CLAY_ERROR_INVALID_ARGUMENT` ("empty document") and a field filtered to nothing by hidden surface groups with `CLAY_ERROR_BACKEND` ("meshing produced no triangles"). So a document whose only visible geometry is mesh layers -- every SDF layer hidden, or none at all -- could not be exported through the one call that exists to export mesh layers (#662). ClaySpaceDesktop worked around it by transforming and concatenating each visible mesh layer itself (CyberdyneCorp/ClaySpaceDesktop#213).

## What Changes

- With at least one visible mesh layer, an empty field contributes nothing to the combined export: the result is the placed mesh layers alone. "Empty" covers an empty tape and a tape whose mesh the hidden-group drop leaves with no triangles.
- With no visible mesh layer the call still returns exactly what `clay_document_mesh` returns, refusals included -- a document with nothing visible at all is still refused with `CLAY_ERROR_INVALID_ARGUMENT`.
- A malformed `clay_mesh_params` is refused whether or not the field is empty.
- No signature changes and no entry point is added: the C ABI version lines do not move.

## Capabilities

### Modified Capabilities

- `c-abi`: "Exporting a document with its imported meshes is explicit" gains the rule that an empty field contributes nothing when mesh layers are visible, and a scenario for it.

## Impact

- `bindings/c/clay_c.cpp`: `clay_document_mesh_combined` places the visible mesh layers first (`place_visible_mesh_layers`, `place_mesh_layer`), and `mesh_tape` takes an `EmptyField` mode the combined call sets to `ContributesNothing`. The two standalone meshing calls keep the default `Refuse`.
- `bindings/c/clay.h`: the comment above `clay_document_mesh_combined`.
- `docs/08-mesh-readback.md`: the "field + visible mesh layers" row.
- Hosts that fell back to `clay_mesh_transform_nonuniform` + `clay_mesh_concat` on refusal can drop the fallback; keeping it is harmless, since the call no longer refuses those documents.
