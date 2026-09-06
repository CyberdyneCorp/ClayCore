## Why

A `Node`'s `volume` and its `gate` are both
`std::shared_ptr<const field::FieldVolume>`, and `scene/types.h` says why:

> "Shared, so two items sampling the same source share one set of samples."
> "Shared for the same reason `volume` is: several items gated by one painted
> mask should not each carry a copy of it."

**Both are true of memory and false of the file.** The writer serializes a
node's payload inside its node record and the reader rebuilds one per node with
`std::make_shared`, so N items sharing one payload write N copies and load as N
unrelated volumes — which means the next save writes N again.

Measured through the C ABI, one captured volume placed N times:

| placements | saved bytes |
|---:|---:|
| 1 | 187,531 |
| 8 | **1,499,457** |

Exactly eight copies. This was found while auditing the field-stamps guide,
whose requirement — "a thousand uses of one 4 MB asset must not consume ~4 GB" —
is a live defect rather than a hypothetical. But the defect is not about stamps:
it is about every shared payload, and a gate is the other one.

**The library already solved this, one level up.** `serialize_document`
deduplicates a shared EDIT LIST: "the first layer holding it writes it, every
later holder names that layer", resolved by a second pass on read (minor 15).
This applies the same rule to node payloads.

## What Changes

- Format **minor 17**: a node's volume and gate are written as a DOCUMENT-WIDE
  payload id, with the bytes following only where the id is new.
- **An id, not a NodeId.** Node ids are per-layer — every layer numbers from 1 —
  so a back-reference by node id would name a different node in another layer.
  Ids ascend from 1 in the order the deterministic node walk first meets each
  payload, and a reader refuses one that does not, since no writer emits it.
- **The reader shares too.** A writer that deduplicates met by a reader calling
  `make_shared` per node has saved disk and rebuilt the duplication in memory,
  and the next save writes N payloads again. Both halves or neither.
- Writing at minor 16 restores the per-node shape exactly, so a build that
  predates 17 opens the document and gets what it always got. What that
  downgrade loses is the deduplication and nothing an artist authored.

**Measured after: 8 placements go from 1,499,457 to 189,117 bytes** — each extra
placement now costs about 198 bytes, which is the node record.

## Capabilities

### Modified Capabilities
- `file-io`: a payload several items share is stored once and comes back shared.

## Impact

- `src/scene/commands.cpp` — the payload table on `Writer` and `Reader`.
- `include/clay/scene/commands.h`, `include/clay/io/clayspace.h` — minor 17 and
  the two blocks that narrate every minor.
- `tests/unit/` — the sharing measurement, the round trip, the minor-16
  downgrade.
- No ABI change and no new entry point; the C and Python surfaces are untouched.
