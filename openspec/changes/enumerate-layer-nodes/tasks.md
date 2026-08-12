# Tasks: enumerate-layer-nodes

- [x] 1.1 Confirm the order already exists and already round-trips: `SdfContent::roots` IS the layer's evaluation order, the writer walks from it and the reader rebuilds it — exposure only, no format change
- [x] 1.2 C ABI: `clay_layer_node_count` / `clay_layer_node_at` over the layer's TOP-LEVEL nodes in evaluation order, mirroring `clay_document_layer_count` / `_at`; an index at or past the count and a non-layer id are both `CLAY_ERROR_NOT_FOUND`; a voxel or mesh layer counts 0 rather than failing, the reading `clay_layer_eval_points` makes of it
- [x] 1.3 Header states the depth plainly: top level only, the sibling of `clay_layer_children`, with the pairing that walks the whole tree spelled out and the reason node 0 is not the layer root
- [x] 1.4 Parity gate: nothing to move — the gate fails on a pyclay capability with no C counterpart, and this adds to the C side
- [x] 1.5 Regression test (`tests/unit/test_c_layer_nodes.cpp`): an armature placed after a run of REMOVED nodes, saved, reloaded, and found through layer enumeration → node enumeration → `clay_layer_node_prim` with no id probing and no gap constant, then both halves read back exactly; plus the top-level-only pinning against a group with items, the id-gap case the probe misses, `clay_layer_children` still refusing node 0, the typed refusals (non-layer id, index past the count, null out-params) and a hidden/ghosted/locked layer answering normally
- [x] 1.6 Docs: `docs/05-claycore-library.md` §11 and the placed-node readback paragraph
