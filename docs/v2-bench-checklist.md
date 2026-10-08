# Bench checklist: SAMBA 2.0.0 release candidate (samba main @ `5a0d102`)

Bench unit F8:B3:B7:C7:C4:18 (locations.csv: wilkinson / 4 / bench). Run `samba` from the
samba_calibration checkout. Plan: `docs/v2-release-plan.md` §9.1–9.3 and §9.5. The candidate
is built from main at or after `9be537c` (no `f_getfree` after mount). It reports build time
2026-10-07 14:58:37: ESPHome only renews that stamp when the config hash changes, and nothing
after that build touched the config. Built 2026-10-09; binaries and `.elf` in `~/samba-rc/2.0.0-rc1-5a0d102`.

## Setup on the bench laptop

```bash
cd <samba>             && git fetch && git switch main && git pull
cd <samba_calibration> && git fetch && git switch main && git pull && uv sync --extra dev
B=~/samba-bench-rc && gh release download v2.0.0-rc1 -R IEQLab/samba -D $B
cd $B && md5 -r *.bin | diff - md5.txt && echo MD5 OK     # Linux: md5sum *.bin | awk '{print $1"  "$2}'
cd <samba_calibration>   # every `uv run samba ...` below runs from here
uv run samba buildings list   # does this laptop hold wilkinson's key, OTA password and token?
```

The binaries come from a **draft** release (collaborators only, no tag, read by no manifest).
Do not rebuild samba for this session: builds are not reproducible, and the stashed `.elf`
files are what decode a crash from these exact binaries.

| File | md5 |
|---|---|
| `samba_v2.0.0.ota.bin` | 8c68a355264c1c35d45a57460c19ae86 |
| `samba_v2.0.0.factory.bin` | ade04b3a14cb0fa660ab4839b6ca9e74 |
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

## §9.1 Safe mode (crashed on 2026.9.0; passes on 2026.9.1)

- [ ] Unit running 2.0 (or main), on WiFi, serial monitor open. Note its IP.
- [ ] `samba ui` → the unit's Device page → *Restart SAMBA (Safe Mode)*.
- [ ] Serial shows `SAFE MODE IS ACTIVE`. Wait over 60 s: **no reboot** means the boot survives.
- [ ] `uv run samba flash ota <IP> --bin $B/samba_v2.0.0.ota.bin --no-verify --require-encryption --building wilkinson`
      (`--no-verify` is required: without it the command stops at the API connect. `--building`
      supplies the key and password no session can name; samba_calibration 67cac74 and later.)
- [ ] **Passes** if the upload runs to completion and the unit comes back in normal mode on the
      pushed build (`samba info`). Safe mode can be told from normal mode without serial: port
      6053 (API) closed, 3232 (OTA) open.
- [ ] A `Guru Meditation Error ... LoadProhibited` at the handshake (`EXCVADDR: 0x00000048`,
      backtrace in `ESPHomeOTAComponent::handle_handshake_`) is the 2026.9.0 bug: the binary was
      built with ESPHome older than 2026.9.1. Save the raw `Backtrace:` line and decode it with
      `xtensa-esp32-elf-addr2line -pfiaC -e <elf> <PC> <addrs...>`.
- [ ] Captive-portal upload as the recovery route is optional; USB always works:
      `uv run samba flash serial --bin $B/samba_v2.0.0.factory.bin`.

## §9.2 Factory-fresh flow

- [ ] `uv run samba flash serial --erase --bin $B/calibration_v1.13.factory.bin`
- [ ] Unit joins the lab WiFi on the calibration image; `uv run samba info <IP>`.
- [ ] `uv run samba flash ota <IP> --bin $B/samba_v2.0.0.ota.bin` with `SAMBA_OTA_PASSWORD` set
      (§0). The unit comes up as an AP, so the command times out waiting for it: expected.
- [ ] Onboard WiFi through the captive portal if it comes up as an AP.
- [ ] `uv run samba info <IP> --entities`: production 2.0.0, API not keyed, *InfluxDB Token*
      `unset`, *InfluxDB Status* `unknown` (it reads `no token` only once a sample has tried to
      upload), *OTA Password* `unset`, `Anemometer 1 [K]` −6.559 and `Anemometer 2 [K]` −6.809
      (the fleet defaults), *Air Speed Model* `exp2`.
- [ ] `uv run samba deploy --mac F8:B3:B7:C7:C4:18 --dry-run`, then without `--dry-run`: key,
      OTA password, tags, token, each readback-verified, and C4:18's coefficients if it has a
      processed row in `data/v2`; without one it writes credentials and tags only until R33 lands.
      Note which happened. (It says "nothing to deploy" if this laptop holds no wilkinson credentials.)
- [ ] Coefficient persistence by hand: on the Device page, Set `Air temperature slope [m1]`
      to 1.600 and `Anemometer 1 [K]` to −6.000; both should read back. Note the values they
      replaced, to restore afterwards.
- [ ] `uv run samba home password <IP>` with the **existing** pairing password (Desktop label).
- [ ] *InfluxDB Status* reads `HTTP 204` within seconds of the token landing.
- [ ] Power-cycle, then `uv run samba status --live --mac F8:B3:B7:C7:C4:18`: same fingerprints,
      encrypted, tags intact, calibration "partly custom" with the two hand-set values kept.
- [ ] One authenticated OTA: `uv run samba flash ota <IP> --bin $B/samba_v2.0.0.ota.bin`
      (uses the building password and key).

## §9.3 Reflash from 1.x

- [ ] `uv run samba flash serial --erase --bin $B/calibration_v1.13.factory.bin`
- [ ] `uv run samba flash ota <IP> --bin $B/samba_v1.99.99.bin`; onboard WiFi if needed.
- [ ] `uv run samba tag <IP> --building wilkinson --level 4 --zone bench`, plus one non-anemometer
      coefficient by hand on the Device page (e.g. Ta slope), and note it.
- [ ] The Device page shows `Air speed 1 [a]…[d1]` (the power-law line) and the unit reads
      "firmware defaults" apart from the hand-set Ta slope. (The line's "takes no coefficients"
      refusal is covered by the test suite; with no processed/ there is nothing to refuse here.)
- [ ] `uv run samba flash ota <IP> --bin $B/samba_v2.0.0.ota.bin`
- [ ] Check: tags survive; the hand-set Ta coefficient survives; the anemometers show the
      default `K` (−6.559 / −6.809) and `Air Speed Model` reads `exp2`; the unit is unkeyed, *InfluxDB Token* `unset`, *OTA Password* `unset`.
- [ ] Re-provision (`samba deploy`) and the same pairing password.

## §9.5 24 h heap watch

- [x] **Method:** a heap variant, `samba.yaml` plus ESPHome `debug:` sensors (*Heap Free*,
      *Heap Largest Block*, *Heap Min Free*, 60 s) as API entities only, polled from a laptop every
      5 min. Production 2.0 has no heap sensors (roadmap R45), so this is not the candidate binary.
- [x] Run 24 h with uploads on, so the TLS upload and the 12 h firmware check both run. Run on the
      three production units instead of C4:18, and cut to ~19.6 h (record below).
- [x] **Passes** if there is no reboot, the lowest free heap stays at or above the 6 KB floor seen
      before `spl-class2-prep` (plan §13; the crash-looping build reached 0.4 KB), and the largest free block does not trend down across the day.

## Record

Crash confirmed or refuted (with PC and EXCVADDR); any step that deviated; the fingerprints after
the power cycle; the heap minimum and largest-block minimum over 24 h. On a pass, `/bump 2.0.0 --tag --no-compile`
in the build directory the candidate came from, so the released binary is the benched one; builds are not reproducible.

### Release candidate, 2026-10-09, samba `5a0d102`, ESPHome 2026.9.1

*§9.2 and §9.3 not run yet.* Flashed OTA, encrypted, to chamber2 (6A:78) and researchers_area
(C8:70) on 2026-10-09; chamber1 (CC:84) was off the network (no ARP) and is not yet flashed.

### 2026-10-07/08, §9.5 heap watch: heap variant of samba 81bdf65, ESPHome 2026.9.1

**Passed, on a shortened run.** Heap variant (`samba_heap.yaml`, built 2026-10-07 15:44:18, md5
`7a69caae4c5782a3c052f8f17c6fc728`) flashed OTA, encrypted, to the three production units in
place at wilkinson / 4, uploads on: chamber1 (CC:84) 15:47, chamber2 (6A:78) 15:58,
researchers_area (C8:70) 16:05 AEDT. Polled every 5 min to 2026-10-08 11:40 AEDT, ~19.6 h from
the last flash, and stopped there because the logging laptop had to leave the network, not
because of a fault. That is not the full 24 h, and it caught one 12 h firmware check, not two.

| Unit | Polls | Uptime at end | Lowest free | *Heap Min Free* | Largest block |
|---|---|---|---|---|---|
| chamber1 | 239 | 71490 s | 123948 B | 77124 B | 45056 B every poll |
| chamber2 | 237 | 70796 s | 122912 B | 76944 B | 45056 B (81920 B before the first upload) |
| researchers_area | 236 | 70630 s | 122844 B | 76844 B | 45056 B every poll |

No reboot (uptime never fell), no failed poll. *Heap Min Free* sits ~13x above the 6 KB floor,
against 0.4 KB on the crash-looping `spl-class2-prep` build. It stepped down ~1.2 KB once per unit
about 12 h after boot (the TLS firmware check, which returns 404 until 2.0.0 is released) and not
otherwise. The largest block was flat all night, so no fragmentation trend. The three units still
run the variant, reporting 2.0.0, and take the tagged build with the R35 reflash.

### 2026-09-24, `v2.0.0-bench` draft binaries (samba 26cb36d), ESPHome 2026.9.0

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

### 2026-09-30, main + `min_version: 2026.9.1` (build 2026-09-30 14:25:32), ESPHome 2026.9.1

**§9.1: passed.** On F8:B3:B7:C7:C4:18 (wilkinson / 4 / bench, uploads off), over WiFi only, no
serial monitor. `samba flash ota --require-encryption` from 2.0.0 on 2026.9.0 verified encrypted.
*Restart SAMBA (Safe Mode)* pressed over the API; for 90 s port 6053 stayed closed and 3232
open, so the unit was in safe mode and stayed up. Pushed the same `.ota.bin` with `push_ota`
directly (`samba flash ota --no-verify` has no API session, so no building key, and refuses
`--require-encryption`), wilkinson key, encryption required: `Encrypted connection established`,
`OTA successful`. The unit came back on 6053 and `samba info` read esphome 2026.9.1, the new
build, key a6f02ba9, OTA ba978482, token 7c11d363, tags and uploads unchanged. The encrypted
session in safe mode is the proof the key came from NVS: there is no API server to borrow it
from. Not tested: a plaintext client in safe mode, which by the code is not asked for a password.
Repeated the same day with the CLI once it gained `--building` (samba_calibration 67cac74):
`samba flash ota --no-verify --require-encryption --building wilkinson` in safe mode, encrypted,
unit back on the new build.
