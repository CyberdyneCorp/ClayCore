# Proposal: Read Curve Points

## Why

The C ABI has four ways to WRITE a curve's control points and none to read one.
That is deliberate for the item builder — `clay_item` is write-only by design,
and the host that filled one already knows what it put there — but it leaves a
host that RELOADS a document holding a node id and nothing else. It cannot draw
the control cage, cannot drag a point, cannot re-tessellate at a new tolerance.
The points are in the file and in the compiled tape; there is simply no door.

A profiled tube is the sharp end of it. `clay_tube_create` produces a SWEPT
node, and a sweep's guide is the same control-point list a curve item uses, so
the same gap makes a reloaded tube unshapeable.

## What it is

`clay_layer_stroke_points` — argument-for-argument the setter it mirrors, with
`size_t count` becoming `size_t* count`, so the two declarations diff cleanly
and what comes out goes straight back in. It follows the size-query convention
the rest of the ABI already uses: NULL primary buffer to learn the count, one
`size_t*` that is capacity going in and count coming out, `BUFFER_TOO_SMALL`
with the needed count written back.

Points come back AS AUTHORED. Tessellation happens when the document is
compiled, so a readback is control points and round trips through the setter
unchanged — which is the property the tests assert, rather than the bytes.

Reading is not editing: a ghosted, locked or hidden layer answers normally,
because protection refuses edits. `clay_document_layer_protection` already
reads a protected layer, and this is the same kind of call.

## The setter widening that came with it

`SetStrokePointsCmd` refused anything but a `Stroke`, while
`clay_item_set_curve_points` has always accepted a swept guide too. A getter
that reads a tube's guide but a setter that will not write it back is a half
door, so the command now accepts both — the guide IS an ordinary curve, and
editing it is the ordinary curve edit.

`clay_layer_set_stroke_points` keeps the closed-guide refusal at the C
boundary, the same one `clay_item_set_curve` makes: transporting a frame around
a loop does not return it to its starting orientation, and the leftover twist
is real geometry. `Layer.set_points` keeps it in pyclay for the same reason —
`clay.Swept` has never offered a closed flag. It sits at the two binding
boundaries rather than in the command because it is a rule about what these
APIs let you author, not an invariant of the scene model, and a command that
refused it would have reported it as a missing node.

## What it is not

Not an item-builder getter. There is not one `clay_item_*` getter in the ABI
and this does not start one: reading a document back is one-phase even though
authoring is two-phase.

Not a tessellated-curve reader. A host that wants the segment chain evaluates
the field or meshes it; this returns what the document stores.

Not `stroke_blend_k`. The setter does not write it, so a read-modify-write
round trip preserves it, and the getter returns exactly the setter's arguments.
