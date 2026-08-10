# Tasks: add-tape-abi-export

- [ ] 1.1 DECIDE and record in `design.md`: ownership across the boundary — copy into caller buffers (two-call size-then-fill, this ABI's existing habit), an opaque snapshot handle the caller releases (the tape cache already holds a `shared_ptr`, so this is cheap), or borrowed pointers valid until the next mutation. The last is fastest and hands a host a use-after-free on one missed invalidation; if it is chosen, say why in a sentence a reviewer can disagree with
- [ ] 1.2 DECIDE and record: whether a culled tape is exportable too. It is the same three buffers plus a `CullRegion`, and a host streaming a region wants it — decide it, do not leave it out by omission
- [ ] 1.3 Export the three buffers plus field info and bounds, in the layout the published headers define
- [ ] 1.4 Export the document revision alongside, so staleness is a comparison of one integer rather than of buffers
- [ ] 1.5 Version the tape encoding with the published kernel package; a mismatch is detectable and refused, never reinterpreted
- [ ] 1.6 Extend the host parity fixture to cover a tape obtained through the export path, at the same tolerance as the bundled tapes
- [ ] 1.7 Test: export, evaluate through `ctape_eval` from `dist/claycore-kernels/`, compare against library evaluation at the fixture's probe points
- [ ] 1.8 Test the lifetime rule directly: hold an export, edit the document, and assert whatever the chosen rule promises — under ASan, since this is the failure mode that lands in someone else's app
- [ ] 1.9 Test: export while another thread evaluates the same document, consistent with "a document stays readable from several threads at once"
- [ ] 1.10 Measure what this replaces: a preview frame drawn by round-tripping through library evaluation vs one drawn from an uploaded tape, in bytes crossed and in milliseconds
- [ ] 1.11 Document the flow in `docs/06-host-gpu-previews.md` — upload on edit, draw at frame rate, check the revision — and note the blob-size question (sampled volumes ride in the blob and a re-upload per stroke is the cost the sampled-field design already identified)
- [ ] 1.12 Mark the row done in `openspec/ROADMAP.md`, where it has been carried as `add-tape-abi-export`
