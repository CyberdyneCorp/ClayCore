# Proposal: a descriptor the caller does not measure is one we cannot bound

## Why

`bound-output-descriptor-fills` fixed every output descriptor that already
probed `struct_size` and stopped at two that did not, because finishing them
needs a decision rather than a fix.

`clay_brick_config_defaults` and `clay_stroke_preset_defaults` SET
`struct_size` instead of reading it — "fills a descriptor with the engine's
defaults, struct_size included". That reads as a convenience and is really the
one case the prefix rule cannot cover: if the caller declares nothing, there is
no size to bound the fill against, so the fill is `sizeof` as **the library**
defines it, into whatever the caller allocated.

`clay_brick_config` had already grown a `colors` field. So this was not latent:
a host built against the 24-byte layout had **8 bytes written past the end of
its struct**, every call, silently. Measured by canary against `main`:

```
clay_brick_config_defaults   wrote 8 bytes past a 24-byte buffer  <-- OVERRUN
```

There is no version of this that rescues an **already-compiled** old host. It
declares nothing, so nothing can be inferred about its buffer, and any fill is
a guess. The realistic choice is between corrupting it silently and refusing it
loudly.

## What

Require `struct_size` on input to both defaults calls, exactly as every other
descriptor in this ABI requires it, and bound the fill to it. A caller that
does not set it gets `CLAY_ERROR_INVALID_ARGUMENT` instead of a filled struct.

`clay_stroke_preset_deserialize` comes along for the ride, and is the reason
the sweep in `bound-output-descriptor-fills` was incomplete: it filled its
output descriptor by DELEGATING to `clay_stroke_preset_defaults` rather than
spelling out `*out = clay_stroke_preset{}`, so it matched no grep for the
pattern and was invisible to the first pass. It is a ninth site.

That miss is the argument for the gate this change adds. `tools/check_c_abi.py`
now walks `clay.h` for every entry point taking a versioned descriptor by
MUTABLE pointer and requires each one's body to reach a bounded fill — so the
next site is caught by construction rather than by whoever greps well.

**The gate immediately earned itself, twice over.** A first draft of it looked
for the two spellings that were in the tree, and using it turned up a third:
`*out = local`, which matched neither. That spelling was hiding
`clay_mesh_brush_defaults`, whose original layout ends at `smooth_iterations`
while `layer_height` and the entire alpha block came after it — **56 bytes past
a 104-byte buffer, the largest overrun in the ABI**, sitting in a function two
sweeps had already walked past. `clay_mesh_sculptor_raycast` was a tenth site
with the same spelling.

The same exercise found the gate's other end was wrong too. Its descriptor scan
used a non-greedy match across the whole header, which spans struct boundaries
and so called every later struct a descriptor — including the array-element
types, whose defining property is that they carry NO `struct_size`. It would
have failed the gate on correct code the first time somebody touched
`clay_brick_cache_take_dirty`. The scan is now bounded to each struct's own
body and honours `ARRAY_ELEMENT_STRUCTS`.

Run against `main` the finished gate reports ten sites; against this change,
none.

## Impact

**Breaking, and deliberately so.** Same signatures and same arity, so a caller
that does not set `struct_size` still compiles and links — the change is only
visible at runtime, as a refusal. Every caller adds one line:

```c
cfg.struct_size = sizeof(cfg);
```

Twenty-six in-repo call sites, all tests, tools and benchmarks; no production
consumer in this repository. Allowed on a minor below 1.0 under SemVer's 0.x
rule, and called out in `docs/RELEASE.md` rather than shipped quietly. ABI
0.35.0.

Two things found on the way and fixed here rather than left:

- `pyproject.toml` had been stuck at **0.30.0** while CMake and the ABI moved
  to 0.34.0. `tools/release_check.py` requires all three to agree, but it runs
  only in `release.yml`, so the drift survived four minor bumps and would have
  failed the next release. All three now read 0.35.0.
- `docs/06-host-gpu-previews.md` shows the brick-cache loop starting from
  `clay_brick_config_defaults`, so its sample needed the new line too.
