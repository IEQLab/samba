# SAMBA firmware 2.0 — release plan

*Drafted 2026-09-23 against main at 4e47cdc (version string 1.99.99, ESPHome 2026.9.0) and
samba_calibration at adb1b0f. Ten units are in the field on the released v1.99.99. The 2.0 line
is for the 100+ units to come.*

2.0 is a deliberate compatibility break, delivered through a second OTA manifest so the field
units never see it. It bundles three things: the anemometer model changes from a power law to
the level-5 fleet model of §4; credential provisioning ships without compiled-in fallbacks; and everything kept only
for 1.x units or old clients goes. The two lab sessions of §8 are done, and the fleet-median
defaults are committed (§4.2).

*Status 2026-10-07:* §11 steps 1 to 5 are done: the defaults are the 15-unit fleet medians of
2026-10-05 (`630cb24`) and no `TODO(v2)` remains. What remains is the release candidate (§9.5),
built from main at or after `9be537c`, the release and the recall. §11 marks each step.

## 1. Decisions taken

| Question | Decision |
|---|---|
| The 1.x line | Bug fixes only, from a branch off `v1.99.99`; then the ten units are recalled, recalibrated and flashed to 2.0, and the line is retired |
| Scope of the break | The air speed model (§4), the manifest split, credential provisioning without fallbacks, and the 1.x shims listed in §6 |
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

*Rewritten 2026-09-25 after the §8 sessions. King's law, the model this section first proposed, is
removed: it is undefined above about 33 °C once k is free, and no unit ever held its coefficients.
The model study and its numbers are in samba_calibration `docs/calibration.md`.*

### 4.1 The model

Per anemometer, with V the median filtered anemometer voltage (volts) and Ta the calibrated air
temperature (22 °C when Ta is NaN, as today):

```
v = exp(K + C1·V + G·(Ta − 22))        clamped to [0.02, 1.0] m/s
```

- `C1` and `G` are **fleet constants**, compiled in as the substitutions `airspeed_c1` (2.9487 V⁻¹)
  and `airspeed_g` (0.2745 °C⁻¹). They are fitted by samba_calibration's `analysis/airspeed_fleet.R`
  and change only with a flash.
- `K` is the **one per-tip coefficient**, measured at level 5 of a calibration run.
- The model has an id, `airspeed_model: "exp2"` (`exp1`, 3.0043 and 0.2321, was fitted before the
  Ta reference was corrected for the tunnel channel's warmer air), published as the `Air Speed Model` text sensor.
  `samba deploy` refuses a `K` fitted under a different id, so refitting the constants means a new
  id here and in the client together.

### 4.2 Entities and globals

| Entity name | id | global | default | range | step |
|---|---|---|---|---|---|
| Anemometer 1 [K] | `cal_as1_k` | `calibration_as1_k` | −6.559 | −12–0 | 0.0001 |
| Anemometer 2 [K] | `cal_as2_k` | `calibration_as2_k` | −6.809 | −12–0 | 0.0001 |
| Air Speed Model (text sensor) | `cal_airspeed_model` | — | `exp2` | — | — |

- `double`, `restore_value: yes`, template numbers with the 60 s update interval, saved through
  `cal_save`, exactly as the existing coefficients.
- New ids on purpose: NVS keys are hashed from the global id, so no power-law or King's-law blob is
  ever read as a `K`. The old entities and globals are removed outright.
- The default is the fleet median of the 14 units that pass exp2 (2026-10-05, roadmap R32),
  replacing the September batch's −6.095 / −6.388. It changes here and in the client's
  `mapping.py` together.
- Units agree: the `as*` columns of `raw/thermal.csv` are volts, the same quantity as the lambda's `x`.

### 4.3 The lambda

```cpp
float ta = id(samba_temperature).state;
if (std::isnan(ta))
  ta = 22.0f;
const float k = id(calibration_as1_k);
if (std::isnan(k))
  return NAN;
const float v = expf(k + ${airspeed_c1}f * x + ${airspeed_g}f * (ta - 22.0f));
return std::min(std::max(v, 0.02f), 1.0f);
```

`x` is at most 5 V, so the exponent stays under about 9 and cannot overflow. `samba_airspeed` takes
`max(as_1, as_2)`; `std::max` is asymmetric with NaN, so it gets explicit `isnan` handling: both
NaN → NaN, one NaN → the other. `tglobe.yaml` already guards a NaN air speed.

## 5. Credentials in 2.0

### 5.1 What stays

`encryption: {}` with the key provisioned over the native API (one image cannot carry per-building
keys); the `influx_set_token`, `ota_set_password` and `fileserver_set_password` actions, their
FNV-1 fingerprint sensors and `global_preferences->sync()` scripts; NVS persistence; the
captive portal as the WiFi onboarding path; `samba flash serial` with the calibration image, then
`samba flash ota`, as the USB route.

### 5.2 What goes

- The compiled-in InfluxDB token: samba's config sets `token: ""`, so "no token" is reported
  until one is provisioned. The component keeps its optional `token:` and the `compiled-in`
  status branch (b942cdf) for open-source builds that upload to their own InfluxDB; a
  provisioned token still takes precedence. The published binaries carry none.
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

Safe mode is a recovery path again from ESPHome 2026.9.1 (§9.1). On 2026.9.0 the OTA component
dereferenced the API server, which safe mode never constructs, and crashed at every handshake.
esphome#19349 guards it and loads the provisioned API key from NVS instead, so a keyed unit in
safe mode takes an OTA encrypted under its building key. The OTA password is still not applied
there (`on_boot` never runs), so a client that declines encryption is not asked for one.

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

*Done 2026-09-25.* The in-channel probe sweep and the confirmation session ran, and the model
study that followed them replaced King's law with the level-5 fleet model (§4). The model, the
exclusions, the gate and the numbers are in samba_calibration `docs/calibration.md`.

## 9. Release gates

1. **Safe mode on the bench unit.** *Fixed upstream in ESPHome 2026.9.1 (esphome#19349) and
   passed on hardware 2026-09-30* on F8:B3:B7:C7:C4:18: normal-mode OTA to the 2026.9.1 build,
   *Restart SAMBA (Safe Mode)*, API port closed and OTA port open for over 90 s, then an OTA
   with encryption required completed (`Encrypted connection established`) and the unit came
   back in normal mode on the new build with key, token, OTA password and tags unchanged.
   `min_version` is 2026.9.1 so no build can regress it.
   *First run, 2026-09-24, ESPHome 2026.9.0* (`docs/v2-bench-checklist.md`, on
   F8:B3:B7:C7:D0:6C): the safe-mode boot survived over
   60 s, and `samba flash ota` crashed it at the handshake with `LoadProhibited`, `EXCVADDR
   0x00000048` (the field offset; the desk reading said 0x4c), in `NoiseContext::has_psk()`
   inlined into `handle_handshake_` (`ota_esphome.cpp:310`). It rebooted into normal firmware.
   *Desk result, 2026-09-23:* the boot survives and the OTA
   crashes. `global_api_server` is null in safe mode (it is set only in the `APIServer`
   constructor, emitted after the early return), and `ota_esphome.cpp:34-36`
   `noise_context_()` dereferences it with no null check. It is reached from `dump_config`,
   which compiles to nothing at `logger: level: INFO`, and from `handle_handshake_` (:310) on
   every client that offers the extended protocol, which `espota2.py` always does. The boot
   counter is already cleared by then, so the crash reboots into normal firmware. The bench
   run is therefore a confirmation. Expect `LoadProhibited`, `EXCVADDR 0x0000004c`, PC in
   `ESPHomeOTAComponent::handle_handshake_`. The fix is upstream: a null guard in
   `noise_context_()` returning a keyless context, so safe mode falls back to the password
   path. (Upstream went further: the context falls back to the key saved in NVS.)
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
5. The release candidate: the `K` default confirmed from the next batch's fleet median and
   committed on both sides (no `TODO(v2)`; *done 2026-10-05*, `630cb24`), then gates 2 and 3 re-run on it, since the air speed
   model, defaults and ranges changed after the 2026-09-24 pass. Gate 3 runs from the
   **published** v1.99.99 binary this time; the first pass started from a main build that already
   carried provisioning. The 24 h free-heap and largest-free-block watch of §13 runs on the same
   build.

## 10. Client changes (samba_calibration)

- `deploy/mapping.py`: `COEFFICIENTS`, `FIRMWARE_DEFAULTS`, `CSV_COEFFICIENTS` and
  `CALIBRATED_COEFFICIENTS` per firmware line, chosen by entity presence alone (a
  `project_version` cross-check only adds a disagreement state). Not a new flavour. Drop the RH
  pair and the below-1.9 aliases. The 2.x line's air speed group is `K` per tip (§4).
- `api/client.py`: `capabilities.coefficients` per line rather than all eighteen names.
- `deploy/updates.py`: the coefficient group per line. A v1.99.99 unit takes tags only: it has no
  token, password or key actions and no `encryption:`, so credentials cannot be pushed to it and no
  power-law rows will ever be written again. `has_group` keys on the new names.
- `data/schemas.py`: the air speed processed table is `as{1,2}_k, _samples, _rmse, _rel_max` and
  `airspeed_model`. `processed/` does not exist yet, so nothing migrates.
- `models/`: `K` per tip from level 5 of the included runs, scored on levels 1–7 against RMSE ≤ 0.20
  and every point within max(25 %, 0.05 m/s); `analysis/models.R` first, port second, goldens third.
- `deploy/live.py`: default detection unchanged in mechanism, keyed by line.
- `docs/deployment.md`: the "credential-provisioning branch" paragraph is stale now; the
  compiled-in-fallback warnings go when 1.x closes.
- Tests: `test_live.py` reads defaults from the sibling checkout and will break on the first
  firmware commit; pin it to the line.

## 11. Sequence

1. *Done 2026-09-24.* Bench: the safe-mode gate (§9.1). Half a day, and it decides whether 2.0
   starts from main.
2. *Done (PR #24, then 60ab521 for the `exp1` model).* Firmware branch `v2`: manifest split, version 2.0.0, credential removals, shim removals
   (with the `senseair_i2c` refresh into the calibration repo), the air speed entities with
   provisional defaults, lambda. `esphome config` clean. `/bump` changes and the new CI.
3. *Done* (samba_calibration `docs/roadmap.md` R26–R28). Client: mapping, capabilities, updates, schema, live, tests. Suite green against the `v2`
   checkout; the calibration image is unaffected and stays as the unconditional check it is.
4. *Done 2026-09-24.* Bench: the factory-fresh and reflash-from-1.x flows (§9.2–9.3) on the provisional build, to
   shake out the flows themselves.
5. *Done 2026-10-05* (`630cb24`, roadmap R30/R32). Lab: the probe
   sweep and the confirmation session (§8). Model, port, goldens, defaults, and
   the ranges if the fit needs them.
6. *Done 2026-10-09* (`f1dda8a`, tag `v2.0.0`, md5 8c68a355…). Bench again on the release candidate (§9.5), then release 2.0.0 through `/bump` to
   `manifest_v2.json`. Tag. The first batch is calibrated and deployed on 2.0.
7. Recall the ten field units over the following batches; rotate the InfluxDB token and the OTA
   password when the last one is in; retire `release/1.x` and `manifest.json`.

## 12. Known exposures during the transition

- The public v1.99.99 binary holds the live InfluxDB token and OTA password until the rotation
  in step 7.
- A 1.99.99 unit is plaintext-API: anyone on its LAN can key it and then flash it without the
  password. A 2.0 unit that loses NVS is in the same state until redeployed.
- The captive-portal upload (§5.3) is unauthenticated in every version. Safe mode is an OTA path
  from 2026.9.1 (§9.1): encrypted under the building key when the client offers it, but with no
  password, so a client that declines encryption can flash a unit in safe mode.

## 13. Open items not decided here

- *Decided 2026-09-24:* `spl-class2-prep` rides along in 2.0. `la_eq` becomes a true energy
  average (it was L50) and LA90/LA10 move to 125ms blocks, so all three shift at the 2.0 boundary;
  2.0 demarcates it in the data. As first merged it crash-looped a bench unit at boot: the two
  2400-value `quantile` filters held 19.2KB of heap and copied a 9.6KB window on every output,
  and the TLS firmware check ~10s after WiFi then ran out (lowest free heap 0.4KB, against
  6-16KB before). `sound_level_meter` now computes all three itself (`type: stats`): 125ms
  levels as uint16 deci-dB in one 4.8KB ring plus a 2.8KB 0.1dB histogram, allocated once in
  setup, with nothing allocated per block or per output. The §9.5 bench run still watches free
  heap and largest free block over 24h.
- The upstream `captive_portal` option, or a different onboarding route.
- Whether the 2.0 firmware exposes anything for the home app that the manifest split changes
  (`project_version` in the TXT record now reads 2.x).
