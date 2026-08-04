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

Every one of these SHALL be expressible in the tape, so a document can use the whole set. Primitives with no finite extent (plane, infinite cylinder) SHALL make their item report infinite influence, and bound-only primitives SHALL downgrade the tape's tracked exactness.

#### Scenario: Exact primitive returns true distance
- **WHEN** an exact primitive is evaluated at a point at known geometric distance d from its surface
- **THEN** the returned value equals ±d (sign by inside/outside) within 1e-6 absolute for unit-scale shapes

#### Scenario: Bound primitive is conservative
- **WHEN** a bound-only primitive is evaluated at any sample point in a property test
- **THEN** the returned value SHALL NOT exceed the true distance to the surface (|f(p)| ≤ true distance, sign correct)

#### Scenario: Every primitive evaluates through the tape
- **WHEN** each primitive is placed in a document and the tape is evaluated
- **THEN** the result equals calling its kernel function directly with the same parameters

#### Scenario: Unbounded primitives are never culled
- **WHEN** an item using a plane or infinite cylinder is compiled for a brick far from the origin
- **THEN** the item appears in the culled tape and the band-clamped result matches the full tape

#### Scenario: Bound primitives downgrade exactness
- **WHEN** a tape contains a bound-only primitive such as the cheap octahedron
- **THEN** its field info is non-exact and stepping by its safe step scale never crosses the surface

### Requirement: 2D profile set
`prim2d.h` SHALL provide exact 2D SDFs for extrude/revolve (01 §1.3): circle, box, segment, hexagon, equilateral triangle, trapezoid, vesica, arbitrary polygon (exact, even-odd sign rule), and quadratic Bézier. Cubic Bézier SHALL be evaluated by adaptive quadratic subdivision, never by quintic root-finding.

The closed profiles — circle, box, hexagon, equilateral triangle, trapezoid, vesica, and the arbitrary polygon — SHALL additionally be expressible in the tape as the profile of a lift, with polygon vertices carried out-of-line. Open curves (segment, Bézier) remain header-only because they are unsigned distances rather than regions; documents reach curved outlines by flattening them to a polygon.

#### Scenario: Polygon profile handles concavity
- **WHEN** a concave polygon profile is evaluated at points inside and outside concave regions
- **THEN** the sign follows the even-odd rule and the distance is exact to the nearest edge

#### Scenario: Profile reachable through a lift
- **WHEN** a document contains an item whose primitive is an extrusion of a polygon profile
- **THEN** the compiled tape evaluates it identically to applying `cop_extrude` to `sd_polygon2` directly

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

All three SHALL be expressible in the tape as per-item modifiers, so a document can build arrays. The exactness condition SHALL be enforced rather than assumed: repetition preserves an exact field only when the item's extent plus its rounding and blend influence fits within its half-cell (or angular sector), and the compiler SHALL downgrade the tracked field info to a bound when it does not.

#### Scenario: Finite repetition boundary correctness
- **WHEN** the field of a finite N×N×N repetition is sampled near a cell boundary or outside the array extent
- **THEN** the distance accounts for neighboring cells (no seam discontinuities, no phantom copies beyond the extent)

#### Scenario: Repetition through the tape matches the kernel
- **WHEN** a document contains an item with a finite grid repetition
- **THEN** the compiled tape evaluates identically to applying `crep_lim_point` and the primitive directly

#### Scenario: Overflowing the half-cell downgrades exactness
- **WHEN** a repeated item's extent plus blend influence exceeds half its cell spacing
- **THEN** the tape reports a non-exact field, and stepping by its safe step scale still never crosses the surface

### Requirement: Deformers with Lipschitz tracking
`deform.h` SHALL provide twist, bend, taper, displacement-by-callable, `bend_linear`, `bend_radial`, `wrap_around`, and `transition_linear`/`transition_radial` (01 §2.5). Each deformer SHALL be flagged bound with a computed Lipschitz factor, and each falloff/transition parameter SHALL accept an easing curve from `ease.h` (≥ 30 curves, fogleman-style).

Twist, bend, taper, and displacement SHALL additionally be expressible in the tape, so a document — not only C++ header code — can use them: an edit item SHALL carry an ordered chain of deformers applied to the evaluation point before its distance function, with each deformer's distance correction applied after, and the composed Lipschitz factor folded into the tape's tracked field info.

#### Scenario: Deformed field remains traceable
- **WHEN** a twisted box is sphere-traced using the tree's safe step scale
- **THEN** the trace converges to the surface without overshoot (no surface holes in the parity render test)

#### Scenario: Deformer through the tape matches the header
- **WHEN** an item with a twist deformer is compiled to a tape and evaluated
- **THEN** results equal applying `ctwist_point` to the point and evaluating the primitive directly

#### Scenario: Deformer chain applies in authoring order
- **WHEN** an item carries a twist followed by a taper
- **THEN** the point is twisted first and tapered second, and reversing the order produces a different field

#### Scenario: Tracked step scale falls under deformation
- **WHEN** a tape containing a twist of Lipschitz factor L > 1 is compiled
- **THEN** its safe step scale is at most 1/L, and stepping by that scale never crosses the surface

### Requirement: Lifts
`lift.h` SHALL provide exact extrusion and exact revolution of exact 2D profiles (01 §2.6), and extrude-to/loft flagged as bound.

Extrusion and revolution SHALL be expressible in the tape as primitive opcodes carrying a profile, so a document can build profile-driven shapes. Loft remains header-only until an item can carry two profiles.

#### Scenario: Revolve preserves exactness
- **WHEN** an exact 2D profile is revolved
- **THEN** the resulting 3D field is exact and the tree exactness state records `exact`

#### Scenario: Lifted items evaluate through the tape
- **WHEN** a circle profile is extruded and, separately, revolved in a document
- **THEN** the fields equal a capped cylinder and a torus respectively, within meshing tolerance

#### Scenario: Lifted items keep tracked exactness
- **WHEN** a tape containing only extrusions and revolutions of exact profiles is compiled
- **THEN** its field info remains exact and its safe step scale stays 1

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

### Requirement: Transition morphs as combine modes
The tape SHALL provide `transition_linear` and `transition_radial` combine modes that mix the accumulated field with an item's field by a spatially varying weight (01 §2.5): the linear weight from the eased projection onto a segment a→b, the radial weight from the eased XZ radius between r0 and r1. Both SHALL mix color by the same weight, and both SHALL fold `cfi_transition` into the tape's tracked field info — a lerp of two distance fields is not a distance, so the safe step scale SHALL drop accordingly.

#### Scenario: Weight endpoints select each operand
- **WHEN** a transition_linear item is evaluated at the segment's start point and at its end point
- **THEN** the result equals the accumulated field at the start and the item's field at the end

#### Scenario: Transition matches the kernel weight
- **WHEN** a transition item is evaluated at arbitrary points
- **THEN** the result equals `mix(accumulated, item, ctransition_*_weight(p, ...))` for the same parameters

#### Scenario: Tracked step scale falls under a transition
- **WHEN** a tape containing a transition is compiled
- **THEN** its field info is non-exact, its safe step scale is below 1, and stepping by that scale never crosses the surface

