## 1. The cull

- [x] 1.1 A culled compile drops a finite-support warp its region cannot reach
- [x] 1.2 Sound along the CHAIN: the region is widened by what each KEPT warp
      can move a point, before the next is tested
- [x] 1.3 Grab only for now; `pose` and `magnify` share `cregion_weight` and can
      join on the same terms. `pose_line`, the noise and the lattices never can
- [x] 1.4 A compile with no cull region carries every warp

## 2. The cost query

- [x] 2.1 `clay_layer_warp_cost_get`, with the whole-document count and the
      finite-support subset a culled tape can drop
- [x] 2.2 Zeroes for a layer that cannot carry one; typed not-found for a layer
      that is not there
- [x] 2.3 pyclay `Layer.warp_cost()`, and the ALIASES entry the parity gate
      needs because the struct owns the bare name

## 3. The note

- [x] 3.1 `clay_layer_move_surface` says a grab is not free after it lands, with
      the measured numbers and what the library does and does not do about it

## 4. Gates

- [x] 4.1 EXACT field equality inside the region, warps dropped and warps kept
- [x] 4.2 The ordering case, where a kept warp displaces the point into the
      reach of one the region alone does not touch
- [x] 4.3 Measured: a brick-sized region far from twelve grabs goes from 176
      tape params to 32, and 0.0824 ms to 0.0052 over 512 probes
- [x] 4.4 2,332 unit cases; 682 pyclay; layering, c-abi, parity green
- [x] 4.5 ABI 0.79.0 -> 0.80.0 in the three version lines
- [ ] 4.6 CI green

## 5. Left open, with the measurement as the case for them

- [ ] 5.1 COMPOSING successive grabs. Two domain warps do not compose into one
      in general, and the sound special cases are narrow enough that a common
      stroke would miss them
- [ ] 5.2 A cheaper flatten than consolidation. Resolving a warp into the edit
      list is exact only where it is rigid over an item's support, and it is not
