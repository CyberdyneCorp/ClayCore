# Tasks: add-voxel-verbs

- [x] 1.1 Shared pocket-fill rule over a snapshot, reused by repair (NOT closing — see the proposal)
- [x] 1.2 `sculpt_fill_cavities`: fills dents, leaves through-holes
- [x] 1.3 `sculpt_scrape`: flatten + smooth from one snapshot
- [x] 1.4 `sculpt_smudge`: surface only, distinct from grab
- [x] 1.5 `sculpt_carve_alpha`: caller-supplied scalar grid, projected along a direction
- [x] 1.6 Both bindings
- [x] 1.7 Tests: each verb's behaviour, through-hole survival, snapshot consistency, smudge != grab, uniform alpha == plain carve, malformed alpha refused, mask gating for all four, C-vs-Python
- [x] 1.8 Docs, example, ABI 0.17.0, full verification
