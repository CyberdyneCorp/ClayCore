# Tasks: the SwiftPM library product is static

## 1. The fix

- [x] 1.1 `Package.swift`: `.library(name: "claycore", type: .static, ...)`,
      with the mechanism recorded at the declaration — an automatic product
      becomes a dynamic PackageProduct framework under Xcode 26, and the empty
      wrapper target references no archive member, so zero objects link.

## 2. The gate

- [x] 2.1 `tools/check_swift_package.py`: the products whose linkage is
      load-bearing are declared static, named in one table with a reason each.
- [x] 2.2 Authoritative mode via `swift package dump-package` where a toolchain
      exists; textual mode where it does not; the mode is always reported.
- [x] 2.3 The failure message carries the consequence and the exact declaration
      to write.
- [x] 2.4 Verified BOTH ways: the gate fails on the manifest as it shipped and
      passes on the fix.

## 3. CI and docs

- [x] 3.1 Wired into the Linux gates job (textual) and the macOS job
      (authoritative, the only runner with a Swift toolchain).
- [x] 3.2 `README.md` repository-checks list.
- [x] 3.3 The limitation recorded rather than hidden: no Xcode-based link test
      exists, `swift run claycore-smoke` cannot catch this class of regression
      because an executable's direct references pull the archive whatever the
      product linkage, and `swift build` does not construct the dynamic
      framework Xcode does.
