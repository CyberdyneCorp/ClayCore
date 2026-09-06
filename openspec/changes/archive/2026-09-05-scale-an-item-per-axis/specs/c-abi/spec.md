# c-abi — scale an item per axis

Delta for `scale-an-item-per-axis`.

## ADDED Requirements

### Requirement: A per-axis scale across the ABI
The C API SHALL let a host give a placed node and an item under construction a per-axis scale, so that the shapes a boolean workflow cuts with — a slot, an oval hole, a stretched chamfer — are expressible from a host. Until this existed every transform in the interface took ONE factor, and the primitives that carry their own extents could say a non-uniform shape at creation and never afterwards.

The header SHALL state what a per-axis scale costs, because the cost is the opposite of the one a caller braces for: the field stays 1-Lipschitz so nothing gets slower and the safe step scale does not move, and what is lost is EXACTNESS — the value becomes a bound on the distance rather than the distance. A uniform value, the default included, SHALL keep the field exact.

Every component SHALL be greater than zero, and the refusal SHALL be typed with the document unchanged.

The per-axis scale SHALL NOT be added to the flat item descriptor. That struct is zero-filled by its own contract, so a zeroed per-axis scale would have to be read as `(1, 1, 1)` rather than as what it says; the builder is where it is composed.

#### Scenario: A placed primitive is squashed
- **WHEN** a host gives a placed unit sphere a per-axis scale of (2, 1, 1)
- **THEN** the field's surface crosses x at 2 and y at 1

#### Scenario: The builder's two scales multiply
- **WHEN** an item is built with a uniform scale of 2 and a per-axis scale of (1.5, 1, 1) and then placed
- **THEN** the placed node reports an effective scale of (3, 2, 2)

#### Scenario: A degenerate scale is refused
- **WHEN** any entry point taking a per-axis scale is given a zero or negative component, or a null pointer
- **THEN** it is refused as an invalid argument and the document is unchanged

### Requirement: Both transform setters write the whole transform
This ABI does not do partial updates — its setters take the whole value because C has no idiomatic "leave this one alone" argument — so the uniform transform setter SHALL mean "this node's scale is uniform s" and SHALL collapse any per-axis scale the node carried.

That SHALL be stated at the declaration rather than left to be discovered, because the alternative behaviour — quietly keeping a component the call did not name — is the kind of thing a host would only find from a wrong-looking model.

Both setters SHALL be one command and one undo step, and the recorded inverse SHALL capture the per-axis scale as well as the transform, so one undo of a squash restores exactly what was there.

#### Scenario: A uniform edit collapses a squash
- **WHEN** a node carrying a per-axis scale is given a uniform transform
- **THEN** its scale becomes uniform and the uniform reader answers again

#### Scenario: One undo restores both halves
- **WHEN** a squashed node is given a different transform and the edit is undone
- **THEN** both its transform and its per-axis scale are exactly what they were

### Requirement: The uniform transform reader refuses what it cannot express
The transform reader that reports a single scale factor SHALL refuse a node carrying a non-uniform scale, as an invalid argument, with nothing written.

One float cannot express three, and every way of pretending otherwise is a lie a host would act on: reporting the uniform factor alone describes a differently-shaped item, and a host doing read-change-write through the uniform setter would silently round the artist's squash away. This is the lesson of the reading surface that preceded it — a positional question answered by a call that could not answer it — and the rule taken from it is that a reader which cannot express what is there must not answer.

A per-axis reader SHALL exist that always can, and SHALL answer for EVERY item: a node with a uniform scale s SHALL report `(s, s, s)`, so a host with one manipulator for both cases can call it alone and never branch. What it returns SHALL be what the per-axis setter takes.

#### Scenario: The uniform reader refuses a squashed node
- **WHEN** the single-factor transform reader names a node carrying a per-axis scale
- **THEN** it is refused as an invalid argument and nothing is written

#### Scenario: The per-axis reader answers for a uniform node
- **WHEN** the per-axis reader names a node placed through the uniform setter with a scale of s
- **THEN** it reports (s, s, s)

#### Scenario: A squashed placement round trips
- **WHEN** a squashed node is read and the values are passed straight back to the per-axis setter
- **THEN** reading again gives the same values

### Requirement: A mesh transform takes a per-axis scale
The ABI SHALL carry a mesh transform taking a per-axis scale. A mesh is real vertices and no field, so nothing about exactness applies to it; what does apply is the NORMALS.

Positions SHALL go through the matrix and normals through its INVERSE TRANSPOSE, renormalized. The uniform call may rotate a normal and stop, because a similarity leaves a direction unchanged; a squash does not, and transforming a normal as a direction leaves every one of them off the surface. A normal that was already degenerate SHALL be left as it was rather than becoming a NaN: this call moves a mesh, it does not repair one.

#### Scenario: Normals follow the surface, not the rotation
- **WHEN** a meshed sphere is transformed with a per-axis scale of (3, 1, 1)
- **THEN** every normal is unit length and agrees with the resulting ellipsoid's gradient, which merely rotating them would not
