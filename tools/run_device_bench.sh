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

# --simulator is an explicit opt-in, never a fallback. Its numbers are labelled
# `platform: simulator` in the record and check_device_bench.py refuses them as
# a source for device figures: a simulator runs the HOST's cores, with the
# host's memory and no thermal ceiling, so it answers no question about a
# tablet. It is useful for the RENDERS and for exercising every brush without
# waiting for hardware to cool.
simulator=0
if [ "${1:-}" = "--simulator" ]; then simulator=1; shift; fi

if [ "$simulator" = "1" ]; then
    udid="${1:-${CLAY_SIM_UDID:-}}"
    if [ -z "$udid" ]; then
        udid="$(xcrun simctl list devices available -j 2>/dev/null \
                | python3 -c 'import json,sys
d = json.load(sys.stdin)["devices"]
c = [v["udid"] for rt, rs in sorted(d.items()) for v in rs if "iPad" in v["name"]]
print(c[-1] if c else "")')"
    fi
    if [ -z "$udid" ]; then
        echo "device-bench: no iPad simulator available." >&2
        exit 1
    fi
    echo "device-bench: SIMULATOR $udid — these are NOT device numbers"
    xcrun simctl bootstatus "$udid" -b > /dev/null 2>&1 || xcrun simctl boot "$udid" || true
    DESTINATION="platform=iOS Simulator,id=$udid"
else

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
DESTINATION="platform=iOS,id=$udid"
fi

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

# -- two sessions, each started cold ------------------------------------------
#
# A gate run is TWO xcodebuild sessions rather than one, and the reason is not
# tidiness. The verb bundle cannot follow the latency bundle inside a session:
# sharing its process it is killed by jetsam at 0 s, and in its own process it
# is killed at the heavy tail (mask_extrude, sdf_consolidate, sdf_relax,
# sdf_move and the four authoring verbs) 21 of its 31 cases in — after 30
# minutes of idle, so cooling does not buy it. A process boundary returns that
# process's high-water mark and does nothing about system-level memory
# pressure or heat, which outlive it.
#
# Running the verb bundle FIRST inside one session fixes the kill and breaks
# the other half: the latency cases are the most thermally sensitive here, and
# measured behind the verb bundle they come in at 1.34-2.16x of baselines they
# match to 1.024x when they run cold. Six of them failed a gate with nothing
# wrong with the engine.
#
# So each half gets a cold start. Verb first because it is the fragile one,
# then a cooldown, then the rest. The verb cases were baselined in second
# position and are measured here in a cold one, which can only make them
# faster: median 1.003x over 62 shape-matched points, worst 1.14x, nothing
# near the 1.4x tolerance. That is the one deliberate looseness in this
# arrangement and it is smaller than the run-to-run spread it replaces.
COOLDOWN="${CLAY_DEVICE_COOLDOWN:-900}"
RESULTS_VERB="${RESULTS%.xcresult}.verb.xcresult"
RESULTS_REST="${RESULTS%.xcresult}.rest.xcresult"
rm -rf "$RESULTS_VERB" "$RESULTS_REST"

run_session() {
    # $1 = result bundle, rest = extra xcodebuild args
    local bundle="$1"; shift
    set +e
    xcodebuild test \
        -project "$PROJECT/ClayCoreDevice.xcodeproj" \
        -scheme ClayCoreDevice \
        -destination "$DESTINATION" \
        -resultBundlePath "$bundle" \
        -allowProvisioningUpdates \
        ${team_arg[@]+"${team_arg[@]}"} \
        "$@"
    local st=$?
    set -e
    return "$st"
}

echo "device-bench: session 1/2 — the verb cases, cold"
run_session "$RESULTS_VERB" -only-testing:ClayCoreDeviceVerbTests
status=$?
if [ "$status" -ne 0 ]; then
    echo "device-bench: FAILED in session 1 (xcodebuild exit $status)" >&2
    echo "  result bundle: $RESULTS_VERB" >&2
    exit "$status"
fi

echo "device-bench: cooling ${COOLDOWN}s before session 2"
echo "  A warm device does not fail loudly here — it returns numbers that look"
echo "  like results. Set CLAY_DEVICE_COOLDOWN to change it; do not set it to 0"
echo "  for a run whose numbers you intend to commit."
sleep "$COOLDOWN"

echo "device-bench: session 2/2 — latency, gallery and parity, cold"
run_session "$RESULTS_REST" -skip-testing:ClayCoreDeviceVerbTests
status=$?
if [ "$status" -ne 0 ]; then
    echo "device-bench: FAILED in session 2 (xcodebuild exit $status)" >&2
    echo "  result bundle: $RESULTS_REST" >&2
    exit "$status"
fi

JSON="${CLAY_DEVICE_JSON:-$ROOT/build/device/device-bench.json}"
python3 "$ROOT/tools/collect_device_bench.py" "$RESULTS_VERB" "$RESULTS_REST" "$JSON"

echo "device-bench: OK"
echo "  result bundles: $RESULTS_VERB"
echo "                  $RESULTS_REST"
