# Tasks: bound-what-undo-changed

## 1. The engine computes what a command targets

- [x] 1.1 Add `command_influence_bound(const Document&, const Command&)` to
      `scene/commands.h`: the world-space influence bound of what the command
      targets in the document AS IT IS NOW. Node commands report the root
      ancestor's bound, unioned over every layer sharing the content; layer
      commands report that layer's bound; commands that cannot change the field
      (rename, protection) report nothing
- [x] 1.2 Give `UndoStack::undo` and `UndoStack::redo` an optional out-bound,
      accumulated as before ∪ after around each `apply` — defaulted to null so
      every existing caller compiles and behaves unchanged

## 2. The ABI reports it

- [x] 2.1 Add `clay_document_undo_bound` / `clay_document_redo_bound`, writing
      the bound through the same `write_influence` three-state shape the
      influence-bound queries use
- [x] 2.2 Leave `clay_document_undo` / `clay_document_redo` alone, and document
      in the header why a cache-keeping host wants the new pair

## 3. Tests

- [x] 3.1 The bound of an undone add contains the item and is smaller than the
      layer — the case from #210
- [x] 3.2 A move is covered at both ends; a removal is covered by the node that
      comes back
- [x] 3.3 A child of a blended group reports the group's bound, so the blend
      seam is not cut off
- [x] 3.4 An edit through one instance covers every layer sharing the content
- [x] 3.5 An unbounded node reports the unbounded state, not a finite box
- [x] 3.6 A rename reports nothing to dirty; an empty stack reports nothing
      undone, nothing to dirty, and `CLAY_OK`
- [x] 3.7 The reporting variants agree with the plain ones: same `undone` flag,
      and a document that serializes bit-identically

## 4. Version and docs

- [x] 4.1 ABI 0.40.0 — `CLAY_ABI_MINOR`, `CMakeLists.txt`. `kSceneMinor` and
      `kClaySpaceMinor` are NOT touched: no command and no document field
      changed
- [x] 4.2 Note the pair in the library docs beside the brick cache's refill loop
