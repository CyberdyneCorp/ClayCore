# Tasks: add-layer-ghost-lock

- [x] 1.1 `ghost` and `locked` on Layer; serialization with backward compatibility
- [x] 1.2 A command that sets them, undoable like every other layer property
- [x] 1.3 Edits refuse a protected layer with a typed error; flag changes stay allowed
- [x] 1.4 Picking and surface snapping skip ghosted layers, not locked ones
- [x] 1.5 Both bindings
- [x] 1.6 Tests: evaluation unchanged, undo, round trip, older document loads unprotected, edits refused, protection reversible, ghost not picked, locked still picked, C-vs-Python
- [x] 1.7 Docs, ABI 0.14.0, full verification
