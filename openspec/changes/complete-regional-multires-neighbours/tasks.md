## 1. Reproduce before designing

- [x] 1.1 Independent repro against a dense hierarchy as oracle: positions
      identical at 0.000000000, normals wrong by 0.406 / 0.209 / 0.103 at levels
      1 / 2 / 3, at 20 / 52 / 116 corners
- [x] 1.2 Locate the cause rather than the symptom: `level_normals` sums
      `conn.faces_of(v)`, and a regional level's boundary vertex has an
      incomplete ring at that level

## 2. The neighbourhood

- [x] 2.1 `build_level_halo`: the child faces of the parent faces a
      level did NOT refine, kept only where they touch a stored vertex
- [x] 2.2 Emitted by the same loop, the same `ChildLayout` and the same corner
      order as `subdivide_topology_for_patches`, so a halo face is the dense
      level's face and not an approximation of one
- [x] 2.3 Halo vertices numbered ABOVE the level's own, so the level's stored
      vertex `i` is still vertex `i` and every existing consumer is unchanged
- [x] 2.4 Halo positions from `subdivide_positions` against the same parent
- [x] 2.5 A dense level builds none

## 3. Consumers, in the order that protects stored detail

- [x] 3.1 Normals
- [x] 3.2 Transported detail frames
- [x] 3.3 NOT Smooth, Relax or the neighbour-dependent automasks — those follow
      in their own change over the same object, because a Smooth over a wrong
      frame is worse than a Smooth that has not been fixed yet

## 4. Cost and lifetime

- [x] 4.1 Cached in `LevelCache`, rebuilt identically, released by
      `drop_all_caches` with the rest of the runtime
- [x] 4.2 Counted in `MultiresMemory::runtime_index`
- [x] 4.3 MEASURED, A/B, minimum of nine runs a side, 16x16 cage with a 4x4
      region: full evaluation +13% at level 4 (1.335 -> 1.513 ms), bytes +7%
      (975,600 -> 1,041,776), and a one-detail re-evaluation UNCHANGED at
      0.0002-0.0003 ms — which is the number that matters, because that is the
      per-dab path
- [x] 4.4 Two costs found and removed rather than accepted: `ChildIndex`'s
      binary search (+71% before a direct layout-id map) and refreshing the halo
      positions on an evaluation that had not moved the parent (18x on a
      re-evaluation)

## 5. Gates

- [x] 5.1 Dense hierarchy as oracle: normals agree at every corner of the
      refined region, boundary included
- [x] 5.2 The existing position bit-identity gate still passes
- [x] 5.3 Detail authored ACROSS a boundary reconstructs to the same world
      offset as on a dense hierarchy — the claim the frame actually protects
- [x] 5.4 A uniform hierarchy's normals and frames are byte-identical to before
- [x] 5.5 PROVEN TO CATCH ITS REGRESSION: with the halo removed, the gate reads
      the 0.406 / 0.209 / 0.103 errors again
- [x] 5.6 Caches dropped and rebuilt gives identical normals

## 6. Verification

- [x] 6.1 Full unit suite green (2,543 cases, 9 ctest entries)
- [ ] 6.2 `python3 tools/release_check.py --skip-slow`
- [ ] 6.3 CI green
