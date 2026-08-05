# Proposal: ghosted and locked layers

## Why

A layer today is either visible or not. That single flag has to stand in for
three different things an artist wants:

- *Show me this for reference, but stay out of my way* — visible, but nothing
  picks it and nothing edits it. **Ghost.**
- *This is finished; don't let me touch it* — visible and pickable, but no edit
  lands on it. **Lock.**
- *Hide it entirely.* — what `visible` already means.

Without the first two, the only way to protect a layer is to hide it, which
also removes the reference you were protecting it against. 3DCoat users work
around exactly this by ghosting, and the complaint that "brushes touch
everything" is downstream of not having the flags. This is a small change that
properly fixes it.

## What Changes

- **Two flags on a layer**, both off by default so nothing changes for a
  document that never sets them:
  - `ghost`: still evaluated and drawn, but excluded from picking and from
    edits.
  - `locked`: still picked, but excluded from edits.
- **Picking honours ghost.** A ghosted layer is never the hit.
- **Edits honour both.** An edit naming a ghosted or locked layer is refused
  with a typed error rather than silently applied or silently dropped: a UI
  that greys out the layer wants to know, and one that does not should not
  quietly discard the artist's stroke.
- **Both flags go through the command vocabulary**, so setting them is
  undoable and serializes with the document like every other layer property.
  Their own bug list says every destructive operation should be undoable
  including hide; the same applies to protecting.
- **Evaluation is untouched.** Neither flag changes what a document evaluates
  to. Ghost is not a rendering mode here — the engine is headless, and how a
  host draws a ghosted layer is its own business.

## Capabilities

### Modified Capabilities

- `scene-model`: the flags, their commands, and their effect on edits.
- `picking`: ghosted layers are not picked.
- `python-bindings` and `c-abi`: both reach the flags.

## Impact

- `include/clay/scene/document.h`, `src/scene/commands.cpp`, `src/pick/*`, both
  bindings, tests, docs.
- ABI 0.14.0 — additive.
- The document format gains two bits per layer, written where the existing
  visibility flag is, so an older file loads with both off.
