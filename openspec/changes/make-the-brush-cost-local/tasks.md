# Tasks: make-the-brush-cost-local

## 1. Retire the two per-stamp memsets

- [x] 1.1 `region_.slot` — cleared for the entries the LAST stamp set, at the
      top of `gather` before `r.classes` is overwritten. It stays a full
      per-class array because the verbs index it by arbitrary ring neighbours;
      what changes is that it is sized once and never cleared wholesale again
- [x] 1.2 `normal_mark_` — the same, retired through `pending_normals_`, which
      names exactly what the last `write` marked
- [x] 1.3 No behaviour change: neither touches what is COMPUTED, only what is
      cleared. The full suite passing unchanged is the check

## 2. The ray tree as the brush's spatial index

- [x] 2.1 `Bvh::triangles_in_ball` — every triangle reaching a ball, by source
      index. Exact in the direction that matters: it over-admits triangles that
      reach the ball without a vertex in it, which the caller filters, and
      misses none that have one
- [x] 2.2 `Bvh::nearest_vertex` — the nearest triangle corner, nearer-child
      first so the far subtree is pruned by a real bound
- [x] 2.3 `MeshSculptor::surface_index()` — the tree, refitted, or NULL. Never
      builds: measured 689 ms to build against 1.24 ms saved per stamp, so it
      would need ~550 stamps to break even
- [x] 2.4 The geodesic seed comes from the index instead of a scan, and the walk
      is now ALWAYS seeded
- [x] 2.5 The euclidean region is a ball query instead of a scan
- [x] 2.6 `nearest_class` is the same query, with the scan kept as the fallback
      for a sculptor with no tree

## 3. Prove it

- [x] 3.1 `triangles_in_ball` against brute force at four radii and 25 centres:
      every triangle with a vertex in the ball is reported, and no duplicates
- [x] 3.2 `nearest_vertex` against brute force over 200 probes
- [x] 3.3 Empty tree answers rather than crashing
- [x] 3.4 **The indexed path and the fallback produce the same region.** This
      caught a real defect: the ball query returned classes in TREE order, so
      the verbs' weighted sums accumulated differently and one vertex landed
      6e-8 away. The region is sorted now, which also makes it independent of
      the tree's shape — a rebuild changes that shape
- [x] 3.5 Re-measured on an idle machine, interleaved before/after against two
      prebuilt binaries so neither run could drift from the other. The first
      pass was taken while another build held eleven cores and every column of
      it was meaningless — including one that appeared to show a 55% REGRESSION
      that was real and would have been dismissed as noise

## 4. Say it

- [x] 4.1 Spec delta on `brush-engine`
- [x] 4.2 The `nearest_class` docstring said it is a linear scan "wrong per
      stamp on a large mesh, which is what `seed_class` is for". Both halves are
      now stale: it is not a scan when a tree exists, and the walk seeds itself
- [x] 4.3 C ABI and pyclay unchanged — no new entry points, so parity is a
      no-op. Confirm rather than assume

## 5. The regression the measurement caught

- [x] 5.1 The first version seeded the walk from `nearest_class` ALWAYS, falling
      back to a scan when there was no tree. Measured 1.30 -> 1.98 ms at a
      million classes for a host that never picks — a 52% regression, confirmed
      over three interleaved runs at load below 3.
      The cause was not the query: it was that the fallback was a SECOND COPY of
      `geodesic_region`'s seed scan, differing by one branch per iteration
      (`best == kNoClass ||`). Two copies of a hot scan is a defect whichever is
      faster, so the seed is now taken from the index only when there IS one and
      otherwise left unset, which leaves that scan where it always was.
- [x] 5.2 Found by bisecting the diff, not by reading it. Four hypotheses were
      wrong first — the two memsets, the added members changing the class
      layout, and the region selection as a whole — and each was eliminated by
      building and measuring rather than by argument.
- [x] 5.3 The benchmark's own "euclid" column was measuring the GEODESIC path.
      `draw` defaults to the walk (`default_geodesic`), so a call that did not
      pass `geodesic=False` never took the euclidean branch. Two columns tracked
      each other exactly for the whole session and that was the tell.
