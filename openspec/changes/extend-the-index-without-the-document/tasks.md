# Tasks

- [x] 1.1 `scene::cull_pad_terms(node, layer)` as the one definition of either pad term, with the whole-layer walk and `cull_pad` folds of it
- [x] 1.2 `CullIndex` keeps a `CullPadTerms` per visible SDF layer and derives `pad_` as the maximum of their sums
- [x] 1.3 `CullIndex::append` raises only the touched layers' terms, from the appended subtree, descending into groups the build does not
- [x] 1.4 Refuse the append when the layer's node map did not grow by exactly that subtree, before anything is written
- [x] 1.5 `scene::append_cached` extends the cached index in place when it holds the only handle and copies otherwise, with `clay_document::cull_index_locked` calling it under the mutex every handle is taken under
- [x] 1.6 Tests: a group whose child widens the pad, an invisible group whose child does, and the cross-layer maximum-of-sums a global pair of maxima would get wrong
- [x] 1.7 Tests: an append the node map does not corroborate is refused
- [x] 1.8 `BM_CullIndexAppendSmall` / `BM_CullIndexAppendShared`, fixtures that do not grow with the machine, and the slope gate in `tools/check_bench.py`
- [x] 1.9 Tests: a stroke extends the cached index without copying it, and an append under a holder copies rather than mutating what the holder has
- [x] 1.10 Update `docs/05-claycore-library.md` where the index's append is described
