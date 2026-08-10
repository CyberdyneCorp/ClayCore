# Tasks: add-consolidation-policy

- [ ] 1.1 DECIDE the trigger: Lipschitz threshold, chain depth, memory budget, or host call — and record why, since the engine already knows the declared Lipschitz and the safe step scale
- [ ] 1.2 DECIDE the scope: a region, a layer, or a stroke
- [ ] 1.3 Report the cost before it is paid, from what `Volume` already exposes: megabytes, brick_count, sample_count, sample_lipschitz
- [ ] 1.4 Consolidate as ONE command whose inverse restores the items it absorbed, so the undo record carries them
- [ ] 1.5 State what a host can still promise about a consolidated region — which parameters are gone, and whether it can be re-expanded
- [ ] 1.6 Both bindings, C ABI additive
- [ ] 1.7 Tests: a chain of N hPolish passes holds its Lipschitz within a stated bound instead of going 1.00 -> 14.0; a Move stroke of nine drags no longer decays x0.615 per drag; consolidation undoes exactly; a document that never consolidates is bit-identical to today
- [ ] 1.8 Update `examples/27_move_strokes.py` and `examples/28_hpolish.py`, which currently PIN the degradation — they must assert the new bound rather than the old decay, and their docstrings stop calling hPolish single-pass
- [ ] 0.1 SEQUENCING (see ROADMAP, "What can run in parallel"): no `.clayspace` bump — volumes already serialise; never touches VoxelGrid, so it runs in parallel
