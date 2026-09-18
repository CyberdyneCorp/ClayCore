## MODIFIED Requirements

### Requirement: Stamps apply to a mesh
The stroke engine SHALL gain `apply_to_mesh`, a fourth consumer of `resolve_stroke`'s stamps alongside `apply_to_grid`, `apply_to_mask` and `stamps_to_nodes`.

It SHALL take a mesh, its adjacency, the resolved stamps, the verb and its settings, and SHALL apply one stamp per resolved stamp, so spacing, pressure response, deterministic jitter, taper, steady stroke and buildup-versus-clamped accumulation reach mesh sculpting with no new machinery.

Each stamp's world radius and strength SHALL come from the stamp rather than from the settings, so pressure and taper shape a mesh stroke exactly as they shape a voxel one.

Under `Buildup` accumulation, overlapping stamps SHALL act repeatedly; under `Clamped`, the stroke SHALL reach its strength once however many stamps overlap. This is what makes one `clay` stamp into ClayBuildup.

`snakehook` SHALL derive its per-stamp displacement from the motion between consecutive stamps, so a drag is a drag rather than a repeated identical pull. `grab` SHALL derive its displacement from the motion since the stroke BEGAN, applied to the region it captured — see "A grab carries the region it captured" — because a grab is one gesture moving one piece of surface and not a sequence of independent nudges.

It SHALL return the number of stamps that actually moved a vertex, and SHALL report the accumulated vertex deltas for the whole call as one coalesced record when the caller asks for it.

#### Scenario: A stroke inherits the engine
- **WHEN** a stroke is resolved with taper, jitter and a pressure curve and applied to a mesh
- **THEN** the stamps that reach the mesh are exactly the ones `resolve_stroke` produced, at their radii and strengths

#### Scenario: One stroke is one undo step
- **WHEN** a whole stroke is applied with a delta record requested and the record is reverted
- **THEN** the mesh is bit-identical to its pre-stroke state

## ADDED Requirements

### Requirement: A grab carries the region it captured
A `grab` stroke SHALL gather its region ONCE, at its first stamp, and SHALL carry that region — the items, the positions they were gathered at, and their falloff weights — for the whole gesture. Every stamp SHALL place each carried item at its captured position offset by its weight times the motion of the stroke SINCE ITS FIRST SAMPLE, rather than re-gathering a region around a point the surface has already left.

This SHALL hold identically on a fixed mesh, on a multiresolution level and on an adaptive surface, because it is a fact about what a grab IS and not about a representation.

Re-gathering every stamp around the first sample is what this requirement replaces, and it is replaced because the falloff weights shrink as the surface leaves the anchor: measured on a unit sphere at `cube_sphere(24)` with a brush radius of 0.3, a grab reached 41% of a 0.6 drag and 18% of a 1.5 drag, on both representations. A carried region reaches the whole drag at its centre, which is the weight-1 vertex, on both.

Centring each stamp on the CURSOR and re-gathering SHALL NOT be adopted, and the reasons are two and independent. It makes the representations disagree by a tenth of the drag, where a carried region makes them agree to within 2e-5. And it loses the surface: the vertex at the centre moves by its falloff weight rather than the whole delta, so the brush falls behind the cursor and is soon outside its own radius — measured, 11 of 26 stamps applied on a 1.5 drag, on both representations, after which the stroke moves nothing at all.

#### Scenario: A grab reaches the whole drag on both representations
- **WHEN** a grab stroke pulls a distance away from a fixed mesh and the same stroke pulls away from an adaptive surface built from the same model
- **THEN** each surface's furthest travel along the drag is the whole drag, and the two representations agree to within 1e-3

#### Scenario: The reach is the rule's and not the fixture's
- **WHEN** the same grab is measured with the drag's sign reversed and on the opposite pole of the same model
- **THEN** the reach is the same, rather than depending on which way the fixture happened to face

#### Scenario: A curved drag carries the same region
- **WHEN** a grab stroke follows a curve rather than a straight line
- **THEN** the captured region is carried along the whole path and arrives at the last sample — displaced by the motion from the first sample to the last, which on a curve is shorter than the path the cursor travelled — rather than being re-gathered around the first

#### Scenario: A grab is one gather, not one per stamp
- **WHEN** a grab stroke of many stamps is applied to a fixed mesh
- **THEN** the surface is walked once for the gesture rather than once per stamp, and the stroke costs less than the same stroke re-gathering every stamp

### Requirement: An adaptive surface maintains a carried region rather than losing it
An adaptive surface retires vertex identities when it collapses an edge and creates them when it splits one, and it does so BETWEEN the stamps of a gesture. A gesture carrying a captured region SHALL therefore have that region MAINTAINED by the remesh rather than left to decay: a split inside the carried region SHALL insert its new vertex into the region with the midpoint of its parents' CAPTURED positions and the mean of their weights, and a collapse SHALL remove the entry whose vertex it retired.

A new vertex's captured position SHALL be reconstructed from its parents' captured positions and SHALL NOT be read from the surface, because its parents have already taken part of the drag and reading the surface would apply that part to it twice.

A COLLAPSE SHALL NOT RETIRE A CARRIED VERTEX for the length of the gesture. Maintaining the region is necessary and is not sufficient: measured over twelve fixtures, maintenance alone keeps the region growing — nine captured entries becoming fifty-nine to sixty-four live, forty-five becoming four hundred and thirty-four to five hundred and thirty-eight — and still loses the weight that carries the drag, because the weight-one centre is collapsed in the first half of the gesture and no operation can create a weight above the one it inherits. With maintenance alone the gesture reaches between 73% and 97% of the drag and the two representations disagree by up to 0.375 in world units, which is worse than re-gathering; with the collapse refused, every one of the twelve fixtures reaches the whole drag and the two representations agree to within 1.6e-4.

Refusing those collapses SHALL NOT be treated as the remesher giving up its work, because measurably it does not: the collapses refused are the ones consuming the gesture's own region, so the region stays dense and the splits refine what the gesture stretched. Measured after a 1.5 drag, the surface left by the protected rule has a longest edge of 0.27 against 0.62 for the unprotected maintenance, 0.12 before the gesture reached that far, and 1.32 with no remesher at all; on four of six fixtures the protected rule leaves the surface exactly as fine as re-gathering does. The gesture is also CHEAPER than re-gathering — between 0.39 and 0.89 of it — because a gesture walks the surface once instead of once per stamp.

A split with exactly ONE carried parent SHALL also insert its new vertex, at the midpoint of the carried parent's CAPTURED position and the uncarried parent's CURRENT position, with half the carried parent's weight. The uncarried parent has taken no part of the drag, so where it is now is where it was captured, and the child is exactly reconstructible. This does not change the reach; it keeps the surface finer and reduces the collapses the rule above has to refuse.

A carried vertex the REMESHER ITSELF moves — a collapse placing its survivor, a relaxation sliding one along the surface — SHALL take the same shift in its captured position. Without it the next stamp writes the captured position plus its weighted share of the drag and puts the vertex back where the remesher moved it from, so the relaxation inside a gesture has no effect at all. Measured, between one and two hundred and ninety-one carried vertices are moved by the remesher on every stamp of every fixture.

A region left unmaintained is not a smaller region, it is a different answer per fixture, and on some brushes it is worse than re-gathering: measured, 7 to 13 of 45 captured vertices survived an eleven-stamp stroke, and the reach followed whether the weight-1 centre happened to be among them — the whole drag pulling one pole of a sphere and 44% of it pulling the other, with the two representations 57% of the drag apart. At a brush radius of 0.15 against a detail resolution of 4 the remesher retired ALL of the captured entries inside one stroke and the reach fell to between 2.8% and 7.0%, below the 9.3% to 22.3% that re-gathering reaches on the same fixtures. With the remesher unable to touch the region the same rule reaches the whole drag on both representations and they agree to within 2e-5.

Because no maintenance operation can create a weight above the one it inherits — a split's child takes the MEAN of its parents' weights — the maintenance SHALL be verified by the weight it preserves and not only by the entries it counts: the largest weight carried at the end of a gesture SHALL be reported alongside the entry counts, so that a region which is numerically maintained while its high-weight core has been retired is visible as a number rather than as a surface that looks about right.

A `grab` stamp's remesh SHALL run at that stamp's own centre and SHALL NOT stay at the gesture's first sample. A remesh left at the first sample while the surface is dragged away refines nothing the gesture stretched: measured after a 1.5 drag, it left a longest edge of 1.12 against 0.36 with the centre following and 0.12 before the gesture reached that far.

The maintenance SHALL be reported as counts — entries carried, entries inserted by a split, entries retired by a collapse, entries the remesher moved, and collapses refused — so that a maintenance which silently does nothing is visible as a count and not merely as a surface that looks about right. Collapses refused SHALL be reported as collapses AVOIDED and not as refusal events: the remesher asks about the same edge on each of its passes on each stamp, so the event count runs an order of magnitude above the operations it actually prevented.

#### Scenario: A carried region survives the remesher
- **WHEN** a long grab stroke drags an adaptive surface far enough that the remesher splits and collapses inside the carried region, with the split count asserted to be non-zero
- **THEN** the region still carried at the end of the gesture accounts for every entry captured, plus those a split inserted, minus those a collapse retired

#### Scenario: A maintained region keeps the weight that carries the drag
- **WHEN** the same stroke is run on a brush small enough against the detail resolution that an unmaintained region loses every entry it captured
- **THEN** the largest weight still carried at the last stamp is near the one captured at the first, rather than a region that is maintained in number while the vertices that carry the whole drag have been retired

#### Scenario: A carried vertex is not collapsed away
- **WHEN** a grab drags an adaptive surface with a brush small enough against the detail resolution that the remesher would otherwise retire the whole captured region
- **THEN** no entry of the carried region is retired for the length of the gesture, the largest weight carried at the last stamp is the one captured at the first, and the gesture reaches the whole drag on both representations

#### Scenario: Refusing those collapses does not coarsen the surface
- **WHEN** the same stroke is compared against the same stroke with the collapse refusal removed
- **THEN** the longest edge left on the surface is no longer with the refusal than without it, so the refusal is not the adaptive representation giving up the work it exists to do

#### Scenario: A vertex born of one carried parent joins the region
- **WHEN** the remesher splits an edge with exactly one endpoint in the carried region, partway through a grab
- **THEN** the new vertex joins the region carrying half its carried parent's weight, and ends the gesture where the midpoint of that parent's captured position and the other endpoint's position, plus its weighted share of the whole drag, puts it

#### Scenario: The remesher's own movement survives the next stamp
- **WHEN** the remesh relaxes a carried vertex along the surface between two stamps of a grab
- **THEN** the next stamp writes that vertex from its shifted captured position, so the relaxation is still visible at the end of the gesture rather than undone by the following write

#### Scenario: A vertex born mid-gesture is not dragged twice
- **WHEN** the remesher splits an edge whose endpoints are both carried, partway through a grab
- **THEN** the new vertex ends the gesture where its parents' captured midpoint plus its weighted share of the whole drag puts it, rather than a further drag beyond it

#### Scenario: The grab remesh follows the stamp
- **WHEN** a grab drags an adaptive surface several brush radii from where it started
- **THEN** the stretched region is remeshed as the gesture passes through it, rather than the remesh repeatedly refining the place the gesture began
