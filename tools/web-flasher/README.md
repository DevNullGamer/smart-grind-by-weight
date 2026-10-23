# Smart Grind By Weight - Web Flasher

A browser-based firmware flashing tool for the Smart Grind By Weight ESP32 coffee grinder.

## Features

### 🔌 Initial Setup (USB)
- First-time firmware installation via ESP Web Tools
- Uses Web Serial API for direct USB connection
- Perfect for factory setup or recovery
- Powered by [ESP Web Tools](https://esphome.github.io/esp-web-tools/) for browser-based flashing

### 📶 OTA Updates (Bluetooth)
- Over-the-air updates for installed grinders
- Web Bluetooth API for wireless connection
- Full firmware updates (no delta compression)
- Progress tracking and status updates

### 📡 WiFi Setup (Bluetooth)
- Stores your WiFi name and password on the grinder and generates the update token used by `python3 tools/grinder.py wifi-upload`
- Logic in `wifi-setup.js` (pure frame builder, unit-tested with `node --test tools/tests/wifi-setup.test.js`)

## Browser Support

- ✅ **Chrome** (Desktop & Android) - Full support
- ✅ **Microsoft Edge** (Desktop) - Full support  
- ❌ **Firefox** - No Web Bluetooth support
- ❌ **Safari/iOS** - No Web Bluetooth support

## Usage

### For Initial Setup
1. Open the web flasher in Chrome/Edge
2. Go to "Initial Setup (USB)" tab
3. Enter firmware URL from GitHub release
4. Click "Flash via USB" - opens ESP Web Tools
5. Connect device via USB and flash

### For OTA Updates
1. Ensure grinder is powered and BLE enabled
2. Go to "OTA Update (BLE)" tab  
3. Enter firmware URL from GitHub release
4. Click "Connect to Device"
5. Click "Flash Firmware" when connected

### For WiFi Setup
1. Turn Bluetooth on at the grinder (Menu → Bluetooth). If the grinder already has WiFi details, open **Menu → WiFi** on it first; otherwise it refuses changes
2. Go to the "WiFi Setup" tab, enter the network name and password (empty password = open network; 2.4 GHz networks only)
3. Press "Send to grinder (Bluetooth)" and choose the grinder in the browser dialog
4. The page shows "Connected to WiFi, IP ..." or "Saved, not connected yet" (check password and signal)
5. **Save the token now** with "Copy" or "Download config" (a file for `~/.grinder-wifi.json`; the browser may rename the download, for example drop the leading dot, so save it as `~/.grinder-wifi.json`). It is shown only on this page and the grinder never shows it again. Then turn on **Receive** in Menu → WiFi when you want to push an update
6. "Remove WiFi from grinder" deletes the stored details (also needs Menu → WiFi open once the grinder is set up)

What the page does with your data:
- The password and token only travel over Bluetooth to the grinder (and the token to your clipboard/download if you press those buttons). Nothing is sent to any server, nothing is stored in the browser, and nothing is written to the console
- The token is generated in the browser with `crypto.getRandomValues` (16 lowercase hex characters)
- The Bluetooth link is not encrypted, so do this at home; anyone within radio range could read the details while they are being sent
- The page cannot check the grinder over WiFi (this page is HTTPS, the grinder speaks plain HTTP); it verifies through Bluetooth instead (hardware info reports the WiFi state and IP)

Troubleshooting: "No grinder selected" means the dialog was closed or Bluetooth is off on the grinder. "Already has WiFi credentials" means Menu → WiFi was not open. "No reply" means the grinder did not answer within 5 s; the details may or may not be stored, open Menu → WiFi and try again.

## Firmware Sources

The firmware list is pulled straight from GitHub Releases—no files are stored in this repo. If you need the exact asset mapping, see [DOC.md](../../docs/DOC.md).

## Technical Details

### BLE Services Used
- **OTA Service**: `12345678-1234-1234-1234-123456789abc`
- **Data Transfer**: `87654321-4321-4321-4321-cba987654321`
- **Control Commands**: `11111111-2222-3333-4444-555555555555`
- **Status Updates**: `aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee`

### Protocol
- Based on existing Python BLE implementation
- 512-byte chunks for firmware transfer
- Status notifications for progress tracking
- Command structure: START → DATA_CHUNKS → END

## Development

The web flasher is automatically deployed via GitHub Pages when pushed to main branch.

### Local Testing

**Quick Start (Recommended):**
```bash
# From the tools directory
python3 start-webflasher.py

# If port 8000 is busy, you'll be prompted to kill the process
# Use a custom port
python3 start-webflasher.py --port 3000
```

The script will automatically:
- Check if the port is available
- Prompt to kill any conflicting process
- Start the server and display the URL
- Handle cleanup on exit

**Manual Start:**
```bash
# Serve locally (required for Web Bluetooth HTTPS requirement)
python3 -m http.server 8000 --directory tools/web-flasher
# Open http://localhost:8000
```

**Note:** While the production site requires HTTPS for Web Bluetooth, `localhost` is an exception and works with plain HTTP.

## Security

- All communications use Web Bluetooth's built-in security
- Firmware is downloaded directly from GitHub releases
- No credentials or keys stored locally (the WiFi Setup tab keeps nothing in the browser)
