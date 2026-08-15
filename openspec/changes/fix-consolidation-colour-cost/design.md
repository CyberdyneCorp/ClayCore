# Design: when the colour pass is earned

## The predicate

Take the colour pass if and only if the absorbed set can produce more than one
colour. Two ways it can:

1. **Two or more distinct `Node::color` among the absorbed nodes.** The
   ordinary case, and the one the requirement was written for: skin and
   armour.
2. **Any absorbed node is a volume whose samples carry colour** —
   `node.volume && node.volume->has_color()`. A previously consolidated
   coloured volume has ONE `Node::color` and many sample colours, so testing
   node colours alone would wrongly skip it and silently flatten a
   re-consolidated character.

Otherwise skip: fill no colour channel, and let the node's own colour answer.

Both tests walk the absorbed node list. The bake already reaches into it at
`consolidate.cpp:256` for `absorb.front()`'s colour, so the nodes are in hand;
this walks the rest of them. O(nodes) against a pass that is O(surviving
samples) and evaluates the whole tape at each one.

## Why this is behaviour-preserving

The spec already says the node's colour is the answer for a volume with no
colour channel. So for a uniform layer:

- **today:** a colour channel filled with one repeated colour, plus a node
  carrying that colour.
- **after:** no colour channel, plus a node carrying that colour.

Both evaluate to the same colour everywhere, inside the sampled box and out.
This is also what v0.30.0 produced, which is why the v0.30.0 baseline is the
right target rather than a number to be renegotiated.

`Op::Paint` needs no special case. A Paint node carries the painted colour in
its own `color`, so a paint that changes the colour puts a second distinct
colour in the absorbed set and takes the pass by rule 1. A paint that applies
the colour already there is correctly skipped — it changes nothing to record.

## The predicate is public

`scene::layer_colors_vary(const Layer&)`, declared in `consolidate.h`. Adding
to the public surface for this was not the plan, and two things asked for it.

`test_consolidate.cpp` asserts the pooled grid bake and a serial full-tape bake
produce the SAME BYTES, and its reference reimplements the bake to do it. A
channel present in one and absent in the other is a difference in the bytes, so
the reference has to apply this rule too — and a rule restated in a test is one
that can drift from the code it is checking. Exporting it keeps one definition.

It is also a genuine question about cost. Filling the channel is a second
evaluation of the tape at every surviving sample; a host with a progress UI has
reason to ask whether consolidating this layer will pay it, in the same way
`ConsolidationCost` exists so a caller can see what it is buying.

## What was rejected

**Moving the budget to fit the measurement.** `sdf_consolidate` is over its
`operation` budget, not merely over its baseline. `check_device_bench.py`
treats those as different failures precisely so that a baseline cannot
enshrine something already too slow. Re-seeding with `--update` would convert
a regression into the new normal and is the reason the budget class exists.

**Making the colour pass cheaper instead of skippable.** It is already pooled
through the same injected evaluator as the distance pass — `e02d8b7` did that
work and it is what took 1115 ms down to 375 ms. What remains is a second
evaluation of the tape at every surviving sample, and no amount of scheduling
removes work that did not need doing.

**A `has_color` flag on `Node`.** It would make the predicate a field read
rather than a scan, and it is a data-model change to every node to avoid an
O(nodes) walk inside an O(samples) operation. The walk is free at this scale.

## Verification

The bisect harness lives outside the repo and drives the public C ABI, so it
compiles unchanged against any commit in the range. Confirming the fix is one
rebuild:

```
clang++ -std=c++20 -O2 -o bench bench_consolidate.cpp \
  -I<wt>/bindings/c -I<wt>/include \
  <wt>/build/metal/libclaycore.a \
  <wt>/build/metal/_deps/meshoptimizer-build/libmeshoptimizer.a \
  -framework Metal -framework Foundation
```

Target: 1000 stamps back to about 272 ms — the `ac7460a` number, which is what
remains once the colour pass is not paid. The Mac only locates the fix; the
gate is the iPad, and the signing certificate expires **2026-09-02**, so the
confirming device run has to land inside that window.
