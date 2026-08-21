# Tasks: add-history-budget

## 1. Measure before designing

- [x] 1.1 Confirm there is no bound today: `UndoStack` holds
      `std::vector<Entry> undo_` / `redo_` with no cap, no byte accounting and
      no eviction; the only control is `enable_undo`
- [x] 1.2 Confirm the shape of an entry: `sizeof(Command)` is 128 bytes inline,
      `sizeof(Node)` is 440, so the inverse of a REMOVAL (an `AddNodeCmd`
      carrying a node) is the expensive one and the inverse of an ADD is 8
      bytes. A session of deletes and a session of adds cost very differently
- [ ] 1.3 MEASURE a real session before choosing a default: bytes per stamp on
      the ordinary sculpt path, and bytes for a stroke that coalesces against
      one that does not. Publish the numbers in `design.md`. A default chosen
      without them is a guess with a unit attached

## 2. Decide

- [ ] 2.1 DECIDE and record: does the budget cover redo as well as undo? Redo
      is discarded on the next edit, so it is transient — but it is real memory
      between an undo and the next action
- [ ] 2.2 DECIDE and record: is the default unbounded, or a number? Unbounded
      preserves today's behaviour exactly and leaves every existing host
      exposed; a default changes behaviour under hosts that never asked. State
      which risk is being taken and why
- [ ] 2.3 DECIDE and record: how a host distinguishes a trimmed history from an
      exhausted one. A counter of dropped steps, a flag, or a horizon id — the
      cheapest thing that lets a UI grey out a menu item honestly

## 3. Build

- [ ] 3.1 Byte accounting on `Entry`, walking what the commands own rather than
      `sizeof`. The deformer chain and stroke points are the payload that matters
- [ ] 3.2 Budget and eviction from the oldest end, with the invariant that a
      non-empty history always undoes its most recent step
- [ ] 3.3 On-demand trim
- [ ] 3.4 One query for bytes and depths, matching the shape
      `clay_document_undo_state` already has rather than inventing a second

## 4. Prove it

- [ ] 4.1 The five scenarios in the spec delta
- [ ] 4.2 A LONG-SESSION test: drive thousands of stamps under a budget and
      assert the history stays inside it. This is the regression for the defect
      itself, and the only test that would have caught it
- [ ] 4.3 Assert an unset budget is bit-identical to today over the golden
      corpus, so a host that ignores this change is genuinely unaffected

## 5. Reach it and say it

- [ ] 5.1 C ABI, with a versioned descriptor per the `struct_size` rule
- [ ] 5.2 pyclay, so `check_binding_parity` stays clean
- [ ] 5.3 `docs/05-claycore-library.md` gains the memory section it does not
      have: brick cache, history, and what a host should do on a pressure
      warning — the three do not currently appear together anywhere
