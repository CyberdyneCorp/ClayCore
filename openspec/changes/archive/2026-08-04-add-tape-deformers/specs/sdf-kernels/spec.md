# sdf-kernels — deformers reachable from a document

Delta for `add-tape-deformers`.

## MODIFIED Requirements

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
