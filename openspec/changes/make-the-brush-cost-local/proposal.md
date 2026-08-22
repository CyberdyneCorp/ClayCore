# Proposal: a dab should cost what it moves

## Why

Measured on flat grids, with the brush radius scaled so the dab touches exactly
188 weld classes at every size — the same work, the mesh growing 256x:

| vertices | euclidean | geodesic | geodesic + `seed_class` |
|---:|---:|---:|---:|
| 4,096 | 0.038 ms | 0.035 ms | 0.028 ms |
| 65,536 | 0.109 | 0.104 | 0.030 |
| 1,048,576 | **1.307** | **1.336** | **0.162** |

**35x for identical work**, and linear at the top. The brush VERBS were never the
problem — `grab`, `draw`, `inflate`, `flatten` all loop over `ctx.region` and
nothing else. What broke the property was bookkeeping around them.

Four terms, and `include/clay/mesh/adjacency.h` already names the disease:

> hundreds of stamps and each one walks a small neighbourhood of a mesh that may
> have a million vertices; **allocating a per-class array per stamp is the
> entire cost of the stroke**, so the caller keeps one of these.

That comment describes `WalkScratch`, which keeps a persistent array and a dirty
list. The diagnosis was made, written down, and applied to one structure of
three.

- **`region_.slot`** is cleared across every class, per stamp, to fill ~188
  entries. 8.4 MB at 2M classes.
- **`normal_mark_`** does the same in `write()`. 2.1 MB more.
- **`geodesic_region` scans every class for a SEED** when the caller gives none,
  and `MeshBrushSettings::seed_class` defaults to none. So the default path is
  linear even for the fourteen verbs that walk.
- **`euclidean_region` is a linear scan by construction** — the path Flatten and
  Scrape take, which are the two verbs that most want a large radius.

## What changes

The first two are retired through the list of what was set, never cleared
wholesale — the discipline the file already describes.

The last two need a spatial index, and **the ray tree is one**. It is built over
the same triangles, and since `add-bvh-refit` it is cheap to keep current under
a stroke. Two exact queries on it — every triangle reaching a ball, and the
nearest triangle corner — answer both.

## The trade this does NOT make

**It never builds a tree on the brush's behalf.** Measured: a build is 689 ms on
a million-vertex grid and saves 1.24 ms per stamp, so building one here would
need ~550 stamps to break even and every shorter session would be worse off. A
brush is not the right caller to decide a host should own a ray tree.

Any host that places a brush by picking already has one — `raycast` builds it on
the first pick. So the common case gets the index for free, and a host that never
picks keeps exactly the behaviour it has today, through the scans as a fallback.
Both paths are checked to produce the SAME region, bit for bit.
