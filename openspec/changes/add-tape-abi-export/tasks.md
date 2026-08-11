# Tasks: add-tape-abi-export

- [x] 1.1 DECIDE and record in `design.md`: ownership across the boundary — copy into caller buffers (two-call size-then-fill, this ABI's existing habit), an opaque snapshot handle the caller releases (the tape cache already holds a `shared_ptr`, so this is cheap), or borrowed pointers valid until the next mutation. The last is fastest and hands a host a use-after-free on one missed invalidation; if it is chosen, say why in a sentence a reviewer can disagree with
- [x] 1.2 DECIDE and record: whether a culled tape is exportable too. It is the same three buffers plus a `CullRegion`, and a host streaming a region wants it — decide it, do not leave it out by omission
- [x] 1.3 Export the three buffers plus field info and bounds, in the layout the published headers define
- [x] 1.4 Export the document revision alongside, so staleness is a comparison of one integer rather than of buffers
- [x] 1.5 Version the tape encoding with the published kernel package; a mismatch is detectable and refused, never reinterpreted
- [x] 1.6 Extend the host parity fixture to cover a tape obtained through the export path, at the same tolerance as the bundled tapes
- [x] 1.7 Test: export, evaluate through `ctape_eval` from `dist/claycore-kernels/`, compare against library evaluation at the fixture's probe points
- [x] 1.8 Test the lifetime rule directly: hold an export, edit the document, and assert whatever the chosen rule promises — under ASan, since this is the failure mode that lands in someone else's app
- [x] 1.9 Test: export while another thread evaluates the same document, consistent with "a document stays readable from several threads at once"
- [x] 1.10 Measured (x86-64, Release, CPU backend), a 512x512 preview against the tape it replaces:

  | items | tape bytes | export | 512x512 frame bytes | frame ms | bytes ratio |
  |---|---|---|---|---|---|
  | 50 | 7 976 | 0.024 ms | 8 388 608 | 131.7 | 1052x |
  | 500 | 79 976 | 0.088 ms | 8 388 608 | 1 510.5 | 105x |
  | 2 400 | 383 976 | 0.232 ms | 8 388 608 | 8 221.1 | 22x |

  The bytes ratio is the conservative half and understates it by the frame rate: the tape crosses once per EDIT and the frame crosses 60 times a second, so at 2 400 items the steady-state ratio is ~1 300x rather than 22x. A **warm** export measures 0.000 ms — a refcount increment, which is the ownership design doing exactly what it was chosen to do. The frame milliseconds are the CPU raycast a host GPU would not be running at all; they are why the round-trip is not merely wasteful but unusable past a few hundred items.
- [x] 1.11 Document the flow in `docs/06-host-gpu-previews.md` — upload on edit, draw at frame rate, check the revision — and note the blob-size question (sampled volumes ride in the blob and a re-upload per stroke is the cost the sampled-field design already identified)
- [x] 1.12 Mark the row done in `openspec/ROADMAP.md`, where it has been carried as `add-tape-abi-export`
