## 1. The serial

- [x] 1.1 `Document::content_serial`, advanced in `scene::apply` — the one funnel
      every command-based mutation reaches, so undo, redo and a replayed journal
      are covered without being enumerated
- [x] 1.2 Not advanced for a refused command
- [x] 1.3 The ABI's `touch()` advances it too, for the paths that mutate without
      a command (consolidation, the replay journal's non-command half)
- [x] 1.4 `revision` DELIBERATELY UNTOUCHED: the append log does arithmetic on it
      and drives the tape-prefix reuse, so an extra bump per edit would restart
      that log on every append

## 2. The memo

- [x] 2.1 Keyed on `content_serial`, so the two bound calls land either side of
      the apply and the memo left after one edit opens the next
- [x] 2.2 `clay_document_extent_stats` — walks and reuses

## 3. Gates

- [x] 3.1 THE COUNTER, not a timing: 20 walks and 20 reuses over a 20-frame drag
- [x] 3.2 A subtracting drag reports zero walks and zero reuses — the extent is
      unreached, not merely fast
- [x] 3.3 The staleness trap as a test: growing the layer grows the intersect's
      bound on the next query, and an UNDO puts it back — undo does not go
      through the ABI's funnel but does go through `scene::apply`, so a memo
      that only knew about the funnel would fail this
- [x] 3.4 2,340 unit cases; layering, c-abi, parity, gallery green
- [x] 3.5 ABI 0.80.0 -> 0.81.0 in the three version lines
- [ ] 3.6 CI green, the benchmark gate included — the append fast path is what
      route 1 risked and what `revision` being untouched protects

## 4. Left open

- [ ] 4.1 #451 IS NOT CLOSED. One walk a frame remains and 96% of it is
      `item_geometry_bound`; closing it means caching those per-item bounds,
      which is now unblocked because a cache keyed on `content_serial` is
      correct across an edit where one keyed on `revision` is not
