#!/usr/bin/env python3
"""Bench test of a unit's SD file server against the wire contract (home-sync.md section 2).

    python3 docs/home-sync/bench.py HOST --password PW [--all] [--soak MINUTES]

Standard library only. Every request goes over one keep-alive connection with digest auth, the
way the app does it. Exit status is the number of failed checks. What it cannot see -- free heap,
the SD append landing during a stream -- is on the serial log (`esphome logs samba.yaml`).
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import re
import sys
import threading
import time
from dataclasses import dataclass

USERNAME = "samba"
HEADER = "time,ta,tg,rh,as,mrt,co2,lux,pm25,tvoc,nox,laeq,la90,la10"
ROW = re.compile(r"^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d(,(-?nan|-?\d+(\.\d+)?)){13}$")
LOG_NAME = re.compile(r"[0-9A-F]{12}_\d{6}_\d{4}\.txt")  # what config/sd.yaml names a log


@dataclass
class Response:
    status: int
    headers: dict[str, str]
    body: bytes
    seconds: float


class Unit:
    """One HTTP/1.1 connection to the unit. Answers the digest challenge and keeps the nonce."""

    def __init__(self, host: str, password: str | None, port: int = 80) -> None:
        self.host, self.port, self.password = host, port, password
        self.conn = http.client.HTTPConnection(host, port, timeout=60)
        self.challenge: dict[str, str] | None = None
        self.nc = 0

    def close(self) -> None:
        self.conn.close()

    def _authorization(self, method: str, path: str) -> str:
        c = self.challenge
        assert c is not None and self.password is not None
        self.nc += 1
        cnonce = hashlib.md5(f"{time.time()}{self.nc}".encode()).hexdigest()[:16]
        ha1 = hashlib.md5(f"{USERNAME}:{c['realm']}:{self.password}".encode()).hexdigest()
        ha2 = hashlib.md5(f"{method}:{path}".encode()).hexdigest()
        response = hashlib.md5(f"{ha1}:{c['nonce']}:{self.nc:08x}:{cnonce}:auth:{ha2}".encode()).hexdigest()
        fields = [f'username="{USERNAME}"', f'realm="{c["realm"]}"', f'nonce="{c["nonce"]}"', f'uri="{path}"',
                  "qop=auth", f"nc={self.nc:08x}", f'cnonce="{cnonce}"', f'response="{response}"']
        if "opaque" in c:
            fields.append(f'opaque="{c["opaque"]}"')
        return "Digest " + ", ".join(fields)

    def _once(self, method: str, path: str, headers: dict[str, str]) -> Response:
        t0 = time.monotonic()
        try:
            self.conn.request(method, path, headers=headers)
            r = self.conn.getresponse()
            body = r.read()
        except (http.client.HTTPException, OSError):
            self.conn.close()  # the server closed it, or it dropped: start a fresh one
            self.conn = http.client.HTTPConnection(self.host, self.port, timeout=60)
            self.conn.request(method, path, headers=headers)
            r = self.conn.getresponse()
            body = r.read()
        return Response(r.status, {k.lower(): v for k, v in r.getheaders()}, body, time.monotonic() - t0)

    def request(self, method: str, path: str, *, headers: dict[str, str] | None = None,
                auth: bool = True) -> Response:
        headers = dict(headers or {})
        if auth and self.password is not None and self.challenge is not None:
            headers["Authorization"] = self._authorization(method, path)
        r = self._once(method, path, headers)
        if r.status == 401 and auth and self.password is not None:
            www = r.headers.get("www-authenticate", "")
            if not www.startswith("Digest "):
                return r
            self.challenge = dict(re.findall(r'(\w+)="?([^",]+)"?', www[len("Digest "):]))
            headers["Authorization"] = self._authorization(method, path)
            r = self._once(method, path, headers)
        return r


class Checks:
    def __init__(self) -> None:
        self.failed = 0
        self.passed = 0

    def ok(self, cond: bool, what: str, detail: str = "") -> bool:
        mark = "ok  " if cond else "FAIL"
        print(f"  {mark} {what}" + (f"  ({detail})" if detail else ""))
        if cond:
            self.passed += 1
        else:
            self.failed += 1
        return cond


def parse_rows(body: bytes) -> tuple[list[str], list[str], str]:
    """(comment lines, data rows, trailing fragment) of a log body."""
    text = body.decode("utf-8", "replace")
    lines = text.split("\n")
    fragment = lines.pop()  # "" when the body ends with a newline
    comments = [line for line in lines if line.startswith("#")]
    rows = [line for line in lines if line and not line.startswith("#") and line != HEADER]
    return comments, rows, fragment


def bench(host: str, password: str, everything: bool) -> int:
    ck = Checks()
    unit = Unit(host, password)

    print("Auth")
    anon = Unit(host, None)
    r = anon.request("GET", "/sd", auth=False)
    ck.ok(r.status == 401, "GET /sd without credentials is 401", f"got {r.status}")
    ck.ok(r.headers.get("www-authenticate", "").startswith("Digest "), "challenge is Digest",
          r.headers.get("www-authenticate", "-")[:60])
    r = anon.request("GET", "/sd/F8B3B7C65CDC_260910_0412.txt", auth=False)
    ck.ok(r.status == 401, "GET /sd/<name> without credentials is 401", f"got {r.status}")
    anon.close()
    wrong = Unit(host, password + "x")
    r = wrong.request("GET", "/sd")
    ck.ok(r.status == 401, "wrong password is 401", f"got {r.status}")
    wrong.close()

    print("Listing")
    r = unit.request("GET", "/sd")
    if not ck.ok(r.status == 200, "GET /sd is 200", f"got {r.status} in {r.seconds:.2f}s"):
        return ck.failed
    ck.ok(r.headers.get("content-type", "").startswith("application/json"), "content-type application/json",
          r.headers.get("content-type", "-"))
    try:
        listing = json.loads(r.body)
    except json.JSONDecodeError as err:
        ck.ok(False, "body is JSON", str(err))
        return ck.failed
    mac = listing.get("mac", "")
    files = listing.get("files", [])
    ck.ok(bool(re.fullmatch(r"[0-9A-F]{12}", mac)), "mac is 12 upper-case hex digits", mac)
    names = [f["name"] for f in files]
    ck.ok(len(set(names)) == len(names), "names are unique", f"{len(names)} files, directory order")
    files.sort(key=lambda f: f["name"])  # the client sorts; the name embeds boot time
    ck.ok(all(n.endswith(".txt") and len(n) <= 40 and "/" not in n for n in names), "names are .txt at the root")
    ck.ok(all(isinstance(f["size"], int) and f["size"] >= 0 for f in files), "sizes are integers")
    for f in files:
        print(f"       {f['name']}  {f['size']} bytes")
    if not files:
        print("  no files on the card; the transfer checks need at least one")
        return ck.failed

    targets = files if everything else [max(files, key=lambda f: f["size"])]
    for f in targets:
        name, size = f["name"], f["size"]
        print(f"Transfer {name}")
        r = unit.request("HEAD", f"/sd/{name}")
        ck.ok(r.status == 200 and r.body == b"", "HEAD is 200 with no body", f"got {r.status}, {len(r.body)} bytes")
        ck.ok(r.headers.get("x-file-size", "").isdigit() and int(r.headers["x-file-size"]) >= size,
              "HEAD X-File-Size is the listing size or newer", r.headers.get("x-file-size", "-"))

        full = unit.request("GET", f"/sd/{name}")
        ck.ok(full.status == 200, "GET is 200", f"got {full.status}")
        ck.ok(full.headers.get("transfer-encoding") == "chunked", "chunked", full.headers.get("transfer-encoding", "-"))
        ck.ok("content-length" not in full.headers, "no Content-Length")
        ck.ok(full.headers.get("content-type", "").startswith("text/csv"), "content-type text/csv",
              full.headers.get("content-type", "-"))
        got = len(full.body)
        rate = got / max(full.seconds, 1e-6) / 1024
        ck.ok(got >= size, "body is at least the listing size", f"{got} bytes in {full.seconds:.2f}s, {rate:.0f} KB/s")
        comments, rows, fragment = parse_rows(full.body)
        if LOG_NAME.fullmatch(name):  # a lab card can carry older files written before the header block
            ck.ok(len(comments) == 4 and comments[0].startswith("# version: "), "four # header lines",
                  " | ".join(comments)[:80])
            ck.ok(full.body.decode("utf-8", "replace").split("\n")[4] == HEADER, "column header row")
        bad = [row for row in rows if not ROW.match(row)]
        ck.ok(not bad, f"{len(rows)} rows have 14 fields", bad[0][:60] if bad else "")
        stamps = [row[:19] for row in rows]
        ck.ok(stamps == sorted(stamps), "rows in time order")
        ck.ok(fragment == "" or not ROW.match(fragment), "ends on a row boundary or a partial row",
              f"fragment {fragment!r}" if fragment else "")

        if size > 2:
            offset = size // 2
            part = unit.request("GET", f"/sd/{name}", headers={"Range": f"bytes={offset}-"})
            ck.ok(part.status == 206, f"Range bytes={offset}- is 206", f"got {part.status}")
            ck.ok(part.headers.get("content-range", "").startswith(f"bytes {offset}-"),
                  "Content-Range starts at the offset", part.headers.get("content-range", "-"))
            ck.ok(part.body == full.body[offset:offset + len(part.body)] and len(part.body) >= size - offset,
                  "ranged body equals the tail of the full body", f"{len(part.body)} bytes")

        seen = int(unit.request("HEAD", f"/sd/{name}").headers.get("x-file-size", size))
        for what, header in (("at the current size", f"bytes={seen}-"), ("past the end", f"bytes={seen + 5000}-"),
                             ("with a closed range", "bytes=0-100")):
            r = unit.request("GET", f"/sd/{name}", headers={"Range": header})
            ck.ok(r.status == 416, f"Range {what} is 416", f"got {r.status}")

    print("Names")
    for path in ("/sd/nonexistent_260101_0000.txt", "/sd/foo.csv", "/sd/../samba.yaml", "/sd/a/b.txt",
                 "/sd/" + "x" * 37 + ".txt", "/sd/x.txt"):
        r = unit.request("GET", path)
        ck.ok(r.status == 404, f"GET {path[:44]} is 404", f"got {r.status}")

    print("Two downloads at once")
    name = targets[0]["name"]
    results: dict[int, Response] = {}

    def fetch(i: int) -> None:
        u = Unit(host, password)
        results[i] = u.request("GET", f"/sd/{name}")
        u.close()

    threads = [threading.Thread(target=fetch, args=(i,)) for i in range(2)]
    t0 = time.monotonic()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t0
    ck.ok(all(r.status == 200 for r in results.values()), "both are 200",
          f"{[r.status for r in results.values()]} in {elapsed:.2f}s")
    ck.ok(len({r.body[:4096] for r in results.values()}) == 1, "both bodies start the same")

    unit.close()
    print(f"\n{ck.passed} passed, {ck.failed} failed")
    return ck.failed


def soak(host: str, password: str, minutes: float) -> int:
    """Poll the listing every 10 s. Pair it with the serial log to watch the heap."""
    unit = Unit(host, password)
    deadline = time.monotonic() + minutes * 60
    polls = failures = 0
    while time.monotonic() < deadline:
        try:
            r = unit.request("GET", "/sd")
            polls += 1
            if r.status != 200:
                failures += 1
                print(f"{time.strftime('%H:%M:%S')}  {r.status}")
        except OSError as err:
            failures += 1
            print(f"{time.strftime('%H:%M:%S')}  {err}")
        time.sleep(10)
    unit.close()
    print(f"{polls} polls, {failures} failures")
    return failures


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("host")
    p.add_argument("--password", required=True, help="the unit's pairing password")
    p.add_argument("--all", action="store_true", help="transfer every file, not just the largest")
    p.add_argument("--soak", type=float, metavar="MINUTES", help="poll the listing every 10 s for this long")
    args = p.parse_args()
    if args.soak:
        sys.exit(min(soak(args.host, args.password, args.soak), 255))
    sys.exit(min(bench(args.host, args.password, args.all), 255))


if __name__ == "__main__":
    main()
