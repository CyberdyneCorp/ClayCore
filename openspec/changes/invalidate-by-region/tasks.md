# Tasks

- [x] 1.1 `Doc::touch_region`, keeping seeds whose brick the change cannot reach and advancing them to the new revision
- [x] 1.2 The command funnel takes `command_influence_bound` on both sides of the apply and unions them
- [x] 1.3 Undo and redo hand over the bound they already compute
- [x] 1.4 A seed at the current revision is served directly, with no suffix
- [x] 1.5 Tests: an edit outside every brick keeps their seeds and the values still match a full refill; an edit the bricks reach is recomputed
- [x] 1.6 Mutation-test that the surviving-seed path is actually taken
- [x] 1.7 Benchmark and `docs/`
