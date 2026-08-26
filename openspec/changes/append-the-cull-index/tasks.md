# Tasks

- [x] 1.1 `CullIndex::append`, extending every chain over the appended root list and re-deriving the pad
- [x] 1.2 Refuse on the same terms `compile_document_append` refuses on, writing nothing until every check has passed
- [x] 1.3 `Doc::cull_index()` in the C ABI takes the fast path, with its own append log rather than the tape's
- [x] 1.4 Tests: per-brick tape byte-identity against a fresh index over the adversarial corpus — one dab, several, a group, a feathered replace, a pad-widening blend, and a whole stroke carried on one index
- [x] 1.5 Tests: every refusal, and that a refused index is still usable
- [x] 1.6 C ABI tests through `clay_eval_grid`, which is the read that consults the index: a stroke, several appends absorbed by one read, and an undo falling back to a rebuild
- [x] 1.7 `BM_CullIndexAppend` / `BM_CullIndexRebuild` and the `check_bench.py` pair
- [x] 1.8 Update `docs/` where the index's per-revision rebuild is described
