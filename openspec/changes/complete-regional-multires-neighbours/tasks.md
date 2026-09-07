## 1. Reproduce before designing

- [x] 1.1 Independent repro against a dense hierarchy as oracle: positions
      identical at 0.000000000, normals wrong by 0.406 / 0.209 / 0.103 at levels
      1 / 2 / 3, at 20 / 52 / 116 corners
- [x] 1.2 Locate the cause rather than the symptom: `level_normals` sums
      `conn.faces_of(v)`, and a regional level's boundary vertex has an
      incomplete ring at that level

## 2. The neighbourhood

- [ ] 2.1 `subdivide_halo_for_patches`: the child faces of the parent faces a
      level did NOT refine, kept only where they touch a stored vertex
- [ ] 2.2 Emitted by the same loop, the same `ChildLayout` and the same corner
      order as `subdivide_topology_for_patches`, so a halo face is the dense
      level's face and not an approximation of one
- [ ] 2.3 Halo vertices numbered ABOVE the level's own, so the level's stored
      vertex `i` is still vertex `i` and every existing consumer is unchanged
- [ ] 2.4 Halo positions from `subdivide_positions` against the same parent
- [ ] 2.5 A dense level builds none

## 3. Consumers, in the order that protects stored detail

- [ ] 3.1 Normals
- [ ] 3.2 Transported detail frames
- [ ] 3.3 NOT Smooth, Relax or the neighbour-dependent automasks — those follow
      in their own change over the same object, because a Smooth over a wrong
      frame is worse than a Smooth that has not been fixed yet

## 4. Cost and lifetime

- [ ] 4.1 Cached in `LevelCache`, rebuilt identically, released by
      `drop_all_caches` with the rest of the runtime
- [ ] 4.2 Counted in `MultiresMemory::runtime_index`
- [ ] 4.3 MEASURED: what the halo adds to a level's evaluation and to its bytes

## 5. Gates

- [ ] 5.1 Dense hierarchy as oracle: normals agree at every corner of the
      refined region, boundary included
- [ ] 5.2 The existing position bit-identity gate still passes
- [ ] 5.3 Detail authored ACROSS a boundary reconstructs to the same world
      offset as on a dense hierarchy — the claim the frame actually protects
- [ ] 5.4 A uniform hierarchy's normals and frames are byte-identical to before
- [ ] 5.5 PROVEN TO CATCH ITS REGRESSION: with the halo removed, the gate reads
      the 0.406 / 0.209 / 0.103 errors again
- [ ] 5.6 Caches dropped and rebuilt gives identical normals

## 6. Verification

- [ ] 6.1 Full unit suite green
- [ ] 6.2 `python3 tools/release_check.py --skip-slow`
- [ ] 6.3 CI green
