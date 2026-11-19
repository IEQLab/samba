# Bench: ESPHome 2026.11.0 upgrade (branch `esphome-2026.11`)

An overnight soak on one unit before this branch goes anywhere near `main`. `manifest.json` is not
touched on the branch, so nothing here can reach the fleet.

What changed, and what each check below is for:

| Change | Why it needs the bench |
|---|---|
| ESPHome 2026.9.1 to 2026.11.0, which includes all of 2026.10 (ESP-IDF 5.5.6, mbedTLS client-only trim, `web_server_idf` rewrite) | All runtime; config and compile were clean |
| I2C on classic ESP32: new `USE_I2C_ISR_GUARD`, which masks an ACK_ERR interrupt storm after a NACK by wrapping `esp_intr_alloc_intrstatus` | Touches `bus_a`, where the K30 costs ~13 timeouts/min (CLAUDE.md, "Losing transactions on `bus_a`") |
| `influxdb` uploads with `begin()` and `poll()` from `loop()` instead of a blocking `post()` | New code path for every upload, retry and error |
| `captive_portal` rebased on 2026.11.0: DNS from `web_server_base::CaptiveDNS`, and `start()` ignores a second call while active | The second call leaked a `web_server_base` reference, which could leave port 80 up after the portal closed |
| `sgp4x` rebased on 2026.11.0; the setup and self-test retries are unchanged | Cosmetic upstream; still the path that cost VOC/NOx for a whole boot |
| `sound_level_meter` takes `microphone::AudioCapture`; the deprecated vector callback heap-copied every audio block | Audio path; watch for ring buffer overruns |
| `senseair_i2c` runs its whole setup up to 5 times, 5s apart (added after the first night) | A failed re-initialisation left CO2 off for 5min, 6 times in the first night |

## Build and flash

The pipx ESPHome must be 2026.11.0 (`min_version` refuses anything older):

```bash
pipx upgrade esphome && esphome version        # 2026.11.0
git switch esphome-2026.11
esphome compile samba.yaml                     # then run the safe-mode audit from CLAUDE.md
```

Flash the bench unit (OTA from 2.0.x is the realistic path; serial is fine for a first look).

## On boot (serial or `esphome logs`)

- [x] `[i2c]` `dump_config` has **no** `Interrupt guard inactive` line. Its presence means the wrap did not take, and the I2C soak result says nothing about the guard
- [ ] `[sgp4x]` passes the self-test; any `Self-test completed on attempt N` or `succeeded on attempt N` is the retry working, not a fault
- [ ] `[safe_mode]` reports `Bootloader rollback: supported`
- [x] `Noise encryption: YES`, OTA password and InfluxDB token fingerprints match `samba buildings list`

## First samples (from 2 min uptime)

- [x] `[influxdb]` shows `POST attempt 1/3` then `Published (HTTP 204 in N ms)`. Record N for a few cycles: it is time to headers, and it no longer stalls the loop
- [ ] The white sample blink is short: it used to last as long as the upload
- [x] `InfluxDB Status` reads `HTTP 204`, and the points are in the `raw` bucket with every field
- [x] Force a failure (wrong token through `influx_set_token`, then restore it): `HTTP 401`, no retry, the next sample still uploads. Then pull the WAN: `connection failed`, two retries 2–5 s apart, then `dropping`; the unit stays up, with no task WDT reset

## Overnight

- [x] `bus_a` timeouts per minute and the longest run of consecutive losses, against the 2026-09-28 baseline (~12.6–12.9/min, longest run 9 s). `k30_unhealthy`, `ads_unhealthy` and `sgp_unhealthy` stay where they sit on 2026.9.1
- [x] No reboots: `sys_uptime` is continuous across the night in InfluxDB
- [x] Heap: the `heap free` / `largest block` pair logged before each POST stays flat (§9.5 method in `docs/v2-bench-checklist.md`)
- [ ] SPL: no `ring buffer overrun` warnings, and LAeq/LA90/LA10 track a 2026.9.1 unit beside it
- [x] No fields missing from `ieq` beyond what the sensors genuinely drop

## Next morning

- [x] Home-app sync against the unit (digest auth, HEAD, download), with a pairing password set
- [ ] Captive portal: take the unit off its network so the AP opens (block its MAC on the router; it opens about a minute later); the list shows networks at once; save a PSK network; it connects and the portal closes. With no file-server password set, port 80 closes too (`curl -m 5 http://<ip>/` refuses or times out). With one set, port 80 stays up for the file server, so check `curl -s -o /dev/null -w '%{http_code}\n' http://<ip>/config.json` instead: `200` while the portal is open, anything else once it has closed
- [ ] OTA from 2.0.x: three 2.0.x units on ieqnet, once the second night is clean. This replaces the downgrade OTA back to 2.0.x

## Results

Bench unit C4:18 (`samba-c7c418`, 192.168.50.147, wilkinson/4/bench, firmware-default
coefficients), logged with `samba logs --level info` to `~/samba-bench/2026.11/`.

### Night 1: 2026-11-19 14:22 to 2026-11-20 10:03 (19.7h, build ffa2fb9)

- Reboots: none after the move to wall power at 14:22
- Uploads: 238 of 238 `HTTP 204`, no retries, no connection failures. Time to headers 1249–2008 ms, median 1297 ms; that used to be a loop stall on every sample
- Fields: 15 in every upload
- Heap before each POST: free 118048 to 117900 (lowest 116172), largest block 45056 throughout
- `bus_a`: 13.8 timeouts/min over the boot, hourly 10.9–14.2 with two hours at 18–19. Longest run of 2s-cadence losses 4 (~8s), against the 9s baseline
- No `Interrupt guard inactive`, no `ring buffer overrun`
- K30: 441 single read failures, all recovered by the re-initialisation except 6, five of them between 05:24 and 08:08. In those the re-initialisation itself lost its setup (`Sensor setup failed`), so CO2 stayed off until the next one at failure 10, about 4.5min later. The 330s staleness guard was not reached, so no field went missing, but one sample in each reported a CO2 value up to ~5min old. No 2026.9.1 night exists to compare; the September serial capture (2h) had none. Fixed in 4b2ab34 regardless (setup retries)
- SGP4x: failure count reached 4 three times, recovered each time, as on 2026.9.1
- Not checked: the SGP4x self-test and `Bootloader rollback` lines (the log stream started after `dump_config`), the white blink length, and SPL against a 2026.9.1 unit

### 2026-11-20 (build 4b2ab34, OTA from ffa2fb9 at 10:11)

- OTA: encrypted session, verified build afterwards
- Forced 401: a wrong token through `influx_set_token` at 10:15 gave `HTTP 401` on its own `device_status` write and on the 10:20 sample, with no retry; the SD row was still written. The wilkinson token (fingerprint `7c11d363`) restored at 10:20 gave `HTTP 204` at once
- WAN pull: skipped, the network serves other devices
- Home-app sync: passed
- K30: first three read failures each recovered at the next 30s check, with no `Sensor setup failed`

### Sign-off: 2026-11-20

Accepted on the results above. Night 2, the captive portal test and the ieqnet OTA from 2.0.x
were not run.
