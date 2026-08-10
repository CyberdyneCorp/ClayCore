# evaluation-backends — the CPU batch path evaluates points in lanes

Delta for `add-cpu-simd-path`.

## MODIFIED Requirements

### Requirement: CPU scalar is the correctness reference
The CPU scalar build SHALL define correctness for every kernel and composed scene. The CPU backend SHALL also provide a SIMD batch path (Apple `simd` on Apple platforms, SSE/NEON via xsimd elsewhere) and thread-pool dispatch; the SIMD path SHALL match scalar within 1e-6 relative on distances.

The SIMD batch path SHALL evaluate a PACKET of points per pass over the tape — one walk of the instruction sequence covering N points — rather than one point per walk. Slicing the scalar evaluator across threads is thread-pool dispatch and SHALL NOT be presented as the SIMD path.

The scalar evaluator SHALL remain untouched by this path and SHALL remain the definition of correctness for every backend, GPU backends included.

Any opcode that cannot be evaluated in lanes SHALL be named in the design and SHALL fall back to per-lane scalar evaluation, producing a result identical to the scalar path. A silent fallback is not permitted: which opcodes are lane-evaluated SHALL be recorded.

A batch whose length is not a multiple of the packet width SHALL produce results identical to one that is, so lane width is not observable above the backend.

#### Scenario: SIMD parity with scalar
- **WHEN** the parity suite evaluates every kernel on the SIMD path against scalar on the standard sample corpus
- **THEN** all distances agree within 1e-6 relative error

#### Scenario: The batch path walks the tape once per packet
- **WHEN** a batch of points is evaluated on the CPU batch path
- **THEN** the tape's instruction sequence is walked once per packet of points, not once per point

#### Scenario: A ragged batch matches an aligned one
- **WHEN** a batch whose length is not a multiple of the packet width is evaluated
- **THEN** every result is identical to evaluating the same points in a batch that is a multiple of the packet width

#### Scenario: Colors and gradients agree too
- **WHEN** the batch path is asked for colors and gradients as well as distances
- **THEN** colors are identical to scalar and gradients agree within the distance tolerance
