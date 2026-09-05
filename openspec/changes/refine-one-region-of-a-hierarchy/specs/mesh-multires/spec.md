## ADDED Requirements

### Requirement: A hierarchy refines a region rather than a surface

Subdivision depth SHALL be a property of a BASE PATCH and not of the surface, so
a model may carry level 5 where an artist is working and level 2 everywhere else.
Uniform depth makes the finest level anyone wants the level the whole model pays
for, which on a device with a memory ceiling is the difference between a session
and a refusal.

The refined region SHALL be identified by patches. It SHALL NOT be a set of fine
vertices: a vertex set cannot be subdivided deterministically, cannot be
balanced against its neighbours, and does not survive a reload as the same
region.

**Adjacent resident patches SHALL differ by at most one level**, with the
intervening levels built automatically where a request would violate that. The
patches that balancing adds SHALL be chosen in a stable order derived from patch
identity, because the mesh verbs promise bit-identical results on every run and a
balance driven by hash-map order produces a different surface on the second one.

**A transition SHALL be watertight by construction rather than by repair**, and
building it split that into two statements that are worth keeping apart.

The first is about the STORED SURFACE and is what makes the second possible. A
regional level SHALL evaluate the same stencils, against the same parent
neighbourhood, that a uniformly refined hierarchy would have evaluated — so every
vertex it stores holds the same bits, and a fine patch's boundary IS the exact
subdivision of the shared coarse edge because it is the same arithmetic. A patch
whose parent neighbourhood is incomplete SHALL be REFUSED rather than refined
approximately: Catmull-Clark's border rule at an edge that is not a border is a
different rule, and the fine side would leave the coarse side it is meant to
meet. `refine_patches_to_level` SHALL grow each intermediate level by a patch
ring so a graded request never meets that refusal.

The second is about EXPORTING one mesh across a transition, and it is not a
T-junction problem. A fine patch's corner vertex has taken one more subdivision
step than the coarse neighbour's has, so the two sides are a subdivision step
apart rather than a hairline apart, and a coarse face beside a finer patch SHALL
be emitted as a transition polygon carrying the finer boundary vertices. Any such
transition geometry is derived display data and SHALL NOT become the
authoritative sculpt representation.

**Adding a level SHALL NOT move the surface.** A newly refined region starts with
zero detail, so refining is invisible until something is authored into it — which
is what lets an artist refine speculatively.

A brush SHALL cross a level boundary without a seam. Its radius and falloff are
world-space and SHALL NOT change with the resolution underneath them, and a
smoothing or relaxing verb SHALL see a coarser neighbour rather than treating the
absence of fine vertices there as the absence of surface.

Refinement SHALL be priced before it is committed and refused before anything is
allocated, on the peak rather than the total, exactly as the existing subdivision
preflight does.

Refinement SHALL be MONOTONIC in this change: there is no removal. Removing a
refined region requires a decision about the detail authored there — discard,
bake, project, or refuse — and making one silently is worse than not offering
the operation.

#### Scenario: A refined patch holds the uniform hierarchy's own numbers
- **WHEN** a region is refined to a level and the same cage is refined uniformly to that level
- **THEN** every patch the regional hierarchy stores at that level carries the uniform hierarchy's positions bit for bit

#### Scenario: One region refines and the rest of the model does not
- **WHEN** a region of a hierarchy is refined several levels beyond the rest
- **THEN** the vertices, the authoritative memory and the per-dab cost follow the refined area rather than the finest level applied to the whole surface

#### Scenario: A steep request builds its own balancing rings
- **WHEN** a patch is refined several levels above a neighbour
- **THEN** the intervening patches are refined so that no two adjacent resident patches differ by more than one level, and the same request produces the same set of patches on every run

#### Scenario: Refining authors nothing
- **WHEN** a region is refined and no detail is written into the new level
- **THEN** the evaluated surface is unchanged within floating-point tolerance

#### Scenario: A brush crosses a level boundary
- **WHEN** a stroke is applied across a boundary between two resident levels
- **THEN** the surface stays watertight and shows no seam at the boundary, and the same stroke at a uniform level produces the same shape where the levels agree

#### Scenario: A mixed-depth hierarchy survives a reload
- **WHEN** a surface with different depths in different regions is saved and reloaded
- **THEN** the depths, the detail and the sculpt layers come back as they were, and a surface saved before regional depth existed loads with its uniform depth everywhere
