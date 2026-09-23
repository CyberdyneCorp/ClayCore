## ADDED Requirements

### Requirement: Batched grab chains preserve the scalar field

The CPU evaluator MAY process compatible grab deformers across blocks of points,
but SHALL preserve the scalar reference's per-point authored warp order and
floating-point results for distances, colours and field-gradient normals.
The optimization SHALL NOT replace field gradients with cached lattice gradients
or collapse the authored deformer chain.

#### Scenario: Deep local Move chains
- **WHEN** a grab-only primitive is evaluated through the CPU point, grid or batch path
- **THEN** distances, colours and gradient outputs are bit-identical to scalar evaluation
- **AND** every grab's invariant parameters can be decoded once per point block

#### Scenario: Easing and front gating
- **WHEN** a compatible chain uses overshooting easing, a zero displacement, or front-only gating
- **THEN** the batch path preserves the scalar support, undershoot and gate behavior

#### Scenario: Unsupported combinations
- **WHEN** a primitive uses repetition or other deformer types
- **THEN** it retains the existing evaluator with unchanged results
