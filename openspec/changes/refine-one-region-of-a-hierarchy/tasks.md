## 0. Read first

- [x] 0.1 The voxel regional path (`clay_voxel_add_level_region` and its C++
      side) BEFORE designing the host-facing API, so the two are one idea rather
      than two vocabularies. READ: it keeps the lattice uniform and complete and
      changes only what is STORED, which is the shape this should take
- [x] 0.2 MEASURE where a level's memory goes before building sparsity into the
      wrong place. `DetailField` is already sparse and costs 0.0 MB on an
      unsculpted level; the 121.4 MB of a level-4 hierarchy is topology (10.4),
      evaluated (37.6), chunk index (10.8) and runtime. The sparsity belongs in
      the topology and the derived buffers
- [x] 0.3 Confirm the identity to reuse: faces are patch-major at every level
      and `LevelTopology::face_patch[]` already names the base patch. One
      patch's faces at one level are a contiguous run

## 1. Per-patch depth

- [ ] 1.1 `patch_max_level[]` on `MultiresSurface`, authoritative
- [ ] 1.2 Old surfaces decode as uniform depth everywhere — no behaviour change
- [ ] 1.3 Core API takes a PATCH LIST; region and sphere helpers on top, so a
      test can name patches and be deterministic

## 2. Balance and transitions

- [ ] 2.1 2:1 balance with automatic rings, queue processed in STABLE patch-id
      order — an unordered queue is a different surface on the second run
- [ ] 2.2 A fine boundary vertex is DERIVED from the shared coarse edge's exact
      subdivision, so a T-junction cannot open rather than being repaired
- [ ] 2.3 Transition polygons are derived data, never authoritative

## 3. Sparsity

- [x] 3.1 `DetailField` blocks exist only for resident (patch, level) pairs —
      ALREADY TRUE, and measured: an unsculpted level costs 0.0 MB of detail.
      Nothing to build; the gate is that it stays true
- [ ] 3.2 `effective_level(patch) = min(requested, patch_max_level[patch])`
- [ ] 3.3 Cross-level gather over patch-aware work items; per-dab work follows
      touched resident detail
- [ ] 3.4 Cross-level neighbours for smooth/relax/normals — a coarser neighbour
      is a neighbour, not an absence
- [ ] 3.5 Propagation reaches resident descendants only, and instantiates none

## 4. The runtime, reused

- [ ] 4.1 ONE `ChunkTable` with the identity it has; regional refinement means
      some `(base patch, quadrant)` chunks do not exist. No second table
- [ ] 4.2 `SculptLayerStack` inherits sparsity; no per-layer topology copies
- [ ] 4.3 Preflight on the PEAK, refusing before allocating

## 5. Gates

- [ ] 5.1 Refining with zero detail does not move the surface — numerically
- [ ] 5.2 All-patches regional refine == global refine
- [ ] 5.3 Watertight/manifold across every transition case; brushes across them
- [ ] 5.4 Deterministic balancing: same request, same patches, twice
- [ ] 5.5 Save/load a mixed-depth hierarchy; old version loads uniform
- [ ] 5.6 THE BENCHMARK THAT IS THE CLAIM: localized-high against
      global-high at matched effective size — memory and update cost follow the
      refined area, not the finest level times the whole surface
- [ ] 5.7 C ABI mirroring the voxel call, pyclay, numbered example
- [ ] 5.8 NO level removal in v1, and the reason documented rather than omitted
