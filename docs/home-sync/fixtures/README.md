# Home sync conformance fixtures

These files are the shared test input for everything that reads a SAMBA SD log: `SambaCore` on
iOS, the Android port, and the firmware bench test. The wire contract itself is section 2 of
[../../home-sync.md](../../home-sync.md); this directory is that contract as bytes, so a port is
written against files rather than against someone else's source.

A parser conforms when, for each log file, it produces exactly the rows in `expected/`, and when
it handles every case in `ranges.json`.

## What is here

| File | What it is |
|---|---|
| `F8B3B7C65CDC_260910_0412.txt` | Six clean hours, 72 rows, ends with a newline. The ordinary case. |
| `F8B3B7C65CDC_260910_2347.txt` | 42 rows across UTC midnight, with sensor faults and a hole in the record. |
| `F8B3B7C65CDC_260911_0753.txt` | 11 rows and a twelfth cut off mid-field by a power loss. No trailing newline. |
| `listing.json` | The `GET /sd` body listing those three files, with their real sizes. |
| `expected/<log>.json` | Every row of that log as parsed: `ts` in Unix seconds, `null` for each `nan`. |
| `ranges.json` | Fetch cases: cold, incremental, resumed mid-row, chunked, and `416`. |
| `make_fixtures.py` | Regenerates all of the above. |

The measurements are synthetic. No home unit has shipped yet, so there is no real log to cut
down, and a fabricated file that is honest about being fabricated is better than a hand-typed one
that quietly gets the number formats wrong. Everything a parser can observe — the header block,
the column order, the printf precision of every field, the two spellings of NaN, the line ends,
the file names — is what `config/sample.yaml` and `config/sd.yaml` actually write. The values
themselves come from a rough model of a flat over an afternoon and a night: they move the way a
room moves so that a chart drawn from them looks wrong when the chart is wrong. Replace them with
a real log the first time a unit has run for a day, and keep the same file names and cases.

## The three logs, and what each one is for

**Clean.** Opens with the row quoted in `home-sync.md` section 2, which is also the row
`SambaCore`'s parser test asserts. Timestamps are five minutes apart throughout, and the file
ends with a newline, so a cold fetch leaves no fragment.

**Faults and a hole.** Three things at once, all of which a port gets wrong by default:

- The CO2 sensor is out for five rows: `co2` reads `nan` and must be stored as null, not zero and
  not the previous value. The other twelve measurands in those rows are good and must be kept.
- An ADS1115 fault takes `tg`, `as` and `mrt` together for three rows, since the globe
  thermistor and both anemometers hang off that ADC and MRT is computed from them. One field is
  spelled `-nan`: newlib prints the sign bit, so a calibration lambda that negates a NaN yields
  that spelling on the card. Both spellings are null.
- SD writing was switched off for half an hour, so rows 02:00 to 02:25 do not exist. The record
  has a 35-minute hole in it, listed in `gaps` in the expected JSON. A chart breaks the line
  there; it never interpolates across it, and the missing rows are not an error.

Row timestamps also cross UTC midnight into the next day, which is the case that breaks a parser
that has quietly assumed local time.

**Cut short.** The last line is a partial row with no newline, and it stays that way: the unit
lost power mid-append and the row is never completed. A parser keeps the 11 complete rows, holds
the partial line as the fragment, and stores nothing for it. The bookmark still advances to the
full file size, so the next listing shows an equal size and the file is skipped.

## Using them

The log files are ASCII with LF line ends. Reading them as bytes and splitting on `0x0A` is the
safe route: some HTTP clients and test helpers rewrite line ends, and a splitter that searches
for a `"\n"` *character* misses a CRLF that a helper introduced. Nothing in these fixtures
contains a CR.

`expected/<log>.json` carries, alongside the rows, the counts a test can assert cheaply first:
`bytes`, `rows`, `first_ts`, `last_ts`, the per-column `nulls`, and the `gaps`. The sample field
names are the CSV column names, so `as` is air speed — in the SQLite schema that column is `as_`,
because `as` is a keyword in both Swift and Kotlin.

`ranges.json` describes each fetch as the state before, the request, the response the firmware
gives, and what the store should hold afterwards. `state_before` is a file's row in the `file`
table: `bytes_synced` and `fragment`. The `resume-mid-row` case is the one worth reading twice —
the bookmark sits inside a row and the fragment holds that row's opening bytes, which is exactly
what a fetch interrupted by a dropped WiFi link leaves behind.

## Regenerating

```bash
python3 docs/home-sync/fixtures/make_fixtures.py
```

Deterministic, stdlib only. The generator writes the log text first and then reads every expected
value back out of that text with a second parser, so the JSON cannot drift from the bytes it
describes.

Because both sides of the port are pinned to these bytes, a regeneration that changes a log file
is a change to the wire contract. Review it as one: update section 2 of `home-sync.md` in the
same commit, and expect the conformance tests in `SambaCore` and the Android port to move with
it.
