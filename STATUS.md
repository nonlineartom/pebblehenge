# Pebblehenge — current status (paused)

## TL;DR for the next session

App runs on a real Pebble Time 2. The sun-dome arc, sunrise / sunset
markers, GPS recency indicator and tilt-aware horizon all behave. The
heading-aware rotation of the arc is the one open bug: the sun stays
at screen centre regardless of which way the user faces.

A diagnostic-build (commit `84292ee`) was loaded onto the watch. Its
top-right header now shows the compass status + heading the OS is
delivering. **Waiting on the user to report what that diagnostic says
before we know which compass bug we're dealing with.**

## Where to pick up the heading-rotation thread

| What the diagnostic reads | Diagnosis | Fix |
|---|---|---|
| `K###` and the number rotates as the user turns | Status reaches `Calibrated` and we're getting heading data — the bug is downstream of `ui_arc_set_heading` | Trace `s_view_center_deg` propagation |
| `C###` and the number rotates as the user turns | `Calibrating` works; we already accept it as of `edcf128` — confirm the arc visually rotates with the number |
| `I---` always, never changes | OS isn't delivering heading on this device | Implement an IMU-only heading fallback (we already subscribe to the accelerometer, the magnetometer side is the work) |
| `?---` always | The status field has a value my switch doesn't recognise — see what number it is on the watch | Add the case to `format_status_buf` and `on_compass_sample` |
| No compass text at all (just `*5m`) | `on_heading` callback never fires | `compass_service_subscribe` may be a stub on emery; check rePebble SDK version notes |

## Phases complete

1. NOAA sun position port + 13 host tests
2. Text-only watchapp
3. Phone GPS + magnetic-declination + persistence
4. Compass overlay (bearing-to-sun arrow)
5. Heading-aware sky-dome arc projection
6. Up/Down timeline scrub + app glance
7. Accelerometer tilt compensation (horizon slides with watch pitch)

Plus the post-7 polish pass: AZ/ALT off by default, on-arc hour ticks,
GPS-recency dot, sample density bumped to 5 min, anti-aliased lines
on emery.

## Known limitations carried over

- **Emery emulator** — `coredevices/qemu pebble-10.1` doesn't boot the
  F7xx Robert machine yet; `test/preview_emery.py` is the visual proxy
- **`configurable` capability** is declared in `package.json` but there's
  no settings page wired up. Tapping "Settings" in the Pebble phone app
  for this app may do nothing or error. Either build the Clay page or
  drop the capability before shipping more widely.
- **Magnetic declination** is fetched from NOAA WMM in `pkjs/index.js`
  with a 30-day cache; no offline static table yet
- **No app icon** — the app glance uses fallback

## Pending work for next session

1. Resolve heading-rotation bug (see decision table above)
2. **Phase 8** — roll compensation. Rotate the (off, alt) → (x, y)
   projection by `-roll_deg` so the arc stays vertical when the user
   twists the watch around the band axis. The `pbh_attitude_t.roll_deg`
   field is already computed in `src/c/imu.c`; just needs to be passed
   to `ui_arc.c` and applied as a 2D rotation in the projection.
3. **Clay settings page** in `src/pkjs/config.js` — manual lat/lon
   override, theme toggle, scrub step size
4. **Static WMM declination fallback** in `pkjs/index.js` for offline
5. **App glance icon** — a 24×24 sun for the launcher
6. Re-evaluate whether to keep the diagnostic in the header or move it
   behind a hidden long-press toggle once the heading bug is fixed

## How to build

All builds now go through `scripts/build.sh`. It:

1. Stamps the current git short hash (plus `+dirty` if working tree
   isn't clean) into `package.json`'s `pebble.versionLabel`
2. Runs `pebble clean && pebble build`
3. Copies the bundle to `build/pebblehenge-<versionLabel>.pbw`
4. Reverts `package.json` so the working tree stays clean

The versionLabel is visible in the Pebble phone app under the installed
app's details, so any .pbw the user has can be cross-referenced to a
git commit unambiguously.

```sh
source ~/pebble-venv/bin/activate
./scripts/build.sh
# -> build/pebblehenge-0.2-edcf128.pbw   (for example)
```

Bump the base version in `package.json`'s `pebble.versionLabel` when
crossing a phase boundary; the script preserves whatever prefix is
there and appends `-<hash>`.

## Re-entry checklist

```sh
git fetch origin claude/pebble-time-2-research-EFVOn
git checkout claude/pebble-time-2-research-EFVOn

# Host-side sun algorithm tests
make -C test test            # expect: SUCCESS: 0 failure(s)

# Produce a fresh versioned bundle
source ~/pebble-venv/bin/activate
./scripts/build.sh
```
