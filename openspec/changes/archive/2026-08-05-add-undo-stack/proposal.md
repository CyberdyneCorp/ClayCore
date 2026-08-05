# Proposal: expose the undo stack

## Why

`scene::UndoStack` exists, works, and no binding reaches it. It applies a
command, records the inverse, coalesces consecutive stroke appends into one
step, and groups arbitrary commands via `begin_group`/`end_group`. A sculpting
app needs exactly that, and today ClaySpace would have to reimplement it in
Swift over an API that — until `add-edit-commands` — could not even express an
edit.

Reimplementing it is the bad outcome, and not only for the duplicated work: the
engine's undo shares one command vocabulary with the document file format, so a
second implementation in the app would drift from what a saved document records
and the two would disagree about what a step is.

## What Changes

- **Undo is opt-in per document.** A document with no stack behaves exactly as
  it does today, so nothing pays for undo it does not use.
- **Edits record automatically** once a stack is attached. The caller does not
  author `Command` values; every editing entry point from `add-edit-commands`
  records its own inverse. That keeps the binding surface small and makes it
  impossible to perform an edit that undo does not know about.
- **`undo()` / `redo()`**, plus the depths, so a UI can enable and label its
  buttons.
- **Grouping**: begin/end around a burst of edits so a compound gesture — a
  mirrored placement, a multi-item transform — undoes as one step.
- **Stroke coalescing is already in the engine** and is exercised rather than
  reimplemented: appending N points to a stroke undoes in one step.

## Capabilities

### Modified Capabilities

- `scene-model`: the undo requirement gains reachability from the bindings.
- `python-bindings` and `c-abi`: undo, redo, grouping and depths.

## Impact

- `bindings/python/pyclay_module.cpp`, `bindings/c/clay.h`, `bindings/c/clay_c.cpp`, tests, docs.
- Depends on `add-edit-commands`: an undo stack over a vocabulary the caller
  cannot invoke is worth nothing.
- ABI 0.5.0 — additive.
- Non-goals: persisting the undo history into `.clayspace` (the format records
  document state; a history chunk is a separate design), and cross-process or
  collaborative editing, which needs command transport and conflict rules
  rather than a stack.
