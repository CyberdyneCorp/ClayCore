# Tasks

- [x] 1.1 `scene::compile_layer_suffix` — `compile_document_append`'s compile without the prefix copy, refusing on the same terms
- [x] 1.2 `eval::eval_points_seeded` — the CPU blocked walk started with the accumulator on the stack
- [x] 1.3 Stack depth for a seeded walk computed from a simulation that starts at one, not by adding one to the unseeded depth
- [x] 1.4 Bit-identity test against the whole document, with and without a layer mirror
- [x] 1.5 Tests for the empty suffix and for every refusal
- [x] 1.6 `BM_DabSuffixSeeded` / `BM_DabFullWalk` and the `check_bench.py` pair
- [x] 1.7 Document what is NOT here: the store for the seed, and why the brick cache cannot be it
