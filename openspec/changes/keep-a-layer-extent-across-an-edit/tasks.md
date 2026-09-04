## 1. The cache

- [x] 1.1 `LayerExtentCache` holding the union of everything except one item.
- [x] 1.2 Lazy: an edit records, a query pays.
- [x] 1.3 An item that becomes unbounded falls back to the walk.

## 2. Deciding what an edit touched

- [x] 2.1 `command_edited_item`, called by both the wiring and the gate.

## 3. Gates

- [x] 3.1 Exhaustive: every command kind leaves the cache agreeing with the walk.
- [x] 3.2 A drag pays one walk and then none.
- [x] 3.3 A dragged cutter that sticks out is still free — the case the first
      design failed.
- [x] 3.4 The held-out item shrinking shrinks the extent.
- [x] 3.5 A layer with no intersect still never walks.

## 4. Wiring

- [x] 4.1 One cache survives `apply_edit`; `touch()` invalidates it.
- [x] 4.2 Reuses report queries answered, not edits that happened not to walk.
