---
name: claycore-device-gate
description: Run claycore's iPad performance gate — the hardware check no CI runner can do. Covers the reference device and its UDID, signing, the seven cold sessions, the tree discipline that decides whether a run counts, and how to read REGRESSION / BUDGET / GROWTH. Use before tagging a release, or whenever a change needs a real latency number.
---

# The device gate

Metal is the iPad app's production path and no CI runner has an attached iPad,
so this runs on hardware, by hand, before a tag. **It is not optional and it
does not skip** — a skipped hardware gate and a passing one are
indistinguishable in a log, which is exactly how "Metal is the iPad app's
production path" reached v0.25.0 without a single iPad ever having run it.

Budget **two and a half to three hours**: seven sessions with a 900 s cooldown
between each is 90 minutes of cooling alone, plus the run itself and 30 minutes
of idle iPad before you start. Raise the cooldown and it grows from there.

## Before you start

**Pass the UDID. Never take the default.** This machine usually has several
iPads attached and only the reference device is accepted — a run from any other
model or OS is *refused*, not scored, after a ~10-minute rebuild.

- Reference device: **iPad15,5 (iPad Air 13-inch, M3) on iOS 27.0.1 (24A446)**,
  listed locally as `iPad (52)`, UDID `00008122-000410410A6B801C`. It moved from
  26.5.2 to 27.0 on 2026-09-18 (re-baselined in PR #625) and from 27.0 to
  27.0.1 by 2026-10-04 (re-baselined in PR #690). `check_device_bench.py`
  refuses a run whose `osVersion` differs from the baseline, **and the match is
  the exact string `Version 27.0.1 (Build 24A446)`, so a point release
  invalidates the baseline exactly as a major one does.** Before committing to
  a three-hour run, compare the device against the baseline — it takes a
  second and the refusal otherwise arrives after the last session:

  ```sh
  xcrun devicectl device info details --device <udid> | grep -E "OS Version|OS Build"
  grep osVersion tests/device/baseline.json
  ```

  **Re-baseline from the previous release's engine, not from the release
  being gated.** A baseline written from the release's own run is compared
  against itself, so that release ships with no `REGRESSION` coverage (v0.120.0
  did; only `BUDGET` and `GROWTH`, which are absolute, still gate). The recipe
  that keeps the before/after comparison is two runs: check out the previous
  release's gate commit (the `claycoreCommit` in `last-gate.json`) in a
  separate `git worktree`, run the full gate there on the new OS, write the
  baseline with `--update`, merge that baseline to `main` in its own PR, rebase
  the release branch onto it, and only then gate the release without
  `--update`. `tests/` is not a device-relevant path, so the baseline PR does
  not stale the release's `device` row.
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

**The keychain check is not enough: Xcode also needs a signed-in account.**
On 2026-10-04 `security find-identity -v` listed a valid identity and the run
still failed every target in session 1/7 with "No Accounts: Add a new account
in Accounts settings", "No profiles for 'com.cyberdyne.claycore.devicehost'
were found" and "Signing certificate ... is not valid for code signing. It may
have been revoked or expired" — for a certificate issued five days earlier.
Xcode's account had been signed out (an Xcode update does this), so
`-allowProvisioningUpdates` could not regenerate the bundle's profile and the
certificate could not be validated against the team. Same fix as expiry: an
interactive sign-in in Xcode > Settings > Accounts, which an agent cannot do.
Check before building: `defaults read com.apple.dt.Xcode
DVTDeveloperAccountManagerAppleIDLists` prints an empty list when nobody is
signed in, and the devicehost profile is absent from
`~/Library/Developer/Xcode/UserData/Provisioning Profiles` (it is recreated on
the first signed run).

**Check the iPad's free storage, not just the Mac's.** The same day, with
signing fixed, session 1/7 died at install: "Not enough space for ...
PromiseStaging ... 62401330 bytes needed, 6060104 bytes available (0 free,
6060104 purgable). Insufficient storage." The host is ~60 MB. The reading
that matters is `AmountDataAvailable` from
`ideviceinfo -u <udid> -q com.apple.disk_usage` — `TotalDataAvailable` in the
same output read 92 GB while the install failed, so do not trust it. Freeing
apps on the iPad is the fix; an agent should ask rather than delete.

**The cheapest preflight for both is a direct install of the last built host**,
which takes seconds and fails the same way a session would, before the ~10
minute xcframework rebuild:

```sh
APP=$(ls -d ~/Library/Developer/Xcode/DerivedData/ClayCoreDevice-*/Build/Products/Debug-iphoneos/ClayCoreDeviceHost.app | head -1)
xcrun devicectl device install app --device <udid> "$APP"
xcrun devicectl device uninstall app --device <udid> com.cyberdyne.claycore.devicehost
```

**Check nothing else claimed the bundle id.** A second checkout that declares
the same identifiers loads *its* test bundle and measures a different suite
under this commit's name:

```sh
xcrun devicectl device info apps --device <udid> | grep -i claycore
```

Treat any `abiVersion` disagreement from `collect_device_bench.py` as this.

**The host app must adopt the scene lifecycle.** An app linked against the iOS
26 or later SDK with no `UIApplicationSceneManifest` is terminated during
launch, before the test runner connects: xcodebuild says "Early unexpected exit,
operation never finished bootstrapping", no test runs, and the console names
`_UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption`. On 2026-09-18
that killed session 1/7 twelve seconds in. **There is no thermal event**, which
is what tells it apart from the heat kill below. Fixed in `tests/device/Host`
(`Info.plist`, `project.yml` and `AppDelegate.swift`); if a host is ever
regenerated or replaced, keep the manifest.

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

**It is SEVEN cold `xcodebuild` sessions with a cooldown between each, and it
must not be collapsed back into one.** `run_device_bench.sh` names them 1/7
through 7/7: light verbs, heavy verbs, latency and parity, the gallery, adaptive
topology, the detail pass, and the sustained session last. Both halves of that
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

**There is currently NO continuous temperature signal on the reference
iPad.** `Thermal level changed` in the console is a transition, so by the time
it prints the app is already being killed; a temperature reading was the one
signal that let you act before that, and on iOS 27.0 it is gone.

```sh
idevicediagnostics -u <udid> ioregentry AppleSmartBattery   # had Temperature, centi-degC
```

On iOS 26.5.2 this returned `Temperature` (centi-degC). **Since the move to iOS
27.0 (24A437) it returns no `Temperature` key on this device**, and on 27.0.1
(24A446) it still does not. **VERIFIED on 2026-10-04 that iOS removed the key,
not that the tool fell behind:** `pymobiledevice3` (installed with
`uv tool install pymobiledevice3`) reads the same IORegistry entry through its
own implementation and agrees with libimobiledevice 1.4.0:

```sh
pymobiledevice3 diagnostics ioregistry --udid <udid> --ioclass AppleSmartBattery
pymobiledevice3 diagnostics battery single --udid <udid>
```

Both return the full `AppleSmartBattery` entry (210 keys) with no live
temperature in it. The only temperature-named keys, `AverageBattSkinTemp` and
`AverageBattVirtualTemp`, sit under `DeadBatteryBootData/GeneralPayload` and
read 0 — a boot record, not a reading. The thermistor itself is still listed
(`--ioclass IOHIDEventService` shows `AppleTMP103`, `temperature,tmp103`) but
publishes no value through the lockdown diagnostics service. So what is left is
the console filter below, which only reports the transition, and cooldowns
long enough that you do not need the number. Both tools have to be installed
first — neither was on this Mac on 2026-10-04 (`brew install libimobiledevice`
for `idevicesyslog`; the crash-report and diagnostics commands come with it).

What the reading meant when it worked, kept for when it works again:
`Temperature = 3350` is 33.50 °C. On the reference iPad: lifetime average 24 °C,
lifetime maximum 37.9 °C, ~30 °C idle, and it fell from a session's heat back to
30 °C in about twelve minutes. A gate run that stays under ~33 °C completes.
Parse it with `/usr/bin/python3`, not Homebrew's — the brewed 3.14 has a broken
`pyexpat` and `plistlib` cannot load.

**On iOS 27.0.1 the console is not reachable from the Mac either.** Checked
2026-10-04: `idevicesyslog -u <udid>` prints `[connected]` then
`[disconnected]` and nothing else, and `pymobiledevice3 syslog live` returns
no lines without a privileged tunnel (`sudo pymobiledevice3 remote tunneld`),
which an agent cannot start. So the filter below captures nothing on this OS;
keep it for when it works again, and do not read an empty `gate.thermal` as
"no thermal event".

**What does work is asking the harness.** `VerbLatencyTests
.testEveryVerbOnDevice` samples `ProcessInfo.thermalState` at both ends of
its first case and fails within ~14 s with `thermal state serious -> serious`
on a hot device, or passes in the same 14 s on a cool one. Run only that case
against the already-generated project — no xcframework rebuild — and the
answer costs under a minute of light work, which does not heat the device
enough to matter:

```sh
xcodebuild test -project tests/device/ClayCoreDevice.xcodeproj -scheme ClayCoreDevice \
  -destination "platform=iOS,id=<udid>" -allowProvisioningUpdates DEVELOPMENT_TEAM=2C69VJZSNR \
  -only-testing:ClayCoreDeviceVerbTests/VerbLatencyTests/testEveryVerbOnDevice 2>&1 \
  | grep -E "thermal state|Test Case .* (passed|failed)"
```

On 2026-10-04 the device read `serious` at 17:20 after an afternoon of OS
update, app deletions and an Xcode launch, with no gate session having run,
and `nominal` at 17:52 after thirty idle minutes with the screen off. Probe
before the run rather than discovering the refusal after the ten-minute
rebuild, and probe between attempts rather than guessing a cooldown.

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

**A cold start is not enough on its own; the cooldowns have to out-pace the
ambient.** On 2026-09-07, gating 0.96.0, the iPad began at **26.09 degC** — a
genuinely cold start — and still climbed to 32.5-32.6 degC by session 3 with the
default 900 s cooldowns, then crossed into `Warn` and lost
`testStrokeRefreshInsideAGroup` to the kill above at 21:19:04. Sessions 1 and 2
had passed. So the starting temperature tells you whether session 1 is safe and
nothing more: watch the number BETWEEN sessions (where a probe still returns
one — see above), and if it is not falling back
under ~30 degC, raise `CLAY_DEVICE_COOLDOWN` before session 3 rather than after
the failure.

Reading the probe (iOS 26.5.2): the temperature is nested under the `IORegistry` key, not at
the top level — `plistlib.loads(out)['IORegistry']['Temperature']`. A top-level
read returns `None`, which looks like an unsupported device rather than a wrong
key.

**After an OS update the iPad heats itself, and no cooldown fixes that.** On
2026-10-04/05, the day the reference iPad moved to 27.0.1, four attempts were
heat-killed (the `signal kill` / no JetsamEvent / no crash report signature
above) 36–80 s into a session, each after a 15–30 minute cooldown and a
`nominal` probe, at 18:09, 19:22, 21:29 and 01:02; the heavy-verb bundle
passed exactly once, at 20:58. What the device was doing in between:
`xcrun devicectl device info processes` listed four
`TGOnDeviceInferenceProviderService`, two `ANECompilerService`, two
`AlchemistInferenceProvider`, plus `photoanalysisd`, `mediaanalysisd`,
`siriinferenced` and `knowledgeconstructiond` — Apple Intelligence and photo
analysis on the Neural Engine — and the iPad wrote three JetsamEvents of its
own (20:00, 21:51, 23:38) with nothing of ours running, with
`ANECompilerService` killed on `per-process-limit`. That load keeps the skin
temperature near the `hot condition` Warn threshold, so a session's own work
crosses it inside a minute while `ProcessInfo.thermalState` still says
`nominal`. Check for it before a post-update gate:

```sh
xcrun devicectl device info processes --device <udid> \
  | grep -ciE "InferenceProvider|ANECompiler|photoanalysisd|mediaanalysisd"
idevicecrashreport -u <udid> -e /tmp/crash && ls /tmp/crash | grep JetsamEvent
```

If it is there, the options are to wait for the indexing to finish (it ran for
more than 15 hours here), to turn Wi-Fi off and pause Apple Intelligence on
the device for the run, or both. Low Power Mode is not one: it changes the
clocks and the run would measure a different device.

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
