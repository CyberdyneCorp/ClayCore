# Design

## Context

Everything this change needs already exists in halves. `relax` smooths a volume
and copies it first. `consolidate_layer` bakes a layer and then installs the
result, in one function. `move_brush` walks the edit list and then produces
warps, in one function. `UndoStack` groups commands, but only one bracket deep.
`report_layer` measures degradation and has no consumer inside the library.

None of those halves was wrong for the caller it was written for. They are
wrong for a caller that spans SIXTY pointer events, because each of them pairs
a cost that scales with the MODEL against a cost that scales with the BRUSH,
and a live gesture has to pay the first one once and the second one per frame.
Splitting them is most of this change; the transactions are what holds the
split together and states which half runs when.

## Goals / Non-Goals

**Goals:**

- A Smooth stroke a host can draw between pointer events, whose commit installs
  the bytes that were drawn.
- A Move drag that writes the persistent document exactly once, at pointer-up,
  and whose per-frame cost counts the items it moves.
- A transaction that cannot silently overwrite an edit made elsewhere while it
  was open.
- One undo step per gesture, including anything the gesture's policy triggers.
- A place for a session to authorise collapsing history, with the engine still
  never deciding to.

**Non-Goals:**

- A local working patch for Smooth (P0 takes the whole layer; see below).
- Reaching this from C, Python or Swift in this change.
- A serializable open gesture, in any form.
- Making the edit-list brushes transactional — they already have the right
  lifetime.

## Decisions

### `session/`, not `scene::Document`

The obvious alternative — a `Document::open_smooth()` returning a handle, with
the working volume held on the layer — was rejected because it makes the
document able to be in a state the format cannot describe. A document that
could hold an open stroke would need a `.clayspace` that describes one, and a
"currently smoothing" field in a saved file is a state every loader, every
journal replay and every crash recovery would have to decide what to do with.
The same argument disqualifies `scene::Layer`, `StrokePreset` and `FieldVolume`
as owners.

`session/` already holds the things that live for a sitting and are never
saved — `session/history.h` is there for the same reason — so the transaction
goes where its lifetime already has a home. The consequence is deliberate and
worth stating: a transaction holds a `Document&`, so it is the caller's job not
to destroy the document under an open gesture, exactly as it is with every
other non-owning reference in the library.

### A whole-layer working volume, not a local patch

Smooth's `begin()` bakes the entire finite layer, and every dab after that
rewrites only the bricks its region selects. The rejected alternative was a
local patch — sample only the ball the brush is over, relax that, composite it
back.

A patch needs exact composition semantics at its boundary: what the patch means
where it meets the field it was cut from, what happens when the next dab's ball
overlaps the previous patch rather than the layer, and how the union of a dozen
overlapping patches becomes one volume at commit. That is new correctness
surface, and it is the kind that fails as a visible seam. The subject of this
P0 is interaction latency, not a new compositing rule.

What makes the whole-layer bake affordable enough to ship first: the volume is
sparse, `rewrite_region` makes every dab after the first cost what it touches
(`make-the-relax-dab-local` did that work), and the one-time cost lands at
pointer-DOWN, where a brush cursor can cover it — as opposed to per dab, which
is what it was before. Lazy local checkpoints are the follow-up, `begin()` is
benchmarked on its own so the trade stays visible rather than becoming the
accepted cost, and nothing in the transaction's contract promises the working
set is the whole layer.

### A content fingerprint, not a document generation counter

A transaction is built against a specific source state and holds it across a
gesture. If anything else edits that layer meanwhile — another tool, a journal
replay, an undo — committing would overwrite that edit with a preview computed
from a document that no longer exists. So the source is stamped at `begin` and
re-checked at `commit`, and a mismatch REFUSES rather than forces.

The alternative was a generation counter on the document or the layer, bumped
by every mutation. It was rejected because there is no such counter today, and
adding one means every mutating entry point in the codebase remembering to bump
it — for ever, including the ones added next year. A counter that is only
mostly maintained is worse than no counter, because the failure is silent and
in the unsafe direction.

A digest is computed from the CONTENT, so it cannot be forgotten. It costs
exactly twice per gesture, never per pointer event. Three properties fall out
of hashing content rather than pointers, and each is load-bearing:

- An instance layer shares its edit list, so an edit through a sibling instance
  IS an edit to this layer — and the shared pointer has not moved. The digest
  walks the content, so it sees it.
- Shared immutable payloads (a volume's samples, a gate) are folded in by
  identity and size, not by hashing megabytes: they are immutable once shared,
  so a replacement is a different object.
- Floats are mixed by their BITS, so a value that prints the same cannot fool
  it. `-0.0f` and `0.0f` differ here and differ to nothing else, which costs a
  spurious refusal at worst and never a missed one.

64 bits collide in principle. The consequence of a false MATCH is a stale
commit, not a corrupt document; the alternative — a full structural compare —
costs the thing the digest exists to avoid.

### `UndoStack` grouping becomes nestable

`begin_group`/`end_group` was a bool: an inner bracket pushed a second entry,
so what the user did once became two undos. That was invisible while no
grouping entry point called another one.

A sculpt transaction calls one. `commit` opens a bracket, `replace_layer_with_
volume` opens its own, and a policy-authorised `consolidate_layer` opens a
third — an artist who made one stroke would have had to undo three times, in an
order that exposes an intermediate state they never saw.

The alternatives were both worse. Threading a "you are already inside a group"
flag through every entry point spreads a stack property across the call graph
and is exactly the kind of parameter callers forget. Making the transaction NOT
open a group, and relying on the inner brackets, gives up the guarantee that a
gesture is one step the moment a commit issues two commands.

Depth is the minimal fix: at depth 0 it does what it always did, nested
brackets collapse into the outermost step, and an unbalanced `end_group` is
ignored rather than corrupting the stack. An outer bracket that recorded
nothing still records nothing.

### Consolidation stays advisory and opt-in

`report_layer` measures and never bakes, because a bake discards the parameters
of everything it absorbs. Deciding on an artist's behalf that a sphere's radius
is no longer editable is not a library's call, and this change does not make it
one.

What it adds is the missing half — somewhere for a SESSION to record that in
this mode, on this device, with this frame budget, collapsing is acceptable.
The shape of that:

- `allow_consolidation` is **off by default**, and a value-initialised policy
  authorises nothing and measures nothing. Over budget with it false is a
  report: `budget().over_budget` is true, `budget().consolidated` is false, and
  the document is exactly what the stroke left.
- The three criteria stay SEPARATE — safe step scale, deformer chain length,
  item count — because the report keeps them separate. A chain of grabs and a
  chain of resampled volumes decay the same number for different reasons, and
  an aggregate cannot say which one to cure.
- Zero disables a criterion, so the empty struct is the safe reading rather
  than the strictest one.
- An authorised collapse runs inside the stroke's own undo bracket, so the
  artist undoes one thing having done one thing.
- A layer that is ALREADY a single volume item is not baked again.
  `consolidation_state` is exactly that question, and Smooth's own commit
  leaves the layer in that state — so without this check an over-budget Smooth
  would resample its own freshly installed samples into a volume of a volume,
  at a worse Lipschitz, for no reduction in what the layer costs.

### Move's update takes TOTAL displacement, never an increment

`update(0.10)`, `update(0.20)`, `update(0.50)` must end at exactly what a
single fresh drag of `0.50` produces.

An incremental API cannot give that. Each increment would be authored against a
different intermediate surface, so three increments compose three warps whose
result is not the drag the artist made — and grab's pull is deliberately less
than the displacement asked for, so the composition does not even converge to
the right place. The same reasoning is already in the codebase: `moved_chain`
REPLACES a leading grab from the same drag rather than stacking on it, and that
rule exists because per-frame stacking is what `add-move-drag-continuity` was
raised to fix.

So every frame resolves from the same two immutable things: the `PreparedMove`
captured at begin, and the pre-stroke deformer chain captured by value at
begin. The commit rebuilds from those same two rather than trusting the
preview, which makes a commit correct even if the host never called `update`
and makes it impossible for the preview and the commit to be computed by
different code.

Total displacement also makes the API honest about what a drag IS: a fixed
anchor and a growing offset. There is no state to lose if a frame is dropped,
no ordering requirement between updates, and re-sending the same displacement
is idempotent.

### The resolution is the caller's

A document has no intrinsic sampling resolution — `ConsolidationParams` says
why, and inventing a cell size for Smooth would silently fix the shape's
resolution at a number nobody chose. `SdfSculptPolicy` carries the same three
numbers with the same meanings, `cell_size` is required and a zero refuses the
transaction, and a policy-triggered consolidation with no numbers of its own
falls back to the gesture's — the resolution the host already chose for this
sculpt mode, and the only one available that nobody has to guess.

### `session` may include `brush`, and the layering table says why

A live drag has to resolve one warp per pointer event, and there is exactly one
correct resolver for that: `brush::resolve_prepared_move`. Growing a second one
inside `session` would mean a preview computed by different arithmetic than the
commit — a preview of something else — so the alternative to the edge was the
one thing this change exists to prevent.

The edge is free. `brush`'s own dependency set is already a subset of
`session`'s, so it adds nothing to the transitive graph, and nothing in `brush`
knows about `session`, so there is no cycle. `tools/check_layering.py` records
both facts beside the entry rather than in a commit message.

What it does not do is put the transactions in `brush`. A transaction is not a
stroke engine: its subject is the GESTURE lifetime — begin, update, commit —
and `stroke.h` is deliberately about turning samples into stamps.

### The C surface hands back COPIES, and says so

The preview half was the part with a real choice in it, and it was left open
until there was an implementation to make it against.

`clay_sdf_smooth_preview_item` returns a fresh `clay_item*` carrying a **copy**
of the working volume. The cheaper option — a borrowed view of the samples the
transaction is still mutating — is the one bug this whole change is about
avoiding, one level down: a compiled tape pointing at a volume that changes
underneath it. So the copy is the contract, it is documented as the cost it is,
and the dirty bounds from `clay_sdf_smooth_update` are what a host uses to
decide how often it needs the samples at all.

Move needs no copy and gets none. `clay_sdf_move_preview_nodes` reports the
affected ids by the size-query pattern the ABI already uses, and
`clay_sdf_move_preview_grab` reports the resolved warp as the parameters
`clay_item_add_deformer(CLAY_DEFORM_GRAB, ...)` already takes — so a host draws
the preview through machinery it has and **no new struct crosses the
boundary**. Exposing a whole preview `Layer` would have needed one, and the
grab is what a host actually wants to draw.

The policy descriptor is FLAT rather than nested, because the ABI has no nested
descriptors and a `struct_size` inside a `struct_size` negotiates nothing. The
consequence is that the consolidation's own `ConsolidationParams` is not
expressible from C, which is why an unset one falls back to the gesture's
sampling: the alternative was a second cell size for a host to get out of step
with.

## Risks / Trade-offs

**A gesture holds a `Document&` it does not own.** Destroying the document
under an open transaction is a use-after-free, exactly as with every other
non-owning reference here. Mitigated by the transactions being move-only and by
`live()` going false at commit or cancel, so a dead transaction updates nothing
and commits nothing. Not mitigated for the destroyed-document case, and a C ABI
handle should not pretend otherwise.

**`begin()` costs a whole-layer bake.** That is the trade taken above: a cost
that used to be paid per dab is now paid once, at the point in the gesture
where a host can cover it. It is still O(model), and on a very large layer at a
fine cell size it will be felt at pointer-down. The follow-up is local
checkpoints; the guard against it becoming the accepted cost is that `begin()`
is measured separately.

**A fingerprint can collide.** 64 bits, FNV-1a, chosen for being three lines.
The consequence is a stale commit rather than a corrupt document, and a full
compare costs what the digest exists to avoid.

**A commit can refuse, and a host has to handle it.** The alternative was to
force, which loses an edit the artist did make in favour of a preview computed
against a document that is gone. Refusing means the host must be able to say
"the layer changed, your smooth was dropped" — which is a UI it did not need
before. That is the right side to fail on and it is not free.

**The C preview copies the working volume every time it is asked for.** A host
that asks per frame pays a full volume copy per frame, which is worse than what
it replaces on a large layer. The dirty bounds exist so it does not have to,
and the header says so — but nothing stops it, and a borrowed view would be
unsafe rather than merely expensive.

**`over_sculpt_budget` names three numbers a host has to choose.** No default
is offered, deliberately, because a safe step scale that suits an iPad's frame
budget is not the one that suits a workstation preview. The value-initialised
policy authorises and measures nothing, so a host that ignores the field gets
today's behaviour.
