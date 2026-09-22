# Design

A cell reaches the owner's neighbors according to whether each coordinate is on the first plane, an interior plane or the last plane. For dimensions greater than one these give three categories per axis. Precompute a three-bit x mask for each y/z category using the existing closed-boundary neighbor rule.

If the interior x category reaches a requested neighbor, both endpoint categories do too: each includes the interior category's x offset zero. Emit the complete row in that case. Otherwise only its first and last cells can qualify. Walk z then y then x exactly as before. A dimension-one cell touches all neighboring closed boxes, so emit it once if any neighbor was requested. Nonpositive dimensions emit nothing.

Keep requested-neighbor membership lookup and packed global coordinates at the call site. Put local enumeration in a small internal helper so independent brute-force closed-box intersection tests can check all supported dimensions and asymmetric masks. All state is local and bounded; allocations and sample evaluation remain unchanged.
