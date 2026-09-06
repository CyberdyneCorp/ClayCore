## MODIFIED Requirements

### Requirement: Detail is stored in a transported local frame
Detail SHALL be stored as coefficients in a local frame — tangent, bitangent and normal — derived from the subdivided parent surface, and SHALL NOT be stored only as a world-space offset.

A world-space offset is adequate for small changes and fails at the case the feature exists for: when the parent surface rotates or bends, a stored world vector no longer points along the surface it belongs to, and detail shears away from the form that carried it.

The frame SHALL be TRANSPORTED rather than rebuilt from whichever neighbour is encountered first: a UV tangent where a valid parametrization exists, a deterministic geometric tangent otherwise, rotated by the shortest arc when the parent normal moves, with sign consistency enforced against the previous frame. An unstable frame rotates detail, and the artefact appears in a render rather than in a numeric test.

**A vertex's frame SHALL NOT depend on which patches are resident at its level.**
The frame is built by rotating the parent frame's tangent onto the child's own
level normal, and that normal is a sum over the faces incident to the vertex — so
where a level stores only part of the surface, the sum is one-sided and the frame
that a coefficient is stored in differs from the one a uniformly refined
hierarchy would have built. Measured on a region boundary, that difference
reaches 0.104 on unit vectors, about 6 degrees.

This is a STORAGE requirement and not a shading one. Because a level's position is
its subdivided parent plus the frame applied to the detail, a frame that differs
means the same authored coefficient reconstructs to a different world offset — so
the guarantee that a regional level holds a uniform hierarchy's numbers survives
only while the detail at a boundary vertex is zero. The normal that builds the
frame SHALL therefore be summed over the vertex's COMPLETE incident face set,
including faces that live at a coarser level, and the same completeness SHALL
apply to the incremental path a stamp takes rather than only to a full rebuild.

Detail SHALL be authoritative in single precision. It SHALL NOT be quantized in this change: high-frequency detail is where a visible artefact appears first, and any compression waits on a measured error bound.

#### Scenario: A wrinkle survives a bend
- **WHEN** detail is sculpted at a fine level and the parent surface is then bent at a coarse one
- **THEN** the detail remains attached to the surface, in the same local orientation, rather than shearing away from it

#### Scenario: A small deformation does not flip the frame
- **WHEN** a parent surface is deformed slightly and the frames are rebuilt
- **THEN** no frame reverses sign, and the reconstructed detail does not rotate

#### Scenario: A coefficient at a region boundary means what it means densely
- **WHEN** the same detail coefficients are written into every vertex of the top level of a regionally refined hierarchy and of a uniformly refined hierarchy over the same cage
- **THEN** every vertex the two hierarchies share evaluates to the same position, including the vertices on the boundary of the refined region

## ADDED Requirements

### Requirement: A vertex's neighbourhood is complete across a depth boundary

Where a level stores only part of the surface, a vertex on the edge of what it
stores SHALL still be given its COMPLETE surface neighbourhood — the faces,
corners and vertices that a uniformly refined hierarchy would have given it,
including those that live at a coarser level. A regional level's own connectivity
ends at the region rim, and every walk built on it therefore sees an open border
where the surface in fact continues.

The failure this forbids is not a wrong neighbour but a MISSING one, and it is
silent: no site refuses, clamps or misdirects, because a coarse neighbour has no
vertex, no weld class and no face at the fine level to be picked wrongly. The
damage is truncation and one-sidedness — an averaged position biased into the
refined region, a normal tipped toward it, a flood fill that stops.

The neighbourhood SHALL be provided as ONE topology answer that names each
incident face by the level it lives at, and SHALL NOT be reimplemented per
algorithm. It SHALL NOT, however, be a single callback carrying every payload:
the readers of a neighbourhood want nine different things — a position at the
evaluated surface, a position at the subdivided surface, detail coefficients, a
geometric normal, a colour, a workset slot, a triangle with its corner and
interior angle, an incident face, and a shared-face count — and a smoothing verb
re-reads its neighbourhood once per pass, so a materialized answer read many
times is required and a per-neighbour callback is not. The two normal evaluators
that exist over a level — one angle-weighted over triangles, one an unweighted
sum over faces — SHALL remain two; merging them would change one of their results.

The order in which a neighbourhood is reported SHALL be deterministic and stated,
because float addition is not associative and the sums taken over a neighbourhood
are part of the result.

An identity that names a vertex at another level SHALL carry the same staleness
discipline as the existing seed token: a level change or a cache generation
change renumbers a level's vertices, and a regional level's numbering is
compacted to what it stores.

**A depth transition SHALL NOT be reported as a border of the model.** Automasking
that protects the open edge of an unclosed mesh SHALL NOT fire at an internal
seam between two resident levels, because the artist cannot see that seam and did
not put it there.

#### Scenario: A boundary vertex has the neighbourhood a uniform hierarchy gives it
- **WHEN** a vertex on the boundary of a refined region is asked for its incident faces
- **THEN** the set is the one a uniformly refined hierarchy would report for the same vertex, counting the faces that live at the coarser level

#### Scenario: A display normal at a boundary matches the uniform hierarchy's
- **WHEN** a regionally refined hierarchy and a uniformly refined hierarchy over the same cage are evaluated to the same level
- **THEN** the display normals and the transported frames agree at every vertex the two share, including those on the region boundary

#### Scenario: A smoothing verb is not dragged inward at a seam
- **WHEN** a smoothing stroke is applied across the boundary of a refined region, and the same stroke is applied to a uniformly refined hierarchy
- **THEN** the vertices the two hierarchies share finish in the same place, rather than the boundary being pulled into the refined region by a one-sided average

#### Scenario: Boundary automasking ignores an internal seam
- **WHEN** boundary automasking is enabled and a stroke crosses a depth transition
- **THEN** no vertex is faded for being at the transition, while a vertex on the model's own open edge is still faded

#### Scenario: A neighbourhood is reported in a stated order
- **WHEN** the same neighbourhood is requested twice, on any platform
- **THEN** the neighbours come back in the same order, and any sum taken over them is bit-identical

### Requirement: A mixed-depth hierarchy exports as one watertight mesh

A hierarchy whose patches carry different depths SHALL be exportable as ONE mesh
that is watertight BY CONSTRUCTION. Today a caller that follows the documented
per-patch path — copying each patch's block at that patch's effective level —
gets a mesh with open edges at every depth boundary, and a caller that exports a
single level gets only the patches resident at it.

Watertightness SHALL be achieved by IDENTITY and never by tolerance welding. A
vertex shared between a coarse face and a finer patch SHALL be one vertex in the
output, carrying the value the finer side computed, because that value is one
subdivision step closer to the limit. The two sides are a SUBDIVISION STEP apart
rather than a hairline apart — measured at 8.7% of a cage edge — so any epsilon
large enough to close the gap would weld unrelated geometry, and the hierarchy's
own adjacency already welds at exactly zero for the same reason.

Adopting the finer value at a shared vertex moves the coarse face's corner, so
EVERY coarse face incident to that vertex SHALL adopt it — including a face whose
patch meets the finer region only at a CORNER and therefore has no split edge and
no T-junction. Emitting only for faces that share an EDGE with a finer patch does
not remove the crack; it relocates it one face over onto a boundary between two
coarse faces, where it presents as an unrelated defect.

The export SHALL keep the mesh's QUAD LIST wherever the transition allows it, and
SHALL NOT keep a quad list that does not describe its indices. Those two are in
tension at a split edge and the arithmetic decides between them: a coarse quad
with one split edge is a pentagon, and a polygon with an ODD number of boundary
vertices has no quadrangulation at all — four edges per quad counts every
interior edge twice, so the boundary count must be even however many vertices are
added inside. Making it even would mean splitting a second edge of that face,
whose new vertex the neighbouring coarse face must then also carry, and so on out
of the transition and across the model.

So the guarantee is stated as a boundary rather than as an absolute: an export
whose transitions are all CORNER-ONLY — where the finer region is met at a cage
vertex and no coarse edge is split — SHALL be emitted as quads with the quad list
intact, as SHALL every uniform-depth export; an export containing a split edge
SHALL be emitted as a triangle list with no quad list, because a quad list
describing indices that no longer exist is a lie a saved document would carry.

The output SHALL be DETERMINISTIC — the same hierarchy emits the same faces in the
same order on every run and on every platform — and STABLE under re-refinement:
refining an unrelated region SHALL leave the emitted faces of a coarse face whose
neighbourhood residency did not change byte-identical.

Transition geometry SHALL be DERIVED and SHALL NOT be serialized. It is a
function of the cage, the subdivision rule and the per-level patch sets, all of
which a stored surface already carries, so adding it SHALL NOT change the
serialization version and SHALL NOT make an older reader refuse a document. It
SHALL NOT become the authoritative sculpt representation.

An export that cannot present the whole surface SHALL SAY SO rather than return a
partial result reported as success.

#### Scenario: A mixed-depth export has no open edges
- **WHEN** a hierarchy with different depths in different regions is exported as one mesh
- **THEN** the result has no boundary edge that the base cage did not have, and no vertex is welded by proximity

#### Scenario: A coarse patch touching a finer one only at a corner is emitted too
- **WHEN** a coarse patch shares only a cage vertex, and no edge, with a finer patch
- **THEN** it is emitted with the finer side's value at that corner, and no crack appears on its boundary with the coarse patches beside it

#### Scenario: The quad list survives exactly as far as the split edges allow
- **WHEN** a hierarchy whose depths all agree, or whose transitions are all corner-only, is exported
- **THEN** every emitted face is a quad and the mesh's quad list describes its indices
- **WHEN** the export contains a coarse face with a split edge
- **THEN** that face is emitted as a pentagon and the mesh carries no quad list at all, rather than a quad list that describes triangles it does not have

#### Scenario: Refining elsewhere does not disturb a transition
- **WHEN** a region of a hierarchy is refined and an unrelated coarse face's neighbourhood residency is unchanged
- **THEN** that face emits the same transition faces, byte for byte, as it did before

#### Scenario: A mixed-depth export survives a round trip without a format change
- **WHEN** a mixed-depth hierarchy is saved, reloaded and exported
- **THEN** the export is identical, and a build without the transition feature still reads the document
