# Tasks

- [x] 1.1 `node_influence_bound_in_document` unions over every layer sharing the node's content
- [x] 1.2 `clay_layer_node_influence_bound` reports it
- [x] 1.3 `clay_brick_cache_mark_dirty_nodes` dirties by it
- [x] 1.4 The per-layer `node_influence_bound` is unchanged, so the cull path is untouched
- [x] 1.5 A property test: move a node, and every band-clamped value outside the box is unchanged
- [x] 1.6 The test runs over the adversarial corpus, an intersect chain, a layer of non-local ops, a mirror, and an instance
- [x] 1.7 Each case guards against a vacuous pass (nodes actually moved, points actually outside)
- [x] 1.8 Verify the property test FAILS without the fix
