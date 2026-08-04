#!/usr/bin/env bash
# Swift smoke gate (c-abi spec): compile and run tests/swift/smoke.swift against
# the xcframework's macOS slice, proving clay.h is consumable from Swift and not
# merely valid C.
#
# Apple-only and slow (it needs the xcframework), so this is a release-time and
# on-demand check rather than a per-push one. Builds the xcframework first when
# it is missing.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "swift smoke: skipped (Apple platforms only)"
  exit 0
fi
if ! command -v swiftc >/dev/null 2>&1; then
  echo "swift smoke: skipped (no swiftc on PATH)"
  exit 0
fi

slice="dist/claycore.xcframework/macos-arm64"
if [[ ! -d "$slice" ]]; then
  echo "swift smoke: building the xcframework first"
  ./tools/build_xcframework.sh
fi

lib="$(ls "$slice"/libclaycore*.a 2>/dev/null | head -1)"
if [[ -z "$lib" ]]; then
  echo "swift smoke: no static library in $slice" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# A module map so Swift sees clay.h as `import claycore`, mirroring what the
# SwiftPM binary target gives the app.
mkdir -p "$work/claycore"
cp "$slice/Headers/clay.h" "$work/claycore/"
cat > "$work/claycore/module.modulemap" <<'MAP'
module claycore {
    header "clay.h"
    export *
}
MAP

swiftc -O \
  -I "$work" \
  -o "$work/smoke" \
  tests/swift/smoke.swift \
  "$lib" \
  -lc++ \
  -framework Foundation

"$work/smoke"
