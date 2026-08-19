# /bump — Version bump and release

Compile firmware, copy binary, generate manifest, and optionally tag, push, and publish a GitHub release.

## Usage

```
/bump <VERSION> "<SUMMARY>" [--tag] [--draft-release] [--no-compile] [--dry-run]
```

## Arguments

- `VERSION` (required) — Semver-style dot-separated number (e.g. `1.99.95`)
- `SUMMARY` (required) — Short release description
- `--tag` — Git add, commit, create annotated tag `v<VERSION>`, push, **and create the GitHub
  release** (steps 7–8). Tagging without the release leaves `manifest.json`'s `release_url` a 404.
- `--draft-release` — With `--tag`, create the GitHub release as a draft for review. Note this
  defers creation of the remote tag until you publish it.
- `--no-compile` — Skip `esphome compile`, use existing build output
- `--dry-run` — Preview all actions without writing files, committing, or pushing

**Pushing is what arms the fleet, not the release.** Devices poll `manifest.json` on `main`
every 12h and apply updates Mondays at 04:00 UTC (`days_of_week: MON` in `config/rtc.yaml`),
with 0–10 min jitter. Validate on a physical unit before the commit touching
`firmware/manifest.json` lands on `main`. To abort, revert `manifest.json` on `main`.

## Steps

Run these steps sequentially from the **samba project root**:

### 1. Validate version format

VERSION must match `^[0-9]+(\.[0-9]+)*$`. Reject otherwise.

### 2. Compile firmware (unless `--no-compile`)

```bash
esphome compile samba.yaml
```

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

### 4. Write `firmware/manifest.json`

Generate this exact JSON structure (use `jq` or write directly):

```json
{
  "name": "SAMBA Indoor Environmental Monitor",
  "version": "<VERSION>",
  "home_assistant_domain": "esphome",
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

### 5. Update deployment firmware OTA URL

Update the production firmware URL and MD5 hash in `../samba_calibration/firmware/samba_deployment.yaml`. Find the `ota.http_request.flash` block under the "Flash production firmware" button and replace the `url` and `md5` values:

```yaml
      - ota.http_request.flash:
          url: https://raw.githubusercontent.com/IEQLab/samba/main/firmware/samba_v<VERSION>.bin
          md5: "<MD5_HASH>"
```

If `../samba_calibration/firmware/samba_deployment.yaml` does not exist, warn the user and skip this step.

### 6. Print summary

Show the version, MD5 hash, and a `jq` summary of the manifest. Confirm whether the deployment firmware was updated.

### 7. Git tag and push (only if `--tag`)

In the **samba** repository:

```bash
git add firmware/samba_v<VERSION>.bin firmware/manifest.json
git commit -m "Release v<VERSION>: <SUMMARY>"
git tag -a "v<VERSION>" -m "<SUMMARY>"
git push
git push --tags
```

If the deployment firmware was updated in step 5, also commit in the **samba_calibration** repository:

```bash
cd ../samba_calibration
git add firmware/samba_deployment.yaml
git commit -m "bump production firmware to v<VERSION>"
git push
cd ../samba
```

Note `main` has a branch-protection rule requiring pull requests. Pushing directly succeeds
for accounts with bypass permission but is reported as `Bypassed rule violations`. If that
rule should be respected, branch and open a PR instead of pushing to `main`.

### 8. Create the GitHub release (only if `--tag`)

**Do not skip this.** Pushing a tag does *not* create a release, and `manifest.json`'s
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

The fleet polls `manifest.json` on `main` every 12h and applies updates Mondays 04:00 UTC, so
a bad hash means every device silently rejects the update:

```bash
curl -sL "https://raw.githubusercontent.com/IEQLab/samba/main/firmware/samba_v<VERSION>.bin" \
  | md5   # must equal the md5 in firmware/manifest.json
```

**Any recompile invalidates the committed binary and manifest md5** — ESPHome embeds a build
timestamp, so builds are never byte-reproducible. If you rebuild after step 3, redo steps 3–5
so `firmware/` holds exactly the binary you validated on hardware.

If `--dry-run`, print what would happen at each step instead of executing.
