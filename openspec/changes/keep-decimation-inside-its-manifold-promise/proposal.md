## Why

**`decimate` could return a mesh with edges carrying four incident triangles,
from an input that had none.** `clay_document_mesh` documents itself as *the
watertight, 2-manifold export path* and decimation is reachable through
`clay_mesh_params.decimate`, so the promise was breachable by whatever input
happened to trigger it — and nothing told a caller it had happened unless they
ran `clay_mesh_validate` themselves.

It is not rare. Measured on an unmodified tree over a sphere-minus-box at four
voxel sizes and five ratios:

```text
20 configurations
 4 produced a non-manifold result   (vs 0.020 @ 0.10 and 0.60,
                                     vs 0.035 @ 0.40, vs 0.050 @ 0.60)
```

The two-torus case is the one pinned as a test: meshed at 0.035 and decimated to
a quarter, it produced **two edges of incidence four** — each with two forward
and two backward triangles — and moved the Euler characteristic from **-4 to
-2**. The collapse closed a handle; the non-manifold edges are the scar.

The cause is that meshoptimizer decides its own collapses and does not apply the
link condition this repository's own `collapse_edge` refuses on
(`mesh/topology_ops.h`), which exists precisely because such a collapse "pinches
the surface into a non-manifold state that looks fine in a render and is
unusable afterwards".

## What this is NOT

**Not a repair pass. One was built first, and measuring it is what rejected
it.**

The plan was to split each pinched edge, giving each pair of triangles its own
copies of the shared vertices — pairing by orientation and radial order around
the edge axis, which is the standard remedy. It was implemented, and then the
radial layout at the two failing edges was printed:

```text
edge(6015,6303) n=4:  [F -0.0deg] [B 178.7deg] [F 178.7deg] [B -177.3deg]
edge(3988,4335) n=4:  [F -0.0deg] [F 175.8deg] [B 175.8deg] [B -176.9deg]
```

**The pinches are FLAT.** Two triangles sit at the same angle to within a
rounding error, so which pair belongs to which sheet is not a geometric fact the
mesh carries — and the two answers are not equivalent. One pairing separated
nothing (the sheets stayed a single fan at both endpoints); the other separated
them into four vertex copies and left the Euler characteristic at 0 rather than
-1. A rule that picks between those is deciding the surface's genus by
floating-point tie-break.

The first pairing rule tried was also simply wrong, and derived correctly only
after the measurement disagreed with it: with the frame right-handed about the
edge `A->B`, a triangle traversing `A->B` carries its outward normal at
`angle + 90 degrees` and one traversing `B->A` at `angle - 90 degrees`, so the
solid wedge runs counterclockwise from a backward triangle to a forward one. The
repair was deleted regardless, because the flatness makes the input undecidable
no matter which rule reads it.

## What changes

`decimate` checks its own result and **never returns a non-manifold mesh where
the input was manifold**. When the requested simplification lands on a pinch it
asks again — strategy first, size second:

| order | what changes | why |
|---|---|---|
| 1 | `meshopt_SimplifyRegularize`, same target | different collapses, same triangle count |
| 2 | `meshopt_SimplifyRegularizeLight`, same target | |
| 3-6 | 1.05x, 1.25x, 1.6x the target | more geometry, as a last resort |
| — | the welded input | correct and larger, rather than broken |

**Every one of the four measured failures recovered on the first retry**, at the
requested size: 22,178 -> 22,180, 133,080 -> 133,078, 30,444 -> 30,444,
22,208 -> 22,208 triangles.

Strategy-before-size is measured, not assumed. The two-torus case needed
**60% more triangles** before an unregularized pass came back clean, against a
different collapse order costing it nothing.

**Regularize is not a manifold-preserving mode and is not treated as one.**
Across 21 shape-and-ratio combinations it fixed cases and also broke cases that
were clean without it. It is used as another thing to try and then check, never
as a thing to trust — which is why the check, not the flag, is the fix.

## What it costs

```text
clean path (76,112 triangles in, 19,028 out)    19.29 -> 19.97 ms    +3.5%
when a retry fires                              one more simplification
```

The clean path pays one edge-map pass over the output. A retry costs roughly a
second decimation, on the ~20% of the configurations measured that need one.

## What changes for a caller

A decimated mesh may now carry a slightly different triangle count than before
for the same options — in the measured cases, within two triangles — because the
collapse order changed on the inputs that were previously pinched. On inputs
that were already clean, nothing changes at all.

A caller passing a mesh that is **already** non-manifold gets the simplified
result as before. This does not promise to repair what it did not break.
