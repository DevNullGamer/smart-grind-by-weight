// WiFi Setup tab: stores WiFi credentials and a freshly generated update token on the grinder over Bluetooth.
//
// The first half is pure (no DOM, no Bluetooth) so it can be unit-tested with `node --test` and is the twin of
// tools/wifi_provision.py. Both are checked against tools/tests/wifi_provision_vectors.json.
// The second half is the browser UI and only runs in a page.
//
// Privacy rules for this file (checked by the unit test): the password and token are never logged (no console
// output at all), never put into any browser storage and never sent anywhere except the Bluetooth write to the grinder.

'use strict';

// ---------------------------------------------------------------------------------------------------------------
// Pure part
// ---------------------------------------------------------------------------------------------------------------

const WIFI_CMD_DEBUG_ENABLE = 0x01;
const WIFI_CMD_DEBUG_DISABLE = 0x02;
const WIFI_CMD_SET = 0x04;
const WIFI_CMD_CLEAR = 0x05;

const WIFI_SSID_MAX_BYTES = 32;
const WIFI_PASSWORD_MIN_BYTES = 8;
const WIFI_PASSWORD_MAX_BYTES = 63;
const WIFI_TOKEN_LENGTH = 16;
const WIFI_DEFAULT_HOST = 'grindbyweight.local';

const WIFI_REPLY = Object.freeze({
    STORED: 'stored',
    CLEARED: 'cleared',
    LOCKED: 'locked',
    REJECTED: 'rejected',
    STORE_FAILED: 'store_failed',
    UNKNOWN: 'unknown'
});

class WifiSetupError extends Error {}

function wifiUtf8(text, what) {
    if (typeof text !== 'string') {
        throw new WifiSetupError(`${what} must be text`);
    }
    const bytes = new TextEncoder().encode(text);
    if (bytes.includes(0)) {
        throw new WifiSetupError(`${what} must not contain NUL characters`);
    }
    return bytes;
}

function encodeSsid(ssid) {
    const bytes = wifiUtf8(ssid, 'SSID');
    if (bytes.length < 1 || bytes.length > WIFI_SSID_MAX_BYTES) {
        throw new WifiSetupError(`SSID must be 1-${WIFI_SSID_MAX_BYTES} bytes (it is ${bytes.length} bytes in UTF-8)`);
    }
    return bytes;
}

// Empty password = open network, otherwise 8-63 bytes
function encodePassword(password) {
    const bytes = wifiUtf8(password, 'Password');
    if (bytes.length > 0 && (bytes.length < WIFI_PASSWORD_MIN_BYTES || bytes.length > WIFI_PASSWORD_MAX_BYTES)) {
        throw new WifiSetupError(
            `Password must be empty (open network) or ${WIFI_PASSWORD_MIN_BYTES}-${WIFI_PASSWORD_MAX_BYTES} bytes ` +
            `(it is ${bytes.length} bytes in UTF-8)`);
    }
    return bytes;
}

function isValidToken(token) {
    return typeof token === 'string' && /^[0-9a-f]{16}$/.test(token);
}

// 16 lowercase hex characters from the browser's secure random source
function generateToken(cryptoObject) {
    const source = cryptoObject || (typeof crypto !== 'undefined' ? crypto : null);
    if (!source || typeof source.getRandomValues !== 'function') {
        throw new WifiSetupError('No secure random source available in this browser');
    }
    const bytes = new Uint8Array(WIFI_TOKEN_LENGTH / 2);
    source.getRandomValues(bytes);
    return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

// [0x04][ssid_len][ssid][pass_len][pass][token16]
function buildSetFrame(ssid, password, token) {
    const ssidBytes = encodeSsid(ssid);
    const passBytes = encodePassword(password);
    if (!isValidToken(token)) {
        throw new WifiSetupError('Token must be 16 lowercase hex characters');
    }
    const frame = new Uint8Array(1 + 1 + ssidBytes.length + 1 + passBytes.length + WIFI_TOKEN_LENGTH);
    let pos = 0;
    frame[pos++] = WIFI_CMD_SET;
    frame[pos++] = ssidBytes.length;
    frame.set(ssidBytes, pos);
    pos += ssidBytes.length;
    frame[pos++] = passBytes.length;
    frame.set(passBytes, pos);
    pos += passBytes.length;
    for (let i = 0; i < WIFI_TOKEN_LENGTH; i++) {
        frame[pos++] = token.charCodeAt(i);
    }
    return frame;
}

function buildClearFrame() {
    return new Uint8Array([WIFI_CMD_CLEAR]);
}

function wifiBytesToHex(bytes) {
    return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

// Maps the grinder's debug text (BluetoothManager::handle_wifi_command) to a WIFI_REPLY value.
// Order matters: the "already stored" refusal and "could not be stored" both contain the word "stored".
function classifyReply(text) {
    if (!text) return WIFI_REPLY.UNKNOWN;
    if (text.includes('credentials already stored') || text.includes('Open Menu -> WiFi')) return WIFI_REPLY.LOCKED;
    if (text.includes('could not be stored') || text.includes('clear failed')) return WIFI_REPLY.STORE_FAILED;
    if (text.includes('credentials stored')) return WIFI_REPLY.STORED;
    if (text.includes('credentials cleared')) return WIFI_REPLY.CLEARED;
    if (text.includes('rejected')) return WIFI_REPLY.REJECTED;
    return WIFI_REPLY.UNKNOWN;
}

// Hardware-info JSON from the BLE system-info service -> {connected, ip, host}, or null for stale/cut-off/odd data
function parseHardwareInfo(text) {
    let data;
    try {
        data = JSON.parse(text);
    } catch (e) {
        return null;
    }
    if (data === null || typeof data !== 'object' || Array.isArray(data) || !('wifi' in data)) {
        return null;
    }
    const ip = typeof data.ip === 'string' ? data.ip : '';
    const host = typeof data.host === 'string' ? data.host : '';
    return { connected: data.wifi === true && ip !== '', ip: ip, host: host };
}

// Content of ~/.grinder-wifi.json for `grinder.py wifi-upload` (same bytes as tools/wifi/grinder-wifi.py writes)
function buildConfigJson(token, host) {
    if (!isValidToken(token)) {
        throw new WifiSetupError('Token must be 16 lowercase hex characters');
    }
    return JSON.stringify({ host: host || WIFI_DEFAULT_HOST, token: token }, null, 2) + '\n';
}

// "New token" must not replace the field while a transfer runs or after the grinder accepted the shown token
function newTokenAllowed(busy, tokenSent) {
    return !busy && !tokenSent;
}

function maskToken(token) {
    if (typeof token !== 'string' || token.length === 0) return '(none)';
    return token.length > 4 ? token.slice(0, 2) + '*'.repeat(token.length - 4) + token.slice(-2) : '*'.repeat(token.length);
}

if (typeof module !== 'undefined' && module.exports) {
    module.exports = {
        WIFI_REPLY, WifiSetupError, encodeSsid, encodePassword, isValidToken, generateToken, buildSetFrame,
        buildClearFrame, wifiBytesToHex, classifyReply, parseHardwareInfo, buildConfigJson, maskToken,
        newTokenAllowed
    };
}

// ---------------------------------------------------------------------------------------------------------------
// Browser part (Web Bluetooth + page elements). Does nothing outside a page.
// ---------------------------------------------------------------------------------------------------------------

if (typeof document !== 'undefined' && typeof window !== 'undefined') {
    (function () {
        const DEBUG_SERVICE = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
        const DEBUG_RX = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
        const DEBUG_TX = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';
        const SYSINFO_SERVICE = '77889900-aabb-ccdd-eeff-112233445566';
        const SYSINFO_HARDWARE = '00112233-ddee-ff11-2233-445566778899';
        const OTA_SERVICE = '12345678-1234-1234-1234-123456789abc';
        const DEVICE_NAME_FILTER = 'GrindByWeight';
        const REPLY_TIMEOUT_MS = 5000;
        const CONNECT_WAIT_MS = 40000;   // hardware info refreshes every 10 s, joining takes a few seconds
        const CONNECT_POLL_MS = 3000;

        let currentToken = '';
        let busy = false;
        let tokenSent = false;   // the grinder has the token shown in the field

        const el = (id) => document.getElementById(id);
        const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

        function setStatus(message, type) {
            const box = el('wifiStatus');
            box.textContent = message;
            box.className = 'status ' + (type || 'info');
            box.style.display = 'block';
        }

        function updateButtons() {
            el('wifiSendBtn').disabled = busy;
            el('wifiClearBtn').disabled = busy;
            el('wifiNewTokenBtn').disabled = !newTokenAllowed(busy, tokenSent);
        }

        function setBusy(value) {
            busy = value;
            updateButtons();
        }

        function refreshToken() {
            currentToken = generateToken();
            el('wifiToken').value = currentToken;
        }

        // The page element is looked up lazily: this file is loaded after the markup, but keep it robust
        function initWifiTab() {
            if (!el('wifiTab')) return;
            refreshToken();
            el('wifiShowPassword').addEventListener('change', (event) => {
                el('wifiPassword').type = event.target.checked ? 'text' : 'password';
            });
            el('wifiSendBtn').addEventListener('click', sendCredentials);
            el('wifiClearBtn').addEventListener('click', clearCredentials);
            el('wifiCopyBtn').addEventListener('click', copyToken);
            el('wifiDownloadBtn').addEventListener('click', downloadConfig);
            el('wifiNewTokenBtn').addEventListener('click', () => {
                if (!newTokenAllowed(busy, tokenSent)) return;
                refreshToken();
                setStatus('New token generated. It is only used when you press "Send to grinder".', 'info');
            });
            if (!('bluetooth' in navigator)) {
                setStatus('This browser has no Web Bluetooth. Use Chrome or Edge (Chrome on Android also works).', 'error');
                setBusy(true);
            }
        }

        async function copyToken() {
            try {
                await navigator.clipboard.writeText(currentToken);
                setStatus('Token copied to the clipboard.', 'success');
            } catch (e) {
                el('wifiToken').select();
                setStatus('Could not copy automatically. The token is selected: press Ctrl+C.', 'warning');
            }
        }

        function downloadConfig() {
            const blob = new Blob([buildConfigJson(currentToken, WIFI_DEFAULT_HOST)], { type: 'application/json' });
            const url = URL.createObjectURL(blob);
            const link = document.createElement('a');
            link.href = url;
            link.download = '.grinder-wifi.json';
            document.body.appendChild(link);
            link.click();
            link.remove();
            URL.revokeObjectURL(url);
            setStatus('Downloaded the config file. Your browser may have renamed it (for example without the leading dot): ' +
                      'save it as ~/.grinder-wifi.json to use wifi-upload.', 'success');
        }

        // Connects and returns {device, rx, tx, hardware, replyText()}; the caller must call close()
        async function openLink() {
            const device = await navigator.bluetooth.requestDevice({
                filters: [{ name: DEVICE_NAME_FILTER }],
                optionalServices: [OTA_SERVICE, DEBUG_SERVICE, SYSINFO_SERVICE]
            });
            const server = await device.gatt.connect();
            const debugService = await server.getPrimaryService(DEBUG_SERVICE);
            const rx = await debugService.getCharacteristic(DEBUG_RX);
            const tx = await debugService.getCharacteristic(DEBUG_TX);
            let hardware = null;
            try {
                const sysinfo = await server.getPrimaryService(SYSINFO_SERVICE);
                hardware = await sysinfo.getCharacteristic(SYSINFO_HARDWARE);
            } catch (e) {
                hardware = null;   // verification step is optional
            }

            let text = '';
            const decoder = new TextDecoder();
            tx.addEventListener('characteristicvaluechanged', (event) => {
                text += decoder.decode(event.target.value, { stream: true });
            });
            await tx.startNotifications();

            return {
                device: device,
                rx: rx,
                tx: tx,
                hardware: hardware,
                resetReply: () => { text = ''; },
                replyText: () => text,
                close: () => { try { if (device.gatt.connected) device.gatt.disconnect(); } catch (e) { /* ignore */ } }
            };
        }

        // Write with response (older Chrome versions only have writeValue, which also waits for the response)
        function writeWithResponse(characteristic, bytes) {
            return typeof characteristic.writeValueWithResponse === 'function'
                ? characteristic.writeValueWithResponse(bytes)
                : characteristic.writeValue(bytes);
        }

        // Writes a frame (with response; a 114 byte frame becomes a GATT long write on a 23 byte ATT MTU)
        // and returns the classified reply
        async function sendFrame(link, frame) {
            await writeWithResponse(link.rx, new Uint8Array([WIFI_CMD_DEBUG_ENABLE]));
            await sleep(500);
            link.resetReply();
            await writeWithResponse(link.rx, frame);
            const deadline = Date.now() + REPLY_TIMEOUT_MS;
            let reply = WIFI_REPLY.UNKNOWN;
            while (Date.now() < deadline) {
                reply = classifyReply(link.replyText());
                if (reply !== WIFI_REPLY.UNKNOWN) break;
                await sleep(100);
            }
            try {
                await writeWithResponse(link.rx, new Uint8Array([WIFI_CMD_DEBUG_DISABLE]));
            } catch (e) { /* the link may already be closing */ }
            return reply;
        }

        async function waitForWifi(link) {
            if (!link.hardware) return null;
            const deadline = Date.now() + CONNECT_WAIT_MS;
            while (Date.now() < deadline) {
                try {
                    const value = await link.hardware.readValue();
                    const info = parseHardwareInfo(new TextDecoder().decode(value));
                    if (info && info.connected) return info;
                } catch (e) { /* stale or unreadable: try again */ }
                await sleep(CONNECT_POLL_MS);
            }
            return false;
        }

        function explainFailure(reply) {
            switch (reply) {
                case WIFI_REPLY.LOCKED:
                    return 'The grinder already has WiFi credentials. On the grinder open Menu -> WiFi, then try again.';
                case WIFI_REPLY.REJECTED:
                    return 'The grinder rejected the data (check SSID and password length).';
                case WIFI_REPLY.STORE_FAILED:
                    return 'The grinder could not store the credentials.';
                default:
                    return 'No reply from the grinder. The credentials may or may not have been stored. ' +
                           'If the grinder was already set up, open Menu -> WiFi on it and try again.';
            }
        }

        function describeBluetoothError(error) {
            if (error && error.name === 'NotFoundError') {
                return 'No grinder selected. Turn Bluetooth on in Menu -> Bluetooth on the grinder and try again.';
            }
            return 'Bluetooth error: ' + (error && error.message ? error.message : 'unknown');
        }

        async function sendCredentials() {
            if (busy) return;
            const ssid = el('wifiSsid').value;
            const password = el('wifiPassword').value;
            let frame;
            try {
                frame = buildSetFrame(ssid, password, currentToken);
            } catch (error) {
                setStatus(error.message, 'error');
                return;
            }
            setBusy(true);
            let link = null;
            try {
                setStatus('Choose your grinder in the Bluetooth dialog...', 'info');
                link = await openLink();
                setStatus('Connected. Sending WiFi details...', 'info');
                const reply = await sendFrame(link, frame);
                if (reply !== WIFI_REPLY.STORED) {
                    setStatus(explainFailure(reply), 'error');
                    return;
                }
                tokenSent = true;   // the field now shows the grinder's token: it must not be replaced
                el('wifiTokenNote').style.display = 'block';
                setStatus('Stored on the grinder. Save the token now (Copy or Download config): it is shown only here. ' +
                          'Checking the WiFi connection...', 'success');
                const info = await waitForWifi(link);
                if (info) {
                    setStatus(`Connected to WiFi, IP ${info.ip}. Save the token now (Copy or Download config).`, 'success');
                } else if (info === null) {
                    setStatus('Stored on the grinder. Connection check is not available; see Menu -> WiFi on the grinder.', 'warning');
                } else {
                    setStatus('Saved, not connected yet. Check the password and the signal strength (2.4 GHz only); ' +
                              'Menu -> WiFi on the grinder shows the status.', 'warning');
                }
            } catch (error) {
                setStatus(describeBluetoothError(error), 'error');
            } finally {
                if (link) link.close();
                setBusy(false);
            }
        }

        async function clearCredentials() {
            if (busy) return;
            setBusy(true);
            let link = null;
            try {
                setStatus('Choose your grinder in the Bluetooth dialog...', 'info');
                link = await openLink();
                const reply = await sendFrame(link, buildClearFrame());
                if (reply === WIFI_REPLY.CLEARED) {
                    setStatus('WiFi details removed from the grinder.', 'success');
                } else if (reply === WIFI_REPLY.LOCKED) {
                    setStatus('On the grinder open Menu -> WiFi, then try again.', 'error');
                } else {
                    setStatus('The grinder did not confirm. Open Menu -> WiFi on it and try again.', 'error');
                }
            } catch (error) {
                setStatus(describeBluetoothError(error), 'error');
            } finally {
                if (link) link.close();
                setBusy(false);
            }
        }

        if (document.readyState === 'loading') {
            document.addEventListener('DOMContentLoaded', initWifiTab);
        } else {
            initWifiTab();
        }
    })();
}
