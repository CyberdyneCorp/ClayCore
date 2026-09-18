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

A region left unmaintained is not a smaller region, it is a different answer per fixture, and on some brushes it is worse than re-gathering: measured, 7 to 13 of 45 captured vertices survived an eleven-stamp stroke, and the reach followed whether the weight-1 centre happened to be among them — the whole drag pulling one pole of a sphere and 44% of it pulling the other, with the two representations 57% of the drag apart. At a brush radius of 0.15 against a detail resolution of 4 the remesher retired ALL of the captured entries inside one stroke and the reach fell to between 2.8% and 7.0%, below the 9.3% to 22.3% that re-gathering reaches on the same fixtures. With the remesher unable to touch the region the same rule reaches the whole drag on both representations and they agree to within 2e-5.

Because no maintenance operation can create a weight above the one it inherits — a split's child takes the MEAN of its parents' weights — the maintenance SHALL be verified by the weight it preserves and not only by the entries it counts: the largest weight carried at the end of a gesture SHALL be reported alongside the entry counts, so that a region which is numerically maintained while its high-weight core has been retired is visible as a number rather than as a surface that looks about right.

A `grab` stamp's remesh SHALL run at that stamp's own centre and SHALL NOT stay at the gesture's first sample. A remesh left at the first sample while the surface is dragged away refines nothing the gesture stretched: measured after a 1.5 drag, it left a longest edge of 1.12 against 0.36 with the centre following and 0.12 before the gesture reached that far.

The maintenance SHALL be reported as counts — entries carried, entries inserted by a split, entries retired by a collapse — so that a maintenance which silently does nothing is visible as a count and not merely as a surface that looks about right.

#### Scenario: A carried region survives the remesher
- **WHEN** a long grab stroke drags an adaptive surface far enough that the remesher splits and collapses inside the carried region, with the split count asserted to be non-zero
- **THEN** the region still carried at the end of the gesture accounts for every entry captured, plus those a split inserted, minus those a collapse retired

#### Scenario: A maintained region keeps the weight that carries the drag
- **WHEN** the same stroke is run on a brush small enough against the detail resolution that an unmaintained region loses every entry it captured
- **THEN** the largest weight still carried at the last stamp is near the one captured at the first, rather than a region that is maintained in number while the vertices that carry the whole drag have been retired

#### Scenario: A vertex born mid-gesture is not dragged twice
- **WHEN** the remesher splits an edge whose endpoints are both carried, partway through a grab
- **THEN** the new vertex ends the gesture where its parents' captured midpoint plus its weighted share of the whole drag puts it, rather than a further drag beyond it

#### Scenario: The grab remesh follows the stamp
- **WHEN** a grab drags an adaptive surface several brush radii from where it started
- **THEN** the stretched region is remeshed as the gesture passes through it, rather than the remesh repeatedly refining the place the gesture began
