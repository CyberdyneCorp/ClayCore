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

# THE GATE NOW SIGNS UNDER TEAM 2C69VJZSNR.
#
#     CLAY_DEVICE_TEAM=2C69VJZSNR tools/run_device_bench.sh <udid>
#
# Worth saying here because the failure is a wall rather than a hint. The team
# this project signed under until 2026-09-02, E9TF4UGU7L, has no unexpired
# certificate left — the last one died at 03:08 UTC that day — and a free
# personal team's certificates lapse yearly, so this will happen again. What
# xcodebuild says when it does is "No Accounts: Add a new account in Accounts
# settings" and "No signing certificate \"iOS Development\" found", neither of
# which mentions expiry, and on one run it segfaulted at GatherProvisioningInputs
# instead of reporting anything. `security find-identity -v -p codesigning` is
# the check that answers it in one line: an identity listed under `-p codesigning`
# but absent from the `-v` output is expired, and the fix is a sign-in in Xcode's
# Accounts settings rather than anything in this repo.
#
# macOS ships bash 3.2, where an empty array expanded under `set -u` is an
# unbound-variable error rather than nothing at all.
team_arg=()
if [ -n "${CLAY_DEVICE_TEAM:-}" ]; then
    team_arg=(DEVELOPMENT_TEAM="$CLAY_DEVICE_TEAM")
fi

# -- six sessions, each started cold ------------------------------------------
#
# A gate run is THREE xcodebuild sessions rather than one, and the reason is not
# tidiness. Every bundle that allocates heavily is killed by jetsam if another
# one ran ahead of it in the same session — the verb cases and the gallery both,
# found one after the other. The verb bundle cannot follow the latency bundle inside a session:
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
# The gallery is the same story one level down: its volume bake at cell_size
# 0.015 costs +75 MB, and behind the latency bundle it is killed too. It holds
# 25 of the 69 gated cases, so it cannot simply be dropped.
#
# So every half gets a cold start: verb, then latency and parity, then the
# gallery, with a cooldown between each. The verb and gallery cases were
# baselined later in a single session and are measured cold here, which can
# only make them faster — for the verb cases, median 1.003x over 62
# shape-matched points, worst 1.14x, nothing near the 1.4x tolerance. That is
# the one deliberate looseness in this arrangement, and it is smaller than the
# run-to-run spread it replaces.
#
# THE RUN NOW TAKES ABOUT FORTY MINUTES. That is the price of numbers that
# mean something on this hardware; it is not a knob to turn down.
COOLDOWN="${CLAY_DEVICE_COOLDOWN:-900}"
RESULTS_VERB="${RESULTS%.xcresult}.verb.xcresult"
RESULTS_VERBH="${RESULTS%.xcresult}.verbheavy.xcresult"
RESULTS_CORE="${RESULTS%.xcresult}.core.xcresult"
RESULTS_GALLERY="${RESULTS%.xcresult}.gallery.xcresult"
RESULTS_DYNTOPO="${RESULTS%.xcresult}.dyntopo.xcresult"
RESULTS_DETAIL="${RESULTS%.xcresult}.detail.xcresult"
rm -rf "$RESULTS_VERB" "$RESULTS_VERBH" "$RESULTS_CORE" "$RESULTS_GALLERY" \
       "$RESULTS_DYNTOPO" "$RESULTS_DETAIL"

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

session() {
    # $1 = label, $2 = result bundle, rest = -only-testing args
    local label="$1" bundle="$2"; shift 2
    echo "device-bench: session $label"
    run_session "$bundle" "$@"
    local st=$?
    if [ "$st" -ne 0 ]; then
        echo "device-bench: FAILED in session $label (xcodebuild exit $st)" >&2
        echo "  result bundle: $bundle" >&2
        exit "$st"
    fi
}

cool() {
    echo "device-bench: cooling ${COOLDOWN}s before the next session"
    sleep "$COOLDOWN"
}

session "1/5 — the light verb cases, cold" "$RESULTS_VERB" \
    -only-testing:ClayCoreDeviceVerbTests
cool
session "2/5 — the heavy verb cases, cold" "$RESULTS_VERBH" \
    -only-testing:ClayCoreDeviceVerbHeavyTests
cool
session "3/5 — latency and parity, cold" "$RESULTS_CORE" \
    -only-testing:ClayCoreDeviceMeasureTests -only-testing:ClayCoreDeviceTests
cool
session "4/5 — the gallery, cold" "$RESULTS_GALLERY" \
    -only-testing:ClayCoreDeviceGalleryTests
cool
# ADAPTIVE TOPOLOGY, LAST. Appended rather than inserted: the 69 committed
# baselines were all taken in the order above, this file measures ordering at
# 2.7x against a 1.4x tolerance, and a new suite has no business moving them.
# The cost is that dyntopo is measured at the warm end of a five-session run,
# which can only make its own figures pessimistic -- the safe direction for a
# suite whose baselines do not exist yet. Move it earlier only together with a
# full re-baseline.
session "5/6 — adaptive topology, cold" "$RESULTS_DYNTOPO" \
    -only-testing:ClayCoreDeviceDyntopoTests
cool
# THE DETAIL CASE, LAST, for the reason dyntopo is second-to-last: its
# baseline does not exist yet, so the warm end of the run is the safe place
# to take it. It needs its own session rather than its own bundle because
# what it costs the run is HEAT -- a whole-form fill at voxel_size 0.01 --
# and a process boundary returns memory, not temperature. Measured: added to
# the latency bundle it took that session from `nominal` to `serious` on both
# sides of an A/B, which marks the run invalid.
session "6/6 — the detail pass, cold" "$RESULTS_DETAIL" \
    -only-testing:ClayCoreDeviceDetailTests

JSON="${CLAY_DEVICE_JSON:-$ROOT/build/device/device-bench.json}"
python3 "$ROOT/tools/collect_device_bench.py" \
    "$RESULTS_VERB" "$RESULTS_VERBH" "$RESULTS_CORE" "$RESULTS_GALLERY" \
    "$RESULTS_DYNTOPO" "$RESULTS_DETAIL" "$JSON"

echo "device-bench: OK"
echo "  result bundles: $RESULTS_VERB"
echo "                  $RESULTS_VERBH"
echo "                  $RESULTS_CORE"
echo "                  $RESULTS_GALLERY"
echo "                  $RESULTS_DYNTOPO"
