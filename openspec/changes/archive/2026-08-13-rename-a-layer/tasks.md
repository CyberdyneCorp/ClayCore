# Tasks: rename-a-layer

- [x] 1.1 Confirm the name already round-trips: `write_layer`/`read_layer` record it length-prefixed, so this is a missing mutation rather than missing persistence — no format minor moves
- [x] 1.2 Scene model: `scene::SetLayerNameCmd` in the one command vocabulary, inverse = the previous name, `edited_layer` reporting the layer so a protected one refuses it; serialize/deserialize with the tag appended
- [x] 1.3 C ABI: `clay_document_set_layer_name` routed through `apply_edit`, refusing NULL and the empty string, with the duplicate-name rule stated in the header rather than left to be discovered
- [x] 1.4 Regression test: create, rename, save, reload, read the name back — the issue's "lost on save", across all three representations
- [x] 1.5 Cover the rest: undo/redo, the voxel lookup moving to the new name, the duplicate shadowing the earlier layer in stack order, a protected layer's refusal, bad id, NULL, empty, a long name and a UTF-8 name
- [x] 1.6 Docs: `docs/05-claycore-library.md` §11 gains the rename paragraph
