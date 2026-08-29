# Tasks

- [ ] 1.1 `TapeCheckpoint` names the CHAIN it was taken in, not only the layer, so a checkpoint inside a group is distinguishable from one in front of the layer's union
- [ ] 1.2 `compile_document`'s walk records a checkpoint after a group's child list as well as after a layer's root list, when that group is in tail position all the way up
- [ ] 1.3 `compile_document_append` resumes from a group checkpoint: the group's chain continues with the appended node, then the combines the checkpoint sat in front of are re-emitted
- [ ] 1.4 `tail_append` accepts a tail append into a group whose ancestors are all in tail position; every other shape keeps today's refusal, shared content included
- [ ] 1.5 `command_frontier`'s root-ordinal resolution reviewed against the same shape — it assumes the edited node's root ordinal is what a seed's prefix is counted against
- [ ] 1.6 Test: a 24-dab stroke into a group compiles a tape byte-identical to a full compile of the same document, dab by dab
- [ ] 1.7 Test: the append path actually FIRES for that stroke — asserted through the counters, since a fast path that silently stopped firing reads as correct
- [ ] 1.8 Test: every refused shape still refuses — an insert one short of the end, a group not in tail position, shared content, a non-local combine above the append
- [ ] 1.9 Re-run `sdf_stroke_in_group_bricks` on the reference iPad; it should approach `sdf_stroke_bricks` and go flat across the axis
- [ ] 1.10 `docs/09-brush-latency-and-coverage.md` — the pair, and what the ratio was before
