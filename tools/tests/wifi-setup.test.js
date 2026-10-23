// node --test tools/tests/wifi-setup.test.js
// Checks the pure functions of tools/web-flasher/wifi-setup.js against the vectors shared with the Python tool
// (tools/tests/wifi_provision_vectors.json) and the firmware native test.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const wifi = require('../web-flasher/wifi-setup.js');

const vectors = JSON.parse(fs.readFileSync(path.join(__dirname, 'wifi_provision_vectors.json'), 'utf8'));
const TOKEN = '0123456789abcdef';

test('frames are byte-identical to the shared vectors', () => {
    for (const v of vectors.set_frames) {
        const frame = wifi.buildSetFrame(v.ssid, v.password, v.token);
        assert.equal(wifi.wifiBytesToHex(frame), v.frame_hex, v.name);
    }
});

test('clear frame', () => {
    assert.equal(wifi.wifiBytesToHex(wifi.buildClearFrame()), vectors.clear_frame_hex);
});

test('max frame is 114 bytes', () => {
    assert.equal(wifi.buildSetFrame('s'.repeat(32), 'p'.repeat(63), TOKEN).length, 114);
});

test('SSID limits count UTF-8 bytes', () => {
    assert.throws(() => wifi.buildSetFrame('', 'secret123', TOKEN), wifi.WifiSetupError);
    assert.throws(() => wifi.buildSetFrame('a'.repeat(33), 'secret123', TOKEN), wifi.WifiSetupError);
    assert.ok(wifi.buildSetFrame('a'.repeat(32), 'secret123', TOKEN));
    assert.ok(wifi.buildSetFrame('é'.repeat(16), 'secret123', TOKEN));
    assert.throws(() => wifi.buildSetFrame('é'.repeat(17), 'secret123', TOKEN), wifi.WifiSetupError);
});

test('password rules', () => {
    assert.ok(wifi.buildSetFrame('net', '', TOKEN));
    for (const bad of ['a', '1234567', 'p'.repeat(64), 'ä'.repeat(32)]) {
        assert.throws(() => wifi.buildSetFrame('net', bad, TOKEN), wifi.WifiSetupError, bad.length + ' chars');
    }
    assert.ok(wifi.buildSetFrame('net', '12345678', TOKEN));
    assert.ok(wifi.buildSetFrame('net', 'p'.repeat(63), TOKEN));
    assert.ok(wifi.buildSetFrame('net', 'ä'.repeat(31), TOKEN));
});

test('NUL and non-text input are rejected', () => {
    assert.throws(() => wifi.buildSetFrame('ne\u0000t', 'secret123', TOKEN), wifi.WifiSetupError);
    assert.throws(() => wifi.buildSetFrame('net', 'secret\u0000123', TOKEN), wifi.WifiSetupError);
    assert.throws(() => wifi.buildSetFrame(null, 'secret123', TOKEN), wifi.WifiSetupError);
    assert.throws(() => wifi.buildSetFrame('net', undefined, TOKEN), wifi.WifiSetupError);
});

test('bad tokens are rejected', () => {
    for (const bad of ['', '0123456789abcde', '0123456789abcdef0', '0123456789ABCDEF', '0123456789abcdeg', null, 12345]) {
        assert.throws(() => wifi.buildSetFrame('net', 'secret123', bad), wifi.WifiSetupError);
    }
});

test('error messages do not contain the secrets', () => {
    try {
        wifi.buildSetFrame('net', 'short', TOKEN);
        assert.fail('should throw');
    } catch (error) {
        assert.ok(!error.message.includes('short'));
    }
    try {
        wifi.buildSetFrame('net', 'secret123', 'ABCDEF');
        assert.fail('should throw');
    } catch (error) {
        assert.ok(!error.message.includes('ABCDEF'));
    }
});

test('generateToken uses the supplied random source and formats 16 lowercase hex characters', () => {
    const fake = { getRandomValues(bytes) { bytes.set([0x00, 0x0f, 0xa5, 0xff, 0x10, 0x20, 0x30, 0x40]); return bytes; } };
    assert.equal(wifi.generateToken(fake), '000fa5ff10203040');
    const real = new Set();
    for (let i = 0; i < 50; i++) {
        const token = wifi.generateToken();
        assert.ok(wifi.isValidToken(token));
        real.add(token);
    }
    assert.equal(real.size, 50);
    assert.throws(() => wifi.generateToken({}), wifi.WifiSetupError);
});

test('classifyReply matches the firmware messages (same table as the Python tests)', () => {
    const R = wifi.WIFI_REPLY;
    const cases = [
        ['BLE_DEBUG: WiFi credentials stored, connecting\n', R.STORED],
        ['BLE_DEBUG: WiFi credentials cleared\n', R.CLEARED],
        ['BLE_DEBUG: WiFi command rejected, credentials already stored. Open Menu -> WiFi first.\n', R.LOCKED],
        ['BLE_DEBUG: WiFi set rejected, bad payload (3), expected [0x04, ssid_len, ssid, pass_len, pass, token16]\n', R.REJECTED],
        ['BLE_DEBUG: WiFi set rejected, invalid credentials\n', R.REJECTED],
        ['BLE_DEBUG: WiFi clear rejected, expected [0x05]\n', R.REJECTED],
        ['BLE_DEBUG: WiFi credentials could not be stored\n', R.STORE_FAILED],
        ['BLE_DEBUG: WiFi clear failed (storage error)\n', R.STORE_FAILED],
        ['BLE_DEBUG: Stream enabled\n', R.UNKNOWN],
        ['', R.UNKNOWN],
        [null, R.UNKNOWN]
    ];
    for (const [text, expected] of cases) {
        assert.equal(wifi.classifyReply(text), expected, String(text));
    }
});

test('parseHardwareInfo', () => {
    const ok = '{"wifi_available":true,"wifi":true,"ip":"192.168.1.20","host":"grindbyweight.local"}';
    assert.deepEqual(wifi.parseHardwareInfo(ok), { connected: true, ip: '192.168.1.20', host: 'grindbyweight.local' });
    assert.equal(wifi.parseHardwareInfo('{"wifi":false,"ip":"","host":"grindbyweight.local"}').connected, false);
    assert.equal(wifi.parseHardwareInfo('{"wifi":true,"ip":""}').connected, false);
    for (const bad of ['', 'not json', '{"wifi":tr', '[1,2]', '{"load_cell_active":true}', 'null']) {
        assert.equal(wifi.parseHardwareInfo(bad), null, bad);
    }
});

test('config JSON equals what grinder-wifi.py writes (indent 2, trailing newline)', () => {
    assert.equal(wifi.buildConfigJson(TOKEN),
        '{\n  "host": "grindbyweight.local",\n  "token": "0123456789abcdef"\n}\n');
    assert.throws(() => wifi.buildConfigJson('nope'), wifi.WifiSetupError);
});

test('New token is only allowed while idle and before the grinder accepted the shown token', () => {
    assert.equal(wifi.newTokenAllowed(false, false), true);
    assert.equal(wifi.newTokenAllowed(true, false), false);
    assert.equal(wifi.newTokenAllowed(false, true), false);
    assert.equal(wifi.newTokenAllowed(true, true), false);
});

test('maskToken hides the middle', () => {
    const masked = wifi.maskToken(TOKEN);
    assert.ok(masked.startsWith('01') && masked.endsWith('ef') && !masked.includes('456789'));
    assert.equal(wifi.maskToken(null), '(none)');
});

test('module has no console output, storage or network access', () => {
    const source = fs.readFileSync(path.join(__dirname, '../web-flasher/wifi-setup.js'), 'utf8');
    assert.ok(!/console\./.test(source));
    assert.ok(!/localStorage|sessionStorage|document\.cookie|fetch\(|XMLHttpRequest|sendBeacon/.test(source));
});
