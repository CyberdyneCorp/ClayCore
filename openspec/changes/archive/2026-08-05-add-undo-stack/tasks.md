# Tasks: add-undo-stack

## 1. Python
- [x] 1.1 Opt-in stack per document; edits record their inverse automatically
- [x] 1.2 `undo` / `redo` / depths / begin+end group
- [x] 1.3 pytest: bit-identical restore via serialization, redo, stroke coalescing as one step, grouping, new edit clears redo, disabled costs nothing

## 2. C ABI
- [x] 2.1 The same surface; empty stack reports rather than fails
- [x] 2.2 C-vs-pyclay equivalence on an identical edit/undo sequence
- [x] 2.3 ABI 0.5.0 across the three version sites

## 3. Close-out
- [x] 3.1 Parity gate mapping entries
- [x] 3.2 Docs: `docs/05`, README, an example that edits and undoes
- [x] 3.3 Full verification: presets, gates, gallery, release checklist, CI
