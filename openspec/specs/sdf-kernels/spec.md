# sdf-kernels Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: Kernel dialect portability
All distance functions and operators SHALL be written once, in header-only files under `include/clay/kernel/`, in a restricted C++ dialect accepted by every target compiler: no virtual functions, no exceptions, no heap allocation, no recursion, `constexpr`-friendly, using only fixed-size vector/scalar types from `shim.h` (`cfloat3`, `cfloat4x4`, …). `shim.h` SHALL map those types and qualifiers to the native types of each backend (Apple `simd`/scalar C++ for CPU, MSL vectors for Metal, CUDA vectors, OpenCL vectors) under the macros `CLAY_KERNEL_CPU`, `CLAY_KERNEL_METAL`, `CLAY_KERNEL_CUDA`, `CLAY_KERNEL_OPENCL`. The `kernel` module SHALL depend on nothing outside itself.

#### Scenario: Same header compiles on every backend
- **WHEN** a kernel header (e.g. `prim3d.h`) is compiled as C++20 (CPU), as MSL (Metal), as CUDA device code, and as OpenCL C-compatible source
- **THEN** it compiles without per-backend edits to the math, with backend differences confined to `shim.h`

#### Scenario: Dialect violation is rejected
- **WHEN** a kernel header introduces a virtual call, exception, allocation, or recursion
- **THEN** the CI kernel-dialect check (compile against the most restrictive target) fails the build

### Requirement: 3D primitive set
`prim3d.h` SHALL provide exact signed distance functions (01 §1.1) for: sphere, box, rounded box, box frame, torus, capped torus, link, capsule, infinite cylinder, capped cylinder (incl. arbitrary-axis), rounded cylinder, cone (exact), capped cone, round cone, plane, hexagonal prism, octahedron, pyramid, cut sphere, cut hollow sphere, solid angle, tetrahedron, and platonic solids via plane folds. It SHALL additionally provide bound-only primitives (01 §1.2): ellipsoid, triangular prism, cheap octahedron, superellipsoid / L-norm sphere — each flagged as `bound`.

#### Scenario: Exact primitive returns true distance
- **WHEN** an exact primitive is evaluated at a point at known geometric distance d from its surface
- **THEN** the returned value equals ±d (sign by inside/outside) within 1e-6 absolute for unit-scale shapes

#### Scenario: Bound primitive is conservative
- **WHEN** a bound-only primitive is evaluated at any sample point in a property test
- **THEN** the returned value SHALL NOT exceed the true distance to the surface (|f(p)| ≤ true distance, sign correct)

### Requirement: 2D profile set
`prim2d.h` SHALL provide exact 2D SDFs for extrude/revolve (01 §1.3): circle, box, segment, hexagon, equilateral triangle, trapezoid, vesica, arbitrary polygon (exact, even-odd sign rule), and quadratic Bézier. Cubic Bézier SHALL be evaluated by adaptive quadratic subdivision, never by quintic root-finding.

#### Scenario: Polygon profile handles concavity
- **WHEN** a concave polygon profile is evaluated at points inside and outside concave regions
- **THEN** the sign follows the even-odd rule and the distance is exact to the nearest edge

### Requirement: Booleans and rigid smooth blends
`ops.h` SHALL provide hard union/subtract/intersect/xor and smooth variants of union/subtract/intersect using quadratic smin (default), cubic (C2), and circular profiles, plus the chamfer (linear) profile (01 §2.1–2.2). All smooth blends SHALL be rigid (finite support): outside the blend radius the result is bit-identical to the hard operation. Every blend SHALL expose its material-mix factor `h` for color blending. The extended vocabulary — groove, tongue/pipe, emboss, deboss, push, avoid/repel, inset, shell, stain/paint (color-only), replace — SHALL be provided as algebraic smin variants with the same rigidity property.

#### Scenario: Blend rigidity
- **WHEN** two shapes are combined with any smooth blend of radius k and the field is sampled at a point farther than k from both surfaces' interaction region
- **THEN** the result is bit-identical to the corresponding hard boolean at that point

#### Scenario: Material mix exposed
- **WHEN** a smooth union of two colored shapes is evaluated inside the blend region
- **THEN** the kernel returns `h ∈ [0,1]` such that color interpolation with `h` matches the distance-blend falloff

### Requirement: Transforms, symmetry, and structure operators
`xform.h` SHALL provide inverse-applied rigid transforms, exact uniform scale, non-uniform scale flagged as bound with tracked Lipschitz factor, elongation, mirror/symmetry planes with Mirror Blend (smooth seam), rounding (dilate/erode), and onion/shell (01 §2.3, §2.6).

#### Scenario: Uniform scale stays exact
- **WHEN** an exact primitive is uniformly scaled by s via the transform operator
- **THEN** the field remains exact (distance multiplied by s) and the tree's exactness state is unchanged

#### Scenario: Non-uniform scale is tracked
- **WHEN** a non-uniform scale is applied
- **THEN** the node is marked bound with Lipschitz bound L = max axis scale ratio, and safe-step queries reflect it

### Requirement: Repetition operators
`repeat.h` SHALL provide infinite grid repetition (round-based), finite grid repetition with clamped cell index and neighbor-cell evaluation to avoid boundary artifacts (01 §2.4), and radial/circular arrays evaluated in O(2) sector evaluations, with per-element transform overrides.

#### Scenario: Finite repetition boundary correctness
- **WHEN** the field of a finite N×N×N repetition is sampled near a cell boundary or outside the array extent
- **THEN** the distance accounts for neighboring cells (no seam discontinuities, no phantom copies beyond the extent)

### Requirement: Deformers with Lipschitz tracking
`deform.h` SHALL provide twist, bend, taper, displacement-by-callable, `bend_linear`, `bend_radial`, `wrap_around`, and `transition_linear`/`transition_radial` (01 §2.5). Each deformer SHALL be flagged bound with a computed Lipschitz factor, and each falloff/transition parameter SHALL accept an easing curve from `ease.h` (≥ 30 curves, fogleman-style).

#### Scenario: Deformed field remains traceable
- **WHEN** a twisted box is sphere-traced using the tree's safe step scale
- **THEN** the trace converges to the surface without overshoot (no surface holes in the parity render test)

### Requirement: Lifts
`lift.h` SHALL provide exact extrusion and exact revolution of exact 2D profiles (01 §2.6), and extrude-to/loft flagged as bound.

#### Scenario: Revolve preserves exactness
- **WHEN** an exact 2D profile is revolved
- **THEN** the resulting 3D field is exact and the tree exactness state records `exact`

### Requirement: Exactness and Lipschitz propagation
The library SHALL track per-node field classification — `exact`, `bound`, or `Lipschitz(L)` — through the expression tree (01 §2.7) and expose the resulting safe step scale for the composed field. No consumer (sphere tracing, meshing, brick fill) SHALL assume |∇f| = 1 unless the tree proves exactness.

#### Scenario: Composition downgrades correctly
- **WHEN** an exact primitive is wrapped in a taper (Lipschitz L) and then smooth-unioned with an exact sphere
- **THEN** the composed tree reports the conservative classification and a safe step scale ≤ 1/L

### Requirement: Stroke items
The kernel vocabulary SHALL include a stroke construct: a polyline chain of capsules / round cones with per-point radius and color (Dreams-style Pencil smear), plus stamp placement, each carrying its own op + blend + color and applied symmetrically through active mirror planes.

#### Scenario: Stroke evaluates as one item
- **WHEN** a 100-point stroke is evaluated at a sample point
- **THEN** the result equals the smooth-union chain of its segments and evaluates in a single tape item (no per-segment scene items)

### Requirement: Field utilities
`field.h` SHALL provide tetrahedron-trick normals, analytic-gradient smin variants, sphere tracing with over-relaxation and pixel-proportional epsilon, safe step scaling from tracked bounds, ambient occlusion and Aaltonen soft-shadow queries, and raycast refinement using the last two samples (01 §3).

#### Scenario: Normals match analytic gradient
- **WHEN** tetrahedron-trick normals are computed on an exact primitive at 10k random surface points
- **THEN** they match the analytic gradient within 1e-3 angular error (radians)

