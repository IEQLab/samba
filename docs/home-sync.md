# SAMBA Home Sync — implementation plan (Option 2)

*Drafted 2026-09-10 against samba firmware 1.99.99, ESPHome 2026.8.1 on ESP-IDF, ESP32-WROOM-32E
(no PSRAM). Target app: iOS 17+, Swift 6, with an Android pathway.*

A read-only file service on the SAMBA firmware, gated behind a per-unit password, and an iPhone
app that discovers the unit on the home WiFi, syncs the SD log incrementally, and charts it
locally. No Home Assistant, no InfluxDB, no cloud.

## 1. What gets built, and what deliberately does not

| Concern | Decision |
|---|---|
| Live reading | The last row of the SD log. Template sensors only change in the 5-minute sample script, so the newest CSV row *is* the live value. No native API client and no stock `web_server` on the phone. |
| History | Range requests on the log files. They are append-only and time-ordered; the app keeps a byte offset per file and fetches only what was appended. |
| Firmware footprint | One new component, one new package, one include line. Zero edits to `sd_spi_card`, `sample.yaml` or the CSV format. The server starts only when a password is provisioned. |
| Build | Single binary, single manifest. The code ships in the fleet build but stays inert. A second build variant would double every release step for a rare deployment type. |

**Why the server is free to add.** `captive_portal` already auto-loads `web_server_base`, which
on ESP-IDF is `esp_http_server`. It is compiled into today's binary, and its auth middleware,
multipart parser and chunked responses are already there. The captive portal stops the listener
(`base_->deinit()`) once WiFi connects, so a home unit starts it back up only after a password is
set, and a building unit never does.

**What a home user gives up.** History is reachable only on the home WiFi. If remote viewing
becomes a requirement later, the Influx path can be layered on without touching the app's local
sync.

## 2. Wire contract

This table is the portability layer. The iOS app, the Android port and the firmware bench test
all conform to it. [`home-sync/fixtures/`](home-sync/fixtures/) holds it as bytes — three log
files, a listing, the expected rows and the fetch cases — so each side is written against files
rather than against the other side's source (see section 6).

Discovery is unchanged firmware behaviour: mDNS `_esphomelib._tcp`, TXT record carrying `mac`,
`project_name`, `project_version` (the firmware's own version, the same string as the log's
`# version:` line) and `version` (the ESPHome release). Only the two HTTP endpoints below are new.
Port 80.

| Request | Response | Notes |
|---|---|---|
| `GET /sd` | `200 application/json` — `{"mac":"F8B3B7C65CDC","files":[{"name":"F8B3B7C65CDC_260910_0412.txt","size":183426}]}` | Root of the card only, `.txt` only, in directory order. The app sorts by name: the name embeds boot time in UTC, so that order is chronological, and sorting on the unit would need a name table or a directory scan per file. No mtime: FATFS timestamps are not reliable before the first SNTP sync. |
| `GET /sd/<name>` | `200 text/csv`, `Transfer-Encoding: chunked` | Whole file streamed in 1460-byte chunks. No `Content-Length`; the listing carries size. |
| `GET /sd/<name>` with `Range: bytes=183000-` | `206`, `Content-Range: bytes 183000-183425/183426` | Open-ended ranges only. An offset at or past the current size returns `416`, which the app reads as "card replaced or file truncated". |
| `HEAD /sd/<name>` | `200` + `X-File-Size` | Optional cheap size probe for one file. The app normally uses the listing. |
| any, no or bad credentials | `401`, `WWW-Authenticate: Digest` | Username is always `samba`. Digest keeps the password off the wire on an unencrypted LAN. |
| any, card not mounted | `503`, `Retry-After: 30` | Mirrors the 30 s remount retry in `sd_spi_card`. |
| second request during a download | waits | The IDF server is one task serving one request at a time, so a second connection queues rather than being refused. The app keeps one connection anyway. |
| name with `/`, `..`, no `.txt`, or over 40 chars | `404` | Path validation before any filesystem call. |

### The file the app parses (already what the firmware writes)

```
# version: 1.99.99
# building: Home
# level: Ground
# zone: Living
time,ta,tg,rh,as,mrt,co2,lux,pm25,tvoc,nox,laeq,la90,la10
2026-09-10 04:15:00,23.41,23.88,51.2,0.083,24.12,612,184,3.2,104,1,38.4,33.1,42.7
2026-09-10 04:20:00,23.39,23.85,51.4,0.079,24.09,nan,181,3.1,103,1,37.9,32.8,41.9
```

One `snprintf` in `config/sample.yaml` writes each row, so the precision per column is fixed and
a conforming writer is byte-exact:

| Column | Measurand | Unit | Written as |
|---|---|---|---|
| `time` | timestamp, UTC | — | `%Y-%m-%d %H:%M:%S` |
| `ta` | air temperature | °C | `%.2f` |
| `tg` | globe temperature | °C | `%.2f` |
| `rh` | relative humidity | % | `%.1f` |
| `as` | air speed | m/s | `%.3f` |
| `mrt` | mean radiant temperature | °C | `%.2f` |
| `co2` | carbon dioxide | ppm | `%.0f` |
| `lux` | illuminance | lux | `%.0f` |
| `pm25` | PM2.5 | µg/m³ | `%.1f` |
| `tvoc`, `nox` | VOC and NOx index | index | `%.0f` |
| `laeq`, `la90`, `la10` | sound pressure level | dB(A) | `%.1f` |

- Timestamps are UTC from the DS1307, second resolution, one row per 5 minutes. About 85 bytes
  per row, 24 KB per day, 9 MB per year.
- File names are `<MAC>_<yymmdd>_<HHMM>.txt` at the card root, the MAC without colons and the
  boot time in UTC. The two-digit year sorts chronologically within the century.
- `nan` marks a sensor that failed that cycle, and `-nan` the same thing with the sign bit set,
  which is what newlib prints when a calibration lambda negates a NaN. The app stores either as
  null and draws a gap, never an interpolation.
- A new file is created per boot, after the first time sync. Rows are only written while the SD
  switch is on, so a gap in time is a gap in the record.
- Values are calibrated. The app applies no coefficients.

## 3. Firmware

Three new files, one new package, one line in `samba.yaml`, and a fourth entry in the credential
provisioning pattern that already exists.

```
components/sd_file_server/__init__.py       ~60 lines: schema; adds USE_WEBSERVER_AUTH and
                                            USE_WEBSERVER_AUTH_DIGEST defines
components/sd_file_server/sd_file_server.h  ~50 lines
components/sd_file_server/sd_file_server.cpp ~220 lines: list, stream, range, validation
config/fileserver.yaml                      ~70 lines: component, password global, api action,
                                            on_boot, fingerprint sensor
samba.yaml                                  +1 line in packages, one logger line
config/globals.yaml                         the fileserver_password global
docs/home-sync/bench.py                     the bench test, standard library only
CLAUDE.md, README.md                        credential #4
```

### Component design

- **Depends on `sd_spi_card` for one call,** `is_mounted()`. Files are read with `stat`,
  `fopen`, `fseek` and `fread` on `/sd`, the same VFS the append already uses. The listing reads
  the root directory with FatFs's own `f_readdir`, which returns each size in the same pass: the
  VFS `readdir` carries no size and a `stat` per file rescans the directory each time, which
  took seconds on a lab card with 90 files. FatFs is built re-entrant, so this sits safely beside
  the append. No new methods on the SD component.
- **Registers with `web_server_base` through `add_handler()`,** which routes every request
  through the existing auth middleware (`WebServerBase::set_auth_username/password`, active
  under `USE_WEBSERVER_AUTH`). The captive portal uses `add_handler_without_auth()` and is
  unaffected.
- **Streams with `httpd_resp_send_chunk` on the raw `httpd_req_t`.** ESPHome's IDF request
  wrapper exposes it via `operator httpd_req_t*`. Every ESPHome response class buffers the whole
  body in a `std::string`, which is why the n-serrette `sd_file_server` cannot serve a month-old
  log on this hardware.
- **One static 1460-byte buffer** as a `std::array` member, matching the TCP segment size the IDF
  server already uses for receives (`RECV_CHUNK_SIZE`). No heap after `setup()`. Nothing
  serialises downloads in the component: the IDF server is one task and serves requests one at
  a time by construction.
- **Handlers run on the httpd task,** not the main loop, so a download cannot trip the loop
  watchdog. The FAT VFS holds one lock per filesystem: the 5-minute append waits for at most one
  chunk read, roughly a millisecond at SPI speed.
- **Listing builds JSON with `snprintf`** into the same buffer, packing entries and sending a
  segment at a time (one chunk per entry cost a round trip each under Nagle). No ArduinoJson, no
  per-request allocation. `TCP_NODELAY` is set on every request so a short tail goes out at once.
- **Password gate.** `setup()` calls `base_->init()` only when the password global is non-empty.
  The api action sets the password, calls `init()` or `deinit()` accordingly, and flushes NVS
  through a `_save` script exactly as the OTA password does. The constructor gives
  `web_server_base` the user name and a null password: the middleware then wraps every
  authenticated route but passes everything until a password exists, so a fleet unit's captive
  portal is unchanged, and once a password is set it also guards the portal's firmware upload.
  With no password the `/sd` routes are not there at all (`canHandle` is false).
- **HEAD.** ESPHome's server registers only GET, POST and OPTIONS with the IDF server; the
  component registers HEAD through the same dispatcher after each start so it meets the same
  middleware and handlers.
- **No `loop()`.** The component is a handler plus a setup hook.

### The package, `config/fileserver.yaml`

Same shape as the token and OTA password blocks, so a reader of `config/ota.yaml` recognises it
immediately.

- `fileserver_password` global, `restore_value: yes`, `max_restore_data_length: 32`, initial
  `""`, kept in `config/globals.yaml` beside the other three credentials. Printable ASCII, 12 to
  32 characters, enforced in the action.
- `fileserver_set_password` api action. Empty string clears it and stops the listener.
- `on_boot` priority 600 hands the stored password to the component before `setup()` runs,
  matching the existing pattern.
- "File Server" text sensor, diagnostic, publishing `off` or the 8-digit FNV-1 fingerprint.
  Never the password. `samba home password` verifies against it.
- The component block itself: `sd_file_server: {id: sd_files, sd_spi_card_id: sd0}`. Port stays
  at 80 from `web_server_base`.

Nothing in `on_safe_mode` changes, and the component takes its base pointer through its
constructor, so there is no post-registration wiring to drift past the early return. The
half-wiring check in CLAUDE.md still runs as part of `/bump`.

### Provisioning, in the lab before shipping

1. **Calibrate and tag as usual.** Building becomes the household name, level and zone the room.
   `samba deploy` is unchanged.
2. **Set the file server password.** New `samba` CLI command, about 40 lines beside the token and
   OTA commands: `samba home password <host> [--generate]`. It calls the action and confirms the
   fingerprint.
3. **Print the pairing label.** A QR code encoding `samba://pair?mac=F8B3B7C65CDC&pw=…` goes on
   the underside of the unit. The app scans it once. Losing the label means a lab visit or a
   factory reset, which is the same rule the other three credentials already have.

### Bench test before the manifest lands on main

`docs/home-sync/bench.py HOST --password PW` runs the contract checks below that a client can
see (auth, listing, chunked and ranged transfers, `416`, `HEAD`, bad names, two downloads at
once) and `--soak MINUTES` polls the listing every 10 s; the heap figures it cannot see are on
the serial log, printed whenever the listener starts or stops.

- Fleet behaviour first: with no password set, confirm port 80 is closed after WiFi connects and
  free heap matches the previous build within noise.
- Set a password, confirm the listener starts without a reboot, and that a wrong password gets
  `401` on both endpoints.
- Stream a 700 KB file while the 5-minute sample fires. Row count and checksum on the phone equal
  the card read on a laptop. Watch for any `sd_spi_card` write failure.
- Range fetch at offset, at exact size, and past size. Expect `206`, `416` and `416`.
- Pull the card mid-download and expect `503` on the next request, then success after remount.
- Leave a unit under a 10-second poll for 24 hours. Heap must be flat; the captive portal must
  still come up after a WiFi loss.
- Run the safe-mode half-wiring script on the generated `main.cpp`.

## 4. ESP32 memory

The WROOM-32E has no PSRAM and the build already runs WiFi, a TLS HTTP client, the Noise API,
I2S audio and DSP. The additions are small, but they must be measured, not assumed.

| Item | When paid | Estimate | Basis |
|---|---|---|---|
| Flash, component code | Every unit, every build | 10–15 KB | Handler, listing; digest auth already compiled for captive portal. OTA image is 1.29 MB in a 7.75 MB app partition. |
| Static RAM, component object | Every unit | ~1.6 KB | 1460-byte buffer plus pointers and flags. Present whether or not the server runs. |
| httpd task stack | Home units only, while listener runs | 4.35 KB | ESPHome sets IDF's 4096 default plus 256. |
| httpd control block and header scratch | Home units only | ~3 KB | `CONFIG_HTTPD_MAX_REQ_HDR_LEN` is 1024 in this build; socket table sized for the default 7 sockets. |
| Per open connection, lwIP | Home units, during a sync | 2–4 KB | The app keeps one keep-alive connection. LRU purge is on, so a stale phone connection cannot pin sockets. |
| FAT file handles | During a download | 1 of 5 | `max_files` is 5. Append uses one transiently, download one; three spare. |
| Heap after `setup()` | Never | 0 | Chunk buffer is static, JSON is `snprintf`, paths are fixed `char` arrays. |

The captive portal already pays the httpd costs whenever a unit is in AP mode, so these figures
are not new territory for the hardware. The important guarantee is the gate: a building unit with
no password never starts the listener, and the fleet keeps today's heap profile.

Measure with a temporary `ESP_LOGI` of `esp_get_free_heap_size()` and
`heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` in the bench build, before and after a
password is set and again after the 24-hour soak. The largest free block is the number that
predicts field crashes; a shrinking one means fragmentation from something on the httpd path.

### Choices made for memory

- **No stock `web_server`.** It registers a state listener on every entity, builds JSON with
  ArduinoJson per request, keeps SSE client buffers, and exposes every switch and number as a
  POST endpoint, including factory reset. The last CSV row gives the same live value for nothing.
- **Chunked, not buffered.** Peak RAM is independent of file size.
- **One download at a time.** Bounded worst case rather than seven parallel reads contending on
  the SPI bus.
- **Digest over Basic** costs an MD5 per request, which is already linked for OTA, and nothing
  extra in RAM beyond ESPHome's nonce handling.

## 5. iOS app

Native Swift, iOS 17 and later. Two targets: a UI-free `SambaCore` package that owns the
contract, and a SwiftUI app that renders it. The split is what makes the Android port a
re-implementation against fixtures rather than a reverse-engineering.

### Stack

- **Discovery:** `NWBrowser` for `_esphomelib._tcp`; the TXT record carries `mac`,
  `project_name`, `project_version` and `version`, so the app matches a paired unit by MAC when
  its DHCP address changes and shows the firmware version it advertises.
- **HTTP:** `URLSession` with `httpMaximumConnectionsPerHost = 1`. Digest auth is handled by the
  system challenge delegate.
- **Store:** SQLite through GRDB. Chosen over SwiftData because the schema must be portable to
  Room on Android.
- **Charts:** Swift Charts.
- **Pairing:** VisionKit `DataScannerViewController` for the QR; password in the Keychain, never
  in the store.

### Info.plist

- `NSBonjourServices`: `_esphomelib._tcp`
- `NSLocalNetworkUsageDescription`
- `NSAppTransportSecurity` → `NSAllowsLocalNetworking`. Plain HTTP to a LAN host is permitted by
  Apple's rules.

### Schema (portable)

```sql
device(mac PK, name, host, last_seen, fw_version)
file(mac, name, size_seen, bytes_synced, fragment TEXT, PK(mac, name))
sample(mac, ts INTEGER, ta, tg, rh, as_, mrt, co2, lux, pm25, tvoc, nox, laeq, la90, la10,
       PK(mac, ts))
index sample(mac, ts DESC)
```

`ts` is Unix seconds. Duplicate timestamps are ignored on insert, so a refetch is harmless. The
`fragment` column carries a partial trailing line across syncs.

### Sync algorithm, in `SambaCore`

1. **Resolve the unit.** Browse Bonjour for up to 3 s, match TXT `mac`. Fall back to the last
   known host.
2. **List.** `GET /sd`. For each entry compare `size` with `bytes_synced`.
3. **Classify.** Unknown name: fetch from 0. Larger: fetch `Range: bytes=<bytes_synced>-`. Equal:
   skip. Smaller, or `416`: card replaced; reset the bookmark and fetch from 0.
4. **Parse streaming.** Prepend the stored fragment, split on newline, hold back the last partial
   line as the new fragment, skip `#` lines and the header. Upsert rows in one transaction per
   file.
5. **Advance.** Set `bytes_synced` to offset plus bytes received only after the transaction
   commits.
6. **Surface state.** Newest row is "now", with its age shown plainly. Older than 15 minutes reads
   as stale; an unreachable unit says so with the last successful sync time.

### Screens

- **Units.** Paired units with reachability and last sync. "Add unit" opens the QR scanner.
- **Now.** The 13 measurands from the newest row, in the units the lab uses: °C, %, m/s, ppm,
  µg/m³, index, lux, dB(A). Age of the reading is the most prominent secondary element.
- **History.** One measurand at a time, day, week and month ranges, gaps drawn as gaps.
  Aggregation is done in SQL on the phone, mirroring the hourly mean, min and max the lab's
  downsampler computes.
- **Settings.** Forget unit, export the local store as CSV, background refresh toggle.

### Milestones

| Week | Deliverable | Exit test |
|---|---|---|
| 1 | `SambaCore`: CSV parser, store, sync engine against the fixture files | Conformance tests pass on the fixtures; no networking yet |
| 2 | Discovery, digest auth, QR pairing, live sync against a bench unit | Cold sync of a 700 KB log, then a 5-minute incremental sync moving under 1 KB |
| 3 | Now and History screens, gaps, staleness, export | A week of real home data reads correctly against the SD card copy |
| 4 | Background refresh, edge cases, TestFlight | Card swap, reboot, address change and unreachable unit all recover without user action |

Background refresh uses `BGAppRefreshTask`. iOS schedules it opportunistically and the unit is
only reachable at home, so it is a convenience, not a guarantee. The reliable path is sync on
foreground.

## 6. Android pathway

Contract first, code second. The port is scoped by what it must conform to, not by what the Swift
code happens to do.

- **Fixtures in this repo.** `docs/home-sync/fixtures/` holds three log files (clean; `nan` rows,
  a `-nan` and a hole in the record; a partial last line), the listing JSON, the expected parsed
  rows, and the fetch cases including a resume mid-row and a `416`. `SambaCore`'s tests run
  against them from day one; the Android port runs the same files through the same assertions.
  The measurements are synthetic until a unit has logged a real day — the formats are not, and
  the directory's README says which is which.
- **Same schema, same algorithm.** The SQLite schema above is created verbatim by Room. The six
  sync steps are the spec; the Kotlin implementation follows them line by line.
- **Kotlin equivalents:** `NsdManager` for mDNS, OkHttp with its digest authenticator, Room, Vico
  for charts, ML Kit for the QR, `WorkManager` for background sync, which Android honours far
  better than iOS.
- **Local network permissions:** Android needs none for LAN HTTP; mDNS needs the multicast lock
  and, on 13+, nothing extra.

**Kotlin Multiplatform is the alternative,** moving `SambaCore` into shared Kotlin with SQLDelight
and Ktor and keeping SwiftUI and Compose as native shells. It saves the second implementation of
about 800 lines of core logic at the cost of a KMP toolchain in the iOS build. Worth it if Android
is committed within six months of iOS shipping; not worth it for an uncertain port. Either way the
fixtures and schema are the same, so the choice can be deferred to the point where Android is
funded.

## 7. Schedule and risks

| Stream | Effort | Depends on |
|---|---|---|
| Contract doc and fixtures | 1 day | Nothing. Do this first; both sides build against it. |
| Firmware component, package, `samba` CLI command | 3–4 days | Contract |
| Bench test and heap soak | 2 days, one of them unattended | Firmware on a physical unit |
| iOS app to TestFlight | 4 weeks | Week 1 needs only fixtures; week 2 onward needs a bench unit |
| Release via `/bump` | usual | Bench test passed; fleet units verified inert |

### Risks and the answer to each

- **Heap headroom on the running fleet build is unknown.** The gate keeps fleet units unchanged,
  and the soak test settles the home case before any unit ships. If the largest free block turns
  out tight, the buffer drops to 512 bytes at the cost of throughput.
- **Digest auth in ESPHome's IDF server is newer code.** Basic auth is a one-define fallback and
  the app's challenge handler covers both.
- **SD append and read contention.** The FAT lock serialises them; the bench test streams during
  a sample cycle to prove the append still lands.
- **Password on a lost label.** Same recovery as the API key and OTA password: factory reset and
  re-pair. Documented in the household setup sheet.
- **Router client isolation.** Some home routers block LAN-to-LAN traffic on guest networks. The
  setup sheet says to join the unit to the main network. The app's "unreachable" state names this
  as a likely cause.
- **Plain HTTP.** Data crosses the home LAN unencrypted. Digest protects the password; the
  measurements are room temperature and CO₂. Accepted, and stated in the sheet.
