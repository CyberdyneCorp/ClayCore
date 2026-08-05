# Tasks: add-edit-commands

## 1. Python
- [ ] 1.1 Node edits: set transform / prim / colour / op+blend+rounding, move, remove
- [ ] 1.2 Layer edits: add, remove, reorder, visible, transform
- [ ] 1.3 Stroke edits: append points, trim last N
- [ ] 1.4 Each entry point applies exactly one `scene::Command` via `scene::apply`
- [ ] 1.5 pytest: id stability, layer visibility round trip, stroke edit equivalence, unknown id refused

## 2. C ABI
- [ ] 2.1 The same surface, addressed by id, `CLAY_ERROR_NOT_FOUND` on a missing id
- [ ] 2.2 Editing a primitive preserves the item's modifiers
- [ ] 2.3 C-vs-pyclay equivalence tests; error paths return codes, never crash
- [ ] 2.4 ABI 0.4.0 across the three version sites

## 3. Close-out
- [ ] 3.1 Parity gate mapping entries for the new capabilities
- [ ] 3.2 Note in the gate's docs that it proves non-drift, not completeness against the engine
- [ ] 3.3 Docs: `docs/05`, README, an example that edits rather than only builds
- [ ] 3.4 Full verification: presets, gates, gallery, release checklist, CI
