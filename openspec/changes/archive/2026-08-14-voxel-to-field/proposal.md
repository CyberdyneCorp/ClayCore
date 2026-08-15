# Proposal: the sculpt should be able to come back

## Why

Issue #90. The engine has two representations and the bridge between them ran
one way. SDF to voxel is direct — `rasterize_tape`. Voxel back existed only as
a detour: mesh the grid, run `mesh::to_field` over the triangles, place the
result. That resamples twice, builds a BVH to do it, drops the palette, and
hands back an OPERAND that is no longer being sculpted.

Each representation owns half a toolkit. SDF has the primitives, booleans,
blends and deformers; voxels have the ten sculpting verbs, the masks and the
repair. A sculptor picks one and lives inside its half — and both flagship
examples pay for it, which is why `34_organic_character` cannot smooth its
arm/torso seam and `35_hard_surface_helmet` is a boolean assembly rather than
something trimmed and sculpted.

v0.30.0 sharpened the argument rather than softening it: the voxel verbs and
their display both fit an interactive frame on the reference device, so the
voxel side is where the interactive sculpting actually is — and there was no
way to get a sculpt back out of it without losing the document.

## What

**Direct, without a mesh in between.** The grid knows where its surface is; it
does not know the DISTANCE to it. `FieldVolume::sample` takes a callable and
`field::redistance` rewrites stored samples as the distance to their own zero
set, so the conversion is: say where the surface is, then measure to it.

**Where the surface is** is the decision that had to be made and is now made
explicitly: occupancy read by trilinear interpolation between cell CENTRES, so
the isosurface is a smooth surface rather than a staircase — the same choice
the smooth mesher makes (#108), and for the same reason. Nothing is filtered
by default, so nothing vanishes; `blur` is offered and costs thin features.

**Colour survives by conversion per palette entry.** A field has nowhere to
put a palette. So `clay_voxel_to_layer` converts once per entry the grid
carries and places one volume item each, with that entry's colour, unioned
without a blend. The union of the parts is the solid, and the interface
between two colours is interior to it.

**Non-destructive, and no format change.** A new layer beside the original,
holding ordinary volume items in an ordinary SDF layer. The grid is untouched,
so a host offers "go back" by keeping the original, and one misclick cannot
cost a parametric model. `.clayspace` does not move.

## The open questions, answered

1. **Destructive or a new layer?** A new layer, and NOT a new layer kind. The
   conversion is irreversible — the procedural history is gone — so a
   destructive default costs a parametric model to one misclick. Volume items
   in an SDF layer are machinery that already exists, which is why no format
   minor moves.
2. **What the palette maps to.** One volume item per entry. `FieldVolume` has
   no colour channel and a `Node` carries exactly one colour, so this is the
   only shape that preserves colour exactly using what exists. A colour
   channel on `FieldVolume` is the eventual answer if palettes get large; it
   is a storage change touching consolidation, serialisation and the brick
   cache, and it is not this.
3. **What tolerance, and measured how.** A DISTANCE bound, held at about a
   cell, with volume difference explicitly rejected: a thin spike has
   near-zero volume and large distance error, so a volume bound hides exactly
   the failure that matters. One cell is the quantum the lattice imposes and
   nothing here beats it.
4. **Does it belong with `add-mesh-layers`?** No. That change is a boundary
   for imported and exported triangle data; this converts between the two
   AUTHORING representations. Merging them would couple a conversion contract
   to an I/O one.

## The catch, stated rather than implied

A round trip is **not lossless in either direction**, and the spec says so:

- Going to voxels quantises to the lattice. A boolean's sharp edge becomes a
  staircase at the cell size and comes back as a rounded one. No care on the
  return recovers it.
- Coming back turns binary occupancy into a distance, and the band decides how
  much of the field means anything.

So this is a CONVERSION, not a view. **Preserved:** the surface within about a
cell, and the colour. **Not preserved:** exactness, and the procedural history
— once converted the items are gone and their parameters are no longer
editable.
