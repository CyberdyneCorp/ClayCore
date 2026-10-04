## Implementation
- [x] `scene::CullRegion` carries `band`, with a constructor so every `CullRegion{box}` spelling keeps meaning "no band".
- [x] `scene::CullSquash`, `item_cull_squash` and `node_cull_squash` (bounds.h). `item_bound_dilation` is factored out of `placed_local_bound`, so the widening multiplies exactly the dilation the bound carries.
- [x] Compiler: the item test, the group test and the planned-entry test widen a squashed bound by `slope * (band + pad) + reach`. The squash is computed only for a node the plain bound would drop.
- [x] `CullIndex::Entry::squash`, set at build and on append; `Chain::widest`; `CullIndex::plan(region, band)` widens a squashed chain's scan region; `CullPlan::serves_band` refuses a plan made for a narrower band, and `usable_plan` drops it.
- [x] The band is set where it is known: the refill's request tapes, its batch plan, the resume and uniform-brick tasks, the frontier jobs, `try_band`, the volume bake, gradient-normal meshing, and `clay_eval_grid` (the band the region adds around the lattice).
- [x] Regression test `tests/unit/test_squashed_cull.cpp`.
- [x] `docs/05` states the widening.

## Measured
`RV_RAWCHECK=1 undo_bound_oracle_probe`, Release, Apple M-series, origin/main 7023dd9a against this change:

| | main | this change |
|---|---|---|
| seed 5743 rich | 5,625 samples off, worst 0.06767 | 0 |
| seed 6290 rich | 151 samples off, worst 0.01986 | 0 |

The sweep table is in the PR body.

## What building it found
- The coarse plan is the part that is easy to get wrong. It knows only a batch region, and a squashed entry's widening depends on the band. A plan made with no band prunes a squashed item a per-brick test would keep. So the plan takes the band, and a compile refuses a plan made for a narrower band where it matters (only a document holding a squashed placement). There are about thirty `plan(region)` callers in tests and benchmarks, and none of them is wrong under the default. They only lose the plan on a squashed document.
- Widening each probe would break the packed scan's one-box-per-entry layout. The scan widens the REGION instead, by the chain's widest entry. That is conservative for the coarse cull only, because the per-brick test re-checks each survivor with its own widening.
- A batch plan over scattered bricks covers the whole document and prunes nothing. The first draft of the planned-path test used one, and it passed with the plan's widening removed. Each brick is now planned alone, and that test fails under the mutation.
- `clay_eval_grid` with a host region does not say what band the region was dilated by, but the lattice it evaluates does: the margin between the lattice and the region is that band.

## Left out
Mechanism A of #649 (seed 978: the #335 chain-pad envelope applies its N = 75 value below 75 contributors) is a pad-versus-performance trade that needs the device gate. This change does not touch it.
