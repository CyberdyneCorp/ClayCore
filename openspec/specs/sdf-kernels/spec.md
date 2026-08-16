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
`lift.h` SHALL provide exact extrusion and exact revolution of exact 2D profiles (01 §2.6), and a loft flagged as bound.

Extrusion and revolution SHALL be expressible in the tape as primitive opcodes carrying a profile, so a document can build profile-driven shapes. **Loft SHALL also be expressible in the tape**, as an opcode carrying two or more profiles interpolated along the lift axis. The header SHALL expose the interpolation as a function taking an already-computed parameter, so bracketing among more than two profiles needs no second entry point — a signature that derived the parameter from the height could only ever serve exactly two.

#### Scenario: Revolve preserves exactness
- **WHEN** an exact 2D profile is revolved
- **THEN** the resulting 3D field is exact and the tree exactness state records `exact`

#### Scenario: Lifted items evaluate through the tape
- **WHEN** a circle profile is extruded and, separately, revolved in a document
- **THEN** the fields equal a capped cylinder and a torus respectively, within meshing tolerance

#### Scenario: Lifted items keep tracked exactness
- **WHEN** a tape containing only extrusions and revolutions of exact profiles is compiled
- **THEN** its field info remains exact and its safe step scale stays 1

#### Scenario: A loft reaches both profiles
- **WHEN** a loft between two different profiles is evaluated at each end of its depth
- **THEN** the cross-section there matches the profile at that end

#### Scenario: A loft interpolates between them
- **WHEN** a loft is evaluated halfway along its depth
- **THEN** the cross-section lies between the two profiles rather than matching either

#### Scenario: More than two profiles are bracketed
- **WHEN** a loft carries three profiles and is evaluated at the middle one's position
- **THEN** the cross-section matches that middle profile, rather than a blend of the outer two

### Requirement: Exactness and Lipschitz propagation
The library SHALL track per-node field classification — `exact`, `bound`, or `Lipschitz(L)` — through the expression tree (01 §2.7) and expose the resulting safe step scale for the composed field. No consumer (sphere tracing, meshing, brick fill) SHALL assume |∇f| = 1 unless the tree proves exactness.

A loft SHALL declare itself **not exact**, and SHALL declare a Lipschitz factor accounting for the interpolation along the lift axis: a lerp of two fields adds a term proportional to how far apart they are over the depth they are mixed across, steepened by the easing curve's own maximum slope. Declaring Lipschitz 1 for a loft SHALL be treated as a defect, because the raymarcher would step as though the field were a distance and miss surfaces.

#### Scenario: Composition downgrades correctly
- **WHEN** an exact primitive is wrapped in a taper (Lipschitz L) and then smooth-unioned with an exact sphere
- **THEN** the composed tree reports the conservative classification and a safe step scale ≤ 1/L

#### Scenario: A loft is not exact
- **WHEN** a document containing a loft is compiled
- **THEN** the tape reports the field as inexact

#### Scenario: Differing profiles over a shorter depth step more carefully
- **WHEN** two lofts differ only in that one interpolates between the same profiles over a shallower depth
- **THEN** that one reports the smaller safe step scale

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

### Requirement: wrap_around is a tape deformer
The tape SHALL carry `wrap_around` as a deformer opcode, bending the local X interval `[x0, x1]` around a cylinder about the Z axis so that a flat item becomes a wrapped one. It SHALL compose with the other deformers in authoring order like any chain member.

Its influence SHALL be bounded by the disc the wrap sweeps: with `r = (x1 - x0) / 2pi`, the deformed local bound is `|x|, |y| <= max(|r + ymin|, |r + ymax|)` over the content's radial extent, with `z` unchanged. Because the deformer is a metric breaker, it SHALL downgrade the node's tracked field info and contribute a Lipschitz factor derived from the content's radial extent, so sphere tracing slows rather than tunnelling.

#### Scenario: A wrapped item bends around the cylinder
- **WHEN** an item spanning the wrap interval is evaluated after the deformer
- **THEN** its surface lies about the cylinder of radius `r`, and the tape agrees with the kernel's own `cwrap_around_point` composed with the primitive

#### Scenario: The bound contains the wrapped geometry
- **WHEN** the deformed bound is computed for a wrapped item
- **THEN** every point of the wrapped surface lies inside it

#### Scenario: Wrapping is not exact
- **WHEN** a wrapped item is compiled
- **THEN** the tape reports a non-exact field and a safe step scale below 1

#### Scenario: Device agreement
- **WHEN** a wrapped item is evaluated on every registered backend
- **THEN** each matches the CPU scalar reference within the parity tolerance

### Requirement: Elongation is a tape deformer
The tape SHALL carry elongation as a deformer opcode, inserting flat sections of half-extent `h` along each axis so a shape stretches without its ends distorting. It SHALL both warp the evaluation point and contribute the corresponding distance correction, and SHALL compose in a deformer chain in authoring order.

Elongation SHALL expand the item's local bound by `h` per axis. Because the map is non-expansive it SHALL NOT reduce the safe step scale; it SHALL preserve the tracked exactness when the elongated primitive is origin-symmetric, and downgrade to a bound otherwise.

#### Scenario: A stretched shape keeps its ends
- **WHEN** a sphere is elongated along one axis
- **THEN** the result is a capsule: the field matches the kernel's own elongation applied by hand, and the caps are undistorted

#### Scenario: Elongation of a symmetric primitive stays exact
- **WHEN** an origin-symmetric primitive is elongated
- **THEN** the tape reports an exact field and a safe step scale of 1

#### Scenario: Elongation of an asymmetric primitive is a bound
- **WHEN** a primitive that is not origin-symmetric is elongated
- **THEN** the tape reports a non-exact field, because the correction is only valid about the origin

#### Scenario: The bound contains the stretched geometry
- **WHEN** the deformed bound is computed for an elongated item
- **THEN** every point of the stretched surface lies inside it

#### Scenario: Device agreement
- **WHEN** an elongated item is evaluated on every registered backend
- **THEN** each matches the CPU scalar reference within the parity tolerance

### Requirement: Ramped bends are tape deformers
The tape SHALL carry `bend_linear` and `bend_radial` as deformer opcodes. `bend_linear` SHALL displace the point by a vector ramped along the segment between two points; `bend_radial` SHALL displace along Y by an amount ramped across a radial band. Both SHALL honour an easing curve, and both SHALL compose in a deformer chain in authoring order.

Both SHALL dilate the item's local bound by the displacement they can apply, and SHALL downgrade the tracked field info with a Lipschitz factor equal to the ramp's slope — the displacement over the span it ramps across.

#### Scenario: A linear ramp displaces only across its span
- **WHEN** a point before the segment start, one after its end, and one midway are evaluated
- **THEN** the first is undisplaced, the last is displaced by the full vector, and the middle by the eased fraction — matching the kernel applied by hand

#### Scenario: A radial ramp displaces only across its band
- **WHEN** points inside `r0`, beyond `r1`, and between them are evaluated
- **THEN** the displacement is zero, full, and the eased fraction respectively

#### Scenario: The easing curve reaches the field
- **WHEN** the same bend is built with two different easing curves
- **THEN** the fields differ within the ramp span

#### Scenario: The bound contains the displaced geometry
- **WHEN** the deformed bound is computed for either bend
- **THEN** every point of the displaced surface lies inside it

#### Scenario: Existing documents still load
- **WHEN** a document saved before these opcodes existed is read
- **THEN** its deformers decode exactly as before, because the reader takes its parameter count from the deformer type

#### Scenario: Device agreement
- **WHEN** an item using either bend is evaluated on every registered backend
- **THEN** each matches the CPU scalar reference within the parity tolerance

### Requirement: Per-axis elongation is a tape deformer
The tape SHALL carry per-axis elongation as a deformer opcode, translating the point outside the half-extents `h` toward the middle and leaving a flat plateau inside. It SHALL work for any primitive, symmetric or not, and SHALL compose in a deformer chain in authoring order.

It SHALL contribute no distance correction, SHALL expand the item's local bound by `h` per axis, and SHALL always downgrade the tracked field to a bound — the interior plateau is not a distance. Because the map is non-expansive it SHALL NOT reduce the safe step scale.

#### Scenario: An asymmetric primitive stretches
- **WHEN** a primitive that is not origin-symmetric is elongated per axis
- **THEN** the field matches the kernel's own map applied by hand, and the shape is stretched along the chosen axes

#### Scenario: Per-axis elongation is always a bound
- **WHEN** any primitive is elongated per axis
- **THEN** the tape reports a non-exact field, even for an origin-symmetric primitive, and the safe step scale is unchanged

#### Scenario: The bound contains the stretched geometry
- **WHEN** the deformed bound is computed
- **THEN** every point of the stretched surface lies inside it

#### Scenario: Device agreement
- **WHEN** the item is evaluated on every registered backend
- **THEN** each matches the CPU scalar reference within the parity tolerance

### Requirement: Grab displaces a region of space with finite support
The tape SHALL carry a grab deformer that displaces the evaluation point by a vector weighted by distance from a centre, falling to zero at a radius. The weight SHALL follow an easing curve from the existing library, and the map SHALL be exactly the identity outside the radius so the deformer's influence stays local.

The item's local bound SHALL dilate by the displacement magnitude and no more. The tracked field SHALL downgrade to a bound with a Lipschitz factor of `1 + |d| · s / r`, where `s` is the easing curve's steepest measured slope — the same form `bend_linear` already uses.

#### Scenario: Only the region moves
- **WHEN** a grab is applied with centre c and radius r
- **THEN** the field at points beyond r from c is unchanged, and the surface within r has moved toward the displacement

#### Scenario: The falloff shapes the pull
- **WHEN** the same grab is applied with two different easing curves
- **THEN** the resulting surfaces differ within the radius and agree outside it

#### Scenario: Culling still holds
- **WHEN** the influence bound is computed for a grabbed item
- **THEN** every point whose field the grab changed lies inside it

#### Scenario: Front-facing only
- **WHEN** a grab is applied with the front-facing option against a stroke direction
- **THEN** surface facing away from that direction is left undisplaced, so the far side of a form does not move with the near side

### Requirement: Pose applies a transform across a region
The tape SHALL carry a pose deformer applying a rigid transform — rotation about a pivot, and translation — weighted by the same radial falloff, so a limb can be rotated about a joint with the influence tapering off along the form.

Pose SHALL report a Lipschitz factor accounting for the rotation's arc over the region, and SHALL offer the same front-facing option as grab.

#### Scenario: Rotation tapers across the region
- **WHEN** a pose rotation is applied about a pivot with radius r
- **THEN** geometry at the pivot is unmoved, geometry near the radius is fully transformed, and the transition follows the easing curve

#### Scenario: Sphere tracing stays safe
- **WHEN** a posed item is compiled
- **THEN** the tape reports a non-exact field and a safe step scale below 1

### Requirement: Pose weighted along a line
The tape SHALL carry a pose deformer whose weight ramps along a segment: zero at the anchor, one at the end, taken from the point's projection onto the segment and shaped by an easing curve. The rotation SHALL be about the given axis through the anchor, so the anchor is a fixed point of the map.

Unlike grab and radial pose this deformer SHALL NOT be claimed to have finite support: the weight clamps, so material beyond the end anchor is fully rotated rather than untouched. Its bound SHALL therefore cover the swept arc — the hull of the item's bound and its fully-rotated image, dilated by the sagitta of the swept angle — rather than a dilation of the original.

The tracked field SHALL downgrade to a bound with a Lipschitz factor derived from the item's extent about the rotation axis against the length of the ramp.

#### Scenario: The anchor stays and the form bends
- **WHEN** a line pose is applied from an anchor to an end with a non-zero angle
- **THEN** the field at the anchor is unchanged and the form curves toward the direction of rotation, increasingly so with the angle. The weight is taken at the sample point rather than its preimage, so this is a bend rather than a rigid swing and the achieved rotation falls short of the nominal angle as it grows — the spec requires the bend, not the exact endpoint.

#### Scenario: The ramp follows the segment, not the distance
- **WHEN** two points lie equidistant from the anchor but at different projections along the segment
- **THEN** they receive different weights, which a radial region could not express

#### Scenario: The easing curve shapes the taper
- **WHEN** the same line pose is applied with two different easing curves
- **THEN** the fields differ between the anchor and the end

#### Scenario: The bound contains the swept geometry
- **WHEN** the deformed bound is computed for a line pose, including a large angle
- **THEN** every point of the rotated surface lies inside it

#### Scenario: Device agreement
- **WHEN** a line-posed item is evaluated on every registered backend
- **THEN** each matches the CPU scalar reference within the parity tolerance

### Requirement: Profiles swept along a guide
The tape SHALL provide an opcode that carries a tessellated guide polyline and two or more 2D profiles, and evaluates a query point by finding the nearest point on the guide, expressing the point in a frame there, and evaluating the profiles interpolated along the guide's arc length.

The frame SHALL be **parallel-transported** along the guide rather than derived per-sample from the curve's own derivatives, so that it does not flip at an inflection point or become undefined where the guide is straight. Transport is sequential, so the frames SHALL be computed once when the item is compiled and interpolated between at evaluation.

Profiles SHALL be distributed by **arc length**, so a guide whose vertices bunch does not bunch the profiles.

The nearest-segment search SHALL resolve ties deterministically. Any query point whose nearest guide point is a vertex shared by two segments is equidistant from both by construction — this is every point outside a bend, not a rare case — and the two carry different tangents, so they build different frames and resolve the profile at different arc lengths. A bare "strictly nearer" comparison decides that on the last ulp, which backends do not agree on. A segment SHALL therefore be required to be nearer by a **relative margin** that comfortably exceeds cross-backend rounding while staying far below the separation between genuinely different segments, so the earliest of a tied group wins everywhere.

#### Scenario: A sweep follows its guide
- **WHEN** a circle is swept along an L-shaped guide
- **THEN** material is present along both limbs and absent off them

#### Scenario: The cross-section is the profile
- **WHEN** a circle of a given radius is swept along a straight guide
- **THEN** between the guide's endpoints the field matches a capsule of that radius, within tolerance

#### Scenario: The ends are the profile, not a rounded cap
- **WHEN** a swept item is evaluated past the end of its guide, on the guide's axis
- **THEN** the distance is the overshoot past a flat end face, because the profile need not be a circle and there is no hemisphere to cap it with

#### Scenario: Profiles interpolate along the guide
- **WHEN** a wide profile and a narrow one are swept along a straight guide
- **THEN** the cross-section is wide at the start and narrow at the end

#### Scenario: The frame does not flip where the guide straightens
- **WHEN** a non-rotationally-symmetric profile is swept along a guide that bends, straightens, then bends back
- **THEN** the profile's orientation varies smoothly along the whole guide

#### Scenario: Tied guide segments resolve the same way on every backend
- **WHEN** a query point lies outside a bend, so its nearest guide point is the vertex shared by two segments, and their squared distances differ only by rounding
- **THEN** the same segment is chosen regardless of that rounding, and every registered backend agrees with the scalar reference within the parity tolerance

#### Scenario: A degenerate sweep is refused
- **WHEN** a sweep is built with fewer than two guide points, or fewer than two profiles
- **THEN** it is refused

### Requirement: A sweep declares its exactness and curvature cost
A swept item SHALL declare itself **not exact**, and SHALL declare a Lipschitz factor derived from the guide's tightest bend against the widest profile's extent: a point at perpendicular offset `r` inside a bend of radius `R` is compressed by `R / (R - r)`.

Where the widest profile reaches or exceeds the tightest bend radius the sweep folds through itself. The engine SHALL NOT refuse this — a guide is editable after the fact — and SHALL instead report a correspondingly large Lipschitz, so the raymarcher takes small steps rather than stepping through a surface it was told was a distance field.

#### Scenario: A sweep is not exact
- **WHEN** a document containing a sweep is compiled
- **THEN** the tape reports the field as inexact

#### Scenario: A tighter guide steps more carefully
- **WHEN** the same profile is swept along a gently curved guide and a sharply curved one
- **THEN** the sharply curved one reports the smaller safe step scale

#### Scenario: An overgrown profile degrades rather than failing
- **WHEN** a profile wider than the guide's tightest bend radius is swept
- **THEN** the document still compiles and evaluates, and its safe step scale is very small

### Requirement: A field can be built from samples
The library SHALL provide a sparse narrow-band signed distance volume built by sampling any callable over a region, storing samples only within a stated band of the surface. Bricks wholly outside the band SHALL store no samples, recording only whether they are inside or outside, so that storage is proportional to surface area rather than to volume.

The volume SHALL be expressible in the tape as a primitive opcode, so a sampled field combines with every op the engine already has.

#### Scenario: A sampled sphere reproduces its source
- **WHEN** a sphere's field is sampled into a volume and evaluated near the surface
- **THEN** the values match the sphere's own field within the sampling tolerance

#### Scenario: Storage follows the surface, not the volume
- **WHEN** a volume is built over a region much larger than the surface it contains
- **THEN** its stored size is far smaller than a dense grid over the same region

#### Scenario: Far from the band the sign is still right
- **WHEN** a volume is evaluated deep inside and far outside its surface
- **THEN** the values are negative and positive respectively

#### Scenario: A volume combines like any primitive
- **WHEN** a sampled volume is subtracted from a box
- **THEN** the result is the box with that shape removed

### Requirement: A sampled field declares what it is
A sampled volume SHALL declare itself **not exact**. Its guarantees divide by whether a point lands where the volume kept samples — which is **not** the same as being within the band, since a brick is kept whole and so holds samples well beyond it.

Where the volume **has** samples, the value is an interpolation. Interpolating a convex field overshoots: trilinear interpolation lies above the function it samples by an amount proportional to the square of the cell size. It SHALL NOT be claimed as a lower bound there. What it SHALL be is accurate to the sampling, with the error shrinking as the cell size does, so that choosing a cell size is a real control over accuracy rather than a hope.

Where it has **none**, the value SHALL be a true lower bound — never larger in magnitude than the real distance — so that sphere tracing cannot overstep across the empty majority of the region.

The declared Lipschitz factor SHALL account for the interpolant being able to be steeper than the field it samples, rather than assuming the source's own factor carries over.

#### Scenario: A volume is not exact
- **WHEN** a document containing a sampled volume is compiled
- **THEN** the tape reports the field as inexact

#### Scenario: Where there are no samples the value is a lower bound
- **WHEN** a volume is evaluated at points that fall in bricks holding no samples, inside and outside the surface
- **THEN** the reported distance never exceeds the true distance in magnitude

#### Scenario: The interpolant's slope is declared, not assumed
- **WHEN** a document containing a sampled volume is compiled
- **THEN** the reported Lipschitz factor exceeds 1, and the document's safe step scale drops accordingly

#### Scenario: Where there are samples the error follows the cell size
- **WHEN** the same surface is sampled at a coarse and at a fine cell size
- **THEN** the worst error where samples are stored is small at both and markedly smaller at the fine one

#### Scenario: A skipped brick meets a stored one safely
- **WHEN** the field is evaluated on both sides of the boundary between a brick that stores samples and one that does not
- **THEN** the value jumps, and each side is separately either a lower bound or accurate to the sampling, so a marcher crossing it cannot overstep

### Requirement: The bound has to be usable, not merely true
A lower bound that is correct but tiny stops a raymarcher as surely as a wrong one: it takes steps that never grow, and runs out of iterations before it arrives. The bound reported where there are no samples SHALL therefore **grow with distance from the surface** rather than being a constant.

The value reported outside the sampled region SHALL NOT fall to zero at the region's edge. The distance to the sampled box alone does, and a sphere tracer reads zero as a surface — every ray would stop on an invisible shell where the sampling happened to stop.

#### Scenario: Empty space opens up as it empties
- **WHEN** a volume is evaluated at increasing distances from the surface, through space holding no samples
- **THEN** the reported distance keeps increasing, and exceeds a single brick well before the region's edge

#### Scenario: Crossing empty space takes a sane number of steps
- **WHEN** a sphere trace crosses the empty part of a sampled region
- **THEN** it arrives in a number of steps proportional to the distance over the growing bound, not to the distance over the band width

#### Scenario: A ray finds the surface, not the edge of the sampling
- **WHEN** a ray is marched at a sampled volume from outside its region
- **THEN** it stops at the real surface, and a ray that misses the surface passes through the region without stopping

### Requirement: A sampled field can be relaxed
The library SHALL smooth a sampled volume, returning a new one whose surface is less bumpy than the one it was given. Repeated application SHALL smooth further, and smoothing SHALL be a no-op in shape terms on a surface that is already smooth.

#### Scenario: A bumpy surface becomes smoother
- **WHEN** a surface with small bumps on it is relaxed
- **THEN** the bumps are reduced, and the more iterations are run the less of them remains

#### Scenario: A smooth surface barely moves
- **WHEN** a sphere is relaxed
- **THEN** its surface stays where it was, to within the sampling

#### Scenario: Relaxing yields an ordinary item
- **WHEN** a relaxed volume is placed in a document
- **THEN** it combines, saves and evaluates exactly as any other volume does

### Requirement: Relaxing preserves the bound that sphere tracing depends on
Smoothing destroys exactness — the relaxed field no longer reports the true distance to its own surface — but it SHALL NOT break the Lipschitz bound. A weighted average of a field cannot vary faster than the field does, and a field whose slope is bounded by one is automatically a conservative bound on the distance to its own zero set.

A relaxed volume SHALL therefore still be safe to sphere trace: a ray marched at it SHALL arrive at the surface rather than step through it.

#### Scenario: The slope does not grow
- **WHEN** the steepest slope of a field is measured before and after relaxing
- **THEN** it has not increased

#### Scenario: A ray still finds the surface
- **WHEN** a ray is marched at a relaxed volume
- **THEN** it stops at the surface rather than passing through it

### Requirement: Relax acts where it is aimed
Relax SHALL accept a region — a centre, a radius and a falloff — so that it is a brush rather than only a global filter. Outside the region the field SHALL be unchanged, and at the edge of the region the change SHALL taper rather than step, so that relaxing does not leave a visible rim.

#### Scenario: Outside the region nothing moves
- **WHEN** a shape is relaxed with a region covering only part of it
- **THEN** the field away from that region is unchanged

#### Scenario: The region's edge does not leave a rim
- **WHEN** the field is examined across the boundary of the relaxed region
- **THEN** it varies continuously rather than stepping

### Requirement: Relax does not inflate a shape without limit
Smoothing shrinks convex features and grows concave ones. Repeated relaxing SHALL NOT cause a shape to grow without bound or to vanish after a moderate number of passes.

#### Scenario: Repeated relaxing converges rather than diverging
- **WHEN** a shape is relaxed many times over
- **THEN** its enclosed size settles rather than running away

### Requirement: An operator that transforms a field works on the samples
A field's value where it has no samples is a lower bound, not a measurement. An operator that reads a volume and writes another SHALL work on the stored samples rather than by re-sampling through evaluation, because evaluation mixes measurements with bounds and re-sampling that mixture records the boundary between them as though it were part of the shape.

An operator that MOVES the surface SHALL narrow the band by how far it moved, since the sample-free bricks were classified against where the surface used to be and their bounds would otherwise overstate the distance to where it is now.

#### Scenario: Smoothing does not manufacture a steep edge
- **WHEN** a volume is relaxed and the steepest slope of the result is measured within the sampled region
- **THEN** it has not risen above the slope of the field that went in

#### Scenario: A sample shared by several bricks is found in any of them
- **WHEN** a stored sample lying on a brick face, edge or corner is read by global coordinate
- **THEN** it is found whichever of the bricks sharing it holds the samples

### Requirement: Kernel headers self-select their backend
When no `CLAY_KERNEL_*` macro is defined, `shim.h` SHALL choose the backend
branch from the compiling toolchain's own predefined macros — `__METAL_VERSION__`
for MSL, `__CUDACC__` for CUDA device code, `__OPENCL_VERSION__` /
`__OPENCL_C_VERSION__` for OpenCL C — and fall back to the CPU branch only when
none is present. An explicitly defined `CLAY_KERNEL_*` SHALL always win, so
existing builds and the host-emulated CI profiles are unaffected.

This is what lets a host compile the headers as shader source with no build
settings: including them from a `.metal` file is enough.

#### Scenario: A .metal file needs no build flags
- **WHEN** a host `#include`s `clay/kernel/kernels.h` from Metal shader source without defining any `CLAY_KERNEL_*` macro
- **THEN** the MSL branch of the shim is selected and the file compiles

#### Scenario: An explicit selection still wins
- **WHEN** a translation unit defines `CLAY_KERNEL_CUDA` and is compiled by a host C++ compiler
- **THEN** the CUDA branch is selected regardless of what the compiler predefines

### Requirement: Umbrella header
The kernel module SHALL provide `clay/kernel/kernels.h`, a single header that
includes the whole dialect in dependency order, so a consumer needs one include
and does not have to track the file list. It SHALL omit the headers a backend
cannot compile — `field.h` is templated and therefore not part of the OpenCL C
subset — rather than failing on them.

#### Scenario: One include reaches the whole vocabulary
- **WHEN** a translation unit includes only `clay/kernel/kernels.h`
- **THEN** every primitive, operator, deformer, lift, easing curve and the tape interpreter are available

### Requirement: Kernel headers are consumable by a host shader compiler
The kernel headers SHALL be publishable as a standalone artifact — no build
step, no generated file, no dependency on anything outside
`include/clay/kernel/` — and SHALL be verified to compile as MSL in CI: with
`xcrun metal` where an Apple toolchain exists, and against a stubbed
`metal_stdlib` elsewhere, so a break in the Metal branch of the shim fails a
Linux runner rather than waiting for an Apple one.

#### Scenario: A Metal-only break fails Linux CI
- **WHEN** a kernel header uses a helper that the Metal branch of `shim.h` does not define
- **THEN** the kernel-dialect check fails on a Linux runner naming the header and the missing symbol

#### Scenario: The artifact stands alone
- **WHEN** the packaged headers are copied into an unrelated project and compiled as MSL
- **THEN** they compile without any other file from this repository

### Requirement: A sampled field can be flattened onto a plane
The library SHALL pull a sampled volume's surface toward a caller-supplied plane, returning a new volume. Where the effect is at full weight the surface SHALL become that plane.

Flattening SHALL mean the same thing it means for voxels: material on the plane's positive side is removed AND hollows on the negative side that touch material are filled. It is two-sided, not a subtract. Two representations sharing a verb's name must share its meaning, or a document means something different depending on which one it is stored in.

#### Scenario: A bump becomes a facet
- **WHEN** a shape with a raised bump is flattened against a plane cutting through it
- **THEN** the surface under the brush lies on that plane

#### Scenario: A dent is filled, not deepened
- **WHEN** a shape with a hollow below the plane is flattened
- **THEN** the hollow is filled up to the plane rather than left or cut deeper

#### Scenario: A surface already on the plane does not move
- **WHEN** a flat face that already lies on the target plane is flattened
- **THEN** it stays where it was, to within the sampling

#### Scenario: Flattening yields an ordinary item
- **WHEN** a flattened volume is placed in a document
- **THEN** it combines, saves and evaluates exactly as any other volume does

### Requirement: Flatten samples a new volume rather than editing one
Flatten SHALL build its result by SAMPLING a source field with the plane blended in, so that the new band brackets the flattened surface. It SHALL NOT transform an existing volume's stored samples in place.

This is why: a narrow band tracks the surface only while the surface stays inside it. Smoothing moves the surface by less than a cell, so `relax` can rewrite samples where they lie. Flatten moves it by many band widths, and once the surface has walked outside the band there are no samples left describing where it now is — the isosurface comes apart. Sampling builds the band around the flattened surface instead, and makes the blend closed-form: no iteration, no step budget, no band to narrow afterwards.

Where an exact source exists — a document's field — flatten SHALL prefer it to a volume, because a volume reports a lower bound rather than a distance outside its own band, and sampling a field that mixes the two records the boundary between them as part of the shape.

#### Scenario: The band brackets the flattened surface
- **WHEN** a shape is flattened so its surface moves well beyond the original band
- **THEN** the result stores samples at the new surface, and none where the old one was

#### Scenario: A ray still finds the surface
- **WHEN** a ray is marched at a flattened volume
- **THEN** it stops at the facet rather than passing through it

### Requirement: The declared Lipschitz is measured, not assumed
A region blends under a weight that varies across it, which adds a term proportional to how far the value moves times the gradient of the weight — so flatten CAN make the field steeper than its source. The result SHALL declare a Lipschitz bound its samples actually satisfy, and that bound SHALL be measured from the samples produced rather than bounded in advance.

#### Scenario: The declared bound is not exceeded
- **WHEN** the steepest slope of a flattened volume is measured inside the sampled region
- **THEN** it does not exceed what the tape declares for that volume

#### Scenario: A tighter taper declares more
- **WHEN** the same flatten is applied with a narrow falloff and with a generous one
- **THEN** the narrow one declares the higher Lipschitz, and the document's safe step scale drops accordingly

### Requirement: Flatten acts where it is aimed, and a region is required
Flatten SHALL REQUIRE a region — a centre, a radius and a falloff. Outside it the field SHALL be unchanged, and the transition SHALL taper rather than step, so flattening does not leave a rim.

The region is not optional, because flatten is local by nature: where its weight is one the result IS the plane. Blending with no region at full strength therefore does not flatten a shape, it replaces it with a half-space — a ball comes back as a box. A request with no region SHALL be refused rather than honoured into destroying the shape.

#### Scenario: Outside the region nothing moves
- **WHEN** a shape is flattened with a region covering only part of it
- **THEN** the field away from that region is unchanged

#### Scenario: The region's edge does not leave a rim
- **WHEN** the change the flatten made is examined across the boundary of the region
- **THEN** it varies continuously rather than stepping

#### Scenario: A request with no region is refused
- **WHEN** flatten is asked for with a region radius of zero
- **THEN** it is refused, and the shape is not replaced by a half-space

### Requirement: A radial scale about a point, with finite support
The library SHALL provide a deformer that scales space radially about a centre within a stated radius, so that the surface swells away from that centre or gathers toward it. ONE signed strength SHALL cover both directions: magnify and pinch are the same deformation with opposite sign, and giving them separate opcodes would be building the same thing twice.

Support SHALL be finite: outside the radius the field SHALL be exactly unchanged, so that item influence bounds stay tight and brick culling keeps working.

#### Scenario: A positive strength swells the surface
- **WHEN** a shape is magnified about a point on it
- **THEN** the surface near that point moves outward from the centre

#### Scenario: A negative strength gathers it
- **WHEN** the same deformer is applied with the sign reversed
- **THEN** the surface near that point moves toward the centre

#### Scenario: Support is finite
- **WHEN** the field is evaluated beyond the deformer's radius
- **THEN** it is identical to the field without the deformer

#### Scenario: Zero strength changes nothing
- **WHEN** the deformer is applied with a strength of zero
- **THEN** the field is unchanged everywhere

### Requirement: The stretch a radial scale costs is declared
Scaling space radially is not distance preserving, so the field is no longer exact and its slope grows. The tape's Lipschitz factor SHALL carry that, as it does for grab and pose, and the stretch SHALL account for the easing curve's slope because the deformation is steepest where the falloff is.

A raymarcher SHALL therefore still land on a magnified or pinched surface rather than stepping through it.

#### Scenario: The field is no longer exact
- **WHEN** a document containing a magnify deformer is compiled
- **THEN** it reports the field as inexact, and the safe step scale is below one

#### Scenario: A stronger deformation declares more
- **WHEN** the same shape is deformed at increasing strength
- **THEN** the reported Lipschitz rises and the safe step scale falls

#### Scenario: A ray still finds the surface
- **WHEN** a ray is marched at a magnified shape
- **THEN** it stops at the surface rather than passing through it

### Requirement: Field verbs honour the mask
Every field verb SHALL accept an optional mask, and where one is given its per-sample weight SHALL be scaled by one minus the mask value at the WORLD position of the sample being written. A fully masked sample SHALL keep its source value exactly, not approximately: a frozen region that drifts by a rounding error is a frozen region that has moved.

This closes a real gap rather than adding a convenience. Relax and flatten rewrite or re-sample a field under a spherical region and a falloff, and nothing else gates them, so a masked region inside that sphere is not frozen — "a mask blocks any effect of sculpting" was false for exactly the verbs SDF layers gained most recently.

The mask SHALL be sampled in world units rather than in the volume's cells, which costs nothing — world addressing is what the mask field exists for — and is what lets one mask gate a voxel layer and an SDF layer at the same time.

#### Scenario: Relax leaves a frozen region alone
- **WHEN** a volume is relaxed over a region that is fully masked
- **THEN** the samples there are unchanged, while the same relax without the mask changes them

#### Scenario: Flatten leaves a frozen region alone
- **WHEN** a field is flattened over a region that is fully masked
- **THEN** the surface there is where the source put it, not on the plane

#### Scenario: Partial masking attenuates
- **WHEN** a field verb runs over a half-masked region
- **THEN** the surface there moves less than it does unmasked, and more than it does fully masked

### Requirement: A mask can be measured as a distance
The library SHALL provide a conversion from a mask to a signed distance field: the distance to the boundary of the region where the mask meets a threshold, negative inside it. The result SHALL be an ordinary sampled volume, so that it is 1-Lipschitz, tape-expressible, blob-carried and identical on every backend.

The conversion exists because a mask is a [0,1] scalar on a lattice. Composing one into a field expression directly would put a step in the result, and the Lipschitz bound the evaluator depends on would no longer hold.

The conversion SHALL pad the sampled region by a caller-given amount, because a band clipped at the mask's own border is no use to an operation that reaches outside it.

An empty mask SHALL yield nothing rather than a volume that reads as empty space everywhere, which is harder to notice than a failure.

#### Scenario: Inside is negative and outside is positive
- **WHEN** a mask is painted as a blob and converted to a field
- **THEN** the field is negative well inside the blob, positive well outside it, and near zero at its border

#### Scenario: It is a distance
- **WHEN** the converted field is sampled along a line crossing the border
- **THEN** the value changes at roughly unit rate with distance, rather than stepping

#### Scenario: An empty mask converts to nothing
- **WHEN** a mask with nothing painted is converted
- **THEN** no field is produced

### Requirement: Mask extrude on a field
The library SHALL resolve a source field, a mask and a thickness into a new field holding only the masked patch of the source's surface, thickened. The result SHALL be an ordinary sampled volume, so meshing, evaluation, picking, serialization and every backend apply to it unchanged.

The thickness SHALL be placeable on either side of the source surface or centred on it. The intersection with the masked region SHALL admit a rounding radius, so the rim can be soft rather than a hard edge at the mask's border.

The result SHALL declare the Lipschitz its samples actually have rather than assuming one: a rounded intersection of two fields is a bound, not an exact distance, and an evaluator told otherwise oversteps.

The extrude SHALL be refused — yielding nothing — for an empty mask, a mask that does not reach the source surface, a non-positive thickness, or a sampling resolution finer than the mask itself can describe. Producing something in those cases would produce something that is not what the caller asked for.

The mask SHALL NOT be modified.

#### Scenario: A plate comes off a sphere
- **WHEN** a cap of a sphere is masked and extruded outward at a thickness
- **THEN** the result is a shell sitting on that cap, of that thickness measured along the surface normal, and empty away from the mask

#### Scenario: Each side means what it says
- **WHEN** the same mask and thickness are extruded outward, inward and centred
- **THEN** each result lies on the corresponding side of the source surface

#### Scenario: The rim rounds
- **WHEN** an extrude is taken with a rounding radius
- **THEN** the field is continuous across the mask's border rather than stepping at it

#### Scenario: Refusals produce nothing
- **WHEN** an extrude is asked for with an empty mask, a mask away from the surface, or a non-positive thickness
- **THEN** nothing is produced, and nothing crashes

#### Scenario: A ray still lands
- **WHEN** the extruded volume is sphere-traced from outside
- **THEN** the march converges on its surface, which is what a declared Lipschitz that held means in practice

### Requirement: Noise is reproducible on every backend
The noise SHALL be hashed from INTEGER lattice coordinates using integer operations, so that every backend produces the same values within the parity tolerance.

A float hash SHALL NOT be used. Cross-backend parity is tolerance-based rather than bit-exact — 1e-6 relative on the CPU backends and 1e-4 on the GPU ones — and the usual `fract(sin(...) * large)` construction amplifies a units-in-the-last-place disagreement in `sin` into an O(1) disagreement in its output, because taking a fractional part of a large product is chaotic by design. It would fail parity on the first case.

#### Scenario: Every backend agrees
- **WHEN** a document containing noise is evaluated on each registered backend
- **THEN** the results agree within the same tolerance every other primitive is held to

#### Scenario: The same seed gives the same field
- **WHEN** two items are given the same noise parameters and seed
- **THEN** their fields are identical

#### Scenario: A different seed gives a different field
- **WHEN** only the seed is changed
- **THEN** the field differs

### Requirement: Noise displaces the distance, as an ordinary deformer
Noise SHALL contribute a distance OFFSET, in the same place `displace` does, rather than warping the point. It SHALL be an ordinary deformer so that it serializes, crosses the C ABI and runs on every backend without a mechanism of its own.

It SHALL be fractal, summing octaves, because one octave of gradient noise is smooth blobs and a weathered surface wants detail at several scales.

#### Scenario: Noise roughens a surface
- **WHEN** a smooth shape is given a noise deformer with a non-zero amplitude
- **THEN** its surface is irregular rather than smooth, and the deviation is bounded by the amplitude

#### Scenario: Zero amplitude changes nothing
- **WHEN** the amplitude is zero
- **THEN** the field is unchanged everywhere

#### Scenario: More octaves add finer detail
- **WHEN** the octave count is raised with the amplitude held
- **THEN** the surface gains detail at smaller scales without the overall deviation growing without bound

#### Scenario: It is irregular, not periodic
- **WHEN** the field is sampled along a line at the noise's own frequency
- **THEN** it does not repeat, which is what distinguishes it from the sine the displace deformer uses

### Requirement: The steepness noise adds is declared
Offsetting the distance by a function raises the field's slope by that function's gradient, so the tape's Lipschitz SHALL carry it — as it already does for `displace`. The bound SHALL account for every octave, because each octave has a higher frequency than the last and so a steeper gradient.

#### Scenario: The field stops being exact
- **WHEN** a document containing a noise deformer is compiled
- **THEN** it reports the field as inexact and the safe step scale is below one

#### Scenario: More amplitude or frequency declares more
- **WHEN** either the amplitude or the frequency is raised
- **THEN** the reported Lipschitz rises and the safe step scale falls

#### Scenario: A ray still finds the surface
- **WHEN** a ray is marched at a noisy shape
- **THEN** it stops at the surface rather than passing through it

### Requirement: An item can displace the surface accumulated before it
The library SHALL provide a combine op that offsets the ACCUMULATED field by an amplitude, weighted by the item's own field used as a region. The item SHALL contribute its shape as a region rather than as geometry, in the way the paint op already uses an item as a region for colour.

Offsetting a distance field moves its isosurface along the field's own gradient, which is the surface normal, so this displaces the existing surface along its normal rather than approximating that.

Building the surface up and cutting into it SHALL be TWO ops sharing one implementation, rather than one op with a signed amplitude. The amplitude rides on `blend_k`, which is required non-negative in three places including the blend constructor — which has no op to be aware of — so a sign has nowhere to live. It is also the existing convention: add and subtract are a pair, and so are engrave and emboss.

Sharing the implementation is what keeps the two each other's inverse as either changes.

#### Scenario: Relief builds the surface up
- **WHEN** a relief item overlaps an existing surface
- **THEN** the surface within the region moves outward along its normal, by the amplitude

#### Scenario: Incise cuts into it
- **WHEN** the same item is given the incise op instead
- **THEN** the surface moves inward by the same amount

#### Scenario: It acts on what came before, not on itself
- **WHEN** a relief item is placed in a layer with no other content
- **THEN** it contributes no surface of its own

#### Scenario: The region is any primitive
- **WHEN** relief items using different primitives as their region are applied
- **THEN** each displaces the surface over the footprint of its own primitive

### Requirement: Relief has finite support
Outside the item's region the accumulated field SHALL be exactly unchanged, so that item influence bounds stay tight and brick culling keeps working — which is what makes relief usable at the densities a stroke produces.

The reach is the region's own extent PLUS its rounding PLUS the falloff width. The rounding does double duty here — it is the falloff width and it also rounds the region's own field, exactly as it does for groove and tongue, where the channel is centred on the rounded surface. The influence bound SHALL be dilated by both terms, not by the falloff alone.

#### Scenario: Beyond the region nothing moves
- **WHEN** the field is evaluated beyond a relief item's falloff
- **THEN** it is identical to the field without that item

#### Scenario: The bound covers the rounded region, not the raw one
- **WHEN** the furthest point a relief item changes is measured
- **THEN** it lies within the region's extent plus its rounding plus the falloff width, and within the item's declared influence bound

#### Scenario: A zero amplitude changes nothing
- **WHEN** a relief item is given an amplitude of zero
- **THEN** the field is unchanged everywhere

### Requirement: The steepness relief adds is declared
Offsetting the accumulated field by a weighted amplitude raises its slope by that term's gradient, which is the amplitude over the falloff width. The tape's Lipschitz SHALL carry it, so a deep relief through a narrow falloff costs the marcher by a declared amount rather than by surprise.

#### Scenario: The field stops being exact
- **WHEN** a document containing a relief item is compiled
- **THEN** it reports the field as inexact and the safe step scale is below one

#### Scenario: A deeper or narrower relief declares more
- **WHEN** the amplitude is raised, or the falloff width narrowed
- **THEN** the reported Lipschitz rises and the safe step scale falls

#### Scenario: The declared bound is one the field meets
- **WHEN** the steepest slope of a field carrying relief is measured
- **THEN** it does not exceed what the tape declares

#### Scenario: A ray still finds the surface
- **WHEN** a ray is marched at a surface carrying relief
- **THEN** it stops at the displaced surface rather than passing through it

### Requirement: Flatten can act on one side of its plane
Flattening SHALL offer three modes: blending the surface toward the plane from both sides, removing material only, and depositing material only. The default SHALL be the two-sided behaviour, so a caller that does not ask for a mode gets what it got before.

Removing only is the hard-surface case — ZBrush's hPolish, Planar and the Trim family. Cutting without filling is what leaves a crisp facet against untouched surface; filling the hollows beside a facet is what a polish must not do.

The mode SHALL be a parameter of the existing operation rather than a separate entry point, since the three differ by one clamp on the blend term.

#### Scenario: Cutting only leaves a hollow alone
- **WHEN** a surface carrying both a bump above the plane and a hollow below it is flattened in cut-only mode
- **THEN** the bump is planed onto the plane and the hollow is unchanged

#### Scenario: Depositing only leaves a bump alone
- **WHEN** the same surface is flattened in fill-only mode
- **THEN** the hollow is filled to the plane and the bump is unchanged

#### Scenario: Two-sided is what it was
- **WHEN** a surface is flattened without asking for a mode
- **THEN** the result is identical to the same flatten before modes existed

#### Scenario: Whichever side it acts on lands on the plane
- **WHEN** any mode is applied at full strength within its region
- **THEN** the material it acted on ends on the plane, not short of it

#### Scenario: A one-sided flatten is no steeper than a two-sided one
- **WHEN** the Lipschitz of a one-sided result is measured
- **THEN** it does not exceed what the same two-sided flatten declares

### Requirement: A move can be weighted by distance through the material
The library SHALL provide a move whose falloff is weighted by geodesic distance from the anchor THROUGH THE MATERIAL, rather than by Euclidean distance through space, so that parts of a form which are close in space but far along the surface are not dragged together.

The distance SHALL be solved over cells the source reports as material. Free space SHALL NOT be part of the graph, which is what stops the weight crossing a gap.

It SHALL bake, for the reason relax and flatten do: the weight is a solved field rather than a closed form, and putting one in the tape would require a deformer that reads out-of-line data, which no deformer does.

#### Scenario: A neighbouring part is not dragged
- **WHEN** a topological move is applied to one of two parts that are close in space and joined only through a distant path, with a radius that spans the gap
- **THEN** the neighbouring part is unchanged, where a Euclidean move of the same radius moves it

#### Scenario: The grabbed part still moves
- **WHEN** the same move is applied
- **THEN** the part under the anchor moves in the direction of the drag

#### Scenario: Distance runs along the material
- **WHEN** the radius is raised until it exceeds the path length through the joining body
- **THEN** the neighbouring part begins to move, because it is now within reach along the material

#### Scenario: A move that reaches nothing changes nothing
- **WHEN** the anchor is placed away from any material, or the displacement is zero
- **THEN** the result matches the source

#### Scenario: The declared steepness is measured
- **WHEN** a topological move is applied
- **THEN** the result declares the Lipschitz its samples actually have, rather than an assumed bound

### Requirement: A loft with fewer than two profiles evaluates far
The loft opcode SHALL return the far value when its profile count is below two, rather than reading the two records it interpolates between. A tape is rebuilt from a document on every compile and a document may come from disk, so the count is not something the authoring layer alone can guarantee.

This is the guard the sweep opcode already applies to its own guide and profile counts.

#### Scenario: A loft carrying no profiles
- **WHEN** a document whose loft node has zero profiles is compiled and evaluated
- **THEN** evaluation returns a finite far value and reads no record

#### Scenario: A loft carrying one profile
- **WHEN** a document whose loft node has a single profile is compiled and evaluated
- **THEN** evaluation returns a finite far value and reads no record beyond that profile

#### Scenario: A well-formed loft is unaffected
- **WHEN** a loft with two or more profiles is evaluated
- **THEN** it interpolates between them exactly as before

### Requirement: A round cone whose ends contain one another is the larger sphere
When the radii of a round cone differ by more than the distance between its ends, one end sphere contains the other and no conical flank exists. Both the origin-and-height form and the endpoint form SHALL return the enclosing sphere's distance rather than evaluating the flank, which takes the square root of a negative quantity and yields NaN.

NaN is not a local error here: every combine op propagates it, so one such item makes the entire document evaluate to NaN, and a document carrying one round-trips through a file without complaint.

The endpoint form is the stroke-segment kernel, so this is reached by an ordinary tapered stroke, and its guard SHALL also cover coincident endpoints.

#### Scenario: A cone whose base swallows its tip
- **WHEN** a round cone is evaluated with a base radius exceeding its tip radius by more than its height
- **THEN** the result is finite and equals the distance to the base sphere

#### Scenario: A cone whose tip swallows its base
- **WHEN** the tip radius exceeds the base radius by more than the height
- **THEN** the result is finite and equals the distance to the tip sphere

#### Scenario: A stroke segment with coincident points
- **WHEN** the endpoint form is evaluated with both endpoints at the same position
- **THEN** the result is finite and equals the distance to the sphere at that point

#### Scenario: A well-formed cone is unchanged
- **WHEN** a round cone whose radii differ by less than its height is evaluated
- **THEN** it returns exactly what it returned before

### Requirement: A sampled volume's brick index is bounded by its sample data
`FieldVolume::from_blob` SHALL reject a blob in which any brick index entry is neither the empty marker nor an offset from which a whole brick of samples lies inside the sample section. Checking only the section offsets leaves one entry able to name an arbitrary offset, which the evaluator and the tape both then read a full brick from.

#### Scenario: An index entry pointing past the samples
- **WHEN** a volume blob carries a brick index entry whose offset plus one brick runs past the sample data
- **THEN** the blob is refused

#### Scenario: A negative entry that is not the empty marker
- **WHEN** a volume blob carries a negative brick index entry other than the empty marker
- **THEN** the blob is refused

#### Scenario: A sparse volume still loads
- **WHEN** an ordinary volume with empty bricks is round-tripped through its blob
- **THEN** it loads and evaluates unchanged

#### Scenario: A volume larger than float can index exactly still loads
- **WHEN** a volume whose sample data exceeds what a float can address in consecutive integers is round-tripped through its blob
- **THEN** each entry is resolved to the brick boundary it names and the volume loads and evaluates unchanged

The check SHALL NOT be tighter than the format's own precision: a bound that assumes exact float integers refuses the last brick of any volume large enough to lose them, which makes a document this library wrote unopenable.

### Requirement: An armature is a tree of spheres
The tape SHALL provide an opcode carrying a set of nodes, each a position and a radius, a parent index per node, and a sign per node, +1 or -1, and SHALL evaluate the field as the armature of the positive nodes MINUS the armature of the negative nodes, each half built exactly as the unsigned armature is.

A link SHALL exist only between two nodes of the same sign — skin between builders, carve between carvers — and a link whose ends disagree SHALL belong to neither half: a node whose parent carries the other sign reads as a root of its own half. This is the membrane cut stated structurally — skin along a negative node's links is never drawn, rather than drawn and patched — and it is what keeps a carve from sweeping a positive parent's radius, which would swallow the head an eye-socket child is cut into.

A segment SHALL use the same construction the stroke opcode already uses: a round cone between two radii, a capsule where the radii agree, and a sphere where the endpoints coincide. Positive segments SHALL be combined with the same smooth union, whose radius is a parameter of the armature; negative segments SHALL be subtracted with the matching smooth subtraction at the same radius, hard where the radius is zero, and the whole carve SHALL come after the whole positive fold, so a sleeve from any other branch that runs through a hollow is cut and no union can re-fill it.

A root — a node whose parent is itself, or whose parent carries the other sign — SHALL contribute its own sphere ONLY when no other node of its half names it as a parent, and the rule SHALL be applied per half: a root with same-sign children is already contained in every link that names it, and contributing it twice is harmless under a hard fold but wrong under a soft one — the smooth union of two overlapping terms pulls the surface outward, and the smooth subtraction of two overlapping terms over-carves the same way.

This is the chain opcode generalised from consecutive pairs to parent pairs. An armature whose parents form a line and whose signs are all positive SHALL therefore evaluate identically to the stroke with the same points, which is what keeps the two from drifting; an all-positive armature SHALL evaluate identically to one that predates signs.

#### Scenario: A chain armature is a stroke
- **WHEN** an armature whose every node's parent is the node before it, all signs positive, is evaluated, and a stroke is built from the same points and blend
- **THEN** the two fields agree everywhere, within the parity tolerance

#### Scenario: A branch is a union, not a chain
- **WHEN** two children share one parent and a point is sampled near each child
- **THEN** both children contribute material, and neither is joined to the other except through the parent

#### Scenario: A single node is a sphere
- **WHEN** an armature has one node, whose parent is itself
- **THEN** the field is that sphere, and nothing is degenerate

#### Scenario: A root that has children contributes no extra sphere
- **WHEN** a chain armature is evaluated with a non-zero blend
- **THEN** it still agrees with the stroke, because the root's sphere already lies inside the link that names it and adding it again would pull the smooth union outward

#### Scenario: A negative node's links carry no skin
- **WHEN** a chain runs A, B, C with B negative, and points are sampled between A and B and between B and C
- **THEN** no sleeve is drawn through the hollow in either direction: A and C keep their own material, and B's ball carves whatever else overlaps it

#### Scenario: A negative child does not swallow its positive parent
- **WHEN** a small negative child is placed inside a large positive parent — an eye socket in a head
- **THEN** the carve is the child's own sphere, the parent's skin survives everywhere the carve does not reach, and no internal void is cut beyond it

#### Scenario: A negative subtree carves as one rig
- **WHEN** two negative nodes in a parent-child pair sit inside positive material
- **THEN** their link is carved as the same sphere-swept segment the positive half skins with, so the hollow is one swept scoop rather than two disjoint balls

#### Scenario: An all-negative armature is empty
- **WHEN** every node of an armature is negative
- **THEN** the field is empty — there is nothing to carve from — and nothing is degenerate

### Requirement: A branch folds in a stated order
The smooth union is not associative, and neither is the subtraction that follows it, so the order SHALL be deterministic and stated: positive segments fold in ascending node index, then negative segments subtract in ascending node index, so that the same armature evaluates identically on every backend and in every process.

#### Scenario: The same armature gives the same field everywhere
- **WHEN** an armature with a branching node is evaluated on every registered backend
- **THEN** the results agree within the parity tolerance

#### Scenario: The same signed armature gives the same field everywhere
- **WHEN** an armature holding negative nodes — one a root of its half, one a negative parent-child link — is evaluated on every registered backend
- **THEN** the results agree within the parity tolerance

### Requirement: An armature declares its exactness
A smooth union of round cones is a bound rather than an exact distance, and an armature SHALL declare that and contribute the Lipschitz factor the smooth union implies, exactly as the chain opcode already does.

#### Scenario: The step scale reflects the blend
- **WHEN** an armature's blend radius is raised
- **THEN** the reported safe step scale reflects it, and sphere tracing does not overshoot the surface

### Requirement: A sampled volume may supply colour per sample
The tape's volume evaluation SHALL be able to report a colour read from the volume's own samples, rather than only the distance. Colour is known where the distance is computed — inside the opcode, from the blob — and the existing split, where a prim returns a distance and the caller applies the item's constant colour, cannot express a field whose colour varies.

The colour SHALL be optional per volume. A volume with no colour section SHALL evaluate exactly as it does today: the same distance, and the item's constant colour. A blob whose colour offset is zero SHALL be indistinguishable in behaviour from a volume authored before this existed.

Colour SHALL be interpolated at the same samples and by the same rule as the distance. A boundary between two colours gradates across a cell; reading the nearest sample instead would put a facet on a surface that does not have one.

Prims other than the volume SHALL NOT pay for this. The colour output SHALL be an out-parameter the volume opcode may write and every other prim ignores, rather than a wider return type every prim constructs.

The dialect SHALL remain single-source. The same header SHALL compile for CPU, CUDA, Metal, OpenCL and Vulkan, and the parity suite SHALL compare COLOUR as well as distance for a coloured volume, on every backend registered in the build.

#### Scenario: A coloured volume evaluates its own colour
- **WHEN** a volume carrying per-sample colour is evaluated at a point inside its sampled box
- **THEN** the reported colour is the volume's own, interpolated between samples, and not the item's constant colour

#### Scenario: An uncoloured volume is unchanged
- **WHEN** a volume with no colour section is evaluated
- **THEN** the distance and the colour are what they were before this change, bit for bit on the CPU reference

#### Scenario: Every backend agrees about colour
- **WHEN** a document containing a coloured volume is evaluated on each registered backend
- **THEN** the colours agree within the parity tolerance, as the distances already must

#### Scenario: Outside the box the item's colour still applies
- **WHEN** a coloured volume is evaluated outside its sampled box
- **THEN** the item's constant colour is reported, since there is no sample to read

### Requirement: Ranged twist and bend
The kernel dialect SHALL provide twist and bend variants whose rotation is RAMPED across a caller-given span with an easing curve and HELD beyond it, so material outside the span travels rigidly rather than continuing to rotate.

Each SHALL be the same rotation as its unranged form with the angle substituted, NOT a second formulation. With a linear ease and a span covering the content, the ranged form SHALL equal the unranged form at every point inside the span, and this SHALL be asserted rather than assumed — it is what makes the pair a range on an existing deformation rather than a second deformation to keep in step.

A zero-width span SHALL be refused at the bindings rather than divided by.

The declared Lipschitz factor SHALL be charged the angular rate the easing curve actually REACHES, not its average across the span: an eased ramp is steeper somewhere in the middle than linear, and a bound taken from the average would be under the field's true slope exactly where the ramp is steepest.

The influence bound SHALL reuse the unranged hull, which contains the ranged warp because a bounded rotation about an axis is contained by the cylinder the unbounded one sweeps.

Both SHALL be reachable from `pyclay` and the C ABI, and SHALL carry a parity-corpus scene with a NON-LINEAR ease, so a backend that applied the range but ignored the ease fails rather than passes.

#### Scenario: A ranged twist over its whole span is the unranged twist
- **WHEN** a ranged twist with a linear ease and a span covering the content is compared to the unranged twist at the same rate
- **THEN** every point inside the span warps identically

#### Scenario: Outside the span the rotation holds
- **WHEN** two points above the span differ only in height
- **THEN** they rotate by the same angle, where under the unranged twist they would not

#### Scenario: The ease is charged to the bound
- **WHEN** the same ranged twist is declared with a linear ease and with a steeper one
- **THEN** the steeper curve reports a tighter safe step scale, and both bound the field's measured slope

#### Scenario: A zero-width span is refused
- **WHEN** a caller passes a span whose ends are equal
- **THEN** the binding refuses it rather than dividing by zero

### Requirement: Bend along a guide curve
The kernel dialect SHALL provide a deformer that maps an item's local span along an axis onto a guide polyline's ARC LENGTH, carrying the material on the guide's parallel-transported frames, so an artist can draw the shape the item's axis takes rather than being limited to the circular arc a constant rate produces.

The map SHALL be the INVERSE of the swept primitive and SHALL be implemented by reusing the sweep's own nearest-point query, frame construction and guide blob layout, not by a second implementation of them. A sweep and a bend along a curve are the same geometry read from either end, and sharing the query is what makes them agree by construction.

A guide running straight along the axis SHALL be the IDENTITY map, asserted rather than assumed — it is what makes this deformer a generalization of the undeformed item rather than a second thing to keep in step.

Agreement with the constant-rate `bend` on a circular guide SHALL NOT be used as evidence: that kernel is the CHEAP bend, a rotation by an angle proportional to the axis coordinate rather than an arc-length-preserving one, so the two are not meant to agree and an assertion that they do would be checking against a known approximation. The arc-length claim SHALL be asserted directly instead — a point ON the guide has zero perpendicular offset by construction, so the deformed field there SHALL equal the undeformed field at the axis point its arc length maps to.

The guide SHALL live in the tape's blob with the deformer record holding an offset and a count, reusing the swept vertex layout. Widening every deformer record to fit the largest payload, or reusing the node's primitive stroke, SHALL NOT be used: the first charges every item for the widest deformer in the innermost evaluation loop, and the second would silently forbid bending the stroke and swept items most likely to want it.

The declared Lipschitz factor SHALL charge BOTH the curvature compression on the inside of a bend and the axial rescale from laying the span onto the guide's length. Where the item's cross-section reaches the tightest bend radius the map folds through itself; this SHALL degrade to a very small step rather than being refused, because a guide is editable after the fact.

The tightest bend radius SHALL be measured as the CIRCUMRADIUS of consecutive guide triples rather than as accumulated turn angle, which reads a finely-tessellated gentle curve as a tight one.

The influence bound SHALL be the guide's own bounds grown by the item's cross-section extent. Unlike a rotation about a fixed axis, a curve can carry material anywhere the guide goes, so the undeformed item's neighbourhood does not contain the warp.

A guide of fewer than two points, or one of zero length, SHALL be refused at the bindings rather than divided by.

It SHALL be reachable from `pyclay` and the C ABI, and SHALL carry a parity-corpus scene whose guide TURNS, so a backend that read the arc length but ignored the transported frames fails rather than passes.

#### Scenario: A straight guide changes nothing
- **WHEN** an item is bent along a guide that runs straight down its own axis
- **THEN** every point evaluates as it did undeformed

#### Scenario: A point on the guide reads the item's axis
- **WHEN** the field is evaluated at a guide vertex whose arc length is known
- **THEN** it equals the undeformed field at the axis point that arc length maps to

#### Scenario: The material follows where the guide goes
- **WHEN** a guide turns away from the item's axis
- **THEN** the item's far end is found near the guide's far end, and the influence bound contains it

#### Scenario: A fold degrades rather than lying
- **WHEN** the item's cross-section is wider than the guide's tightest bend radius
- **THEN** the compiled tape reports a very small safe step scale rather than claiming the field is a distance

#### Scenario: A guide that cannot mean anything is refused
- **WHEN** a caller passes fewer than two guide points, or a guide of zero length
- **THEN** the binding refuses it rather than dividing by zero

### Requirement: Deformers may read the tape blob
The deformer point map SHALL receive the tape's blob pointer, so a deformer whose payload is not a fixed size can hold an offset into the blob exactly as a primitive does.

The blob is already in hand at the point map's only call site, so this SHALL be a threading change rather than new plumbing; deformers that carry no payload SHALL ignore the pointer and behave identically.

#### Scenario: Payload-free deformers are unaffected
- **WHEN** the blob pointer is threaded through the deformer point map
- **THEN** every existing deformer produces the values it produced before, on every backend

### Requirement: A lattice deformer warps an SDF item through a cage
The kernel dialect SHALL provide a lattice (free-form deformation) deformer, so an item can be reshaped by dragging a few control points rather than by composing rotations and tapers.

Because a claycore deformer is an INVERSE point map and forward FFD has no closed-form inverse, the cage's control-point offsets SHALL BE the inverse warp: a point samples the undeformed field at `p + Bernstein(offsets, param(p))`. Newton-inverting per sample SHALL NOT be used, because iteration inside the evaluator breaks the single-source dialect the backends share; baking through a sampled volume SHALL NOT be used, because it ends the cage's editability.

The consequence SHALL be documented rather than implied, and SHALL NOT be described as `grab`'s character. The inverse cage is not the exact inverse of forward FFD: the two differ by a term proportional to how the basis VARIES along the displacement, so the error points the way the basis gradient does — over-travelling a drag toward rising weight and under-travelling one pointing away. `grab`'s weight always falls off along its drag, which is why that one always under-travels; a lattice does not inherit the sign. The magnitude SHALL be measured against the forward cage rather than asserted.

The cage SHALL store OFFSETS rather than positions, so an untouched cage is EXACTLY the identity with no special case, and material outside the box travels rigidly with the nearest part of the cage rather than being drawn onto it.

Evaluation SHALL be trivariate Bernstein, one formula for every cage size, with degree one less than the control-point count per axis — so two per axis is exactly trilinear, and the corner control points are interpolated.

Divisions SHALL be capped at four per axis. Unlike the mesh lattice, which evaluates once per vertex, this evaluates PER SAMPLE inside the raymarcher, at a cost of `nx * ny * nz` multiply-adds each time. The cap SHALL be stated where a caller meets it rather than discovered as a frame-rate cliff.

The cage SHALL live in the item's LOCAL space, as every other deformer does.

The declared Lipschitz factor SHALL be derived from the Bernstein DERIVATIVE bound — the control-point offset DIFFERENCES along each axis, scaled by the degree over the box's extent — and not from the offsets' magnitudes, which say how far the warp moves rather than how fast it varies.

The influence bound SHALL be the item's own bound grown by the largest control-point offset, since a Bernstein combination of the offsets cannot exceed the largest of them.

The item SHALL be marked inexact: a lattice is a bound field, not a distance.

It SHALL be reachable from `pyclay` and the C ABI, and SHALL carry a parity-corpus scene whose cage is NOT uniform, so a backend that applied a translation but not the basis fails rather than passes.

#### Scenario: An untouched cage changes nothing
- **WHEN** a lattice whose offsets are all zero is compiled onto an item
- **THEN** the field is identical to the undeformed item's at every point

#### Scenario: A uniformly dragged cage translates the item
- **WHEN** every control point is offset by the same vector
- **THEN** the field equals the undeformed field translated by it, since the basis is a partition of unity

#### Scenario: The bound follows how fast the cage varies, not how far
- **WHEN** two cages have the same largest offset but different differences between neighbouring control points
- **THEN** the one whose neighbours differ more reports the tighter safe step scale

#### Scenario: A cage steep enough to fold degrades rather than lying
- **WHEN** neighbouring control points differ by more than the cage's own cell spacing
- **THEN** the compiled tape reports a step scale below one rather than claiming the field is a distance

#### Scenario: Material outside the box travels rigidly
- **WHEN** a point outside the cage's box is evaluated
- **THEN** it samples at the offset of the nearest point of the cage, and is not drawn onto the box

#### Scenario: Divisions are capped where a caller meets them
- **WHEN** a caller asks for more than four control points on an axis
- **THEN** the binding refuses, naming the per-sample cost as the reason

### Requirement: A blob deformer applies noise with finite support
The kernel dialect SHALL provide a blob deformer: fractal noise offsetting the distance, weighted by a radial falloff, so an artist gets ZBrush's irregular swelling under the brush where `draw` gives a smooth one.

It SHALL reuse the EXISTING fractal and the EXISTING region weight rather than defining either again. Two noises would be two things to keep in step, and a second definition of reach is the bug avoided elsewhere by keeping one definition of the local cull test.

It SHALL offset the DISTANCE rather than warping the point, as the whole-item noise does and for the same reason: the wanted irregularity is the surface moving along its own normal, where a point warp would slide material sideways.

Outside the radius the field SHALL be untouched exactly, not nearly — that is what makes it a brush rather than a modifier, and what lets the influence bound stay tight.

The amplitude SHALL be signed and SHALL NOT be split into two verbs. The noise is signed, so one dab both swells and eats in, which is what reads as blobby rather than as a uniform bulge.

The declared Lipschitz factor SHALL charge BOTH the noise's own gradient and the REGION's: the offset is amplitude times weight times fractal, so by the product rule the weight varying over a constant noise contributes too. A tight radius has a steep weight even where the noise is flat, and that is exactly where a blob is used — so a bound charging only the noise would under-bound the common case.

The influence bound SHALL grow by the AMPLITUDE. The whole-item noise stores its amplitude in a different slot, so sharing its hull case would dilate by a coordinate — a wrong bound that shows up as culled-away geometry rather than as a failure.

The seed SHALL be an ordinary parameter rather than global state, so two items with the same seed look the same and an item's appearance never depends on the order it was compiled in.

It SHALL be reachable from `pyclay` and the C ABI, and SHALL carry a parity-corpus scene in which BOTH the falloff and the fractal are non-trivial, since a backend that applied one and ignored the other would otherwise pass.

#### Scenario: Past the radius the field is untouched
- **WHEN** a blob is applied and the field is evaluated outside its radius
- **THEN** the value equals the undeformed field's, to within float equality rather than a tolerance

#### Scenario: One dab swells and eats in
- **WHEN** the field is compared to the undeformed one at many points inside the radius
- **THEN** some points moved outward and some inward

#### Scenario: A tighter radius costs more step scale
- **WHEN** the same blob is declared with a small radius and a large one
- **THEN** the smaller reports the tighter safe step scale, because the region's own gradient is charged

#### Scenario: The influence bound follows the amplitude
- **WHEN** a blob whose region centre is far from the origin is bounded
- **THEN** the item's bound grows by the amplitude and not by the centre's coordinate

### Requirement: A degrading chain is detectable
The engine SHALL make the degradation of a chained region-verb edit observable, from the declared Lipschitz and the safe step scale it already tracks, so that a host can act before the field becomes unusable rather than discovering it by eye.

TWO causes SHALL be distinguishable, because they are different faults with
different cures and an aggregate cannot tell them apart:

- a verb that samples a document and hands back a volume, so a second
  application samples a VOLUME — and outside its band a volume reports a lower
  bound rather than a distance, so the blend works from the wrong value;
- a verb that appends a domain warp per gesture, so a stroke multiplies the
  chain's Lipschitz factor without any volume being involved at all.

#### Scenario: A chain reports its own degradation
- **WHEN** a region verb is applied to the result of a previous region verb
- **THEN** the declared Lipschitz and the safe step scale reflect the chained cost, and both are readable before the next edit

#### Scenario: The report names which mechanism degraded the chain
- **WHEN** a host asks a layer what its chain costs
- **THEN** it is told the steepest sample Lipschitz among the layer's volumes and the longest deformer chain on any of its items, alongside the aggregate

### Requirement: A sampled volume declares what its samples measure
A volume produced by sampling a field SHALL declare a sample Lipschitz no smaller than the slope its stored samples actually have.

Declaring 1 without measuring is not a conservative default, it is an
overclaim: the marcher steps by `f(p) / L`, so a field whose samples vary at
fourteen times the cell size and reports 1 licenses a step fourteen times too
long — the precise overstep the declared bound exists to prevent.

#### Scenario: Baking a steep chain does not claim to be 1-Lipschitz
- **WHEN** a steep field is sampled into a volume
- **THEN** the volume's declared sample Lipschitz is at least the slope measured between its stored samples

### Requirement: Redistancing bounds a baked field's Lipschitz
The engine SHALL be able to replace a sampled volume's stored samples with the distance to their own zero set, preserving each sample's sign and the surface it describes, and SHALL re-declare the volume's sample Lipschitz from what the result measures.

BAKING ALONE DOES NOT BOUND ANYTHING, and this requirement exists because the
opposite is the intuitive guess. Steepness is a property of the FIELD; sampling
it onto a lattice reproduces the steepness, and a finer cell makes it worse
rather than better because there are then more cells across the same steep
shell. Measured: two polish passes over a sphere bake to samples varying at 14x
the cell at 0.04, 31x at 0.02 and 38x at 0.01.

#### Scenario: A redistanced bake measures about 1
- **WHEN** a field whose samples vary many times the cell size is redistanced
- **THEN** its samples measure about one, and its zero set is where it was to the accuracy of the sampling

### Requirement: Consolidation bounds the cost of a stroke
After consolidation, a chain of region-verb edits SHALL hold its declared Lipschitz within a stated bound rather than multiplying per edit, and the bound SHALL NOT grow with the length of the chain.

The stated bound is `sqrt(3) x 1.10`: `sqrt(3)` is `cfi_volume`'s interpolation
factor, which any volume pays, and the 1.10 is the slack a redistanced bake is
allowed over a perfect 1.

A stroke is many gestures — a polish is repeated until it looks right, and a
move is a sequence of drags — so the per-edit multiplication is the difference
between a verb that exists and a verb that is usable.

#### Scenario: Repeated polishing stays usable
- **WHEN** the polish verb is applied several times over the same region with consolidation in effect
- **THEN** the declared Lipschitz stays within the stated bound and the form is not corrupted

#### Scenario: A drag sequence does not decay geometrically
- **WHEN** a move stroke is made as a sequence of drags with consolidation in effect
- **THEN** the safe step scale does not decay by a constant factor per drag

#### Scenario: Repeated consolidation does not grow the stored field
- **WHEN** a layer is consolidated repeatedly over the same region
- **THEN** the stored sample count does not grow with the number of consolidations

### Requirement: An alpha stamp displaces a surface under finite support
An SDF item SHALL accept a caller-supplied 2D scalar stamp applied as a distance offset under a bounded region, projected along a caller-supplied direction, with the same radial falloff the grab and magnify deformers use.

The engine SHALL NOT decode images. The caller SHALL supply `width * height` samples in [0, 1].

Outside the region the field SHALL be untouched, so a stamp is a local edit rather than a whole-item one.

#### Scenario: A stamp of all zeros is the identity
- **WHEN** an alpha whose samples are all zero is applied to an item
- **THEN** the field is identical to the item without it, exactly

#### Scenario: Material outside the region is untouched
- **WHEN** a point outside the stamp's radius is evaluated
- **THEN** the value is exactly the undeformed item's

#### Scenario: A stamp displaces along its direction
- **WHEN** a stamp with a non-zero amplitude is applied to a surface facing its direction
- **THEN** the surface moves by the amplitude scaled by the stamp value and the falloff at that point

### Requirement: An alpha's Lipschitz bound comes from its steepness
The tape SHALL derive an alpha's Lipschitz contribution from the largest difference between adjacent samples over the texel size, together with the falloff's own gradient — NOT from the largest sample value, which is flat when constant and would bound a stamp that displaces nothing as though it were the steepest possible.

#### Scenario: A flat stamp contributes no steepness of its own
- **WHEN** two alphas with the same peak value and amplitude but different sample steepness are applied
- **THEN** the flat one's safe step scale is higher, and a flat stamp's step scale does not change with its resolution

#### Scenario: A steep stamp costs step scale honestly
- **WHEN** an alpha with a large adjacent-sample difference is applied
- **THEN** the safe step scale falls, and raymarching the result produces no overshoot

#### Scenario: The same relief spread wider is less steep
- **WHEN** the same stamp is applied over a larger extent
- **THEN** the safe step scale rises, because the bound is a world-space slope rather than a sample difference

