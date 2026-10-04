## Why

#618 measured the mesh sculptor's averaged normal 16.8 deg off +y on a fin 0.1
thick, where the fin's mirror symmetry says it must be exactly +y (0.5 deg on a
sphere, 2.2 on a saddle). Draw moves the stamp along that normal, and Clay,
Crease, Layer, Flatten and Scrape all read the same frame, so on a thin feature
they deposit sideways onto one face. The cause was not chased (#631).

## What measuring found

The fixture is the #618 fin rebuilt in `test_mesh_sculpt.cpp`: a box 0.1 x 1.0
on a slab, meshed by `mesh_tape` at voxel 0.01, one Draw stamp of radius 0.45
centred on the ridge, `MeshSculptor::workset().average_normal` read back.

| hypothesis | measurement | verdict |
|---|---|---|
| the geodesic walk's seed and path taper favour one face | `geodesic = true` and `false` give the same workset and the same tilt to the printed digit (11.07 deg) | refuted |
| the mesher's triangulation is not mirror-symmetric | lattice through the faces: 11.5 deg; the same fin with the lattice offset half a step, so it falls between them: 1.4 deg | confirmed |
| the vote is per vertex, so density decides | lattice through the faces: +x face 3,184 entries with a +y vote sum of 33, -x face 3,453 entries summing 111 | confirmed |

The mesher's Freudenthal split shares one body diagonal in every cell, so it is
chiral: the mirror image of the fin is not triangulated as the mirror image of
its triangulation. Where the lattice passes through the faces (the box's faces
fall on lattice samples, and a zero sample counts as outside) one corner of the
ridge comes out sharp and the other chamfered, and the chamfered face carries
more vertices, tilted toward +y. On a ridge the two faces' normals cancel, the sum
keeps only a few percent of its terms, and that imbalance becomes the tilt.

So the defect is the averaging rule, and the mesher only exposes it: one equal
vote per vertex makes the average a property of the triangulation. Weighting
each vote by the area its vertex stands for (one third of each incident
triangle) turns the sum into a discretisation of the falloff-weighted integral
of the normal over the surface.

| fixture | equal vote | area-weighted |
|---|---|---|
| fin, lattice through the faces | 11.47 deg | 0.15 deg |
| fin, lattice between the faces | 1.38 deg | 0.09 deg |
| fin on the adaptive surface (between) | 1.38 deg | 0.09 deg |
| unit sphere, pole | 0.90 deg | < 0.01 deg |
| torus R = 1, r = 0.4, inner equator | 4.28 deg | < 0.01 deg |

A sum of the incident triangles' area vectors instead (the exact surface
integral, ignoring the angle-weighted normal) measured 0.03 / 0.00 deg on the
two fin lattices — better, but it would make the vote a second normal estimator
beside the one the kernels displace along. The area-weighted angle normal keeps
one estimator and is already far inside the bound.

## What Changes

- `WorkItemReader::normal_at` returns the item's area with its normal, from the
  same pass: `class_normal` reads it off the cross products it already takes
  (plus the derived faces a multires level adds at a depth boundary), and the
  adaptive surface from its fan (`DynamicSurface::vertex_area`).
- `SculptWorkset::areas` carries it; `resolve_frame` sums
  `normal * (weight * area)`. The centroid keeps the equal vote.
- Unit normals and weights are unchanged bit for bit. The six frame verbs move,
  so the golden tables are re-baselined (30 of 80 rows, sphere/flatten's moved
  count 8 -> 10). The parity case now also writes its table when hashes differ,
  so a deliberate change can re-baseline the toolchains no one runs by hand from
  CI's artifact.
- No ABI or format change.

## Cost

Interleaved A/B, arm64 macOS (M-series), cpu-only Release, medians of 6-8
process runs after a discarded warm-up, each 200 iterations:

| case | before | after |
|---|---|---|
| `BM_MeshStampNoAutomask/224` | 0.058 ms | 0.058-0.063 ms |
| `BM_MeshStampNoAutomask/707` | 0.489 ms | 0.484-0.503 ms |
| `BM_DynamicStampNoAutomask/224` | 0.082-0.089 ms | 0.089-0.090 ms |
| `BM_DynamicStampNoAutomask/707` | 0.089-0.091 ms | 0.095-0.096 ms |
| `BM_MultiresDabLocal` | 0.402 ms | 0.391-0.397 ms |

The adaptive surface pays a fan walk per kept vertex, about 6 us a stamp here,
also on verbs that do not read the frame (these cases are Grab). The first
spelling went through `face_area_x2` and cost 14%; reading the corners off the
fan, as `compute_vertex_normal` does, brought it to 1-8%. The fixed mesh and
multires paths sit inside run-to-run noise.

## What building it found

- `BM_DynamicStampNoTopology` read 0.45x (faster) in one filter set and 1.04x
  when run alone, on the before binary only. It is 20 iterations, and its first
  case in a process is cold; it is not evidence either way.
