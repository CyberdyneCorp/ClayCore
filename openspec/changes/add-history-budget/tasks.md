# Tasks: add-history-budget

## 1. Measure before designing

- [x] 1.1 Confirm there is no bound today: `UndoStack` holds
      `std::vector<Entry> undo_` / `redo_` with no cap, no byte accounting and
      no eviction; the only control is `enable_undo`
- [x] 1.2 Confirm the shape of an entry: `sizeof(Command)` is 128 bytes inline,
      `sizeof(Node)` is 440, so the inverse of a REMOVAL (an `AddNodeCmd`
      carrying a node) is the expensive one and the inverse of an ADD is 8
      bytes. A session of deletes and a session of adds cost very differently
- [x] 1.3 MEASURE a real session before choosing a default: bytes per stamp on
      the ordinary sculpt path, and bytes for a stroke that coalesces against
      one that does not. Publish the numbers in `design.md`. A default chosen
      without them is a guess with a unit attached

## 2. Decide

- [x] 2.1 DECIDED: yes, and redo is spent FIRST — it is transient, since the
      next edit discards it anyway, so spending the budget on it before the
      undo the user can actually reach is the wrong trade.
      Original question: Redo
      is discarded on the next edit, so it is transient — but it is real memory
      between an undo and the next action
- [x] 2.2 DECIDED: unbounded. A default would change behaviour under hosts
      that never asked, and this library's rule is that a host ignoring a
      change is unaffected by it.
      Original question: Unbounded
      preserves today's behaviour exactly and leaves every existing host
      exposed; a default changes behaviour under hosts that never asked. State
      which risk is being taken and why
- [x] 2.3 DECIDED: a `dropped_steps` counter, monotonic for the session. The
      cheapest thing that lets a UI show a horizon honestly.
      Original question: A counter of dropped steps, a flag, or a horizon id — the
      cheapest thing that lets a UI grey out a menu item honestly

## 3. Build

- [x] 3.1 Byte accounting that walks what is OWNED rather than `sizeof` —
      `scene::command_bytes`, `UndoStack::undo_bytes/redo_bytes`,
      `VertexDeltas::bytes`, and the cell runs of voxel and mask steps
- [x] 3.2 Budget and eviction from the oldest end, with the invariant that a
      non-empty history always undoes its most recent step
- [x] 3.3 On-demand trim
- [x] 3.4 One query for bytes and depths, matching the shape
      `clay_document_undo_state` already has rather than inventing a second

## 4. Prove it

- [x] 4.1 The scenarios in the spec delta
- [x] 4.2 A LONG-SESSION test: drive thousands of stamps under a budget and
      assert the history stays inside it. This is the regression for the defect
      itself, and the only test that would have caught it
- [x] 4.3 Assert an unset budget is bit-identical to today over the golden
      corpus, so a host that ignores this change is genuinely unaffected

## 5. Reach it and say it

- [x] 5.1 C ABI, with a versioned descriptor per the `struct_size` rule
- [x] 5.2 pyclay, so `check_binding_parity` stays clean
- [x] 5.3 `docs/05-claycore-library.md` gains the memory section it did not
      have: brick cache, history, and what a host should do on a pressure
      warning — the three do not currently appear together anywhere

## 6. What building it changed

- [x] 6.1 The spec was written when the history held SDF edits alone. It now
      holds FOUR step kinds and a journal, and the journal keeps its own copy of
      every payload — so a session with crash recovery on holds roughly twice
      what one without does. That is reported separately rather than folded
      into one number, because it is a cost a host can act on with a different
      lever (`journal_trim`)
- [x] 6.2 The budget deliberately does NOT evict from the journal. Those bytes
      are the host's crash recovery, and dropping them silently would lose
      exactly what that feature exists to keep — the same silent-loss failure
      mode `survive-a-crash` refuses a journal to avoid
- [x] 6.3 THREE fixtures of mine were degenerate before this landed and each
      was caught by an assertion rather than by review: a smooth of the CENTRE
      of a solid block (changes nothing), a snapshot of an EMPTY document
      (measures the opposite of the real case), and a 50-base cycle with a
      parity that made every base always receive the SAME value — because 50 is
      even, so i and i+50 share a parity. The pattern is worth naming: a
      fixture that does not do what it claims produces a number that looks fine
