# Tasks — refine a region

## 1. Storage

- [x] 1.1 `Level` gains a refined-chunk set and a "whole lattice" flag; level 0
      is always whole
- [x] 1.2 `cell_at` falls back to the parent for an unrefined chunk, up to
      level 0
- [x] 1.3 `write_cell` refines an unrefined chunk, seeded from the parent, and
      materialises its ancestors
- [x] 1.4 `add_level(region)` seeds only the chunks the region touches;
      `add_level()` keeps today's behaviour exactly
- [x] 1.5 `propagate_up` stops at an unrefined chunk instead of materialising it

## 2. Reporting

- [x] 2.1 `level_occupied_count` counts what is stored; a refined-chunk count so
      a caller can report what a level costs

## 3. Persistence

- [x] 3.1 The voxel tail records the refined set per level, tagged
- [x] 3.2 A wholly refined grid serialises byte-identically to before

## 4. Bindings

- [x] 4.1 `clay_voxel_add_level_region`
- [x] 4.2 `pyclay` `add_level(region=...)`
- [x] 4.3 Binding parity gate green

## 5. Evidence

- [x] 5.1 An unrefined chunk reads its parent, not zero
- [x] 5.2 Adding a level over a region does not move the surface at any level
- [x] 5.3 A region costs its region — stored cells proportional to the region
      rather than to the occupied volume, measured against the whole-lattice case
- [x] 5.4 Whole-lattice refinement is unchanged, including byte-identical bytes
- [x] 5.5 A brush straddling the boundary writes every cell and refines what it
      reached
- [x] 5.6 Seeding an unrefined chunk preserves every cell the write did not touch
- [x] 5.7 Round trip with a partially refined level
- [x] 5.8 Meshing a partially refined level has no crack at the boundary
- [x] 5.9 An example that refines one region and reports what each level costs;
      render inspected

## 6. Bounds, which the feature forced

- [x] 6.1 One cached walk for both ends, invalidated by `write_cell` and by
      every level above the one written
- [x] 6.2 Inherited chunks read their ancestor's data directly rather than
      through a recursive `cell_at` per cell
- [x] 6.3 Cache invalidation covered: growing, shrinking, emptying, and edits
      at a level other than the one asked
- [x] 6.4 A partially refined level reports the same bounds as a whole one

## 7. Docs

- [x] 7.1 `docs/09` — the level-cost table gains the regional case
- [x] 7.2 The "Not done" note in `add-multi-resolution` is answered where it is
      quoted
