# Tasks: add-voxel-repair

- [x] 2.1 Exterior flood over empty cells from outside the bounds
- [x] 2.2 `repair_report`: void count, total, largest, airtight — non-destructive
- [x] 2.3 `repair_close_holes(passes)` on the shared pocket rule; never removes material
- [x] 2.4 `repair_fill_voids()`, colouring from the enclosing shell
- [x] 2.5 Mask gating for both repairs
- [x] 2.6 Both bindings
- [x] 2.7 Tests: hollow reports a void, solid is airtight, perforated is not enclosed, seal then enclose, large opening survives, closing adds only, open cavity survives, fill colours from the shell, mask spares a void, C-vs-Python
- [x] 2.8 Docs, example, full verification
