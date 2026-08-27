# Tasks

- [x] 1.1 The eviction order becomes a list whose nodes the entries own, so an entry can be removed from the middle and moved to the back in O(1)
- [x] 1.2 `touch_region` removes an invalidated entry's node with the entry
- [x] 1.3 The order is refreshed wherever a seed is used: handed out, rewritten by the resumed path, re-stored by the full path
- [x] 1.4 Eviction takes the front and drops the "never the brick just written" special case
- [x] 1.5 The byte accounting sums capacity, so the budget bounds what the store has allocated
- [x] 1.6 The budget becomes a member, settable and observable only through `bindings/c/clay_internal.h`
- [x] 1.7 Regression test: the order's size equals the entry count across region-invalidating edits (fails before: 28 nodes against 4 entries after six edits)
- [x] 1.8 Regression test: under a budget sized to what is held, the brick rewritten by every dab survives and the one touched once is dropped (fails before: the rewritten brick is the one evicted)
- [x] 1.9 ASan/UBSan over the touched suites, and `docs/05-claycore-library.md`
- [x] 1.10 The budget's carve-outs are stated rather than left to be found: the most recently used seed is kept whatever the budget, and `store_active` does not evict — with a regression test pinning the first
