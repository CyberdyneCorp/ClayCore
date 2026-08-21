# Tasks

## 1. Establish the extent

- [x] 1.1 Measure every output descriptor's current `sizeof` against its
      original layout, rather than reasoning about which ones "look grown"
- [x] 1.2 Confirm the overruns by canary — a sentinel-filled over-allocated
      buffer declaring the original layout — not by reading the source
- [x] 1.3 Correct the roadmap's deferred entry, which claimed none of the
      structs had grown

## 2. Bound the fills

- [x] 2.1 `clay_document_layer_info`
- [x] 2.2 `clay_layer_field_report`
- [x] 2.3 `clay_layer_consolidation_cost` — `begin_out_cost` bounds its zero
      fill, `write_cost` bounds the payload, and the paths where `begin` runs
      without `write` keep the zeroed struct they returned before
- [x] 2.4 `clay_mesh_quad_report`
- [x] 2.5 `clay_voxel_repair_report`
- [x] 2.6 `clay_brick_cache_config`
- [x] 2.7 Drop the `out_stats_v` alias left in `clay_brick_cache_stats` by
      #162, so all seven read as one pattern

## 3. Pin it

- [x] 3.1 A regression test that calls each output descriptor the way an old
      host does — declaring the original layout — and counts bytes disturbed
      past it
- [x] 3.2 Verify the test FAILS against the unfixed source (it catches four)
      rather than trusting a green run
- [x] 3.3 A companion test that a caller declaring the CURRENT layout still
      receives the appended fields, so the bound cannot become a truncation
- [x] 3.4 Full suite green

## 4. Write the rule down

- [x] 4.1 State the output half of the prefix rule in the c-abi spec
- [x] 4.2 Record the two `_defaults` entry points as an open decision, with
      what makes them different and what fixing them would cost
