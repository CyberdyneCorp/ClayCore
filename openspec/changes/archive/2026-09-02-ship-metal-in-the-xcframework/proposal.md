# Proposal: the shipped Apple artifact did not carry the backend it exists for

## Why

`CLAY_BACKEND_METAL` is an `option()` defaulting to `OFF`, and
`tools/build_xcframework.sh` never passed it. The xcframework is the only
distribution path for Apple hosts, so every host that linked it got:

```
clay_list_backends -> "cpu"
```

Metal is described throughout this repository as "the iPad app's production
path". It was not in the product.

There is no consumer-side recovery. `CLAY_BACKEND_METAL` decides which sources
are *compiled into* `libclaycore-*.a`; an app consuming a prebuilt static
library cannot enable it by setting a flag or linking `Metal.framework`. The
script's own comment said otherwise —

> The Metal backend is wired per-app during Xcode integration (the app links
> Metal.framework and enables CLAY_BACKEND_METAL in its build)

— and that is not a thing an app can do.

Measured by the consuming app on an iPad Air 13-inch M3, Release, identical app
code with only the framework differing:

| items | bake, cpu | bake, metal |
|---|---|---|
| 4 | 357.5 ms | 330.0 ms |
| 13 | 404.5 ms | 366.9 ms |
| 19 | 755.5 ms | 356.7 ms |
| 25 | 958.7 ms | 375.1 ms |

CPU degrades 2.7x over eight strokes; Metal stays flat. The shape matters as
much as the ratio: a cost that grows with what the artist has already drawn is
the same slope `add-item-spatial-index` exists to remove, and this artifact was
paying it for no reason.

## What changes

**The xcframework is built with Metal on**, and every backend option is passed
explicitly — including the ones left off. CMake caches `option()` values and
these build directories are not deleted between runs, so an inherited cache
could otherwise decide what ships. That is not hypothetical: it produced a
silently wrong A/B on the consumer's side, where a run intended as CPU-only was
still Metal and read as "the backend does nothing".

**The metallib is compiled for the slice being built.** This is the part the
bug report did not reach, and the reason a one-line flag flip would not have
worked: `xcrun -sdk macosx metal` was hardcoded, so enabling the backend would
have embedded a **macOS** metallib in the iOS slices. That builds and links
cleanly and then fails at runtime — `newLibrary` rejects it, `create()` returns
null, the backend does not register, and the framework is CPU-only again, one
level further from anything that would notice.

**Two gates, because the bug was that nothing asserted this.** The build fails
if a slice has no embedded metallib, and the Swift smoke test asserts the
backend actually registers. They check different things on purpose: the archive
gate is about what the build SHIPS and holds without a GPU; the smoke assertion
is about what a host GETS and needs a Metal device to mean anything.

**`Package.swift` declares Metal and Foundation.** A `binaryTarget` cannot
carry linker settings, so the target that links it must, and an app needs the
same two. Without them the archive has undefined Metal symbols.

## What it is not

**Not a change to any result.** Backends change speed, never values — the
parity contract is unaffected, and this ships no new capability.

**Not an ABI change.** No symbol, no descriptor, no struct. A host that
recompiles nothing sees a faster library.

**Not a claim about the simulator.** The consumer reported a baked field
departing 0.166 from the document where CPU gives 0.033, on the iOS Simulator
only, not on device. That is recorded as an open question below rather than
waved away: "the simulator emulates Metal" is an explanation, not evidence.

## Open questions

- **Should a CPU-only artifact still be available?** The report suggests a
  separate slice or variant if it is wanted. Nothing in this repository needs
  one, and shipping a second artifact whose only difference is invisible at the
  API is a way to get the wrong one deployed. Not doing it unless asked.
- **The Simulator parity deviation.** Worth reproducing under the parity suite
  on the simulator rather than through an app fixture, so it is either a known
  emulation artefact with a number attached or a real finding. It needs a
  Metal-enabled framework to investigate at all, which is what this change
  provides.
- **Deployment target for the metallib.** The slices set
  `CMAKE_OSX_DEPLOYMENT_TARGET`; whether the Metal compiler needs a matching
  `-mmacosx-version-min` / `-miphoneos-version-min` to avoid an AIR version a
  16.0 device refuses is unverified, and would show up as the same silent
  non-registration this change is about.

## Impact

`build-packaging` gains the requirement that the shipped artifact carries the
backend it is for, and that the build fails rather than shipping one that does
not. No ABI change, no behaviour change beyond speed.
