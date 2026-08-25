# Proposal: mesh the cells no surface brick owns

## Why

`clay_brick_cache_mesh` leaves pinholes — one to a few pixels — in the surface
of a worked SDF document. `clay_document_mesh` of the same document at the same
moment has none (#292). They are background pixels ringed by lit surface, not
shading: the geometry is missing.

Reproduced in-engine on the reported shape — a ball with forty relief dabs
around a ring, every third one incised:

| voxel size | surface bricks | brick mesh boundary edges | document mesh |
|---:|---:|---:|---:|
| 0.06 | 138 | 30 | 0 |
| 0.05 | 168 | 28 | 0 |
| 0.03 | 510 | 158 | 0 |
| 0.02 | 1183 | 386 | 0 |

The whole-surface path and the all-keys subset path give the same count, which
is what the report's third row said: it is not the host's per-key store.

## The mechanism

A cell is owned by the brick its LOW corner falls in, and it takes its other
seven corners from up to seven neighbours. The mesher marches the cells owned
by surface bricks, and that is not every cell that crosses.

A brick with no lattice — every sample at least a band from the surface, so
classified uniformly Inside or uniformly Outside and stored as a state byte —
still owns cells whose far corners belong to its neighbours. Such a cell
crosses as soon as one of those neighbours holds a sample of the opposite
sign. Nobody marches it, so the triangles are never emitted and the surface is
open there.

That needs the field to move more than a band across one voxel step. A
1-Lipschitz distance field never does, which is why the defect does not show on
an ordinary sphere and why the invariant held for so long. A worked document
does it routinely, and says so: relief and incise displace the surface by an
amplitude over a region narrower than the amplitude, and the tape's
`safe_step_scale` for the reproduction above is ~1e-8. Measured directly on a
hand-fed lattice, the whole-surface mesh is watertight up to a field gradient
of about 5 and opens above it — `sqrt(3)` voxels of diagonal against the
3-voxel band, as the geometry predicts.

The subset path mirrored the same rule on purpose: `shell_cells` skipped ring
cells whose owner was not a surface brick, with the comment "the whole-surface
mesh marches no cell of theirs". It was faithful to a whole that was wrong.

## What Changes

Both paths collect the straddlers, and the straddler pass no longer consults
the owner's state.

A cell owned by a brick that stores no lattice at this level is marched by
nobody else, so it is kept WHOLE rather than filtered per corner: its crossing
vertices sit inside the owner's box, strictly outside every requested brick's,
so the existing per-corner attribution would have dropped every one of them.
It is attributed to the lexicographically lowest requested key whose closed box
holds one of the cell's eight corner lattice points — the same rule, applied to
the cell instead of the triangle.

For a field the band does bracket there are no such cells and the mesh is
unchanged, byte for byte.

## Cost

The straddler pass now runs on the whole-surface path too, so it was made to
pay for itself.

`shell_cells` walks OWNER BRICKS rather than each key's ring. Ringing every key
visits a shared cell from up to eight keys, so it needed a hash set to dedup
and a sort to order; an owner owns each of its cells exactly once, so walking
owners emits each cell once, in order, with neither. The cell march then fans
out across the pool, recorded and replayed in sorted order exactly as the brick
march already is.

On a 6,003-brick, 1.35M-triangle surface (M-series, 8 cores):

| path | before | after |
|---|---:|---:|
| whole cache | 171 ms | 190 ms |
| 128-key subset (the frame path) | 7.9 ms | **5.9 ms** |

The straddler pass itself: 51 ms naive, 22 ms as landed, of which the scan is
14 ms and the marching 8 ms. The frame path is faster than before because it
already paid the per-cell ring scan and no longer does.

`check_bench.py`'s own scene is 276 bricks, where the difference is inside the
run-to-run spread: `BM_MeshBricksWhole` 7.0 -> 7.6 ms, `BM_MeshBricksSubset`
0.7 -> 0.6 ms. No gate moves, the `MeshBricksSubset < MeshBricksWhole` ratio
holds, and the one gate that fails — `BM_MetalTapeResident`, in a build with no
Metal backend — fails identically on `main`.

## Impact

Every consumer of `clay_brick_cache_mesh` and `clay_brick_cache_mesh_lod`,
which is the frame path — the mesh a sculptor looks at for a whole session.
`clay_document_mesh` is unaffected; it marched every cell already, which is why
it was the control.

## Non-goals

**Making the classification cover the gap.** `BrickCache::submit` sees one
brick's samples and cannot know a neighbour's, so no classification rule
computed there can decide whether a cell it owns crosses. The mesher is where
both sides are visible.

**Bounding the field's Lipschitz.** The document already declares it. This
change makes the mesher correct for whatever the document declares rather than
restricting what a document may be.
