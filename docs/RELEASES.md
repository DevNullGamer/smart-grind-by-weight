# Release Guide

This project uses automated releases triggered by Git tags. Here's how it works:

### Branch Strategy
- `main` branch: Stable releases only
- `dev` branch or feature branches: Active development
- Pull requests: Reviewed before merging to main

### Creating Releases

When you've tested your code and want to create a release:

```bash
# Use the integrated release tool (recommended)
python3 tools/grinder.py release
```

The release tool will:
1. Show you recent commits since the last release (for your reference only; they are not copied into the release)
2. Let you choose version increment type (patch/minor/major/RC/custom)
3. Update firmware version in `src/config/build_info.h` and commit the change
4. Create annotated git tag
5. Push both the commit and tag to trigger automated GitHub Actions


### What Happens Automatically

Once you push a tag (like `v1.0.0`):

1. **GitHub Actions builds the firmware**
   - Compiles for the ESP32-S3 target with the committed version
   - Creates release artifacts (.bin files and packages)
   - Generates OTA patches with heatshrink compression

2. **Updates Web Flasher**
   - Generates the ESP Web Tools manifest alongside bootloader, partition, OTA, and blank NVS images
   - Uploads each artifact to the GitHub Release
   - The hosted web flasher queries GitHub releases directly, so no binary files are committed back to `main`

3. **Creates GitHub Release (as draft)**
   - Uses fixed release notes (installation instructions and build info); no changelog is generated, so add highlights by hand when publishing if wanted
   - Uploads firmware binaries for download
   - Marks as pre-release if tag contains `beta`, `alpha`, or `rc`
   - **You must manually publish the draft release**

### Version Numbering

We use semantic versioning (semver):
- `v1.0.0` - Major release (breaking changes)
- `v1.1.0` - Minor release (new features)
- `v1.0.1` - Patch release (bug fixes)
- `v1.0.0-beta.1` - Pre-release (automatically marked as pre-release)

### Pre-releases

Tags containing `beta`, `alpha`, or `rc` are automatically marked as pre-releases:
- `v1.2.0-beta.1` - Beta release
- `v1.2.0-rc.1` - Release candidate
- `v1.2.0-alpha.1` - Alpha release

This keeps experimental versions separate from stable releases.

### Update index for WiFi updates (stable and beta)

The grinder can download releases itself (Menu → WiFi → Firmware). It does not read the GitHub API; it reads two small files from GitHub Pages:

- `update-stable.json`: the highest non-prerelease release.
- `update-beta.json`: the highest release overall, including `-rc.N` / `-beta.N`.

**How they are made:** the `deploy-web-flasher.yml` workflow downloads every published release into `tools/web-flasher/firmware/<tag>/`, writes the web flasher's `firmware/index.json`, and then runs `tools/update_index.py`, which writes both files next to `index.html` (the Pages root). Each file has the version, tag, size, SHA-256 and the URL of `smart-grind-by-weight-<tag>.bin` (the raw image, not the `-web-ota.bin` patch). If no release qualifies the file only says `"none": true` and the grinder shows "No release published". Schema and parser rules: `docs/DESIGN-NOTES.md`, section "Release index and SemVer".

**Which releases are offered:** only published (not draft) releases whose manifest is the unified **auto-detect** image (V1 and V2). Older single-board releases are never offered over WiFi. Tags must look like `v1.5.0` or `v1.5.0-rc.2` (SemVer; a prerelease is anything with a `-` part). Ordering is SemVer: `1.4.1-rc.3 < 1.4.1-rc.10 < 1.4.1 < 1.5.0-beta < 1.5.0`. A grinder running a newer version than the channel head (for example an RC of the next version) is told "Up to date", never offered a downgrade.

**When it runs (Pages deploy trigger):** the workflow runs when a release is published, unpublished or deleted, when `tools/web-flasher/**` changes on `main`, and manually (workflow_dispatch). So a draft release is invisible until you publish it, and the index files appear on Pages only after the first deploy on `main` that includes this feature. Until then the menu Check shows "Check failed" or "No release published"; Bluetooth and `wifi-upload` are not affected. This path has not been exercised against the real Pages site yet (host-tested only).

**Trust:** the index and images are as trustworthy as the Pages deployment; there is no firmware signing. The device verifies the SHA-256 from the index and the TLS certificate chain (no expiry check, the grinder has no clock).

**Rollback caveat:** a new image is confirmed after 30 s of stable running, otherwise the grinder returns to the previous firmware. The **first** update from a firmware that predates this feature is not rollback-protected (the old firmware confirms the new image at once); mention this in the release notes of the first release that contains it.
