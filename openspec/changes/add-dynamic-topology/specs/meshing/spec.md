# meshing — the fixed-topology contract, scoped

Delta for `add-dynamic-topology`.

## MODIFIED Requirements

### Requirement: Fixed-topology mesh brushes
The library SHALL provide vertex-displacement brushes over a mesh's own triangles: `grab`, `draw`, `inflate`, `smooth`, `pinch`, `flatten`, `clay`, `crease`, `scrape`, `polish` and `snakehook`.

**Topology SHALL NOT change.** No verb SHALL create, split, delete or reorder a polygon or a vertex. `Mesh::indices` and `Mesh::quads` SHALL be byte-identical before and after any verb, so a quad mesh sculpted here is still a quad mesh.

That contract belongs to THIS sculptor and is not a statement about the library. A separate representation whose connectivity changes under a brush is specified in `dynamic-topology`; it is a representation a caller converts into deliberately, never a mode these verbs enter. The guarantee here is what makes a mesh layer worth holding after a retopology pass, and adding adaptive topology elsewhere SHALL NOT weaken it.

`draw` SHALL displace along the region's AVERAGED normal — one shared direction per stamp — and `inflate` SHALL displace along each vertex's OWN normal. That difference SHALL be the distinction between the two verbs.

`pinch` SHALL be ONE signed deformation, gathering tangentially toward the brush centre for a positive strength and spreading (magnify) for a negative one, matching the convention the field and voxel verbs already set. Its displacement SHALL be tangential — the component along the vertex normal SHALL be removed — so a pinch gathers along the surface rather than sinking the region.

`flatten` SHALL take the same `TwoSided` / `CutOnly` / `FillOnly` mode the field flatten established, because cut-only is Trim Dynamic and hPolish.

Every verb SHALL take an optional gate and SHALL scale each class's weight by `1 - gate`, so one rule masks all eleven verbs with no per-verb code.

Applying a verb SHALL be DETERMINISTIC: the same mesh, the same settings and the same stamps SHALL produce bit-identical positions on every run and every platform.

#### Scenario: Topology survives every verb
- **WHEN** each verb is applied to a quad-exported mesh
- **THEN** `indices` and `quads` are byte-identical to the input, and only `positions` and `normals` differ

#### Scenario: Draw and inflate differ where it matters
- **WHEN** `draw` and `inflate` are applied at equal strength to a region straddling a saddle
- **THEN** `draw` moves every vertex in one direction and `inflate` moves them along their own normals, and the two results differ

#### Scenario: A gate protects what it covers
- **WHEN** a displacement verb and `smooth` are each applied over a region half of which is fully gated
- **THEN** the gated vertices are bit-identical to their input positions and the ungated ones moved

### Requirement: Whole-form deformers on a mesh layer
The library SHALL provide frame-relative deformers over a mesh layer's own vertices — `taper`, `twist` and `bend` — so that the transforms an artist shapes a blockout with are reachable on a mesh, not only on an SDF item. Without them the only route is converting a mesh into a field and back, which resamples the surface and discards the vertex colours and UVs that are the reason to hold a mesh layer at all.

A mesh deformer SHALL be applied as a FORWARD point map evaluated once per vertex, not as the inverse map an SDF item requires. This is the easier direction and the more accurate one: an SDF deformer must answer "where did the material at this point come from", which for free-form deformation has no closed-form inverse, and the SDF lattice accepts a bounded error and a low control-point cap as the price. A mesh deformer SHALL NOT inherit that approximation.

A deformer SHALL carry the FRAME it acts in, and the warp SHALL happen in that frame's local space. The canonical twist and taper are maps about one axis; an SDF item supplies the axis through its own transform and a mesh layer has none to supply, so the frame is the deformer's own.

A deformer SHALL apply to the WHOLE mesh, scaled by the mask, rather than to a brush region. A deformer states something about the form; a brush states something about a dab. A fully masked vertex SHALL be bit-identical to its rest position.

Vertices SHALL be deformed BY WELD CLASS. Position-coincident vertices — the split ones carrying a hard edge or a UV seam — SHALL remain coincident bit for bit, which evaluating each copy independently does not guarantee.

**Topology SHALL NOT change.** `indices` and `quads` SHALL be byte-identical before and after, as they are for every mesh verb, and the deformation SHALL be recorded by the same per-gesture record that carries the brushes so that it reverts bit-identically.

A deformer whose parameters describe no deformation — a zero angle, a unit scale, an empty span — SHALL move no vertex and record nothing, rather than rewriting every position with itself.

Deforming a mesh SHALL be DETERMINISTIC: the same mesh, frame and parameters SHALL produce bit-identical positions on every run and every platform.

A FIXED-TOPOLOGY mesh layer SHALL NOT be re-tessellated to recover from a deformation. Stretching the triangles a mesh already has is the accepted cost of fixed topology, and the recovery is a conversion to the adaptive representation specified in `dynamic-topology` rather than a hidden re-tessellation of this one. `relax` SHALL NOT be documented as the recovery for a deformation: a taper leaves a cross-section with the same vertex count around a smaller circumference, which is anisotropy rather than uneven spacing, and a verb that slides vertices along the surface cannot change how many of them a cross-section has.

The sentence this replaces said remeshing was outside the engine's scope. That was a decision about the whole library and it was reversed on 2026-08-29 for the reason recorded in `openspec/ROADMAP.md`: shipping fixed-topology brushes made "the stretch is your signal to retopologise elsewhere" a signal to leave the engine. What survives the reversal is the narrower and more useful rule stated here — this layer does not re-tessellate, and a caller who wants adaptive topology asks for it by name.

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
