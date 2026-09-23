# /bump — Version bump and release

Compile firmware, copy binary, generate manifest, and optionally tag, push, and publish a GitHub release.

## Usage

```
/bump <VERSION> "<SUMMARY>" [--tag] [--draft-release] [--no-compile] [--dry-run]
```

## Arguments

- `VERSION` (required) — Semver-style dot-separated number (e.g. `2.0.1`). Its major number picks
  the manifest (step 1a).
- `SUMMARY` (required) — Short release description
- `--tag` — Git add, commit, create annotated tag `v<VERSION>`, push, **and create the GitHub
  release** (steps 7–8). Tagging without the release leaves the manifest's `release_url` a 404.
- `--draft-release` — With `--tag`, create the GitHub release as a draft for review. Note this
  defers creation of the remote tag until you publish it.
- `--no-compile` — Skip `esphome compile`, use existing build output
- `--dry-run` — Preview all actions without writing files, committing, or pushing

**Pushing is what arms the fleet, not the release.** There are two fleets, each reading one
manifest on `main` every 12h and applying updates Mondays at 04:00 UTC (`days_of_week: MON` in
`config/rtc.yaml`), with 0–10 min jitter:

| Manifest | Read by | Built from |
|---|---|---|
| `firmware/manifest.json` | the 1.99.99 field units | `release/1.x` |
| `firmware/manifest_v2.json` | every 2.x unit | `main` |

A unit installs whatever version its manifest names, **lower ones included**, so a manifest
pointing at the other line's binary cross-flashes that whole fleet. Validate on a physical unit
before the commit touching either manifest lands on `main`. To abort, revert that manifest on
`main`. See docs/v2-release-plan.md §2–3.

## Steps

Run these steps sequentially from the **samba project root**:

### 1. Validate version format

VERSION must match `^[0-9]+(\.[0-9]+)*$`. Reject otherwise.

### 1a. Pick the manifest from the major number (BLOCKING)

| Major | Manifest | `MANIFEST_URL` compiled into the binary |
|---|---|---|
| `2` | `firmware/manifest_v2.json` | `https://github.com/IEQLab/samba/raw/main/firmware/manifest_v2.json` |
| `1` | refuse — see below | |
| anything else | refuse | |

The `update:` `source:` in `config/ota.yaml` must equal `MANIFEST_URL`; refuse if it does not.
That is the branch telling you it is on the other line.

**Refuse a 1.x version.** 1.x artefacts are built on `release/1.x` but must land on `main`'s
`firmware/`, where the field units read them, and this skill does not yet do that two-branch
procedure. Point the user to docs/v2-release-plan.md §3 and stop. Never commit 1.x artefacts to
`release/1.x` alone, and never write a 2.x binary into `manifest.json`.

### 1b. Set the project version in `samba.yaml`

This must happen **before** the compile: `project.version` is baked into the binary's
`esp_app_desc_t`, so a build made before the edit reports the previous version forever — which
is what a device shows in Home Assistant and what the field build is identified by.

```bash
sed -i '' 's/^    version: ".*"$/    version: "<VERSION>"/' samba.yaml
grep -n 'version:' samba.yaml | head -3   # confirm project.version, not min_version
```

Leave `min_version` alone unless the release actually requires a newer ESPHome.

### 2. Compile firmware (unless `--no-compile`)

```bash
esphome compile samba.yaml
```

### 2b. Gate: no half-wired components in safe mode (BLOCKING)

`safe_mode` calls `App.setup()` and `on_safe_mode` from inside `should_enter_safe_mode()`, then
the generated `setup()` returns early. Anything registered before that early return but wired
after it is set up with a null pointer and crashes on **every** boot, needing a USB reflash.
Whether it happens depends on codegen order, so it can reappear silently from an unrelated
config change — most often an action in `on_safe_mode` that references another component by id.

**If this check reports anything, stop and fix it. Do not cut a release.**

```bash
python3 - <<'EOF'
import re, sys
L = open('.esphome/build/samba/src/main.cpp').read().split('\n')
cut = next(i for i, l in enumerate(L, 1) if 'should_enter_safe_mode' in l)
reg = {m.group(1): i for i, l in enumerate(L, 1)
       if (m := re.search(r'App\.register_component_\((\w+),', l))}
bad = [(v, reg[v], i) for i, l in enumerate(L, 1)
       if (m := re.search(r'(\w+)->set_(parent|source|uart_parent|http_request|request_parent)\b', l))
       and (v := m.group(1)) in reg and reg[v] < cut < i]
print(f"safe-mode early return at line {cut}")
if bad:
    for v, r, w in bad:
        print(f"  HALF-WIRED: {v} registered {r}, wired {w}")
    sys.exit(1)
print("  OK: no half-wired components")
EOF
```

See the "Never reference an id from `on_safe_mode`" section of CLAUDE.md for the mechanism.

### 2c. Gate: the binary reads the manifest it will be published in (BLOCKING)

```bash
grep -c "firmware/manifest_v2.json" .esphome/build/samba/build/firmware.ota.bin   # must be >= 1
grep -c "firmware/manifest.json"    .esphome/build/samba/build/firmware.ota.bin   # must be 0
```

Refuse on any other result. `source:` is compiled in and nothing at run time can change it, so a
binary that fails this would move its whole fleet onto the wrong line at the next update.

### 3. Copy binary and compute MD5

- Source: `.esphome/build/samba/build/firmware.ota.bin`
  - ESPHome moved this path in 2026.7.x. On ESPHome < 2026.7 it is
    `.esphome/build/samba/.pioenvs/samba/firmware.ota.bin`. Resolve it, and **fail loudly
    if neither exists** — never fall through to a stale binary from a previous build:
    ```bash
    OTA_BIN=.esphome/build/samba/build/firmware.ota.bin
    [ -f "$OTA_BIN" ] || OTA_BIN=.esphome/build/samba/.pioenvs/samba/firmware.ota.bin
    [ -f "$OTA_BIN" ] || { echo "no firmware.ota.bin found - did the compile succeed?"; exit 1; }
    ```
  - Confirm its mtime is newer than the compile you just ran.
- Destination: `firmware/samba_v<VERSION>.bin`
- Compute MD5 hash of the destination file:
  - macOS: `md5 -q <file>`
  - Linux: `md5sum <file> | awk '{print $1}'`

### 4. Write the manifest from step 1a

Generate this exact JSON structure (use `jq` or write directly):

```json
{
  "name": "SAMBA Indoor Environmental Monitor",
  "version": "<VERSION>",
  "builds": [
    {
      "chipFamily": "ESP32",
      "ota": {
        "md5": "<MD5_HASH>",
        "path": "https://raw.githubusercontent.com/IEQLab/samba/main/firmware/samba_v<VERSION>.bin",
        "release_url": "https://github.com/IEQLab/samba/releases/tag/v<VERSION>",
        "summary": "<SUMMARY>"
      }
    }
  ]
}
```

### 5. Validate on a bench unit

Push the exact binary from step 3 — the copy under `firmware/`, not the build directory — to a
bench unit with samba_app and confirm it comes back running the new version:

```bash
samba flash ota <BENCH_IP> --bin firmware/samba_v<VERSION>.bin
samba info <BENCH_IP>     # project version must be <VERSION>
```

Then the safe-mode check (docs/v2-release-plan.md §9.1): press `Restart SAMBA (Safe Mode)`, wait
a minute, push the same binary with `samba flash ota` while the unit is in safe mode, and confirm
the upload completes and the unit boots normally. On ESPHome 2026.9.0 this is expected to crash
the unit at the OTA handshake until the upstream `noise_context_()` null guard lands; say so in
the summary rather than skipping it. The boot counter is already cleared by then, so the crash
should reboot the unit into its normal firmware; if it does not, recover over USB (step 5b).

`samba flash ota` waits for the device to reconnect and exits 1 if its build time did not
change. Without samba_app, `esphome upload samba.yaml --device <BENCH_IP> --file firmware/samba_v<VERSION>.bin`
does the push without the check. If no bench unit is available, say so in the summary — do not
skip silently.

### 5b. The way back onto a unit

The release binaries here are OTA images with no bootloader or partition table, so they cannot be
written over serial. To recover a unit that will not take an OTA, flash the calibration factory
image from samba_calibration with `samba flash serial`, then push the release with
`samba flash ota HOST --bin firmware/samba_v<VERSION>.bin`. Nothing in that repo pins a release
version, so a bump needs no edit there.

### 6. Print summary

Show the version, MD5 hash, a `jq` summary of the manifest, and the bench validation result.

### 7. Git tag and push (only if `--tag`)

In the **samba** repository:

```bash
git add samba.yaml firmware/samba_v<VERSION>.bin firmware/manifest_v2.json
git commit -m "Release v<VERSION>: <SUMMARY>"
git tag -a "v<VERSION>" -m "<SUMMARY>"
git push
git push --tags
```

Note `main` has a branch-protection rule requiring pull requests. Pushing directly succeeds
for accounts with bypass permission but is reported as `Bypassed rule violations`. If that
rule should be respected, branch and open a PR instead of pushing to `main`.

### 8. Create the GitHub release (only if `--tag`)

**Do not skip this.** Pushing a tag does *not* create a release, and the manifest's
`release_url` points at the release page — so skipping it leaves a 404 for every device
running that firmware. This was missed for v1.99.97 and had to be backfilled.

```bash
gh release create "v<VERSION>" \
  --title "v<VERSION>" \
  --notes-file <NOTES_FILE> \
  --verify-tag
```

- `--verify-tag` aborts if the tag was not pushed, rather than silently creating one.
- Add `--draft` to hold it for review. A draft does **not** create the remote tag until it is
  published, is never marked "Latest", and its URL is `releases/tag/untagged-<hash>` — so
  `release_url` stays a 404 until you publish.
- Notes follow the existing convention: a `## Changes` heading followed by `*` bullets. No
  binaries are attached as release assets; the firmware lives in `firmware/` in the repo.
- Verify afterwards, since the badge is only computed over published, non-prerelease releases:
  ```bash
  gh release list --limit 3 --json tagName,isDraft,isLatest
  curl -s -o /dev/null -w "%{http_code}\n" "https://github.com/IEQLab/samba/releases/tag/v<VERSION>"
  ```

### 9. Verify the OTA payload is fetchable and matches

The fleet polls its manifest on `main` every 12h and applies updates Mondays 04:00 UTC, so
a bad hash means every device silently rejects the update:

```bash
curl -sL "https://raw.githubusercontent.com/IEQLab/samba/main/firmware/samba_v<VERSION>.bin" \
  | md5   # must equal the md5 in firmware/manifest_v2.json
```

**Any recompile invalidates the committed binary and manifest md5** — ESPHome embeds a build
timestamp, so builds are never byte-reproducible. If you rebuild after step 3, redo steps 3–5
so `firmware/` holds exactly the binary you validated on hardware.

If `--dry-run`, print what would happen at each step instead of executing.
