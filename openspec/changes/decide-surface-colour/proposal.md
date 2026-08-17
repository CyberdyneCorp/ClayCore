# Proposal: what surface colour is, and what it is not

## Why this is a decision record

#118's workstream E asks for "a decision record first (spec change, not code):
per-surface-point colour on an SDF layer means a sampled colour field beside the
distance field — cost, storage, and whether the voxel palette generalises — or a
declared non-goal with the bake/export story as the answer."

`docs/sculpt_comparison.md` calls this "the axis worth *deliberately* revisiting
rather than drifting into, because it is 3DCoat's moat and it caps the product
ceiling."

**The premise needs correcting before the decision can be made.** The epic says
"there is no polypaint and no PBR painting". Half of that is wrong, and the
half that is right is a different shape than the sentence implies.

## What actually exists, measured rather than assumed

Every claim below was verified against this build.

**Freehand colour strokes on an SDF layer work today.** `apply_stroke` takes an
op and a colour, so `Op::Paint` with a stroke is a paint brush — with pressure,
spacing, jitter, taper and masking, like any other stroke. Measured: a 24-sample
stroke appended 9 stamps, the field came back **bit-identical**, and the colour
at a painted point read `(0.878, 0.188, 0.125)` — exactly the authored
`#e03020` — against `0.69` grey elsewhere. That is polypaint.

**A sampled volume already carries per-sample colour.** `FieldVolume` has an
optional RGB8 array and `ctape_volume` reads it, on every backend
(`add-volume-color-channel`, shipped). That IS "a sampled colour field beside
the distance field" — the thing this decision was going to weigh building.

**And consolidation converts one into the other, exactly.** Painting as items is
parametric and re-editable; consolidating bakes the layer to a volume whose
colour is per sample at a chosen cell size. Measured across a consolidate at
cell 0.02: the colour at every probe was **identical before and after**.

Also present: per-item `Node::color`; the 256-entry voxel palette with
`paint`, `paint_brush` and `paint_mirrored`, mask-gated; `rasterize_mesh`
carrying an imported model's vertex colours into that palette; mesh vertex
colours through meshing and export; and colour readback at a point through
`Document.colors()` and `clay_eval_points`'s `out_colors_rgb`.

## So the real question is not capability

Base colour is not missing. What is bounded is **resolution**, and the bound has
a shape worth stating:

- **While parametric, colour resolution is ITEM-bound.** Every paint stroke
  appends items, and each is evaluated per sample. A detailed texture is
  thousands of items — the cost is the edit list's, not a texture's.
- **After consolidation it is TEXEL-bound**, at the cell size the artist chose,
  and the measured cost is on the table: at cell 0.02 on a unit sphere,
  1 113 bricks / 811 377 samples / **6.2 MB**.

That is the whole cost-and-storage question the epic asked, and it is already
answered by shipped code. The escape hatch from one regime to the other exists,
round-trips exactly, and is one undo step.

## The decisions

**1. Base colour on SDF layers is DONE, with its ceiling stated.** No new colour
field is needed, because one exists. What this change owes is documentation that
stops calling it absent, and a spec requirement pinning the property the whole
story rests on — that consolidation preserves colour exactly.

**2. The voxel palette does NOT generalise, and should not.** It is 256 indexed
entries over a lattice, which is right for a representation whose material is
quantised to cells and wrong for one whose colour is already continuous RGB per
sample. Two representations, two correct answers. The bridge between them is the
conversion that already exists — `rasterize_mesh` quantises INTO the palette,
`to_field` carries the palette OUT as per-sample colour.

**3. PBR channels are a declared NON-GOAL for painting, with bake and export as
the answer.** Roughness, metallic and normal maps are material authoring, and
material authoring wants a UV parameterisation and a texture set. UVs are
CyberRemesherAndUV's half of the seam; `add-claycore-bridge` is where the
field-sampling callback for a baker belongs. Adding roughness as a fourth
channel beside RGB would quadruple every volume, reach every backend, and still
not be paintable in the way an artist means — because what they mean is a
texture, and a texture needs UVs.

This is the deliberate revisit the comparison doc asked for, and the answer is:
**claycore owns colour as a field property; it does not own material authoring.**

**4. One gap is real and newly worth naming: colour on a MESH layer's brushes.**
`MeshSculptor` has fourteen verbs and every one of them moves vertices; nothing
writes `Mesh::colors`. Blender's Paint and Smear are the missing pair. A mesh
layer can *carry* imported vertex colours and export them, but cannot have them
edited — which is now the odd one out, since the SDF and voxel sides both paint.

## What this change does NOT do

No code. This is the decision the epic asked to make before anything is built,
plus the documentation corrections it forces — `docs/sculpt_comparison.md`
currently reads "no polypaint", which measurement contradicts.

## Impact

`sdf-kernels` gains the requirement that painting does not disturb the field and
that consolidation preserves colour. `meshing` gains the statement that vertex
colours survive a brush stroke unchanged, which is what makes the gap in item 4
a gap rather than a silent corruption. Docs are corrected. No format, no ABI, no
behaviour.
