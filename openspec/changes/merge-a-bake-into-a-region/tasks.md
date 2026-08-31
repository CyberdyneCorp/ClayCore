## 1. The closure

- [x] 1.1 `plan_region_merge` computes the influence closure of a region as a
      fixed point, not a single pass
- [x] 1.2 An item with an INFINITE reach pulls the closure out to the layer,
      which is the correct answer rather than a special case
- [x] 1.3 Hidden roots are left alone, as `absorbable_roots` already leaves them
- [x] 1.4 The plan is pure, so a host can show what it would lose first

## 2. The merge

- [x] 2.1 `consolidate_region` bakes over the closure — not the caller's region,
      which would leave the absorbed items' contribution outside it in nothing
- [x] 2.2 It reuses `install_bake`, so the removals and the add are ONE group
      whose inverse restores ids, parameters, colours and deformers
- [x] 2.3 The protection check happens before the bake, not after

## 3. The bindings

- [x] 3.1 `clay_layer_consolidate_region` / `_plan_region_merge` with a
      versioned `clay_region_merge`
- [x] 3.2 A missing or empty region is REFUSED — a region merge without a region
      is a whole-layer consolidate and a host should ask for that by name
- [x] 3.3 `Layer.consolidate_region` / `Layer.plan_region_merge`

## 4. Proof

- [x] 4.1 The closure takes what the region reaches and no more
- [x] 4.2 It grows until self-contained, on a fixture where ONE PASS takes one
      item and the fixed point takes two
- [x] 4.3 A chain of overlaps is taken whole
- [x] 4.4 A region that reaches nothing merges nothing
- [x] 4.5 The surface is unchanged outside the closure...
- [x] 4.6 ...and the merged region still holds its own surface, so 4.5 is not
      satisfied by deleting it
- [x] 4.7 Six gestures on one patch leave ONE baked item, against ten for the
      appending path
- [x] 4.8 A whole-layer closure is consolidation and says so
- [x] 4.9 It undoes to the parametric form in one step, bit for bit
- [x] 4.10 A protected layer is refused before it is sampled
- [x] 4.11 The same claims at the C boundary and from pyclay
- [x] 4.12 4.2 fails when the fixed point is reduced to a single pass
