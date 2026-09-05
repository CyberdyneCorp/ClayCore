---
name: claycore-device-gate
description: Run claycore's iPad performance gate — the hardware check no CI runner can do. Covers the reference device and its UDID, signing, the four cold sessions, the tree discipline that decides whether a run counts, and how to read REGRESSION / BUDGET / GROWTH. Use before tagging a release, or whenever a change needs a real latency number.
---

# The device gate

Metal is the iPad app's production path and no CI runner has an attached iPad,
so this runs on hardware, by hand, before a tag. **It is not optional and it
does not skip** — a skipped hardware gate and a passing one are
indistinguishable in a log, which is exactly how "Metal is the iPad app's
production path" reached v0.25.0 without a single iPad ever having run it.

Budget **an hour**: ~40 minutes of run, plus 30 minutes of idle iPad first.

## Before you start

**Pass the UDID. Never take the default.** This machine usually has several
iPads attached and only the reference device is accepted — a run from any other
model or OS is *refused*, not scored, after a ~10-minute rebuild.

- Reference device: **iPad15,5 (iPad Air 13-inch, M3) on iOS 26.5.2**, listed
  locally as `iPad (52)`, UDID `00008122-000410410A6B801C`.
- Confirm it is above the `== Devices Offline ==` line:
  `xcrun xctrace list devices`

**Signing.** Team `2C69VJZSNR`, passed explicitly:

```sh
CLAY_DEVICE_TEAM=2C69VJZSNR tools/run_device_bench.sh 00008122-000410410A6B801C
```

Read the certificate **inside the profile whose name matches the bundle**, not
the first row of the listing — the wildcard profile on the same team still
carries a certificate that expired 2026-09-02, while
`com.cyberdyne.claycore.devicehost` carries one valid to 2027-09-02. The listing
loop is in `docs/RELEASE.md` under "Prerequisites".

An expired identity does not say so: `xcodebuild` reports "No Accounts" and "No
signing certificate found", and has once segfaulted at
`GatherProvisioningInputs`. One-line diagnostic — an identity that appears under
`security find-identity -p codesigning` but **not** under `-v` is expired. The
fix is an interactive Xcode sign-in, which an agent cannot do: ask. Then
uninstall the stale host, which was signed by the dead identity:

```sh
xcrun devicectl device uninstall app --device <udid> com.cyberdyne.claycore.devicehost
```

**Check nothing else claimed the bundle id.** A second checkout that declares
the same identifiers loads *its* test bundle and measures a different suite
under this commit's name:

```sh
xcrun devicectl device info apps --device <udid> | grep -i claycore
```

Treat any `abiVersion` disagreement from `collect_device_bench.py` as this.

**`xcodegen`** must be installed (`brew install xcodegen`); the Xcode project is
generated from `tests/device/project.yml` and is not committed.

## The tree discipline (this is what wastes runs)

- **Every `src/`, `include/`, `backends/`, `bindings/`, `CMakeLists.txt` edit
  must be done and committed BEFORE the run.** `release_check.py` fails the
  device row for any change under those paths since the gate ran — a header
  comment counts, because it diffs paths and not semantics.
- **Touch nothing at all while it runs.** `collect_device_bench.py` computes
  `treeDirty` from `git status --porcelain` at *collection* time and bakes it
  into the record. Re-running the checker cannot repair it, and the checklist
  fails a dirty stamp. An unrelated docs edit mid-run costs a full re-run.
- `git restore tests/device/last-gate.json` before starting if it is locally
  modified, so the stamp records `treeDirty: false`.
- Catching a stray edit early is cheap: killing the run 8 minutes in and
  restarting costs almost nothing. Discovering it at the end costs the hour.

## Running it

```sh
CLAY_DEVICE_TEAM=2C69VJZSNR tools/run_device_bench.sh 00008122-000410410A6B801C
python3 tools/check_device_bench.py build/device/device-bench.json
python3 tools/check_device_coverage.py build/device/device-bench.json
cp build/device/device-bench.json build/device/runs/gate-v<X><Y><Z>.json   # keep it
git add tests/device/last-gate.json && git commit -m "Record the device gate for vX.Y.Z"
```

**It is FOUR (five with dyntopo) cold `xcodebuild` sessions with a cooldown
between each, and it must not be collapsed back into one.** Both halves of that
are measured, not chosen:

- A jetsam kill is about the **peak**, not the schedule. The verb bundle died
  after 25 minutes of idle and again after 40, having passed twice at 30.
  Cooling is a coin flip that costs an hour to toss; splitting the bundle at
  `mask_extrude` is what fixed it.
- **Ordering is thermal and measured.** The latency cases are the most
  thermally sensitive suite here: 1.00–1.15x of baseline running first,
  **1.34–2.16x running second**, which fails six cases with nothing wrong with
  the engine.

`CLAY_DEVICE_COOLDOWN` sets the gap (default 900 s). **Never set it to 0 for
numbers you intend to commit** — a warm device does not fail loudly, it returns
numbers that look like results. And give the iPad half an hour before starting:
four runs inside half an hour will fail the verb bundle whatever the ordering.

`run_device_bench.sh` rebuilds the xcframework first rather than trusting what
is on disk; the Swift smoke consumes the prebuilt artifact and has been caught
passing against a stale one.

**Keep the run record.** `collect_device_bench.py` overwrites
`build/device/device-bench.json` every run, and the *next* release wants this
one to compute a real release-to-release delta rather than only a
comparison against committed baselines.

## Reading a result

Each case reports p50 and p95 at three document sizes plus a `growthExponent`
(0 flat, 1 linear, 2 quadratic).

| Failure | Means |
|---|---|
| `REGRESSION` | slower than the committed baseline by more than tolerance |
| `BUDGET` | slower than the interaction class allows, regressed or not |
| `GROWTH` | scaling faster than the document (over `N^1.25`) |

## `signal kill` has two causes and they need opposite fixes

**Read RunningBoard's reason before assuming jetsam.** Found on 2026-09-05
gating v0.84.0, after an afternoon spent fixing the wrong thing.

The heavy verb bundle died 75-100 s in on six runs -- at HEAD, after thirty
minutes idle, after a full reboot, at the commit the previous gate passed at,
with the case alone in its own bundle, and with a 1800 s execution allowance.
None of those was the cause. The console was:

```
SpringBoard:   hot condition changed from 0 to 20
SpringBoard:   Thermal level changed to Warn (1)
runningboardd: Acquiring assertion targeting system ... "Thermal Condition"
runningboardd: [app<...devicehost>:706] Terminating with context:
  <RBSTerminateContext| code:0x05CA1DED explanation:Conditions changed, forcing
  termination due to outstanding assertion ... 'Developer testing'
  reportType:None ...>
```

Crossing into thermal `Warn` makes RunningBoard force-terminate the app holding
the `Developer testing` assertion. `reportType:None` means **no crash report and
no JetsamEvent** — both places you would look to confirm a memory kill are
empty, which is the tell.

Tell them apart, cheapest first:

```sh
idevicecrashreport -u <udid> -e /tmp/crash    # a memory kill leaves JetsamEvent-*.ips
idevicesyslog -u <udid> > /tmp/log            # across the run; grep the three lines above
```

and measure the footprint on the **simulator**, which is a fair proxy for memory
and none at all for heat (`mask_extrude` peaks at 313 MB and the bundle
completes there in 158 s).

**Do not reason from repeatability.** A constant workload from a similar
starting temperature crosses the threshold at the same second — three of those
kills landed within one second of each other, which is exactly what argued
against heat until the console said otherwise.

**The harness's own thermal guard cannot catch this**: `ProcessInfo.thermalState`
is sampled at case boundaries, and the OS kills the process before the boundary
arrives. It covers a run measured while warm, not one ended for being warm.

## Watching the device while it runs

**Temperature is readable over the wire**, and it is the only *continuous*
signal — `Thermal level changed` in the console is a transition, so by the time
it prints the app is already being killed.

```sh
idevicediagnostics -u <udid> ioregentry AppleSmartBattery   # Temperature, centi-degC
```

`Temperature = 3350` is 33.50 °C. On the reference iPad: lifetime average 24 °C,
lifetime maximum 37.9 °C, ~30 °C idle, and it fell from a session's heat back to
30 °C in about twelve minutes. A gate run that stays under ~33 °C completes.
Parse it with `/usr/bin/python3`, not Homebrew's — the brewed 3.14 has a broken
`pyexpat` and `plistlib` cannot load.

**Do NOT redirect a full `idevicesyslog` into a file for a long run.** It writes
~1.1 GB an hour, and on 2026-09-05 that filled the disk and killed the gallery
session with "No space left on device" — a *host* failure that looks nothing
like one in the xcodebuild output. Pipe it through a filter so only the lines
that matter land:

```sh
idevicesyslog -u <udid> 2>/dev/null \
  | grep --line-buffered -E "hot condition changed|Thermal level changed|Terminating with context" \
  > gate.thermal &
```

**Check free disk before starting.** The whole gate needs little (~70 MB of
result bundles), but Xcode's `iOS DeviceSupport` grows ~5.5 GB per device-and-OS
ever attached and is the usual reason a dev Mac has nothing left.

**The fix is cooling, not splitting.** This device took ~12 minutes to fall from
`Warn` to level 0 after one session. Give it a genuinely cold start, raise
`CLAY_DEVICE_COOLDOWN` above 900 s, run it somewhere cool, and never stack
attempts. Splitting the bundle is the fix for the *jetsam* kill and does nothing
here — the case alone in its own process died identically.

Two **refusals**, which are not scores: a run from different hardware, and a
thermally throttled run (`ProcessInfo` thermal state sampled at both ends;
anything but `nominal` invalidates it). Let it cool rather than reaching for the
tolerance.

The checker also names **which** cases were measured while the canary had
drifted, not merely that it drifted — a bundle that spikes on its last sample is
a run where the last few cases are suspect, not all of them.

**A case below 0.125 ms cannot gate**, however stable it is: the check requires
the absolute difference to clear a 0.05 ms noise floor as well as the tolerance,
so a case gates only above `NOISE_FLOOR_MS / (tolerance - 1)`. A regression in a
0.041 ms case is never 0.05 ms. `check_device_coverage.py` prints which cases
are GATED and which are REPORTED ONLY. **A perf win can push its own case under
that floor and switch off the gate protecting it** — size a case to a comfortable
multiple of 0.125 ms, and where a case times a batch, record the batch count so
the figure stays a statement about the verb.

Gallery cases are scored on the **median** of their passes, not the worst: they
time each stroke of a progressive sculpt once, and the max of N one-shot draws is
the worst draw. Two records at the same commit once read 0.149 and 0.434 and
cost a four-merge bisect.

**Simulator and Mac numbers are never device numbers.** The `metal` CMake preset
on a Mac answers "does the Metal backend agree"; it answers nothing about
latency.

## Adding a case

Probe it with a one-stamp host replay first — an hour of gate time will not find
a fixture defect that a replay finds in a minute. A case must actually exercise
the path it names: a brick-refill case needs a stroke along a path, a latency
case that resets every iteration cannot reach the append path, and a batched
case that saturates memory dies to jetsam rather than reporting.
