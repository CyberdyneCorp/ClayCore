## 1. Count what the claim is about

- [x] 1.1 `mesh::BvhWalkStats`: nodes visited, triangles tested, subtrees
      summarized — through a borrowed pointer, null by default
- [x] 1.2 Counted in `winding_number` only, where summarizing lives. `closest`
      prunes by box distance, which is ordinary BVH pruning and not this claim

## 2. Zero cost when nobody is measuring

- [x] 2.1 MEASURED at each step rather than assumed: both walks instrumented
      cost 3.8% of a `signed_distance` sweep with the pointer null; winding only
      cost 2.3%; `if constexpr` costs 0.4%, which is noise
- [x] 2.2 A template rather than a null check, so the uncounted instantiation
      has no branches at all
- [x] 2.3 `Bvh`'s public signatures unchanged

## 3. The gate

- [x] 3.1 Assert the WORK ratio: 0.863x for 16x the triangles, against 5.333
- [x] 3.2 Assert summarizing actually fired, so a walk that never took the
      dipole branch cannot pass by descending both sides equally badly
- [x] 3.3 PROVEN TO CATCH ITS REGRESSION, in the same file so the two cannot
      drift: with `beta = 0` the ratio is 16.016x, the triangle ratio exactly
- [x] 3.4 Full suite green (2,337 cases)
- [ ] 3.5 CI green, macos-latest included — which is the point of the exercise
