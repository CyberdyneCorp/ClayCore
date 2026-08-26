# Tasks

- [x] 1.1 `apply_tape_attributes` builds the vertex point array once and asks the CPU backend for colours and gradients in one `eval_points` call, scattering the results back
- [x] 1.2 Keep the serial walk as the no-backend fallback, factored out so it is also the parity reference
- [x] 1.3 Preserve the old branch's behaviour for every combination: face normals with colour, colour alone, gradient alone, `NormalMode::None`, and an empty mesh
- [x] 1.4 Bit-identity test against the serial walk, in `test_points_batch.cpp` beside the brick-mesh one, with the dual-contouring mesher covered through the same function
- [x] 1.5 Verify the test has teeth — a one-ULP perturbation of the batched result must fail it
- [x] 1.6 `BM_MeshTapeAttributes` / `BM_MeshTapeAttributesSerial` and the `check_bench.py` FASTER_THAN pair, so the serial walk cannot come back unnoticed
- [x] 1.7 Update `docs/` where the attribute pass's cost is described
- [x] 1.8 Remove the `BM_SurfaceNets` / `BM_MeshTape` pair from `check_bench.py`, which this change reveals was measuring the attribute pass rather than the mesher; record the measurement where it stood and file #304
