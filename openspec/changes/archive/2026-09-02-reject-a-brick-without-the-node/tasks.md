# Tasks

- [x] 1.1 the per-brick loop decides the survive test from the entry, before dereferencing the node
- [x] 1.2 the tail-path check reads the entry's id rather than the node's, so nothing needs the node first
- [x] 1.3 the group and item cull tests below are skipped for a planned chain, which this has already decided
- [x] 1.4 a test pins the invariant the fast path leans on -- a plan handed in without a cull region is dropped
- [x] 1.5 breaking that invariant fails that test; culling non-local items in the fast path fails the byte-identical corpus
- [x] 1.6 the packed-box alternative is measured and recorded as rejected, with the numbers
