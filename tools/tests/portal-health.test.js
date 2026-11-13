// node --test tools/tests/portal-health.test.js
// Checks the Health card of the Device section (chg021 G3c): web/portal/portal-view.js (diagnosticsView and the polling
// rules) and its wiring in portal-app.js / page.src.html. The contract is docs/PORTAL-API.md, "Diagnostics".
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const PV = require('../../web/portal/portal-view.js');

const ROOT = path.join(__dirname, '..', '..');
const ONLINE = { state: 'online' };
const OFFLINE = { state: 'offline' };

function doc(over) {
    return Object.assign({ calibrated: true, cal_factor: 1234.56, std_dev_g: 0.012, noise_ok: true, sample_rate_sps: 10.0,
        motor_latency_ms: 50, wifi_rssi_dbm: -58, ble: 'off', board: 'V2', board_source: 'probe', push_armed: false }, over || {});
}

function rowsOf(over) {
    return PV.diagnosticsRows(PV.parseDiagnostics(doc(over)));
}

function row(rows, id) {
    return rows.find((r) => r.id === id);
}

function loaded(over, at) {
    return PV.diagnosticsLoaded(PV.newStats(), doc(over), at === undefined ? 0 : at);
}

function allText(view) {
    return [view.message, view.note].concat(view.rows.map((r) => [r.label, r.value, r.note].join('|'))).join('|');
}

// ---------------------------------------------------------------------------------------------- rows

test('the example of the contract gives eight rows in order, with the words of the page', () => {
    const rows = rowsOf();
    assert.deepEqual(rows.map((r) => r.id), ['cal', 'noise', 'rate', 'latency', 'wifi', 'ble', 'push', 'board']);
    assert.deepEqual(rows.map((r) => [r.label, r.value, r.level]), [
        ['Load cell', 'Calibrated', 'ok'],
        ['Noise', 'OK', 'ok'],
        ['Sample rate', '10.0 samples/s', 'plain'],
        ['Motor latency', '50 ms', 'plain'],
        ['WiFi signal', 'Good, -58 dBm', 'ok'],
        ['Bluetooth', 'Off', 'plain'],
        ['Receive updates', 'Off', 'plain'],
        ['Board detection', 'Detected at start-up', 'plain']
    ]);
    assert.equal(row(rows, 'cal').note, 'Calibration factor 1,234.56');
    assert.equal(row(rows, 'noise').note, 'Standard deviation 0.012 g');
    assert.equal(row(rows, 'board').note, '');
    // the board name is the About card's: no row for it here
    assert.equal(rows.some((r) => /^Board$/.test(r.label) || r.value === 'V2'), false);
});

test('not calibrated is a warning with the way out; the factor is shown only for a calibrated cell', () => {
    const r = row(rowsOf({ calibrated: false }), 'cal');
    assert.deepEqual([r.value, r.level], ['Not calibrated', 'warning']);
    assert.match(r.note, /Menu, Tools, Calibrate/);
    assert.doesNotMatch(r.note, /factor/i);
    assert.equal(row(rowsOf({ cal_factor: 0 }), 'cal').note, '');          // 0.0 is "unknown"
    assert.equal(row(rowsOf({ cal_factor: null }), 'cal').note, '');
    assert.equal(row(rowsOf({ cal_factor: -2.5 }), 'cal').note, 'Calibration factor -2.50'); // may be negative
    assert.equal(row(rowsOf({ cal_factor: 1e9 }), 'cal').note, 'Calibration factor 1,000,000,000.00');
});

test('a calibration factor that rounds to zero has no minus sign', () => {
    assert.equal(row(rowsOf({ cal_factor: -0.001 }), 'cal').note, 'Calibration factor 0.00');
});

test('noise: OK or Too high with the standard deviation in grams, decimals by size', () => {
    assert.deepEqual([row(rowsOf({ noise_ok: false }), 'noise').value, row(rowsOf({ noise_ok: false }), 'noise').level], ['Too high', 'warning']);
    const note = (g) => row(rowsOf({ std_dev_g: g }), 'noise').note;
    assert.equal(note(0), 'Standard deviation 0.000 g');
    assert.equal(note(0.5), 'Standard deviation 0.500 g');
    assert.equal(note(0.9996), 'Standard deviation 1.000 g');
    assert.equal(note(12.5), 'Standard deviation 12.50 g');
    assert.equal(note(1234.56), 'Standard deviation 1,234.6 g');
    assert.equal(note(1e9), 'Standard deviation 1,000,000,000.0 g');
    assert.equal(note(null), '');
    assert.equal(note(-0.5), ''); // never negative: an out-of-contract value is not shown
});

test('noise without a verdict shows the reading alone; without either the row is gone', () => {
    const r = row(rowsOf({ noise_ok: undefined }), 'noise');
    assert.deepEqual([r.value, r.level], ['0.012 g', 'plain']);
    assert.equal(row(rowsOf({ noise_ok: 'yes', std_dev_g: 'x' }), 'noise'), undefined);
});

test('sample rate: one decimal with a unit; 0 (unknown), negative and huge values hide or format safely', () => {
    assert.equal(row(rowsOf({ sample_rate_sps: 80 }), 'rate').value, '80.0 samples/s');
    assert.equal(row(rowsOf({ sample_rate_sps: 10.04 }), 'rate').value, '10.0 samples/s');
    assert.equal(row(rowsOf({ sample_rate_sps: 0 }), 'rate'), undefined);
    assert.equal(row(rowsOf({ sample_rate_sps: -10 }), 'rate'), undefined);
    assert.equal(row(rowsOf({ sample_rate_sps: 1e9 }), 'rate').value, '1,000,000,000.0 samples/s');
    assert.equal(row(rowsOf({ sample_rate_sps: 1e300 }), 'rate'), undefined);
});

test('motor latency: whole milliseconds with the contract\'s own words; 0 is shown as 0, not as "not measured"', () => {
    const latency = (v) => row(rowsOf({ motor_latency_ms: v }), 'latency');
    assert.equal(latency(50).value, '50 ms');
    assert.equal(latency(0).value, '0 ms');
    assert.equal(latency(49.6).value, '50 ms');
    assert.equal(latency(1000000).value, '1,000,000 ms');
    assert.equal(latency(-5), undefined);
    assert.equal(latency(null), undefined);
    assert.equal(latency(1e300), undefined);
    assert.equal(latency(50).note, 'How long the motor takes to respond, as the grinder measured it.');
    assert.doesNotMatch(latency(0).note + latency(0).value, /not measured|unknown/i);
});

test('WiFi signal: the touchscreen\'s words at its limits, null and unusable readings hide the row', () => {
    const wifi = (v) => row(rowsOf({ wifi_rssi_dbm: v }), 'wifi');
    const cases = [[-1, 'Excellent', 'ok'], [-55, 'Excellent', 'ok'], [-56, 'Good', 'ok'], [-67, 'Good', 'ok'], [-68, 'Fair', 'plain'],
        [-75, 'Fair', 'plain'], [-76, 'Weak', 'warning'], [-127, 'Weak', 'warning']];
    for (const [dbm, word, level] of cases) {
        assert.deepEqual([wifi(dbm).value, wifi(dbm).level], [word + ', ' + dbm + ' dBm', level], String(dbm));
    }
    assert.equal(wifi(-58.4).value, 'Good, -58 dBm');
    for (const bad of [null, undefined, 0, 5, -128, -1000, NaN, Infinity, -Infinity, '-58', true, [], {}, -0.4]) {
        assert.equal(wifi(bad), undefined, String(bad));
    }
    // the limits are the ones of the grinder's WiFi page (src/network/wifi_status_text.cpp, wifi_rssi_quality)
    const src = fs.readFileSync(path.join(ROOT, 'src', 'network', 'wifi_status_text.cpp'), 'utf8');
    for (const q of PV.WIFI_QUALITY.filter((x) => x.min !== -Infinity)) {
        assert.ok(src.includes('rssi_dbm >= ' + q.min + ') return "' + q.word + '"'), q.word);
    }
    assert.ok(src.includes('return "Weak"'));
});

test('Bluetooth: a sentence for each of the six words', () => {
    const ble = (v) => row(rowsOf({ ble: v }), 'ble');
    const words = { off: 'Off', advertising: 'On, waiting for a connection', connected: 'On, connected',
        link_listening: 'GaggiMate link, waiting for a client', link_connected: 'GaggiMate link, client connected',
        link_linked: 'GaggiMate link, client linked' };
    assert.deepEqual(Object.keys(PV.BLE_TEXT).sort(), Object.keys(words).sort());
    for (const [word, text] of Object.entries(words)) {
        assert.equal(ble(word).value, text, word);
        assert.equal(ble(word).level, 'plain');
    }
});

test('an unknown Bluetooth word is shown as received, cut like an unknown warning code; unusable ones hide the row', () => {
    assert.equal(row(rowsOf({ ble: 'mesh_pairing' }), 'ble').value, 'mesh_pairing');
    assert.equal(row(rowsOf({ ble: '  padded  ' }), 'ble').value, 'padded');
    assert.equal(row(rowsOf({ ble: 'x'.repeat(100) }), 'ble').value, 'x'.repeat(PV.WARNING_CODE_MAX));
    const emoji = '\u{1F600}'.repeat(PV.WARNING_CODE_MAX + 5);
    assert.equal(row(rowsOf({ ble: emoji }), 'ble').value, '\u{1F600}'.repeat(PV.WARNING_CODE_MAX)); // no split surrogate pair
    assert.equal(row(rowsOf({ ble: '<b>x</b>' }), 'ble').value, '<b>x</b>'); // text only: the DOM glue uses textContent
    for (const bad of ['', '   ', null, undefined, 5, true, [], {}]) assert.equal(row(rowsOf({ ble: bad }), 'ble'), undefined, String(bad));
    // inherited property names are not words of the table
    assert.equal(row(rowsOf({ ble: 'constructor' }), 'ble').value, 'constructor');
    assert.equal(row(rowsOf({ ble: '__proto__' }), 'ble').value, '__proto__');
});

test('Receive updates: On or Off in the grinder\'s word, a note only while on', () => {
    const on = row(rowsOf({ push_armed: true }), 'push');
    assert.deepEqual([on.value, on.note], ['On', 'A computer with the update token can send firmware; Confirm decides if a tap is needed.']);
    const off = row(rowsOf({ push_armed: false }), 'push');
    assert.deepEqual([off.value, off.note], ['Off', '']);
    for (const bad of ['true', 1, 0, null, undefined]) assert.equal(row(rowsOf({ push_armed: bad }), 'push'), undefined, String(bad));
});

test('board detection: the five words, the motor note where the board is not confirmed, an unknown word as received', () => {
    const board = (v) => row(rowsOf({ board_source: v }), 'board');
    assert.deepEqual(Object.keys(PV.BOARD_SOURCE_TEXT).sort(), ['cached', 'default', 'none', 'probe', 'user']);
    assert.equal(board('probe').value, 'Detected at start-up');
    assert.equal(board('user').value, 'Chosen on the grinder');
    assert.equal(board('probe').note, '');
    assert.equal(board('user').note, '');
    for (const w of ['cached', 'default', 'none']) assert.match(board(w).note, /motor stays off/, w);
    assert.equal(board('cached').value, 'Remembered from an earlier start');
    assert.equal(board('default').value, 'Not confirmed (default)');
    assert.equal(board('none').value, 'Not known yet');
    assert.equal(board('later_word').value, 'later_word');
    assert.equal(board('y'.repeat(80)).value, 'y'.repeat(PV.WARNING_CODE_MAX));
    for (const bad of [null, '', 7, undefined]) assert.equal(board(bad), undefined, String(bad));
});

test('a missing member hides only its row', () => {
    const members = { calibrated: 'cal', std_dev_g: null, noise_ok: 'noise', sample_rate_sps: 'rate', motor_latency_ms: 'latency',
        wifi_rssi_dbm: 'wifi', ble: 'ble', board_source: 'board', push_armed: 'push' };
    for (const [key, id] of Object.entries(members)) {
        const d = doc();
        delete d[key];
        const rows = PV.diagnosticsRows(PV.parseDiagnostics(d));
        if (key === 'noise_ok') {
            assert.deepEqual([row(rows, 'noise').value, row(rows, 'noise').level], ['0.012 g', 'plain'], key); // the reading alone
        } else if (id) {
            assert.equal(row(rows, id), undefined, key);
            assert.equal(rows.length, 7, key);
        } else {
            assert.equal(row(rows, 'noise').note, '', key); // std_dev_g only feeds the noise note
        }
    }
});

test('no text of any row, note or message ever contains undefined, NaN, null, Infinity or [object', () => {
    const odd = [undefined, null, NaN, Infinity, -Infinity, 0, -0, -1, 1e300, -1e300, 1e9, 1e12, 1e13, 0.0004, '5', '', ' ', 'x', true, false, [], [1], {}, { a: 1 }];
    const keys = Object.keys(doc());
    const docs = [{}, [], doc()];
    for (const key of keys) {
        for (const v of odd) docs.push(doc({ [key]: v }));
    }
    for (const v of odd) docs.push(Object.fromEntries(keys.map((k) => [k, v])));
    for (const d of docs) {
        const parsed = PV.parseDiagnostics(d);
        if (parsed === null) {
            assert.equal(Array.isArray(d), true);
            continue;
        }
        const state = PV.diagnosticsLoaded(PV.newStats(), d, 0);
        const text = allText(PV.diagnosticsView(state, ONLINE, 5000)) + allText(PV.diagnosticsView(state, OFFLINE, 5000));
        assert.doesNotMatch(text, /undefined|NaN|null|Infinity|\[object/, JSON.stringify(d));
    }
});

test('wrong types in every member hide every row; the card then says so', () => {
    const wrong = Object.fromEntries(Object.keys(doc()).map((k) => [k, ['x']]));
    assert.deepEqual(PV.diagnosticsRows(PV.parseDiagnostics(wrong)), []);
    const view = PV.diagnosticsView(PV.diagnosticsLoaded(PV.newStats(), wrong, 0), ONLINE, 0);
    assert.deepEqual([view.visible, view.kind, view.rows.length], [true, 'empty', 0]);
    assert.equal(view.message, 'The grinder sent no health values this page can read.');
    const none = PV.diagnosticsView(PV.diagnosticsLoaded(PV.newStats(), {}, 0), ONLINE, 0);
    assert.equal(none.kind, 'empty');
});

test('extreme and negative numbers stay readable and finite', () => {
    const rows = rowsOf({ cal_factor: -1e9, std_dev_g: 1e9, sample_rate_sps: 1e9, motor_latency_ms: 1e6, wifi_rssi_dbm: -127 });
    assert.equal(row(rows, 'cal').note, 'Calibration factor -1,000,000,000.00');
    assert.equal(row(rows, 'latency').value, '1,000,000 ms');
    assert.equal(row(rows, 'wifi').value, 'Weak, -127 dBm');
    assert.doesNotMatch(JSON.stringify(rows), /e\+|e-/);
});

// ---------------------------------------------------------------------------------------------- reply parsing

test('a reply that is not an object is bad_reply: the card keeps what it had and retries', () => {
    for (const bad of [null, undefined, [], 'text', 5, true]) {
        const s = PV.diagnosticsLoaded(loaded(), bad, 1000);
        assert.deepEqual(s.doc, loaded().doc, String(bad));
        assert.equal(s.retryAt, 1000 + PV.DIAG_RETRY_MS);
        assert.equal(s.unavailable, false);
    }
});

// ---------------------------------------------------------------------------------------------- view states

test('the card is visible and says it is reading while nothing has arrived', () => {
    const view = PV.diagnosticsView(PV.newStats(), { state: 'connecting' }, 0);
    assert.deepEqual([view.visible, view.kind, view.message, view.rows.length], [true, 'loading', 'Reading the health values.', 0]);
    assert.equal(PV.diagnosticsView(PV.newStats(), ONLINE, 0).kind, 'loading');
});

test('a 404 removes the card and ends the polling for good', () => {
    const s = PV.diagnosticsFailed(PV.newStats(), 'not_supported', 1000);
    assert.equal(s.unavailable, true);
    assert.equal(s.error, '');
    const view = PV.diagnosticsView(s, ONLINE, 2000);
    assert.deepEqual([view.visible, view.message, view.note, view.rows.length], [false, '', '', 0]);
    assert.equal(PV.diagnosticsDue(s, ctx({ nowMs: 1e9 })), false);
    // even after data had arrived (a firmware downgrade): gone
    assert.equal(PV.diagnosticsView(PV.diagnosticsFailed(loaded(), 'not_supported', 9), ONLINE, 10).visible, false);
});

test('ui_busy before any data: a short sentence, the card stays, a retry is due after DIAG_RETRY_MS', () => {
    const s = PV.diagnosticsFailed(PV.newStats(), 'ui_busy', 1000);
    const view = PV.diagnosticsView(s, ONLINE, 1500);
    assert.equal(view.visible, true);
    assert.equal(view.kind, 'error');
    assert.equal(view.message, 'The health values could not be read. The grinder screen is busy. This page tries again by itself.');
    assert.doesNotMatch(view.message, /settings/i); // not the settings sentence of the shared error text
    assert.equal(PV.diagnosticsDue(s, ctx({ nowMs: 1000 + PV.DIAG_RETRY_MS - 1 })), false);
    assert.equal(PV.diagnosticsDue(s, ctx({ nowMs: 1000 + PV.DIAG_RETRY_MS })), true);
    // a good reply clears it
    const ok = PV.diagnosticsLoaded(s, doc(), 7000);
    assert.deepEqual([ok.error, ok.retryAt, PV.diagnosticsView(ok, ONLINE, 7000).kind], ['', 0, 'ready']);
});

test('ui_busy after data keeps the rows and says how old they are', () => {
    const s = PV.diagnosticsFailed(loaded({}, 1000), 'ui_busy', 20000);
    const view = PV.diagnosticsView(s, ONLINE, 21000);
    assert.equal(view.kind, 'ready');
    assert.equal(view.rows.length, 8);
    assert.equal(view.note, 'The values could not be refreshed (The grinder screen is busy.) and are from 20 s ago.');
});

test('other failures say why and retry; a lost connection says the grinder is not reachable', () => {
    const s = PV.diagnosticsFailed(PV.newStats(), 'timeout', 0);
    assert.match(PV.diagnosticsView(s, ONLINE, 1).message, /^The health values could not be read\. The grinder took too long/);
    assert.match(PV.diagnosticsView(PV.newStats(), OFFLINE, 1).message, /^The grinder is not reachable, so the health values cannot be read/);
    assert.equal(PV.diagnosticsView(PV.newStats(), OFFLINE, 1).kind, 'error');
    const old = PV.diagnosticsView(loaded({}, 0), OFFLINE, 65000);
    assert.equal(old.note, 'The grinder is not reachable. These values are from 1 min ago.');
    assert.equal(old.rows.length, 8);
});

test('a fresh reading has no note and no message', () => {
    const view = PV.diagnosticsView(loaded({}, 1000), ONLINE, 3000);
    assert.deepEqual([view.kind, view.message, view.note], ['ready', '', '']);
});

test('the view never changes the state it is given', () => {
    const s = loaded({}, 0);
    const before = JSON.stringify(s);
    PV.diagnosticsView(s, ONLINE, 100);
    PV.diagnosticsFailed(s, 'ui_busy', 100);
    PV.diagnosticsDue(s, ctx({ nowMs: 100 }));
    assert.equal(JSON.stringify(s), before);
});

// ---------------------------------------------------------------------------------------------- polling rules

function ctx(over) {
    return Object.assign({ online: true, haveStatus: true, hidden: false, tabShown: true, commandBusy: false, nowMs: 0 }, over || {});
}

test('diagnosticsDue: only while the Device section is shown, online, with a status, no command, tab visible', () => {
    const fresh = PV.newStats();
    assert.equal(PV.diagnosticsDue(fresh, ctx()), true);
    assert.equal(PV.diagnosticsDue(fresh, ctx({ tabShown: false })), false); // no probe from another section
    assert.equal(PV.diagnosticsDue(fresh, ctx({ hidden: true })), false);    // paused with the tab
    assert.equal(PV.diagnosticsDue(fresh, ctx({ online: false })), false);
    assert.equal(PV.diagnosticsDue(fresh, ctx({ haveStatus: false })), false);
    assert.equal(PV.diagnosticsDue(fresh, ctx({ commandBusy: true })), false);
});

test('diagnosticsDue: a loaded card refreshes every DIAG_PERIOD_MS and at once when the section is opened', () => {
    const s = loaded({}, 1000);
    assert.equal(PV.diagnosticsDue(s, ctx({ nowMs: 1000 })), false);
    assert.equal(PV.diagnosticsDue(s, ctx({ nowMs: 1000 + PV.DIAG_PERIOD_MS - 1 })), false);
    assert.equal(PV.diagnosticsDue(s, ctx({ nowMs: 1000 + PV.DIAG_PERIOD_MS })), true);
    const reopened = PV.statsShown(s, 2000);
    assert.equal(PV.diagnosticsDue(reopened, ctx({ nowMs: 2000 })), true);
    assert.equal(PV.diagnosticsDue(PV.diagnosticsLoaded(reopened, doc(), 2100), ctx({ nowMs: 2200 })), false);
});

test('diagnosticsDue: a request is not repeated before its retry time, whatever else is asked', () => {
    const failed = PV.statsShown(PV.diagnosticsFailed(PV.newStats(), 'network', 1000), 1000);
    assert.equal(PV.diagnosticsDue(failed, ctx({ nowMs: 1000 })), false);
    assert.equal(PV.diagnosticsDue(failed, ctx({ nowMs: 1000 + PV.DIAG_RETRY_MS })), true);
});

test('nextPollTask: diagnostics come after the other optional reads and never starve status or the needs', () => {
    const f = { haveStatus: true, online: true, needSchema: false, needSettings: false, needInfo: false, forceStatus: false,
        statsDue: false, historyDue: false, diagnosticsDue: true };
    assert.equal(PV.nextPollTask(f), 'diagnostics');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { needInfo: true })), 'info');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { needSettings: true })), 'settings');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { needSchema: true })), 'schema');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { statsDue: true })), 'stats');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { historyDue: true })), 'history');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { forceStatus: true })), 'status');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { online: false })), 'status');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { haveStatus: false })), 'status');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { diagnosticsDue: false })), 'status');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { diagnosticsDue: undefined })), 'status'); // an older caller
});

test('a failed health request keeps the connection, asks for status next; a lost connection still counts', () => {
    for (const code of ['not_supported', 'ui_busy', 'bad_reply', 'http_error']) {
        assert.deepEqual(PV.pollFailed('diagnostics', code), { schemaMissing: false, connectionLost: false, forceStatus: true }, code);
    }
    for (const code of ['network', 'timeout', 'web_interface_off']) assert.equal(PV.pollFailed('diagnostics', code).connectionLost, true, code);
    const needs = { needSchema: false, needSettings: false, needInfo: false };
    const next = PV.pollSucceeded('diagnostics', needs, doc(), true, 'ready');
    assert.deepEqual([next.needSchema, next.needSettings, next.needInfo, next.forceStatus, next.quick], [false, false, false, false, false]);
});

// A small model of the page's polling loop: one request per turn, the delay between turns from pollDelay.
function simulate(opts) {
    let diag = opts.diag || PV.newStats();
    let t = 0;
    const log = [];
    const conn = PV.connectionOk(PV.newConnection(), 0);
    const status = { busy: false, ui_state: 'ready' };
    const reply = opts.reply || (() => ({ ok: doc() }));
    while (t < opts.durationMs) {
        const tabShown = opts.tabShown ? opts.tabShown(t) : true;
        const hidden = opts.hidden ? opts.hidden(t) : false;
        if (hidden) { t += 1000; continue; } // polling is paused, nothing is requested
        const due = PV.diagnosticsDue(diag, ctx({ tabShown: tabShown, nowMs: t, commandBusy: opts.commandBusy ? opts.commandBusy(t) : false }));
        const task = PV.nextPollTask({ haveStatus: true, needSchema: false, needSettings: false, needInfo: false, online: true,
            forceStatus: log.length > 0 && log[log.length - 1].failedOptional === true, statsDue: false, historyDue: false, diagnosticsDue: due });
        const entry = { t: t, task: task };
        if (task === 'diagnostics') {
            const r = reply(t, entry);
            if (r.ok) diag = PV.diagnosticsLoaded(diag, r.ok, t);
            else { diag = PV.diagnosticsFailed(diag, r.error, t); entry.failedOptional = PV.pollFailed('diagnostics', r.error).forceStatus; }
        }
        log.push(entry);
        t += PV.pollDelay(conn, status, false, false); // the same delay whatever the task was
    }
    return { log: log, diag: diag };
}

test('simulation: on the Device section the page asks about every 5 s, between status requests, one request per turn', () => {
    const { log } = simulate({ durationMs: 60000 });
    const diagTurns = log.filter((e) => e.task === 'diagnostics');
    assert.ok(diagTurns.length >= 10 && diagTurns.length <= 13, String(diagTurns.length));
    for (let i = 1; i < log.length; i++) assert.equal(log[i - 1].task === 'diagnostics' && log[i].task === 'diagnostics', false); // never back to back
    for (let i = 1; i < diagTurns.length; i++) assert.ok(diagTurns[i].t - diagTurns[i - 1].t >= PV.DIAG_PERIOD_MS);
    // the cadence of the loop is the status cadence, untouched: one turn per POLL_IDLE_MS
    for (let i = 1; i < log.length; i++) assert.equal(log[i].t - log[i - 1].t, PV.POLL_IDLE_MS);
    assert.equal(log.filter((e) => e.task === 'status').length + diagTurns.length, log.length);
});

test('simulation: nothing is asked while another section is shown, and the first look comes when Device opens', () => {
    const { log } = simulate({ durationMs: 30000, tabShown: (t) => t >= 12000 });
    const diagTurns = log.filter((e) => e.task === 'diagnostics');
    assert.ok(diagTurns.length > 0);
    assert.ok(diagTurns.every((e) => e.t >= 12000));
    // opening the section asks for a fresh reading right away (statsShown), even for an old card
    let s = loaded({}, 0);
    s = PV.statsShown(s, 3000);
    assert.equal(PV.diagnosticsDue(s, ctx({ nowMs: 3000 })), true);
});

test('simulation: no request during a command and none while the tab is hidden; polling resumes afterwards', () => {
    const during = simulate({ durationMs: 30000, commandBusy: (t) => t >= 5000 && t < 20000 });
    assert.ok(during.log.filter((e) => e.task === 'diagnostics').every((e) => e.t < 5000 || e.t >= 20000));
    assert.ok(during.log.some((e) => e.task === 'diagnostics' && e.t >= 20000));
    const hiddenRun = simulate({ durationMs: 30000, hidden: (t) => t >= 5000 && t < 20000 });
    assert.ok(hiddenRun.log.every((e) => e.t < 5000 || e.t >= 20000));
    assert.ok(hiddenRun.log.some((e) => e.task === 'diagnostics' && e.t >= 20000));
    assert.equal(PV.pollDelay(PV.connectionOk(PV.newConnection(), 0), null, false, true), null); // the loop's own pause is unchanged
});

test('simulation: ui_busy is retried after DIAG_RETRY_MS with a status request in between; a 404 ends it', () => {
    let n = 0;
    const busy = simulate({ durationMs: 30000, reply: () => (++n <= 2 ? { error: 'ui_busy' } : { ok: doc() }) });
    const turns = busy.log.filter((e) => e.task === 'diagnostics');
    assert.ok(turns[1].t - turns[0].t >= PV.DIAG_RETRY_MS);
    assert.equal(busy.log[busy.log.findIndex((e) => e.task === 'diagnostics') + 1].task, 'status'); // never starves status
    assert.equal(PV.diagnosticsView(busy.diag, ONLINE, 30000).kind, 'ready');
    const gone = simulate({ durationMs: 60000, reply: () => ({ error: 'not_supported' }) });
    assert.equal(gone.log.filter((e) => e.task === 'diagnostics').length, 1); // gives up after the first 404
    assert.equal(PV.diagnosticsView(gone.diag, ONLINE, 60000).visible, false);
    const always = simulate({ durationMs: 60000, reply: () => ({ error: 'ui_busy' }) });
    const rhythm = always.log.filter((e) => e.task === 'diagnostics');
    assert.ok(rhythm.length <= 12); // a retry rhythm (one per DIAG_RETRY_MS at most), not a flood
    for (let i = 1; i < rhythm.length; i++) assert.ok(rhythm[i].t - rhythm[i - 1].t >= PV.DIAG_RETRY_MS);
});

// ---------------------------------------------------------------------------------------------- the page

const html = fs.readFileSync(path.join(ROOT, 'web', 'portal', 'page.src.html'), 'utf8');
const app = fs.readFileSync(path.join(ROOT, 'web', 'portal', 'portal-app.js'), 'utf8');

test('the Health card is wired in the page: a hidden card in the Device section, rows from a template, text only', () => {
    assert.match(html, /<section id="health" class="card full" aria-labelledby="health-h" hidden>/);
    assert.match(html, /<dl id="health-list" class="about" hidden><\/dl>/);
    assert.match(html, /<template id="health-row">/);
    const device = html.slice(html.indexOf('<section id="panel-device"'), html.indexOf('</main>'));
    assert.ok(device.includes('id="health"') && device.includes('id="about-h"'));
    assert.ok(device.indexOf('id="about-h"') < device.indexOf('id="health-h"')); // About first
    assert.equal(app.includes('innerHTML'), false);
    assert.match(app, /client\.getDiagnostics\(\)/);
    assert.equal((app.match(/client\.getDiagnostics\(\)/g) || []).length, 1);
    assert.match(app, /V\.diagnosticsView\(diag, conn, now\(\)\)/);
    assert.match(app, /var diagnosticsDue = V\.diagnosticsDue\(diag, \{\s*online: conn\.state === 'online', haveStatus: status !== null, hidden: document\.hidden,\s*tabShown: !\$\('panel-device'\)\.hidden, commandBusy: commandPending \|\| client\.pending\(\) > 0, nowMs: now\(\)/);
    assert.match(app, /online: conn\.state === 'online', forceStatus: forceStatus, statsDue: statsDue, historyDue: historyDue,\s*diagnosticsDue: diagnosticsDue/);
    // a click on the Device tab while it is open does not ask again; arriving from another section does
    assert.match(app, /var deviceWasShown = !\$\('panel-device'\)\.hidden;/);
    assert.match(app, /if \(!deviceWasShown\) diag = V\.statsShown\(diag, now\(\)\)/);
    assert.match(app, /if \(!deviceWasShown\) pollNow\(\);/);
    // the card is drawn only while the section is shown, rows are rebuilt only when their content changes
    assert.match(app, /function renderHealth\(\) \{\s*if \(\$\('panel-device'\)\.hidden\) return;/);
    assert.match(app, /if \(key === healthKey\) return;/);
    // a level row is a badge with an icon and a word, never colour alone
    assert.match(html, /<template id="health-row">[\s\S]*class="h-badge"[\s\S]*#i-check[\s\S]*#i-warning/);
});

test('the status polling cadence is untouched: pollDelay and the schedule call do not mention the health card', () => {
    assert.match(app, /schedule\(document\.hidden \? null : quickNext \? 100 : V\.pollDelay\(conn, status, leadInIsActive\(\), document\.hidden\)\)/);
    const view = fs.readFileSync(path.join(ROOT, 'web', 'portal', 'portal-view.js'), 'utf8');
    const delay = view.slice(view.indexOf('function pollDelay'), view.indexOf('// What the single polling loop requests next'));
    assert.doesNotMatch(delay, /diag/i);
});

test('the view module does not touch the DOM, a clock or a timer for the Health card', () => {
    const view = fs.readFileSync(path.join(ROOT, 'web', 'portal', 'portal-view.js'), 'utf8');
    const card = view.slice(view.indexOf('Device: health card'), view.indexOf('Grind: last grind card'));
    assert.doesNotMatch(card, /document|window|Date\.now|setTimeout|setInterval|innerHTML|console/);
});
