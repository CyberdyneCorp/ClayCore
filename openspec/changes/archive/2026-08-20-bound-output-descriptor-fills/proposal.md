# Proposal: an output descriptor should not be filled past what the caller declared

## Why

The `struct_size` prefix rule is the whole reason a host compiled against an
older `clay.h` can call a newer `libclaycore`. The spec states one half of it —
"the library SHALL never copy more than the caller declared" — and that half is
about reading a descriptor IN. Nothing states the other half, and nothing
implemented it: every output descriptor was filled with `*out = clay_thing{}`
and then assigned field by field, which writes `sizeof` **as this build defines
it**, not the size the caller allocated.

That is a buffer overrun the moment a descriptor grows a field, silently, only
on hosts built against the older layout — which is precisely the case the rule
exists to serve, and precisely the case no in-repo caller exercises, because
every in-repo caller is recompiled.

It is not hypothetical, and it was not one latent site. Measuring each output
descriptor against its own original layout:

| descriptor | original | current | overrun for an old host |
|---|---|---|---|
| `clay_brick_stats` | 48 | 64 | 16 bytes — *fixed in #162* |
| `clay_brick_config` | 24 | 32 | **8 bytes** |
| `clay_consolidation_cost` | 76 | 80 | **4 bytes** |
| `clay_quad_report` | 36 | 40 | **4 bytes** |
| `clay_repair_report` | 36 | 40 | **4 bytes** |
| `clay_layer_info` | 28 | 28 | none yet |
| `clay_field_report` | 28 | 28 | none yet |
| `clay_stroke_preset` | 64 | 64 | none yet |

**Four had already grown.** The roadmap's deferred entry recorded this as seven
latent sites on the reasoning that "none of those structs has grown since its
callers' header" — that reasoning was wrong, and the table is what corrects it.
`clay_brick_stats` was not the first to grow; it was the first to grow *while
something was watching*. `tools/check_c_abi.py` declares original layouts on
purpose, it covers `clay_brick_stats`, and it segfaulted. It does not read the
other four back, so they grew in silence.

Confirmed by canary rather than by inspection: an over-allocated buffer filled
with a sentinel, `struct_size` set to the original layout, and a count of bytes
disturbed past that layout. Against `main`, `clay_brick_config_defaults` wrote
8 bytes past a 24-byte buffer, `clay_brick_cache_config` 8, and
`clay_voxel_repair_report` 4, while the already-fixed `clay_brick_cache_stats`
wrote none.

## What

Adopt `write_desc` — the bounded fill added in #162 — at every output
descriptor, and state the output half of the prefix rule in the spec so the
next descriptor to grow is covered by a requirement rather than by whoever
remembers.

Six sites already probe the incoming `struct_size` with `read_desc` and then
ignore it when writing back. For those this is a pure bug fix with no
caller-visible change: `clay_document_layer_info`, `clay_layer_field_report`,
`clay_layer_consolidation_cost` (via `begin_out_cost`/`write_cost`),
`clay_mesh_quad_report`, `clay_voxel_repair_report` and
`clay_brick_cache_config`.

**Two are deliberately left alone**, and this proposal does not decide them.
`clay_brick_config_defaults` and `clay_stroke_preset_defaults` never probe:
their documented contract is that they *set* `struct_size` ("fills a descriptor
with the engine's defaults, struct_size included"), so the caller does not
declare a size and the library has nothing to bound against. Making them safe
requires requiring `struct_size` on input, which is a caller-visible contract
change to two published entry points and a separate decision — recorded in the
roadmap, not smuggled in here. `clay_brick_config_defaults` is the live one:
8 bytes, today.

## Impact

Nothing a current caller can observe. A caller declaring the current layout
still receives every field, and the returned `struct_size` still describes the
caller's buffer. What changes is only what an OLD caller receives: its own
struct, intact, instead of its stack corrupted.
