# evaluation-backends — batched point evaluation

Delta for `batch-mesh-attribute-taps`. Adds the batched point form of the
interface, mirroring the batched grid form refill uses: the brick-mesh
attribute pass evaluates many small per-brick point runs, and a per-run
dispatch leaves most of the CPU pool idle.

## ADDED Requirements

### Requirement: Batched point evaluation amortizes dispatch across runs

The backend interface SHALL accept a batch of point runs, each carrying its own (typically per-brick culled) tape, as one call (`eval_points_batch`). Every backend SHALL answer the batch with exactly the values its per-run `eval_points` path produces — per-point results are independent of how the batch is split, so batching changes speed, never results. The CPU backend SHALL dispatch the flattened batch across its thread pool rather than barrier per run, so that a batch of many small runs occupies the pool as one large run would. The brick-mesh attribute pass (gradient normals and vertex colors of `mesh_bricks`) SHALL evaluate its per-brick vertex groups through this batched form.

#### Scenario: Batched and per-run evaluation agree bitwise

- **WHEN** the same point runs and per-run culled tapes are evaluated one `eval_points` call per run and as one `eval_points_batch`, on any registered backend
- **THEN** the distances, gradients and colors are byte-identical, including for empty runs and for runs smaller than a dispatch chunk

#### Scenario: A dense re-mesh is not one core's worth of serial taps

- **WHEN** a fixed brick set whose culled tapes are long (a densely sculpted region) is re-meshed with gradient normals and colors
- **THEN** the attribute evaluation runs across the CPU pool and its cost stays a bounded multiple of refilling the same bricks, which the benchmark gate enforces as a ratio
