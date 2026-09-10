#!/usr/bin/env python3
"""Regenerate the home-sync conformance fixtures.

    python3 docs/home-sync/fixtures/make_fixtures.py

Deterministic: the same source produces byte-identical files, so a regeneration that changes a
log file is a change to the wire contract and has to be reviewed as one.

The log text is written first, and every expected value is then read back out of that text by a
second, deliberately separate parser. The expected JSON therefore cannot drift from the bytes it
describes.
"""

import json
import math
import random
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAC = "F8B3B7C65CDC"
VERSION = "1.99.99"
TAGS = {"building": "Home", "level": "Ground", "zone": "Living"}
LOCAL_OFFSET = 10  # these units sit in UTC+10; the log itself is always UTC

# config/sample.yaml writes one printf per row. The precision per column is part of the contract.
PRECISION = {"ta": 2, "tg": 2, "rh": 1, "as": 3, "mrt": 2, "co2": 0, "lux": 0, "pm25": 1,
             "tvoc": 0, "nox": 0, "laeq": 1, "la90": 1, "la10": 1}
COLUMNS = ["time"] + list(PRECISION)
HEADER = ",".join(COLUMNS)


def preamble() -> str:
    return (f"# version: {VERSION}\n# building: {TAGS['building']}\n"
            f"# level: {TAGS['level']}\n# zone: {TAGS['zone']}\n{HEADER}\n")


def fmt_row(t: datetime, v: dict, faults: dict) -> str:
    """One CSV row. `faults` gives the columns that read as a NaN, and which spelling of it."""
    fields = [t.strftime("%Y-%m-%d %H:%M:%S")]
    for col, dp in PRECISION.items():
        fields.append(faults[col] if col in faults else f"{v[col]:.{dp}f}")
    return ",".join(fields) + "\n"


class House:
    """A plausible one-bedroom flat: an afternoon that warms, an evening that cooks and settles.

    Not a model of anything. It exists so the numbers move the way a reviewer expects a room to
    move, and so a chart drawn from these files looks like a room rather than noise. The first
    row is given, and the model is blended onto it over the first hour so the file opens where it
    is told to without a step change in the second row.
    """

    def __init__(self, seed: int, start: dict):
        self.r = random.Random(seed)
        self.start = start
        self.co2 = start["co2"]
        self.tvoc = start["tvoc"]
        self.offset = None  # set from the first modelled row, then decays away

    # Columns that carry the opening offset. co2 and tvoc do not: they are stateful and already
    # start at the given value, and nox is a small integer index.
    BLENDED = ("ta", "tg", "rh", "as", "mrt", "lux", "pm25", "laeq", "la90", "la10")

    def at(self, t: datetime, i: int) -> dict:
        h = (t.hour + t.minute / 60 + LOCAL_OFFSET) % 24  # the local clock drives behaviour
        j = self.r.uniform

        cooking = 18.25 <= h < 19.25
        occupied = h >= 16 or h < 8
        asleep = h >= 23 or h < 6.5

        day = math.sin((h - 9) / 24 * 2 * math.pi)  # coolest before dawn, warmest late afternoon
        ta = 22.6 + 1.9 * day + (0.4 if cooking else 0) + j(-.04, .04)
        tg = ta + 0.45 + (0.25 if 9 < h < 16 else 0) + j(-.03, .03)
        rh = 52.5 - 1.4 * day + (3.5 if cooking else 0) + j(-.2, .2)
        speed = (0.14 if cooking else 0.075 if occupied else 0.045) + j(-.008, .008)
        mrt = tg + (tg - ta) * 0.6 + j(-.03, .03)

        # First order towards the level the room's use implies, so a quiet night settles onto
        # outdoor CO2 instead of hitting a floor.
        co2_target = 980 if cooking else 760 if occupied else 470 if asleep else 445
        self.co2 += (co2_target - self.co2) * 0.09 + j(-7, 7)
        voc_target = 300 if cooking else 102
        self.tvoc += (voc_target - self.tvoc) * (0.35 if cooking else 0.12) + j(-3, 3)

        if asleep:
            lux = j(0, 1.4)
        elif 6.5 <= h < 17.5:
            lux = 60 + 380 * math.sin((h - 6.5) / 11 * math.pi) ** 2 + j(-25, 25)
        else:
            lux = 118 + j(-14, 14)

        pm25 = 3.0 + j(-.5, .8) + (24 * math.exp(-abs(h - 18.8) * 6) if 18 < h < 20 else 0)
        nox = 1 + (3 if cooking else 0) + (1 if 19.25 <= h < 19.75 else 0)
        laeq = (32.5 if asleep else 46.5 if cooking else 41.0 if occupied else 36.5) + j(-1.2, 1.2)

        v = {"ta": ta, "tg": tg, "rh": rh, "as": speed, "mrt": mrt, "co2": self.co2,
             "lux": lux, "pm25": pm25, "tvoc": self.tvoc, "nox": nox,
             "laeq": laeq, "la90": laeq - 5.4 + j(-.3, .3), "la10": laeq + 4.6 + j(-.3, .3)}

        if self.offset is None:
            self.offset = {c: self.start[c] - v[c] for c in self.BLENDED}
        decay = math.exp(-i / 6)  # gone within the hour
        for c in self.BLENDED:
            v[c] += self.offset[c] * decay
        return v


# The row quoted in docs/home-sync.md section 2, which SambaCore's parser test asserts.
DOC_ROW = {"ta": 23.41, "tg": 23.88, "rh": 51.2, "as": 0.083, "mrt": 24.12, "co2": 612,
           "lux": 184, "pm25": 3.2, "tvoc": 104, "nox": 1, "laeq": 38.4, "la90": 33.1,
           "la10": 42.7}


def build(start: datetime, count: int, seed: int, first: dict,
          faults=None, gaps=(), trailing=None) -> str:
    """Render one log file. `faults` maps a row index to {column: nan spelling}."""
    faults = faults or {}
    house = House(seed, first)
    out = [preamble()]
    for i in range(count):
        t = start + timedelta(minutes=5 * i)
        v = house.at(t, i)  # advance the room even for rows that never reach the card
        if any(a <= t < b for a, b in gaps):
            continue
        out.append(fmt_row(t, v, faults.get(i, {})))
    text = "".join(out)
    if trailing is not None:
        text += trailing  # a row cut short: no newline, and never completed
    return text


def parse(text: str) -> dict:
    """A second parser, kept separate on purpose: it defines the expected JSON, so it shares no
    code with the row rendering the ports conform to."""
    body, _, fragment = text.rpartition("\n")
    header, samples = {}, []
    for line in (body.split("\n") if body else []):
        if line.startswith("#"):
            k, _, v = line[1:].partition(":")
            header[k.strip()] = v.strip()
            continue
        if line in (HEADER, ""):
            continue
        f = line.split(",")
        assert len(f) == len(COLUMNS), f"malformed row: {line!r}"
        t = datetime.strptime(f[0], "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc)
        row = {"ts": int(t.timestamp()), "time": f[0]}
        for col, raw in zip(COLUMNS[1:], f[1:]):
            row[col] = None if raw in ("nan", "-nan") else float(raw)
        samples.append(row)
    return {"header": header, "samples": samples, "fragment": fragment}


def expected(name: str, text: str, note: str) -> dict:
    p = parse(text)
    s = p["samples"]
    # Rows more than one interval apart: the record has a hole there and a chart must break the
    # line rather than join across it.
    gaps = [{"after_ts": a["ts"], "before_ts": b["ts"], "minutes": (b["ts"] - a["ts"]) // 60}
            for a, b in zip(s, s[1:]) if b["ts"] - a["ts"] > 300]
    return {"file": name, "note": note, "bytes": len(text.encode()), "header": p["header"],
            "rows": len(s), "fragment": p["fragment"], "interval_seconds": 300, "gaps": gaps,
            "first_ts": s[0]["ts"], "last_ts": s[-1]["ts"],
            "nulls": {c: n for c in COLUMNS[1:] if (n := sum(1 for r in s if r[c] is None))},
            "samples": s}


# --- the three logs --------------------------------------------------------------------------

CLEAN_START = datetime(2026, 9, 10, 4, 15, tzinfo=timezone.utc)
FAULTY_START = datetime(2026, 9, 10, 23, 50, tzinfo=timezone.utc)
CUT_START = datetime(2026, 9, 11, 7, 55, tzinfo=timezone.utc)

# Where each file opens: mid-afternoon, mid-morning and just before dinner, local time.
MORNING_ROW = {"ta": 22.94, "tg": 23.31, "rh": 53.4, "as": 0.061, "mrt": 23.62, "co2": 604,
               "lux": 288, "pm25": 3.4, "tvoc": 108, "nox": 1, "laeq": 38.9, "la90": 33.4,
               "la10": 43.2}
EVENING_ROW = {"ta": 23.80, "tg": 24.27, "rh": 50.6, "as": 0.072, "mrt": 24.55, "co2": 688,
               "lux": 141, "pm25": 3.6, "tvoc": 112, "nox": 1, "laeq": 42.6, "la90": 37.1,
               "la10": 47.0}

FILES = [
    ("F8B3B7C65CDC_260910_0412.txt",
     build(CLEAN_START, 72, seed=10, first=DOC_ROW),
     "Six clean hours of 5-minute rows, ending with a newline. The first row is the one quoted "
     "in docs/home-sync.md section 2."),
    ("F8B3B7C65CDC_260910_2347.txt",
     build(FAULTY_START, 48, seed=11, first=MORNING_ROW,
           # The K30 out for five cycles, then the ADS1115 channels (tg, as, mrt) for three.
           faults={**{i: {"co2": "nan"} for i in range(5, 10)},
                   **{i: {"tg": "-nan", "as": "nan", "mrt": "nan"} for i in range(18, 21)}},
           # SD writing switched off for half an hour: a gap in the record, not missing data.
           gaps=[(datetime(2026, 9, 11, 2, 0, tzinfo=timezone.utc),
                  datetime(2026, 9, 11, 2, 30, tzinfo=timezone.utc))]),
     "Crosses UTC midnight. Carries a dead CO2 sensor, an ADS1115 fault covering tg, as and mrt "
     "with one field spelled -nan, and a half-hour hole where SD writing was switched off."),
    ("F8B3B7C65CDC_260911_0753.txt",
     build(CUT_START, 11, seed=12, first=EVENING_ROW,
           trailing="2026-09-11 08:50:00,24.31,24.7"),
     "Power was cut mid-append. The last line is a partial row with no newline and is never "
     "completed, so the file stays this size and a resync skips it."),
]


def ranges(name: str, text: str) -> list:
    """Fetch cases over the clean log, computed from its actual bytes."""
    raw = text.encode()
    size = len(raw)
    newlines = [i for i, b in enumerate(raw) if b == 0x0A]
    boundary = newlines[40] + 1  # the start of a row: the bookmark a clean sync leaves
    mid = boundary + 37          # inside a row: where a fetch cut short would leave one

    def tail(off):
        return parse(raw[off:].decode())

    cases = [
        {"name": "cold-fetch", "file": name,
         "state_before": {"bytes_synced": 0, "fragment": ""},
         "request": {"method": "GET", "range": None},
         "response": {"status": 200, "transfer_encoding": "chunked"},
         "expect": {"rows": len(parse(text)["samples"]), "fragment": "",
                    "bytes_synced_after": size}},
        {"name": "incremental-at-row-boundary", "file": name,
         "state_before": {"bytes_synced": boundary, "fragment": ""},
         "request": {"method": "GET", "range": f"bytes={boundary}-"},
         "response": {"status": 206, "content_range": f"bytes {boundary}-{size - 1}/{size}"},
         "expect": {"rows": len(tail(boundary)["samples"]),
                    "first_ts": tail(boundary)["samples"][0]["ts"],
                    "fragment": "", "bytes_synced_after": size}},
        {"name": "resume-mid-row", "file": name,
         "note": "The previous fetch stopped inside a row. Its tail was kept as the fragment and "
                 "the bookmark still advanced past it.",
         "state_before": {"bytes_synced": mid, "fragment": raw[boundary:mid].decode()},
         "request": {"method": "GET", "range": f"bytes={mid}-"},
         "response": {"status": 206, "content_range": f"bytes {mid}-{size - 1}/{size}"},
         "expect": {"rows": len(tail(boundary)["samples"]),
                    "first_ts": tail(boundary)["samples"][0]["ts"],
                    "fragment": "", "bytes_synced_after": size,
                    "why": "Prepending the fragment restores the row the cut split, so the same "
                           "rows arrive as if the fetch had started at the row boundary."}},
        {"name": "offset-at-size", "file": name,
         "state_before": {"bytes_synced": size, "fragment": ""},
         "request": {"method": "GET", "range": f"bytes={size}-"},
         "response": {"status": 416},
         "expect": {"action": "reset the bookmark to 0 and refetch the whole file",
                    "why": "416 means the card was replaced or the file truncated. In ordinary "
                           "sync the listing reports an equal size and the file is skipped "
                           "without a request."}},
    ]

    offs = list(range(0, size, 1460)) + [size]
    chunks, frag = [], ""
    for a, b in zip(offs, offs[1:]):
        p = parse(frag + raw[a:b].decode())
        chunks.append({"bytes": [a, b], "rows": len(p["samples"]), "fragment_after": p["fragment"]})
        frag = p["fragment"]
    cases.append({"name": "chunked-stream", "file": name,
                  "note": "One cold fetch delivered in the 1460-byte chunks the firmware sends, "
                          "fed to the splitter in order. Rows summed over the chunks equal the "
                          "whole-file parse.",
                  "state_before": {"bytes_synced": 0, "fragment": ""},
                  "request": {"method": "GET", "range": None},
                  "response": {"status": 200, "transfer_encoding": "chunked"},
                  "chunks": chunks,
                  "expect": {"rows": sum(c["rows"] for c in chunks), "fragment": frag}})
    return cases


def main() -> None:
    (HERE / "expected").mkdir(exist_ok=True)
    listing = {"mac": MAC, "files": []}
    for name, text, note in FILES:
        (HERE / name).write_bytes(text.encode())
        listing["files"].append({"name": name, "size": len(text.encode())})
        (HERE / "expected" / f"{name[:-4]}.json").write_text(
            json.dumps(expected(name, text, note), indent=1) + "\n")
    listing["files"].sort(key=lambda f: f["name"])  # GET /sd sorts by name
    (HERE / "listing.json").write_text(json.dumps(listing, indent=1) + "\n")

    clean_name, clean_text, _ = FILES[0]
    cut_name, cut_text, _ = FILES[2]
    cut = parse(cut_text)
    (HERE / "ranges.json").write_text(json.dumps({
        "note": "Fetch cases over the fixtures. Byte offsets are into the files as committed.",
        "cases": ranges(clean_name, clean_text) + [{
            "name": "partial-last-line", "file": cut_name,
            "state_before": {"bytes_synced": 0, "fragment": ""},
            "request": {"method": "GET", "range": None},
            "response": {"status": 200},
            "expect": {"rows": len(cut["samples"]), "fragment": cut["fragment"],
                       "bytes_synced_after": len(cut_text.encode()),
                       "why": "The bookmark covers the partial line too, and the fragment is "
                              "kept with it. The next listing reports an equal size, so the "
                              "file is skipped and the partial row is never stored."}}]},
        indent=1) + "\n")

    for name, text, _ in FILES:
        p = parse(text)
        print(f"{name}  {len(text.encode()):>6} bytes  {len(p['samples']):>3} rows"
              f"  fragment={p['fragment']!r}")


if __name__ == "__main__":
    main()
