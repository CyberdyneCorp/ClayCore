#!/usr/bin/env bash
# Swift smoke gate (c-abi spec): compile and run tests/swift/smoke.swift against
# the xcframework, proving clay.h is consumable from Swift and not merely valid
# C — and that the engine runs on the platform ClaySpace ships to.
#
# Two targets:
#   macos  — builds and runs against the macos-arm64 slice on this machine.
#   sim    — builds against the ios-arm64-simulator slice and runs the binary
#            inside a booted simulator via `simctl spawn`. This is the one that
#            proves the iOS slice works, rather than merely linking.
#
# Apple-only and slow (it needs the xcframework), so this is a release-time and
# on-demand check rather than a per-push one.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"

target="${1:-all}"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "swift smoke: skipped (Apple platforms only)"
  exit 0
fi
if ! command -v swiftc >/dev/null 2>&1; then
  echo "swift smoke: skipped (no swiftc on PATH)"
  exit 0
fi

if [[ ! -d "dist/claycore.xcframework/macos-arm64" ]]; then
  echo "swift smoke: building the xcframework first"
  ./tools/build_xcframework.sh
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# A module map so Swift sees clay.h as `import claycore`, mirroring what the
# SwiftPM binary target gives the app.
make_module_dir() {
  local slice="$1" dir="$2"
  mkdir -p "$dir/claycore"
  cp "$slice/Headers/clay.h" "$dir/claycore/"
  cat > "$dir/claycore/module.modulemap" <<'MAP'
module claycore {
    header "clay.h"
    export *
}
MAP
}

run_macos() {
  local slice="dist/claycore.xcframework/macos-arm64"
  local lib
  lib="$(ls "$slice"/libclaycore*.a 2>/dev/null | head -1)"
  [[ -n "$lib" ]] || { echo "swift smoke: no macOS library in $slice" >&2; exit 1; }

  local dir="$work/macos"
  make_module_dir "$slice" "$dir"
  echo "== macOS =="
  swiftc -O -I "$dir" -o "$dir/smoke" tests/swift/smoke.swift "$lib" -lc++ -framework Foundation
  "$dir/smoke"
}

run_simulator() {
  local slice="dist/claycore.xcframework/ios-arm64-simulator"
  local lib
  lib="$(ls "$slice"/libclaycore*.a 2>/dev/null | head -1)"
  [[ -n "$lib" ]] || { echo "swift smoke: no simulator library in $slice" >&2; exit 1; }

  local sdk
  sdk="$(xcrun --sdk iphonesimulator --show-sdk-path 2>/dev/null || true)"
  [[ -n "$sdk" ]] || { echo "swift smoke: no iOS simulator SDK, skipping"; return 0; }

  # A booted device to spawn inside. Prefer one already running so a developer
  # does not get a simulator opened underneath them; otherwise boot the newest
  # available iPhone and shut it down again afterwards.
  local udid booted_here=0
  udid="$(xcrun simctl list devices booted -j 2>/dev/null \
          | python3 -c 'import json,sys
d=json.load(sys.stdin)["devices"]
print(next((v["udid"] for rs in d.values() for v in rs), ""))' || true)"
  if [[ -z "$udid" ]]; then
    udid="$(xcrun simctl list devices available -j 2>/dev/null \
            | python3 -c 'import json,sys
d=json.load(sys.stdin)["devices"]
c=[v["udid"] for rt,rs in sorted(d.items()) for v in rs if "iPhone" in v["name"]]
print(c[-1] if c else "")' || true)"
    [[ -n "$udid" ]] || { echo "swift smoke: no iPhone simulator available, skipping"; return 0; }
    echo "booting simulator $udid"
    xcrun simctl boot "$udid"
    booted_here=1
  fi

  local dir="$work/sim"
  make_module_dir "$slice" "$dir"
  echo "== iOS Simulator =="
  # arm64-apple-ios<v>-simulator is the triple the simulator slice was built
  # for; linking Foundation from the simulator SDK keeps it self-consistent.
  swiftc -O \
    -target arm64-apple-ios16.0-simulator \
    -sdk "$sdk" \
    -I "$dir" \
    -o "$dir/smoke" \
    tests/swift/smoke.swift \
    "$lib" -lc++ \
    -Xlinker -syslibroot -Xlinker "$sdk"   # else the linker takes the macOS sysroot

  local status=0
  xcrun simctl spawn "$udid" "$dir/smoke" || status=$?
  if [[ "$booted_here" == "1" ]]; then
    xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true
  fi
  return $status
}

case "$target" in
  macos) run_macos ;;
  sim|simulator) run_simulator ;;
  all) run_macos; echo; run_simulator ;;
  *) echo "usage: $0 [macos|sim|all]" >&2; exit 2 ;;
esac
