# Tasks: expose-layer-enumeration

- [x] 1.1 Confirm what the document already stores and round-trips: name, kind, visibility, protection and stack order all survive `write_layer`/`read_layer` and layers serialize in vector order — exposure only, no format change
- [x] 1.2 C ABI: `clay_document_layer_count` / `clay_document_layer_at` in stack order; `clay_document_layer_info` as an OUTPUT descriptor with leading `struct_size`; `clay_layer_name` by the size-query pattern; `clay_layer_representation` matching the layer record's kind byte
- [x] 1.3 Parity gate: drop the now-stale `Layer.name` exemption — `clay_layer_name` satisfies it
- [x] 1.4 Regression test: both representations plus a mesh layer, hide/lock/ghost, a reorder, a removal, then save/load and enumerate — count, stack order and every info field survive; the removed id is refused; the struct_size and buffer-too-small refusals are typed
- [x] 1.5 Docs: `docs/05-claycore-library.md` §11 gains the discovery paragraph
