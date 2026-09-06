# Tasks

- [x] 1.1 `scene::compile_document_part` emits one side of the layer split, both sides culling under the whole document's pad
- [x] 1.2 Validate the decomposition standalone: union(below, active) equals the whole-document compile, distance and colour, before building on it
- [x] 1.3 The seed keeps the half beneath as its own planes, carried forward untouched
- [x] 1.4 The suffix compiles with `doc_have_acc` false and the refill applies the union itself, through the kernel's own combine
- [x] 1.5 The cold path runs two batched passes; neither is extra work, since the items partition
- [x] 1.6 ABI tests: a stroke on the upper of two overlapping layers, a check that the lower one actually reaches the bricks, and an edit to the lower layer falling back
- [x] 1.7 Mutation-test that the multi-layer resumed path is actually taken
- [x] 1.8 Benchmark and `docs/`
