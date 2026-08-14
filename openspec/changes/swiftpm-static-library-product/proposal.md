# Proposal: the SwiftPM library product is static, and a gate says so

## Why

**Every Xcode 26 consumer of the shipped package fails to link**, with every
`clay_*` symbol undefined, against an archive that contains all of them
(#112). It has been true since the 0.26 packaging change and shipped through
four releases.

The mechanism, which is worth stating because the symptom points nowhere near
the cause. `Package.swift` vends `claycore` as an **automatic** library
product, so SwiftPM picks the linkage — and Xcode 26 picks a *dynamic
PackageProduct framework* in Debug. That framework links the slice archive, but
the product's only target is `ClayCoreLink`: one empty Swift file that exists to
carry the `Metal`/`Foundation` linker settings a `binaryTarget` cannot hold. It
references no archive member, so the linker pulls **zero** objects. The
resulting framework exports nothing, and the app that depends on it fails with
a wall of undefined symbols while `nm -gU` on the archive shows every one of
them present.

At 0.25.0 the product WAS the `binaryTarget`, which Xcode always links directly
into the client. Vending `ClayCoreLink` instead — correct, and the only way to
attach the linker settings — is what opened the hole.

**The linkage was never a decision.** It was left unspecified, and an Xcode
release changed what unspecified meant.

## What

1. **`.library(name: "claycore", type: .static, targets: ["ClayCoreLink"])`.**
   Static removes the dynamic-framework path entirely rather than depending on
   which configuration Xcode chose. One line.
2. **`tools/check_swift_package.py`**, run by CI, asserting that the products
   whose linkage is load-bearing are declared static — with the reason in the
   failure message, so the next person to hit it does not re-derive the
   mechanism above.

## Why the gate is a manifest check and not a test

Because the one consumer shape the repository already builds is the one that
**cannot see the bug**.

`swift run claycore-smoke` is an *executable*. Its direct symbol references pull
the archive members whatever the product's linkage, so it passed throughout —
it is precisely the reference the empty wrapper lacks. Reproducing the real
failure needs Xcode, a Debug configuration and an app target consuming the
package as a library; none of those exist in CI, and adding a library target to
this package would not reproduce it either, because `swift build` does not
construct the dynamic PackageProduct framework Xcode does. A test that cannot
fail is worse than a stated limitation.

So the gate asserts the **declaration**. That is a weaker claim than "an app
links", it is stated as one in the tool's own output, and it is exactly the
claim that was violated.

It runs in two modes and always says which: **authoritative** via
`swift package dump-package` on the macOS runner, where SwiftPM's own reading
of the manifest is available; **textual** on Linux and on a contributor's
machine with no toolchain.

## Impact

- **Consumers:** an Xcode 26 app links again, with no `-force_load` workaround
  in `OTHER_LDFLAGS`. Static was already the effective linkage on every Xcode
  before 26, so nothing that worked stops working.
- **`build-packaging` spec:** gains a requirement that the linkage is declared
  rather than defaulted.
- **CI:** one new gate on two jobs. No build cost.

## What this does NOT do

- It does not add an Xcode-based link test. The gap between "declared static"
  and "an app links" stays open and is recorded here rather than papered over.
- It does not change the xcframework, the archive, or any symbol in it. The
  archive was always correct.
