# Bench checklist: SAMBA 2.0.0 candidate (samba v2 @ 26cb36d)

Bench unit F8:B3:B7:C7:C4:18 (locations.csv: wilkinson / 4 / chamber2). Run `samba` from the
samba_calibration checkout. Plan: `docs/v2-release-plan.md` §9.1–9.3.

## Setup on the bench laptop

```bash
cd <samba>             && git fetch && git switch v2 && git pull
cd <samba_calibration> && git fetch && git switch v2 && git pull && uv sync --extra dev
B=~/samba-bench && gh release download v2.0.0-bench -R IEQLab/samba -D $B
cd $B && md5 -r *.bin | diff - md5.txt && echo MD5 OK     # Linux: md5sum *.bin | awk '{print $1"  "$2}'
cd <samba_calibration>   # every `uv run samba ...` below runs from here
uv run samba buildings list   # does this laptop hold wilkinson's key, OTA password and token?
```

The binaries come from a **draft** release (collaborators only, no tag, read by no manifest).
Do not rebuild samba for this session: builds are not reproducible, and the stashed `.elf`
files are what decode a crash from these exact binaries.

| File | md5 |
|---|---|
| `samba_v2.0.0.ota.bin` | 59d1b0af52ba0dbdcfe9246064f768c9 |
| `samba_v2.0.0.factory.bin` | 4b5f61ee37c42cd53bf19be9bec61d6b |
| `samba_v1.99.99.bin` (published) | 8d9356725174bbff9655deaa6a2743ca |
| `calibration_v1.13.factory.bin` | see `md5.txt` |

## 0. Before you start

- [ ] **Credentials.** `buildings.csv` records wilkinson's fingerprints (API key a6f02ba9,
      OTA ba978482, token 7c11d363); the home laptop's config.toml holds none of them. If
      `samba buildings list` shows them here, you are set. Otherwise find them (password
      manager: `samba buildings secrets set` / `samba buildings token`) or mint new ones (`samba buildings secrets mint wilkinson`, `samba buildings mint wilkinson`),
      which changes the fingerprints for every wilkinson unit. §9.2 cannot run without them.
- [ ] If the bench unit holds the wilkinson key and you cannot recover it, `samba` cannot reach
      it over the API. Start at §9.2's erase and run §9.1 on the 2.0 build instead, which is
      the build that matters anyway.
- [ ] USB cable and a serial monitor: `uv run samba flash ports`, then keep
      `uv run python -m serial.tools.miniterm /dev/cu.<port> 115200` open. Safe mode has no API,
      so `samba logs` shows nothing there. Close it before any `samba flash serial`.
      A data cable: a charge-only one powers the unit and no port appears.
- [ ] **Opening the port resets the unit** (CP2102N; presetting DTR/RTS does not stop it). Open
      one monitor before a test and leave it open rather than reopening it to look. Never open it
      within 60 s of an OTA boot: the image is only marked valid at `Boot seems successful`, so
      the reset rolls it back (`OTA rollback detected! Rolled back from partition 'app0'`).
- [ ] OTA **from the calibration image** needs the lab OTA password, which config.toml does not
      hold: `export SAMBA_OTA_PASSWORD="$(uv run python -c "import yaml; print(yaml.safe_load(open('firmware/secrets.yaml'))['ota_password'])")"`.
      Without it `samba flash ota` fails with `ESP requests password, but no password given!`.
- [ ] Turn *InfluxDB Upload* off before any token lands if the bench data must stay out of the
      buckets. It persists across reboots and OTA, and the post-token test write honours it.
- [ ] Probably: after every OTA from the calibration image, the unit comes up as an access point
      (`samba-xxxxxx`). The calibration image's WiFi is compiled in, and neither 2.0 nor 1.99.99
      carries networks, so onboard it through the captive portal with a phone.

## §9.1 Safe mode (expected to crash at the OTA handshake)

- [ ] Unit running 2.0 (or main), on WiFi, serial monitor open. Note its IP.
- [ ] `samba ui` → the unit's Device page → *Restart SAMBA (Safe Mode)*.
- [ ] Serial shows `SAFE MODE IS ACTIVE`. Wait over 60 s: **no reboot** means the boot survives.
- [ ] `uv run samba flash ota <IP> --bin $B/samba_v2.0.0.ota.bin --no-verify`
      (`--no-verify` is required: without it the command stops at the API connect.)
- [ ] **Confirmed** if the serial log shows `Guru Meditation Error ... LoadProhibited`,
      `EXCVADDR: 0x00000048` (0x4c was predicted; 0x48 on the bench), with a backtrace in `ESPHomeOTAComponent::handle_handshake_`,
      then the unit reboots into normal firmware. Save the raw `Backtrace:` line; decode it
      wherever an Xtensa toolchain exists (any machine that has built ESPHome):
      `xtensa-esp32-elf-addr2line -pfiaC -e $B/samba_v2.0.0.elf <PC> <addrs...>`
- [ ] **Refuted** if the upload runs to completion. Record which one happened.
- [ ] Captive-portal upload as the recovery route is optional; USB always works:
      `uv run samba flash serial --bin $B/samba_v2.0.0.factory.bin`.
- Confirmed → submit the upstream patch (the `noise_context_()` null guard).

## §9.2 Factory-fresh flow

- [ ] `uv run samba flash serial --erase --bin $B/calibration_v1.13.factory.bin`
- [ ] Unit joins the lab WiFi on the calibration image; `uv run samba info <IP>`.
- [ ] `uv run samba flash ota <IP> --bin $B/samba_v2.0.0.ota.bin` with `SAMBA_OTA_PASSWORD` set
      (§0). The unit comes up as an AP, so the command times out waiting for it: expected.
- [ ] Onboard WiFi through the captive portal if it comes up as an AP.
- [ ] `uv run samba info <IP> --entities`: production 2.0.0, API not keyed, *InfluxDB Token*
      `unset`, *InfluxDB Status* `unknown` (it reads `no token` only once a sample has tried to
      upload), *OTA Password* `unset`, anemometer entities
      `Anemometer 1 [A]…[k]` at the placeholders (A 0, B 9.671, n 1.173, k 0).
- [ ] `uv run samba deploy --mac F8:B3:B7:C7:C4:18 --dry-run`, then without `--dry-run`: key,
      OTA password, tags, token, each readback-verified. **No coefficients**: `data/v2` has no
      `processed/` and the bench unit has no raw calibration rows, so deploy writes credentials
      and tags only (it says "nothing to deploy" if this laptop holds no wilkinson credentials).
- [ ] Coefficient persistence by hand instead: on the Device page, Set `Air temperature slope [m1]`
      to 1.600 and `Anemometer 1 [B]` to 9.000; both should read back.
- [ ] `uv run samba home password <IP>` with the **existing** pairing password (Desktop label).
- [ ] *InfluxDB Status* reads `HTTP 204` within seconds of the token landing.
- [ ] Power-cycle, then `uv run samba status --live --mac F8:B3:B7:C7:C4:18`: same fingerprints,
      encrypted, tags intact, calibration "partly custom" with the two hand-set values kept.
- [ ] One authenticated OTA: `uv run samba flash ota <IP> --bin $B/samba_v2.0.0.ota.bin`
      (uses the building password and key).

## §9.3 Reflash from 1.x

- [ ] `uv run samba flash serial --erase --bin $B/calibration_v1.13.factory.bin`
- [ ] `uv run samba flash ota <IP> --bin $B/samba_v1.99.99.bin`; onboard WiFi if needed.
- [ ] `uv run samba tag <IP> --building wilkinson --level 4 --zone chamber2`, plus one non-anemometer
      coefficient by hand on the Device page (e.g. Ta slope), and note it.
- [ ] The Device page shows `Air speed 1 [a]…[d1]` (the power-law line) and the unit reads
      "firmware defaults" apart from the hand-set Ta slope. (The line's "takes no coefficients"
      refusal is covered by the test suite; with no processed/ there is nothing to refuse here.)
- [ ] `uv run samba flash ota <IP> --bin $B/samba_v2.0.0.ota.bin`
- [ ] Check: tags survive; the hand-set Ta coefficient survives; the anemometers show the King
      placeholders; the unit is unkeyed, *InfluxDB Token* `unset`, *OTA Password* `unset`.
- [ ] Re-provision (`samba deploy`) and the same pairing password.

## Record

Crash confirmed or refuted (with PC and EXCVADDR); any step that deviated; the fingerprints after
the power cycle. Then gates 9.2 and 9.3 are re-run on the release candidate after §8.

### 2026-09-24, bench binaries above (samba 26cb36d), ESPHome 2026.9.0

§9.1 and §9.2 ran on **F8:B3:B7:C7:D0:6C** (tunnel channel 5, just calibrated), not the bench
unit, which was away as a home test unit. It will be deployed at wilkinson / 4 / chamber1.
§9.3 ran on F8:B3:B7:C7:C4:18, re-tagged wilkinson / 4 / researchers_area. *InfluxDB Upload*
was off on both throughout, so no bench data reached the buckets.

**§9.1: confirmed.** The safe-mode boot survived over 60 s. `samba flash ota --no-verify` then
crashed it at the handshake and it rebooted into normal 2.0.0:

```
Guru Meditation Error: Core  1 panic'ed (LoadProhibited). Exception was unhandled.
PC      : 0x400eba65  EXCCAUSE: 0x0000001c  EXCVADDR: 0x00000048
Backtrace: 0x400eba62:0x3ffb68d0 0x400ebb98:0x3ffb6920 0x400ddca1:0x3ffb6940
ELF file SHA256: b9bdcb9f1
```

```
0x400eba62: esphome::noise::NoiseContext::has_psk() const at esphome/components/noise/noise.h:32
 (inlined by) esphome::ESPHomeOTAComponent::handle_handshake_() at esphome/components/esphome/ota/ota_esphome.cpp:310
0x400ebb98: esphome::ESPHomeOTAComponent::loop() at esphome/components/esphome/ota/ota_esphome.cpp:178
0x400ddca1: esphome::loop_task(void*) at esphome/components/esp32/core.cpp:28
```

The desk reading holds: a null context dereferenced in `handle_handshake_`. EXCVADDR is 0x48,
not 0x4c, which is only the offset of the field read. The upstream null guard goes ahead. The
client's automatic retry then reached the rebooted normal firmware, which asked for the
building password it had not been given; that is the retry, not safe mode.

**§9.2: passed, three deviations.**
- The calibration image refused the OTA without its lab password (now in §0).
- *InfluxDB Status* read `unknown`, not `no token`, before the first sample, and `HTTP 204`
  was not checked because uploads were off on purpose.
- `samba home password` was skipped: a building unit has no pairing password.

Deploy wrote 6/6 with readback: API key a6f02ba9, OTA ba978482, token 7c11d363, tags.
After a hard reset the unit was encrypted with the same three fingerprints, the tags, both
hand-set values (Ta slope 1.600, Anemometer 1 [B] 9.000; since restored to 1.506 and 9.671),
and uploads still off. The authenticated OTA ran encrypted with the building password. At the
first sample every measurand reported: Ta 24.8 °C, Tg 24.5 °C, RH 52 %, CO2 1047 ppm, PM2.5
3 µg/m³, 168 lx, LAeq 35.0 dBA, air speed 0.18 m/s on the placeholders. The SGP4x failed every
poll that boot (TVOC and NOx `nan`, the dropout working as designed); after a power cycle it
read TVOC 76 ppb, NOx 1. Bursts of 2 s-cadence I2C timeouts (the ADS1115) appeared in the
first ~40 s of a boot, stopped, and did not cost globe temperature or air speed. Before the
remote was reseated they ran continuously.

**§9.3: passed, on a different starting build.** C4:18 ran a `main` build from 2026-09-20 that
reports 1.99.99 but already carries provisioning, not the published v1.99.99 binary. So the step
tested more of the upgrade and less of the field path. With the wilkinson credentials deployed
on 1.99.99 and Ta slope set to 1.600, `samba flash ota --require-encryption` to 2.0.0 kept:
encryption with key a6f02ba9, OTA ba978482, token 7c11d363, file server 56afa0b7, tags, the
Ta slope, uploads off. `Air speed [a]…[d1]`, the RH pair and *Factory Restore SAMBA* were gone
and `Anemometer [A]…[k]` sat at the placeholders (54 → 51 entities). Ta slope restored to 1.506.
**The published-v1.99.99 path (unkeyed and tokenless after the upgrade) is still untested**; run
§9.3 as written on the release candidate.

Also seen: `manifest_v2.json` returns 404 until the first 2.x `/bump`, so a 2.0 unit logs an
update-check error at boot and every 12 h and does not update itself. Both units stay on 2.0.0.
