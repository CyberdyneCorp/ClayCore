## Why

A `.clayspace` cannot carry a multiresolution hierarchy. `add-mesh-multires`
shipped `mesh::MultiresSurface` as a **standalone handle** — no layer owns one,
`io::ClaySpaceDoc` has never seen one, and `io::document_memory` does not count
one. Saving a document therefore saves the base cage and silently drops every
sculpted level above it.

This is the **ClaySpaceDesktop host's rank-2 ask**, collected 2026-09-06 and
recorded in `openspec/ROADMAP.md`. In their words, a hierarchy row is two
objects: a mesh layer holding the cage, and a `clay_multires` beside it. Because
the engine reports that layer as an ordinary MESH layer, *their side-car file is
the only thing in the world that knows the row was ever a hierarchy*. Lose the
side-car — a copy, a move between machines, an export to anyone without their
application — and the sculptor's levels are gone and the row comes back as the
flat cage it demonstrably is. They made the loss loud in three panels and a
diagnostics report, and record that **loud is not fixed**.

Now, because the expensive half is already built and the cheap half is a shape
this file has used twice. `MultiresSurface::encode()` / `decode()` exist, are
versioned, and already accept a version-1 stream as a hierarchy with no layers
(`add-mesh-sculpt-layers` moved them to `kSurfaceVersion` 2). What is missing is
a chunk, a home, and a format minor.

## What Changes

- `io::ClaySpaceDoc` gains `multires_layers`, a `std::map<scene::LayerId,
  mesh::MultiresSurface>`, beside `mesh_layers`, `voxel_layers` and `masks`.
  **The home is forced, not chosen**: `tools/check_layering.py` withholds
  `clay/mesh` from `clay::scene`, so a hierarchy physically cannot be owned by a
  `scene::Layer`, which is what keeps "this content does not change what the
  document evaluates to" structural rather than maintained.
- A new `'MRES'` chunk in `.clayspace`, one per hierarchy, carrying the layer id
  and the bytes `MultiresSurface::encode()` already produces. Older readers skip
  it, which is the mild kind of format change `add-mesh-multires`' design
  anticipated in as many words.
- `kClaySpaceMinor` 17 → 18, **writable at 17**, where writing at the older minor
  omits the chunk and the release notes say exactly what that loses: the levels,
  the detail field, and the sculpt-layer stack — everything above the cage.
- The orphan policy already written for `mesh_layers` applies verbatim: an entry
  survives its layer's removal so undo within a session works, the writer emits a
  chunk only for an id that is still a mesh layer, and the reader drops a chunk
  naming none.
- The engine can answer **whether a layer carries a hierarchy**, so a host does
  not need a side-car to know what a row is. The host's ask names a
  `LayerRepresentation` type; no such type exists in this tree, and a chunk keyed
  by layer id makes the question answerable without inventing one.
- `io::document_memory` counts hierarchies, which it does not today —
  `MultiresSurface::memory()` exists and nothing calls it from the document side.

Not in this change, and stated so the boundary is not re-litigated: a hierarchy
still does not reach the evaluated field, and `clay_multires` remains a
host-owned handle with its own lifetime. This change makes a document able to
carry one and hand it back; it does not make a layer *be* one.

## Capabilities

### New Capabilities

None. The behaviour belongs to capabilities that already exist.

### Modified Capabilities

- `file-io`: `.clayspace` carries a multires hierarchy — the `'MRES'` chunk, the
  minor 17 → 18 with its documented downgrade, the orphan rule, and the round
  trip that a saved hierarchy reloads to a bit-identical surface.
- `mesh-multires`: a hierarchy is a document payload rather than only a
  standalone handle — where it lives, that its identity is the layer id, and that
  it is counted by document memory.
- `c-abi`: the entry points a host needs to ask whether a layer carries a
  hierarchy and to take the one a load produced, plus what they refuse.
- `python-bindings`: pyclay follows, or `tools/check_binding_parity.py` fails.

## Impact

- `include/clay/io/clayspace.h`, `src/io/clayspace*.cpp` — the struct member, the
  chunk, the minor and its gating.
- `include/clay/mesh/multires.h` — no encoding change expected; `encode()` /
  `decode()` are used as they stand. If that proves false, the finding is
  recorded rather than the format quietly widened.
- `bindings/c/clay.h`, `bindings/c/clay_c.cpp`, `bindings/python/pyclay_module.cpp`.
- `tools/check_gallery.py` and the gallery documents, which a minor bump
  regenerates.
- `docs/05-claycore-library.md`; `docs/RELEASE.md` at release time, not here.
- **Coordination**: PR #477 (`fold-the-layers-with-an-operator`) is in flight and
  touches `include/clay/io/clayspace.h` and nine gallery `.clayspace` files.
  Implementation waits for it to land rather than racing it; this proposal is
  written in the meantime because it touches only this directory.
