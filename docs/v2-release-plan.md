# SAMBA firmware 2.0 — release plan

*Drafted 2026-09-23 against main at 4e47cdc (version string 1.99.99, ESPHome 2026.9.0) and
samba_calibration at adb1b0f. Ten units are in the field on the released v1.99.99. The 2.0 line
is for the 100+ units to come.*

2.0 is a deliberate compatibility break, delivered through a second OTA manifest so the field
units never see it. It bundles three things: the anemometer model changes from a power law to
King's law; credential provisioning ships without compiled-in fallbacks; and everything kept only
for 1.x units or old clients goes. The coefficient *values* wait on two lab sessions; the
firmware and client do not.

## 1. Decisions taken

| Question | Decision |
|---|---|
| The 1.x line | Bug fixes only, from a branch off `v1.99.99`; then the ten units are recalled, recalibrated and flashed to 2.0, and the line is retired |
| Scope of the break | King's-law coefficients, the manifest split, credential provisioning without fallbacks, and the 1.x shims listed in §6 |
| Uncalibrated 2.0 unit | Reports air speed from compiled-in fleet-median defaults; the client already tells default from bespoke by value |
| Compiled-in InfluxDB token and OTA password | Dropped. Captive portal and USB remain the recovery paths. No `provisioning:` window |
| The live token and password inside the public v1.99.99 binary | Known exposure, accepted until the ten units are recalled; rotated then |
| `Factory Restore SAMBA` over the API | Removed. A wipe is a USB or lab operation |
| Captive-portal firmware upload | Cannot be closed by config (§5.3); tracked, not a 2.0 blocker |
| Safe-mode crash on main | A release gate, tested on the bench unit before anything ships from main |

## 2. The fork mechanism

The update component fetches `source:` every 12 h, compares the manifest `version` with
`ESPHOME_PROJECT_VERSION`, and installs whenever they differ. `source:` is compiled in and nothing
at run time can change it. So:

- `config/ota.yaml` on the 2.x line: `source: https://github.com/IEQLab/samba/raw/main/firmware/manifest_v2.json`, a literal.
- `firmware/manifest.json` and `firmware/samba_v1.*.bin` stay where they are, unchanged in name,
  read by the 1.x units. `firmware/manifest_v2.json` and `firmware/samba_v2.*.bin` join them.
- A 1.x unit reaches 2.0 only by hand, `samba flash ota` from the lab, after recalibration.

Because a lower manifest version also installs, `manifest.json` must never point at a 2.x
binary. That is the accident `/bump` has to make impossible (§7).

## 3. The 1.x line

Main cannot serve it: every commit after the tag also carries provisioning and 2026.9.0, and
after this plan main drops the compiled-in fallbacks that 1.99.99 units rely on.

- Branch `release/1.x` from `v1.99.99`. Cherry-pick the dropout and watchdog fixes only when a
  fix is actually needed in the field; do not pre-emptively cut a release.
- Versions 1.99.100 and up, built with the ESPHome the tag pins (`min_version: 2026.8.1`), not the
  laptop's current one; a 1.x fix must not also move the toolchain.
- `/bump` as written commits the binary and manifest on the current branch and pushes it, and main
  takes PRs only, so on `release/1.x` the artefacts would land where no unit reads them. The
  procedure is two-branch: bump, build and tag on `release/1.x`; copy `samba_v1.99.1NN.bin` and the
  rewritten `manifest.json` into main's `firmware/` through a worktree and a PR; verify the md5 on
  main before the PR merges. `/bump` refuses to run on a 1.x version until it does this itself (§7).
- The line closes when the last field unit is on 2.0.

## 4. Anemometer calibration in 2.0

### 4.1 The model

Per anemometer, with V the median filtered anemometer voltage and Ta the calibrated air
temperature (22 °C when Ta is NaN, as today):

```
s = 1 − k·Ta
e = V²/s − A
v = (e / B)^(1/n)
```

A is in V², B in V²·(m/s)^−n, n dimensionless, k per °C. k = 0 gives a temperature-free King's
law, so the firmware carries the general form and the fit decides how much temperature it uses.
Evidence for the form: on the September batch and on the March/July runs the fixed-exponent power
law leaves the same concave residual on every tip (+20–35 % at levels 3–5, −25 % at level 1);
King's law per session removes it; the overheat-scaled form with one k per tip cuts in-sample RMSE
0.069 → 0.027 m/s. See samba_calibration `docs/audit-2026-09-23.md` D5 and the memory note on
the air speed model.

### 4.2 Entities and globals

New ids and new names, both deliberately. NVS keys are hashed from the global id, so reusing
`calibration_as1_a` would load a power-law value into King's A on a unit reflashed from 1.x. The
client falls back to matching on object_id, so "Air speed 1 [A]" would collide with "[a]".

| Entity name | id | global | default | range | step |
|---|---|---|---|---|---|
| Anemometer 1 [A] | `cal_as1_king_a` | `calibration_as1_king_a` | fleet median | 0–10 | 0.001 |
| Anemometer 1 [B] | `cal_as1_king_b` | `calibration_as1_king_b` | fleet median | 0.01–20 | 0.001 |
| Anemometer 1 [n] | `cal_as1_king_n` | `calibration_as1_king_n` | fleet median | 0.1–2 | 0.001 |
| Anemometer 1 [k] | `cal_as1_king_k` | `calibration_as1_king_k` | fleet median or 0 | −0.019–0.019 | 0.00001 |
| Anemometer 2 [A] … [k] | `cal_as2_king_*` | `calibration_as2_king_*` | same | same | same |

- `double`, `restore_value: yes`, template numbers with the 60 s update interval, saved through
  `cal_save`, exactly as the existing coefficients.
- The k range keeps `1 − k·Ta` strictly positive below 50 °C (at k = 0.02 it reaches zero at
  exactly 50 °C, hence 0.019). The fitting scripts' ±0.05 bound does not; the fit adopts the
  firmware's range.
- **Open (found 2026-09-23): the September batch does not fit inside that range.** Pooled per
  tip over its three sessions (28.2, 22.5, 18.3 °C), every tip wants k ≈ 0.028–0.031. At the
  0.019 cap k pins on all ten tips and RMSE is 0.14–0.17 m/s, worse than the power law's
  0.060–0.076. With k free, RMSE is 0.021–0.039, but `1 − k·Ta` reaches zero at 33.5 °C, so every
  unit reads NaN above about 33 °C, and with n ≈ 0.4 a shared default is poor (pooled RMSE
  0.26 m/s). A pins at 0 on every tip and is not identified. The §8 sessions decide between
  widening k (with a documented hot-room cutoff) and a different temperature form; the `v2`
  branch carries the structure with placeholder values until then.
- Units agree: the `as*` columns of `raw/thermal.csv` are volts (0.47–2.66 in the September
  batch, filtered 0.3–4.7 in `models.R`), the same quantity as the lambda's `x`.
- The fleet-median defaults are computed from the confirmed batch (§8) and written into
  `globals.yaml` and the client's `FIRMWARE_DEFAULTS` in the same commit. Until then the
  defaults are placeholders marked `TODO(v2)`: the September k = 0 medians (A 0, B 9.671,
  n 1.173, k 0), which read level 1 about 2× high and level 7 about 40 % low.
- The old `[a] [b] [d0] [d1]` entities and their globals are removed outright. Their NVS blobs are
  orphaned on a reflashed unit and read by nothing.

### 4.3 The lambda

Replaces the current `airspeed.yaml` lambda and its trailing `clamp`, which would turn NaN into
0.02:

```cpp
float t = isnan(ta) ? 22.0f : ta;
if (isnan(A) || isnan(B) || B <= 0 || n <= 0) return NAN;
float s = 1.0f - k * t;
if (s <= 0) return NAN;
float e = x * x / s - A;
if (e <= 0) return 0.02f;                     // still air below the fitted floor
float v = powf(e / B, 1.0f / n);
return v < 0.02f ? 0.02f : (v > 1.0f ? 1.0f : v);
```

`samba_airspeed` takes `max(as_1, as_2)`; `std::max` is asymmetric with NaN, so it gets explicit
`isnan` handling: both NaN → NaN, one NaN → the other. `tglobe.yaml` already guards a NaN air speed.

## 5. Credentials in 2.0

### 5.1 What stays

`encryption: {}` with the key provisioned over the native API (one image cannot carry per-building
keys); the `influx_set_token`, `ota_set_password` and `fileserver_set_password` actions, their
FNV-1 fingerprint sensors and `global_preferences->sync()` scripts; NVS persistence; the
captive portal as the WiFi onboarding path; `samba flash serial` with the calibration image, then
`samba flash ota`, as the USB route.

### 5.2 What goes

- The compiled-in InfluxDB token: `token: ""` (the component already accepts it) and delete the
  `compiled-in` status branch in `influxdb.cpp`; "no token" is reported until one is provisioned.
- The compiled-in OTA password: `password: ""` stays in the YAML so `set_auth_password`
  compiles; the "clear reverts to compiled" branch of `ota_set_password` becomes "clear leaves it
  empty".
- The unused wifi, hotspot and enterprise substitutions, the commented `networks` / `manual_ip`
  block in `wifi.yaml`, and their keys in `secrets.yaml.example`.
- `Factory Restore SAMBA`.

Consequence to document: a 2.0 unit that loses NVS loses its API key too. It comes back with a
plaintext API, no token and no OTA password, so until it is redeployed over the LAN it does not
upload, and anyone on its LAN can key it, set its OTA password and flash it. The SD card keeps
logging. This is the same state a factory-fresh unit is in, and the same state every 1.99.99 unit
is in today (§12).

Safe mode is not a recovery path on current main (§9.1): the API server is never constructed
there, and the OTA component dereferences it on `dump_config` and on every OTA handshake, so an
OTA in safe mode crashes the unit rather than being merely unauthenticated. Until that is fixed,
USB is the only way back from safe mode, which the removal of the compiled-in fallbacks does not
change.

### 5.3 The captive-portal upload

In ESPHome 2026.9 `captive_portal` auto-loads `ota.web_server`; the upload form is part of the
portal and no option removes it. It is only reachable when WiFi has failed and the access point is
up. Two ways to close it, neither in 2.0: an upstream option on `captive_portal` to skip the OTA
platform (a small PR, the cleanest), or WiFi onboarding by another route so the portal is not
needed. Tracked as a follow-up.

## 6. 1.x shims and dead code removed

| Item | Where | Cost of removal |
|---|---|---|
| RH slope and offset entities | `calibration.yaml`, `globals.yaml` | none: never applied; one client mapping edit |
| `disabled_by_default` on the 18 numbers | `calibration.yaml` | none |
| The `on_client_connected` coefficient dump | `homeassistant.yaml:13-46` | none; the client reads the entities |
| `home_assistant_domain` in the manifest | `/bump` | none |
| `senseair_i2c.cpp` "keeping for compatibility" method | `components/senseair_i2c` | none if no caller; `samba_calibration/firmware/components/` is a verbatim copy and must be refreshed in the same change |
| README lines on editing lambdas, "no web interface", the archived samba_app | `README.md` | docs only |
| CLAUDE.md on Home Assistant-editable coefficients and the 1.x fallback rollout | `CLAUDE.md` | docs only |

Kept on purpose: the 22 °C fallback when Ta is NaN (physics, not compatibility), the `ap: {}`
fallback access point (onboarding), and the Home Assistant API name, which the client matches on.

## 7. `/bump` and CI

- The manifest is chosen from the version's major number: `manifest.json` for 1.x,
  `manifest_v2.json` for 2.x. Refuse a mismatch. The fleet warning at the top of the skill names
  both manifests and which units read each.
- On a 1.x version the skill either performs the two-branch procedure of §3 or refuses; it never
  commits 1.x artefacts to `release/1.x` alone.
- After the build, grep the binary for the manifest URL and refuse if it is not the one the major
  number implies.
- 1.x artefacts land on main's `firmware/` even when built on `release/1.x`.
- The bench check stays: `samba flash ota`, `samba info`, and now the safe-mode gate of §9.1 on
  the release candidate.
- samba has no CI today. Adding `esphome config` on every push is a scoped task in step 2 of
  §11, not an assumption.
- The version string on main moves to 2.0.0 with the first 2.0 commit, so main's binaries and
  logs stop reading 1.99.99.

## 8. What the coefficient values wait on

Neither of these blocks the firmware or client work; both block writing values to anything that
ships.

1. **In-channel probe sweep.** The Innova 1221 probe at a head position in each tunnel channel,
   stepped through the seven calibration setpoints plus zero, on the day of a SAMBA run. Levels 1
   and 2 of the current reference are extrapolated below the 09-21 sweep, and the still-air
   reading on the King curve reads as 0.06–0.08 m/s; this measurement pins both.
2. **Confirmation session.** Same five units and channels, both tips: a 22 °C pair with the
   Dysons fan-only at speed 3, then a 22 °C pair with them off, then a gated 28 °C pair, every pair
   through the room gate without override and with no lost flush hold; RPMs recorded with a
   no-append sweep. It decides whether the 28 °C gain drop is temperature (k carries it) or air
   movement (k ≈ 0 and the 28 °C session is excluded).

Then, together: `analysis/models.R`, the numpy port, the goldens, the per-level gate, the
population check on A, B, n, k, the fleet-median defaults, and `FIRMWARE_DEFAULTS`.

## 9. Release gates

1. **Safe mode on the bench unit.** *Desk result, 2026-09-23:* the boot survives and the OTA
   crashes. `global_api_server` is null in safe mode (it is set only in the `APIServer`
   constructor, emitted after the early return), and `ota_esphome.cpp:34-36`
   `noise_context_()` dereferences it with no null check. It is reached from `dump_config`,
   which compiles to nothing at `logger: level: INFO`, and from `handle_handshake_` (:310) on
   every client that offers the extended protocol, which `espota2.py` always does. The boot
   counter is already cleared by then, so the crash reboots into normal firmware. The bench
   run is therefore a confirmation. Expect `LoadProhibited`, `EXCVADDR 0x0000004c`, PC in
   `ESPHomeOTAComponent::handle_handshake_`. The fix is upstream: a null guard in
   `noise_context_()` returning a keyless context, so safe mode falls back to the password
   path. Until it ships, captive-portal upload or USB recovers a unit from safe mode.
   Original procedure: on F8:B3:B7:C7:C4:18 running a main build, press
   `Restart SAMBA (Safe Mode)`, wait past the point where `dump_config` runs (a minute), then push
   a `samba flash ota` while it is in safe mode and watch the upload complete and the unit boot
   normally. The reading of `main.cpp` says `APIServer` is constructed after the safe-mode return
   while `ota_esphome` dereferences it in `dump_config` and in the OTA handshake, so either the
   unit crash-loops at boot or the upload crashes it. Whether the boot survives depends on when
   `dump_config` runs, which only hardware tells. If it reproduces: fix or report upstream before
   any release from main; the `/bump` half-wiring gate does not catch it. Recovery from a failed
   test is a USB reflash.
2. **Factory-fresh bench flow** on the bench unit, starting from `samba flash serial --erase`:
   nothing in NVS survives a flash without `--erase`, and the bench unit already holds every
   credential, so without it this tests nothing. Then calibration image → `samba flash ota` to 2.0
   → key → InfluxDB token → OTA password → the file-server pairing password the unit already had
   (the label on the Desktop; never a new one) → coefficients → tags, every step readback-verified
   by `samba deploy`, then a power cycle and a second `samba status --live` showing the same
   fingerprints.
3. **Reflash-from-1.x flow** on the same unit: `--erase`, flash v1.99.99, provision tags (the only
   thing 1.99.99 can take over the API), then `samba flash ota` to 2.0. Confirm the tags and the
   non-anemometer coefficients survive, the anemometers report the defaults, and the unit is
   unkeyed and tokenless until provisioned; re-provision the same pairing password.
4. `esphome config` clean and the calibration repo's suite green; samba's new CI green.
5. The two lab sessions of §8 complete and the coefficient set committed on both sides, then
   gates 2 and 3 re-run on the release candidate, since the defaults and ranges changed after
   the first pass.

## 10. Client changes (samba_calibration)

- `deploy/mapping.py`: `COEFFICIENTS`, `FIRMWARE_DEFAULTS`, `CSV_COEFFICIENTS` and
  `CALIBRATED_COEFFICIENTS` per firmware line, chosen by entity presence alone (a
  `project_version` cross-check only adds a disagreement state). Not a new flavour. Drop the RH
  pair and the below-1.9 aliases. `CALIBRATED_COEFFICIENTS` for the King line is A, B and n; k
  is excluded the way the exponents are today, because if §8 fixes it at zero its value says
  nothing about whether the unit was calibrated.
- `api/client.py`: `capabilities.coefficients` per line rather than all eighteen names.
- `deploy/updates.py`: the coefficient group per line. A v1.99.99 unit takes tags only: it has no
  token, password or key actions and no `encryption:`, so credentials cannot be pushed to it and no
  power-law rows will ever be written again. `has_group` keys on the new names.
- `data/schemas.py`: the air speed processed table becomes `as{1,2}_king_a, _king_b, _king_n,
  _king_k, _samples, _rmse, _rel_max`, matching the entity ids and never differing from the old
  names by case alone. `processed/` does not exist yet, so nothing migrates.
- `models/`: King's-law fit per tip over all its sessions on levels 1–7 (A, B, n, k by nonlinear
  least squares within the firmware ranges), the RMSE and per-level gates, population check on the
  four; `analysis/models.R` first, port second, goldens third.
- `deploy/live.py`: default detection unchanged in mechanism, keyed by line.
- `docs/deployment.md`: the "credential-provisioning branch" paragraph is stale now; the
  compiled-in-fallback warnings go when 1.x closes.
- Tests: `test_live.py` reads defaults from the sibling checkout and will break on the first
  firmware commit; pin it to the line.

## 11. Sequence

1. Bench: the safe-mode gate (§9.1). Half a day, and it decides whether 2.0 starts from main.
2. Firmware branch `v2`: manifest split, version 2.0.0, credential removals, shim removals
   (with the `senseair_i2c` refresh into the calibration repo), King's-law entities with
   provisional defaults, lambda. `esphome config` clean. `/bump` changes and the new CI.
3. Client: mapping, capabilities, updates, schema, live, tests. Suite green against the `v2`
   checkout; the calibration image is unaffected and stays as the unconditional check it is.
4. Bench: the factory-fresh and reflash-from-1.x flows (§9.2–9.3) on the provisional build, to
   shake out the flows themselves.
5. Lab: the probe sweep and the confirmation session (§8). Model, port, goldens, defaults, and
   the ranges if the fit needs them.
6. Bench again on the release candidate (§9.5), then release 2.0.0 through `/bump` to
   `manifest_v2.json`. Tag. The first batch is calibrated and deployed on 2.0.
7. Recall the ten field units over the following batches; rotate the InfluxDB token and the OTA
   password when the last one is in; retire `release/1.x` and `manifest.json`.

## 12. Known exposures during the transition

- The public v1.99.99 binary holds the live InfluxDB token and OTA password until the rotation
  in step 7.
- A 1.99.99 unit is plaintext-API: anyone on its LAN can key it and then flash it without the
  password. A 2.0 unit that loses NVS is in the same state until redeployed.
- The captive-portal upload (§5.3) is unauthenticated in every version. Safe mode is not an OTA
  path at all on current main (§9.1); once fixed, OTA there is unauthenticated plaintext.

## 13. Open items not decided here

- Whether `spl-class2-prep` (LAeq energy average) rides along in 2.0.
- The upstream `captive_portal` option, or a different onboarding route.
- Whether the 2.0 firmware exposes anything for the home app that the manifest split changes
  (`project_version` in the TXT record now reads 2.x).
