# /bump — Version bump and release

Compile firmware, copy binary, generate manifest, and optionally tag+push a release.

## Usage

```
/bump <VERSION> "<SUMMARY>" [--tag] [--no-compile] [--dry-run]
```

## Arguments

- `VERSION` (required) — Semver-style dot-separated number (e.g. `1.99.95`)
- `SUMMARY` (required) — Short release description
- `--tag` — Git add, commit, create annotated tag `v<VERSION>`, and push
- `--no-compile` — Skip `esphome compile`, use existing build output
- `--dry-run` — Preview all actions without writing files, committing, or pushing

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

If `--dry-run`, print what would happen at each step instead of executing.
