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
      `EXCVADDR: 0x0000004c`, with a backtrace in `ESPHomeOTAComponent::handle_handshake_`,
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
- [ ] `uv run samba flash ota <IP> --bin $B/samba_v2.0.0.ota.bin`, which reports the new build.
- [ ] Onboard WiFi through the captive portal if it comes up as an AP.
- [ ] `uv run samba info <IP> --entities`: production 2.0.0, API not keyed, *InfluxDB Token*
      `unset`, *InfluxDB Status* `no token`, *OTA Password* `unset`, anemometer entities
      `Anemometer 1 [A]…[k]` at the placeholders (A 0, B 9.671, n 1.173, k 0).
- [ ] `uv run samba deploy --mac F8:B3:B7:C7:C4:18`: key, then OTA password, tags, token and
      coefficients, each readback-verified. Air speed coefficients will be absent: there is no
      King's-law fit in processed/ yet (expected).
- [ ] `uv run samba home password <IP>` with the **existing** pairing password (Desktop label).
- [ ] *InfluxDB Status* reads `HTTP 204` within seconds of the token landing.
- [ ] Power-cycle, then `uv run samba status --live --mac F8:B3:B7:C7:C4:18`: same fingerprints,
      encrypted, tags intact.
- [ ] One authenticated OTA: `uv run samba flash ota <IP> --bin $B/samba_v2.0.0.ota.bin`
      (uses the building password and key).

## §9.3 Reflash from 1.x

- [ ] `uv run samba flash serial --erase --bin $B/calibration_v1.13.factory.bin`
- [ ] `uv run samba flash ota <IP> --bin $B/samba_v1.99.99.bin`; onboard WiFi if needed.
- [ ] `uv run samba tag <IP> --building wilkinson --level 4 --zone chamber2`, plus one non-anemometer
      coefficient by hand on the Device page (e.g. Ta slope), and note it.
- [ ] The Device page shows `Air speed 1 [a]…[d1]` (the power-law line), and
      `uv run samba deploy --mac F8:B3:B7:C7:C4:18 --verify` reports every coefficient as
      "power law firmware takes no coefficients".
- [ ] `uv run samba flash ota <IP> --bin $B/samba_v2.0.0.ota.bin`
- [ ] Check: tags survive; the hand-set Ta coefficient survives; the anemometers show the King
      placeholders; the unit is unkeyed, *InfluxDB Token* `unset`, *OTA Password* `unset`.
- [ ] Re-provision (`samba deploy`) and the same pairing password.

## Record

Crash confirmed or refuted (with PC and EXCVADDR); any step that deviated; the fingerprints after
the power cycle. Then gates 9.2 and 9.3 are re-run on the release candidate after §8.
