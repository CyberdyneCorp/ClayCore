## Why

**`clay_document_writable_at_minor` shipped with a comment recording its own
gap**, and closing that gap found the call itself wrong.

The gap, in the header's words:

> A RECORDED GAP, not an oversight: this ABI has no way to write at an older
> minor at all. `clay_document_save` takes a path and `clay_document_save_memory`
> takes a blob; neither takes a version, and the layout is a parameter on the C++
> serializer that does not cross this boundary.

**The defect underneath it.** That call promises "every layer of `doc` can be
written at scene format minor `minor` **with nothing an artist authored
dropped**", and it asks only `scene::layer_blocking_minor`, which knows about a
layer's COMPOSITION and nothing else. Container minor 19 added an `'MRES'` chunk
carrying a mesh layer's multiresolution hierarchy, and this call never learned
about it. Demonstrated through the ABI:

```text
no hierarchy:      writable at 18 -> 0 (CLAY_OK)
hierarchy attached: present=1
WITH a hierarchy:  writable at 18 -> 0 (CLAY_OK)   <-- and 18 has no MRES chunk
```

**And the reason it went unnoticed is written down too**, in `io/clayspace.h`:
there is deliberately no `multires_blocking_minor`, because "`save_clayspace`
takes no such parameter … a query answering which layer blocks a write nobody
can request would be an entry point with no caller." That reasoning was correct.
Adding the parameter is what makes the query necessary — and is what made the
existing hole visible.

## What Changes

**`save_clayspace` and `save_clayspace_file` take a minor.** A chunk a minor did
not have is not written at that minor: below 19 the hierarchies are absent.

**`io::multires_blocking_minor` and `io::document_blocking_minor`** — one answer
for a host with one question, so the two per-field queries cannot get out of
step.

**The line is `multires_carries_detail`, which the tree already drew.** A
hierarchy holding only its cage rebuilds from that cage by a deterministic
subdivision, so losing it is the ordinary "smaller or plainer" degrade every
earlier minor makes. A hierarchy an artist has refined cannot be rebuilt, and
the write is refused.

**`clay_document_save_at_minor` and `clay_document_save_memory_at_minor`**, and
`minor=` on pyclay's `save` and `to_bytes`.

**`clay_document_writable_at_minor` answers for the whole document**, from the
same predicate the saves refuse on — so asking before and being refused after
cannot disagree.

**Nothing is written on a refusal.** An existing file is left exactly as it was:
a save that cannot represent the document must not first destroy the last one
that could.

## What measuring refuted

**"Add a distinct `DOCUMENT_NOT_REPRESENTABLE_AT_MINOR` result."** The roadmap
asks for one and it would be a second answer to one question:
`CLAY_ERROR_UNSUPPORTED` already means exactly this, already carries the blocking
layer, and is already what `clay_document_writable_at_minor` returns for the same
condition. A host would have to handle both codes to be correct. The requirement
underneath it — never `CLAY_OK` with empty or downgraded content — is met, and
the refusal names *which* of the two reasons blocked it in two different
sentences, because a host reading "a layer carries a composition" about a layer
that carries a hierarchy would go and look at the wrong thing.

## Capabilities

### Modified Capabilities
- `c-abi`: a host can write a document at an older format layout, and is
  refused rather than silently downgraded when that layout cannot say what the
  document says.
- `file-io`: the container can be written at an older minor, and the query that
  decides whether it can answers for every chunk rather than for one field.

## Impact

- `include/clay/io/clayspace.h`, `src/io/clayspace.cpp`
- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`
- `bindings/python/pyclay_module.cpp`
- `tests/unit/test_layer_composition.cpp`, `bindings/python/tests/test_pyclay.py`
- ABI 0.92.0 -> 0.93.0. **No format change**: this writes layouts that already
  exist and adds none.
