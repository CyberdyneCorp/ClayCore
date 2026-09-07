## ADDED Requirements

### Requirement: Every mesh-sculpting call states the space it works in

Each C entry point that takes or returns a position, a radius, a direction or a
normal for a mesh layer SHALL state, beside itself in the header, whether that
value is in the layer's LOCAL space or in the document's WORLD space.

**This is a correctness requirement, not a documentation one.** Two calls a host
makes back to back are in different spaces today: `clay_mesh_sculptor_raycast`
takes a world ray and returns a world hit, and `clay_mesh_sculptor_stamp` reads
its centre and radius as local. Feeding the first into the second — which is
what a host does to sculpt where the finger is — moves nothing on a transformed
layer and returns CLAY_OK with zero classes moved, which is the same answer the
ABI documents for a stamp that was fully masked or that amounted to no
displacement.

#### Scenario: A stamp fed a world hit on a transformed layer
- **WHEN** a host raycasts a transformed mesh layer and passes the returned hit position as a brush centre without converting it
- **THEN** the mismatch is discoverable from the header rather than only from a stamp that quietly did nothing

### Requirement: A sculpting session can declare that it speaks world space

A host SHALL be able to give a mesh sculpting session the frame its layer sits
under, after which every position, radius and direction it passes to that
session is in the document's world space and every position and normal read back
is in world space.

Declaring no frame SHALL be exactly the behaviour that existed before this
capability: the identity frame, in which world and local coincide, so a host
that has not heard of it is not opted into anything.

A host SHALL also be able to adopt the LAYER'S OWN transform without
reconstructing it, because reconstructing it is the step that can be got wrong
and because a layer's transform can carry a per-axis scale the host would have
to decompose to pass by hand. A sculptor over a standalone mesh, which belongs
to no layer, SHALL refuse that call rather than silently adopting an identity.

#### Scenario: A raycast feeds a stamp
- **WHEN** a session has adopted its layer's transform, a world ray is cast, and the returned hit position is passed straight back as a brush centre
- **THEN** the stamp lands under the ray, and the number of classes it moves is what the same stamp moves on an untransformed copy of the same mesh

#### Scenario: No frame is today's behaviour
- **WHEN** a session declares no frame
- **THEN** every call behaves exactly as it did before the capability existed

#### Scenario: A standalone mesh has no layer to adopt
- **WHEN** a sculptor built over a mesh that belongs to no document is asked to adopt its layer's transform
- **THEN** the call is refused rather than adopting an identity

### Requirement: A frame carries what a layer's transform carries

The frame a sculpting session accepts SHALL be able to express what a layer's
transform can express, including a PER-AXIS scale, or SHALL refuse a layer whose
transform it cannot express.

A per-axis scale silently read as uniform is the same class of failure as the
space mismatch above: the result is plausible, wrong, and reports success. The
document's own bounds and its combined mesh export both honour that scale
already, so a sculptor that cannot is inconsistent with the document it is
editing.

Normals SHALL be carried by the inverse transpose and never by the position map,
so that a non-uniform scale does not tilt them.

#### Scenario: A non-uniformly scaled layer
- **WHEN** a session adopts the transform of a layer carrying a per-axis scale
- **THEN** a world brush centre resolves to the same surface point the layer's bounds and its exported mesh put there, or the call refuses

#### Scenario: Normals under a non-uniform scale
- **WHEN** a hit normal is read back through a session whose layer carries a per-axis scale
- **THEN** it is the surface normal transformed by the inverse transpose, and it is unit length
