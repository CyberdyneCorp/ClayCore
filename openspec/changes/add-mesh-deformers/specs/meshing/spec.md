# meshing

## ADDED Requirements

### Requirement: Whole-form deformers on a mesh layer
The library SHALL provide frame-relative deformers over a mesh layer's own vertices — `taper`, `twist` and `bend` — so that the transforms an artist shapes a blockout with are reachable on a mesh, not only on an SDF item. Without them the only route is converting a mesh into a field and back, which resamples the surface and discards the vertex colours and UVs that are the reason to hold a mesh layer at all.

A mesh deformer SHALL be applied as a FORWARD point map evaluated once per vertex, not as the inverse map an SDF item requires. This is the easier direction and the more accurate one: an SDF deformer must answer "where did the material at this point come from", which for free-form deformation has no closed-form inverse, and the SDF lattice accepts a bounded error and a low control-point cap as the price. A mesh deformer SHALL NOT inherit that approximation.

A deformer SHALL carry the FRAME it acts in, and the warp SHALL happen in that frame's local space. The canonical twist and taper are maps about one axis; an SDF item supplies the axis through its own transform and a mesh layer has none to supply, so the frame is the deformer's own.

A deformer SHALL apply to the WHOLE mesh, scaled by the mask, rather than to a brush region. A deformer states something about the form; a brush states something about a dab. A fully masked vertex SHALL be bit-identical to its rest position.

Vertices SHALL be deformed BY WELD CLASS. Position-coincident vertices — the split ones carrying a hard edge or a UV seam — SHALL remain coincident bit for bit, which evaluating each copy independently does not guarantee.

**Topology SHALL NOT change.** `indices` and `quads` SHALL be byte-identical before and after, as they are for every mesh verb, and the deformation SHALL be recorded by the same per-gesture record that carries the brushes so that it reverts bit-identically.

A deformer whose parameters describe no deformation — a zero angle, a unit scale, an empty span — SHALL move no vertex and record nothing, rather than rewriting every position with itself.

Deforming a mesh SHALL be DETERMINISTIC: the same mesh, frame and parameters SHALL produce bit-identical positions on every run and every platform.

The library SHALL NOT re-tessellate to recover from a deformation. Stretching the triangles a mesh already has is the accepted cost of fixed topology, `relax` is the verb that redistributes them, and remeshing remains outside this engine's scope.

#### Scenario: A taper on a mesh and on a field agree
- **WHEN** the same shape is tapered as a mesh layer and as an SDF item, and both are meshed
- **THEN** the two surfaces agree to within the sampling tolerance

#### Scenario: Topology and seams survive a deformation
- **WHEN** any deformer is applied to a mesh carrying split vertices at a hard edge
- **THEN** `indices` and `quads` are byte-identical, and every set of coincident vertices is still coincident bit for bit

#### Scenario: A mask holds part of the form still
- **WHEN** a deformer is applied to a mesh half of which is fully masked
- **THEN** the masked vertices are bit-identical to their rest positions and the unmasked ones moved

#### Scenario: An identity deformer is free
- **WHEN** a deformer is applied with parameters describing no deformation
- **THEN** no vertex moves, and the gesture record is empty

#### Scenario: A deformation reverts exactly
- **WHEN** a deformation is reverted through its record
- **THEN** the mesh is bit-identical to before it
