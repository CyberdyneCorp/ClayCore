# Proposal: close two C ABI gaps the app hit

## Why

An audit of issues #5, #6 and #7 found that two of their sub-items were left
half-done by the same shape of omission: the C ABI could not do something the
Python bindings could.

**Baking a document.** `add-sampled-fields` gave the C ABI
`clay_item_volume_from_mesh` and stopped there, and I recorded a parity-gate
exemption arguing that sampling a *document* was "a scripting convenience …
C samples a MESH, which is what an app imports". That was wrong, and the issues
say why. Relax and flatten both operate on a volume, so with a mesh as the only
source an app could smooth an imported scan but not its own sculpt (issue #7
item 2), and could not collapse a long edit list into one item (issue #7 item
5). One missing entry point was holding up two sub-items.

**The import budget.** `clay_mesh_load` shipped without the `ImportBudget` the
issue explicitly asked for, so a C caller was hard-pinned to the library
defaults of 50M vertices and 100M triangles and could not tighten the guardrail
for untrusted input. `openspec/specs/file-io/spec.md` requires budgets be
"configurable"; at the ABI they were enforced but not configurable.

The same audit turned up a plain bug alongside it: `clay_mesh_load` compared the
file extension case-sensitively, so `model.OBJ` was refused as an unknown format
while the Python loader had always accepted it.

## An announced ABI break

`clay_mesh_load` gains a parameter rather than acquiring a `_budgeted` sibling.
Two entry points that differ only by one nullable argument would be two ways to
say one thing, which this codebase avoids deliberately; and the function is one
release old.

Under SemVer's 0.x rule a break is allowed on a minor bump, but `docs/RELEASE.md`
requires that it never be silent. It is announced here and in the release notes.
A caller compiled against 0.21.0 gets a **compile error**, not a misread — the
arity changed, so there is no way for old code to link and behave differently,
which is the property that rule is protecting.

## What Changes

- **`clay_item_volume_from_document`**: sample a document's own field into an
  item carrying a volume, with an optional region. The parity-gate exemption
  that argued against it is removed rather than reworded.
- **`clay_import_budget`** and a `budget` parameter on `clay_mesh_load`,
  nullable for the library's defaults, with a zeroed field meaning "the default"
  rather than "allow nothing".
- **Case-insensitive extension matching** in the C loader, and the same budget
  threaded through the Python loader, which dropped it the same way.

## What this change does not do

- **No node-range scoping.** Consolidation bakes a whole document; collapsing a
  chosen node range (issue #7 item 5's fuller ask) needs a way to name a subset
  of an edit list, which is its own design.
- **No cell-size default for a document.** A mesh's bounds give one; a document
  has no intrinsic scale, and after a bake the resolution IS the shape, so
  guessing it would silently pick something the caller never chose. It is
  required.

## Capabilities

### Modified Capabilities

- `c-abi`, `python-bindings`.

## Impact

- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`, the Python loader,
  `tools/check_binding_parity.py`, tests, docs.
