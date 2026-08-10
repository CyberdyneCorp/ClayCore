#!/usr/bin/env bash
# Run the claycore harness on an attached iPad.
#
# This is the only check that covers the path the iPad app actually ships on.
# `evaluation-backends` calls Metal "the iPad app's production path"; until this
# script existed, no iPad had ever run it.
#
# Deliberately refuses to fall back. A simulator or the host Mac would produce
# numbers that look like device numbers and are not — different thermals,
# different core counts, no memory-pressure kills — so a missing device is an
# error rather than a quieter run.
#
#   tools/run_device_bench.sh                 # first connected iPad
#   tools/run_device_bench.sh <udid>          # a specific device
#   CLAY_DEVICE_TEAM=XXXXXXXXXX tools/run_device_bench.sh
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
PROJECT="$ROOT/tests/device"
RESULTS="${CLAY_DEVICE_RESULTS:-$ROOT/build/device/results.xcresult}"

# -- the device ---------------------------------------------------------------

udid="${1:-${CLAY_DEVICE_UDID:-}}"
if [ -z "$udid" ]; then
    # xctrace lists ATTACHED devices before the "Devices Offline" heading and
    # prints the hardware udid xcodebuild wants, as the last parenthesised
    # field. Reading only the first section is what keeps a paired-but-absent
    # iPad — which is listed by name and cannot be run on — from being picked.
    udid="$(xcrun xctrace list devices 2>/dev/null \
            | sed -n '/^== Devices ==/,/^== Devices Offline ==/p' \
            | grep -i 'ipad' | head -1 \
            | sed -E 's/.*\(([^)]*)\)[[:space:]]*$/\1/')"
fi
if [ -z "$udid" ]; then
    echo "device-bench: no attached iPad found." >&2
    echo "  Attach one with Developer Mode enabled, or pass a udid." >&2
    echo "  Attached devices:" >&2
    xcrun xctrace list devices 2>/dev/null \
        | sed -n '/^== Devices ==/,/^== Devices Offline ==/p' | sed 's/^/    /' >&2
    exit 1
fi
echo "device-bench: target udid $udid"

# -- the artifact under test --------------------------------------------------

# The harness links the xcframework, and its iOS slice is where the Metal
# backend lives. A stale one is the failure mode that already bit this repo
# once (v0.24.0 shipped against a stale xcframework), so rebuild rather than
# trust what is on disk.
echo "device-bench: building the xcframework"
"$ROOT/tools/build_xcframework.sh" > /dev/null

# -- the project --------------------------------------------------------------

command -v xcodegen > /dev/null || {
    echo "device-bench: xcodegen not found (brew install xcodegen)" >&2; exit 1; }
( cd "$PROJECT" && xcodegen generate > /dev/null )

# -- run ----------------------------------------------------------------------

rm -rf "$RESULTS"
mkdir -p "$(dirname "$RESULTS")"

# macOS ships bash 3.2, where an empty array expanded under `set -u` is an
# unbound-variable error rather than nothing at all.
team_arg=()
if [ -n "${CLAY_DEVICE_TEAM:-}" ]; then
    team_arg=(DEVELOPMENT_TEAM="$CLAY_DEVICE_TEAM")
fi

set +e
xcodebuild test \
    -project "$PROJECT/ClayCoreDevice.xcodeproj" \
    -scheme ClayCoreDevice \
    -destination "platform=iOS,id=$udid" \
    -resultBundlePath "$RESULTS" \
    -allowProvisioningUpdates \
    ${team_arg[@]+"${team_arg[@]}"}
status=$?
set -e

if [ "$status" -ne 0 ]; then
    echo "device-bench: FAILED (xcodebuild exit $status)" >&2
    echo "  result bundle: $RESULTS" >&2
    exit "$status"
fi

JSON="${CLAY_DEVICE_JSON:-$ROOT/build/device/device-bench.json}"
python3 "$ROOT/tools/collect_device_bench.py" "$RESULTS" "$JSON"

echo "device-bench: OK"
echo "  result bundle: $RESULTS"
