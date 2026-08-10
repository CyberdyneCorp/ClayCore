# Tasks: speed-the-metal-path

- [ ] 1.1 DECIDE and record in `design.md`: what keys tape residency. The C ABI has a revision counter; the backend interface only has `const scene::Tape&`. Hashing the tape per dispatch to avoid uploading it is a trade — measure it before choosing it, and reject any key that can collide
- [ ] 1.2 Baseline on `main`, on Apple silicon: per-brick fill at 8³, and the grid sweep that put the CPU/Metal crossover at 16³. Break the 288 µs down into allocation, upload, dispatch and readback, so the change can be attributed
- [ ] 1.3 Tape residency: upload on change, reuse otherwise. A dab of N bricks against one document uploads one tape
- [ ] 1.4 Buffer pool for point/result/color buffers, grown to the largest recent request, released with the backend
- [ ] 1.5 Avoid the copies unified memory does not need: `bytesNoCopy` for inputs where alignment permits, read results from `contents()` in place. Keep the copying path for callers whose memory cannot be wrapped, and state which callers those are
- [ ] 1.6 Gradients on the device — remove `gradients_from_taps`' whole-batch fallback to `eval_points_reference`
- [ ] 1.7 RESOLVE the meshing contradiction: either implement device meshing, or amend the spec to say the backend triangulates on the host through `grid_mesh` and leave `device_meshing` false. Record which and why. A flag that disagrees with the spec is not an outcome
- [ ] 1.8 Parity is unchanged and is the acceptance test: Metal vs CPU over a full brick fill is exactly 0.0 max abs difference, as it is today
- [ ] 1.9 Equivalence test: buffer reuse vs fresh-allocation-per-call is bit-identical; residency vs no-residency is bit-identical after an edit
- [ ] 1.10 Re-measure the crossover after the change, and update `docs/RELEASE.md`'s "keep `clay_brick_cache_eval_requests` on cpu" advice against the new number rather than leaving the old one standing
- [ ] 1.11 Confirm no regression in the raycast path, which shares the same dispatch helper
