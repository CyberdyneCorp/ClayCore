# evaluation-backends — the CPU batch path evaluates a block of points per tape walk

Delta for `add-cpu-simd-path`.

## MODIFIED Requirements

### Requirement: CPU scalar is the correctness reference
The CPU scalar build SHALL define correctness for every kernel and composed scene. The CPU backend SHALL also provide a BATCH path that evaluates a block of points per pass over the tape, and thread-pool dispatch; the batch path SHALL match scalar within 1e-6 relative on distances.

The requirement is the SHAPE of the walk, not the instruction set. The batch path SHALL walk the tape's instruction sequence ONCE per block of points, loading each instruction's parameter header — the inverse transform, the scale, the rounding radius — once per block rather than once per point. Whether any lane width is used underneath is an implementation choice and SHALL NOT be specified here: the measured win is loads and per-instruction bookkeeping, and a packet width of 4 or 8 is below the block size at which that win appears.

Slicing the scalar evaluator across threads is thread-pool dispatch and SHALL NOT be presented as the batch path.

The scalar evaluator SHALL remain untouched by this path and SHALL remain the definition of correctness for every backend, GPU backends included.

Any opcode that cannot be evaluated over a block SHALL be named in the design and SHALL fall back to per-point scalar evaluation, producing a result identical to the scalar path. A silent fallback is not permitted: which opcodes are block-evaluated SHALL be recorded.

A batch whose length is not a multiple of the block size SHALL produce results identical to one that is, so block size is not observable above the backend.

<!-- The scenario below keeps its published title. OpenSpec matches scenarios by
     title inside a MODIFIED requirement, so renaming it to "Batch parity" reads
     as DROPPING it and archive would silently lose the parity contract. The title
     is therefore historical; its body is the requirement. -->

#### Scenario: SIMD parity with scalar
- **WHEN** the parity suite evaluates every kernel on the CPU batch path against scalar on the standard sample corpus
- **THEN** all distances agree within 1e-6 relative error

#### Scenario: The batch path walks the tape once per block
- **WHEN** a batch of points is evaluated on the CPU batch path
- **THEN** the tape's instruction sequence is walked once per block of points, not once per point

#### Scenario: A ragged batch matches an aligned one
- **WHEN** a batch whose length is not a multiple of the block size is evaluated
- **THEN** every result is identical to evaluating the same points in a batch that is a multiple of the block size

#### Scenario: Colors and gradients agree too
- **WHEN** the batch path is asked for colors and gradients as well as distances
- **THEN** colors are identical to scalar and gradients agree within the distance tolerance
