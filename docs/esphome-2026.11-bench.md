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

## Build and flash

The pipx ESPHome must be 2026.11.0 (`min_version` refuses anything older):

```bash
pipx upgrade esphome && esphome version        # 2026.11.0
git switch esphome-2026.11
esphome compile samba.yaml                     # then run the safe-mode audit from CLAUDE.md
```

Flash the bench unit (OTA from 2.0.x is the realistic path; serial is fine for a first look).

## On boot (serial or `esphome logs`)

- [ ] `[i2c]` `dump_config` has **no** `Interrupt guard inactive` line. Its presence means the wrap did not take, and the I2C soak result says nothing about the guard
- [ ] `[sgp4x]` passes the self-test; any `Self-test completed on attempt N` or `succeeded on attempt N` is the retry working, not a fault
- [ ] `[safe_mode]` reports `Bootloader rollback: supported`
- [ ] `Noise encryption: YES`, OTA password and InfluxDB token fingerprints match `samba buildings list`

## First samples (from 2 min uptime)

- [ ] `[influxdb]` shows `POST attempt 1/3` then `Published (HTTP 204 in N ms)`. Record N for a few cycles: it is time to headers, and it no longer stalls the loop
- [ ] The white sample blink is short: it used to last as long as the upload
- [ ] `InfluxDB Status` reads `HTTP 204`, and the points are in the `raw` bucket with every field
- [ ] Force a failure (wrong token through `influx_set_token`, then restore it): `HTTP 401`, no retry, the next sample still uploads. Then pull the WAN: `connection failed`, two retries 2–5 s apart, then `dropping`; the unit stays up, with no task WDT reset

## Overnight

- [ ] `bus_a` timeouts per minute and the longest run of consecutive losses, against the 2026-09-28 baseline (~12.6–12.9/min, longest run 9 s). `k30_unhealthy`, `ads_unhealthy` and `sgp_unhealthy` stay where they sit on 2026.9.1
- [ ] No reboots: `sys_uptime` is continuous across the night in InfluxDB
- [ ] Heap: the `heap free` / `largest block` pair logged before each POST stays flat (§9.5 method in `docs/v2-bench-checklist.md`)
- [ ] SPL: no `ring buffer overrun` warnings, and LAeq/LA90/LA10 track a 2026.9.1 unit beside it
- [ ] No fields missing from `ieq` beyond what the sensors genuinely drop

## Next morning

- [ ] Home-app sync against the unit (digest auth, HEAD, download), with a pairing password set
- [ ] Captive portal: take the unit off its network so the AP opens; the list shows networks at once; save a PSK network; it connects and port 80 closes again (`curl -m 5 http://<ip>/` refuses or times out with no file-server password set)
- [ ] `samba flash ota` back to the 2.0.x release binary, to prove the downgrade path
