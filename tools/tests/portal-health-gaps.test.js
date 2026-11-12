// node --test tools/tests/portal-health-gaps.test.js
// Gap tests for the Health card of the Device section (chg021 G3c), written against docs/PORTAL-API.md "Diagnostics"
// and the acceptance of plan.md "G3c". portal-health.test.js holds the main tests; this file adds the client error
// mapping, the view's limits and a session simulation that follows web/portal/portal-app.js (showTab, pollNow,
// visibilitychange, poll, finalize) on a 100 ms tick, so the status cadence can be compared with and without the card.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const PC = require('../../web/portal/portal-client.js');
const PV = require('../../web/portal/portal-view.js');

const ROOT = path.join(__dirname, '..', '..');
const ONLINE = { state: 'online' };

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

function reply(status, body) {
    const text = typeof body === 'string' ? body : JSON.stringify(body);
    return { ok: status >= 200 && status < 300, status: status, text: async () => text };
}

function fakeFetch(handler) {
    return { fetch: async (url, init) => handler(url, init) };
}

// ---------------------------------------------------------------------------------------------- client mapping

test('getDiagnostics: a 404 is not_supported whatever its body (plain text or a JSON object without an error)', async () => {
    let f = fakeFetch(async () => reply(404, 'Not Found'));
    await assert.rejects(PC.createClient({ fetch: f.fetch }).getDiagnostics(), (e) => e.code === 'not_supported' && e.status === 404);
    f = fakeFetch(async () => reply(404, {}));
    await assert.rejects(PC.createClient({ fetch: f.fetch }).getDiagnostics(), (e) => e.code === 'not_supported' && e.status === 404);
});

test('getDiagnostics: a 500 with a plain body is http_error with its status; the text is not shown', async () => {
    const f = fakeFetch(async () => reply(500, 'stack trace here'));
    await assert.rejects(PC.createClient({ fetch: f.fetch }).getDiagnostics(), (e) => e.code === 'http_error' && e.status === 500 && e.message === PC.ERROR_TEXT.http_error);
});

test('getDiagnostics: a request that never answers is a timeout (the limit is shortened here)', async () => {
    const hang = (url, init) => new Promise((resolve, reject) => init.signal.addEventListener('abort', () => reject(new Error('aborted'))));
    await assert.rejects(PC.createClient({ fetch: hang, timeoutMs: 20 }).getDiagnostics(), (e) => e.code === 'timeout' && e.status === 0);
});

test('getDiagnostics: a 200 body that is not a JSON object is bad_reply (null, a number, text, true, empty, HTML)', async () => {
    for (const body of ['null', '5', '"text"', 'true', '', '<html>']) {
        const f = fakeFetch(async () => reply(200, body));
        await assert.rejects(PC.createClient({ fetch: f.fetch }).getDiagnostics(), (e) => e.code === 'bad_reply', JSON.stringify(body));
    }
});

test('getDiagnostics: an array body is not kept as health values; the card keeps what it had and retries', async () => {
    // The client resolves an array (as it does for every read); the view turns it into bad_reply.
    const f = fakeFetch(async () => reply(200, [1, 2]));
    const got = await PC.createClient({ fetch: f.fetch }).getDiagnostics();
    const first = PV.diagnosticsLoaded(PV.newStats(), doc(), 1000);
    const next = PV.diagnosticsLoaded(first, got, 9000);
    assert.deepEqual(next.doc, first.doc);
    assert.equal(next.retryAt, 9000 + PV.DIAG_RETRY_MS);
    assert.equal(PV.diagnosticsView(next, ONLINE, 9000).kind, 'ready');
});

// ---------------------------------------------------------------------------------------------- view: limits

test('an unknown Bluetooth or board word of 5000 characters is cut to 40 characters and shown as received', () => {
    const long = 'k'.repeat(5000);
    assert.equal(row(rowsOf({ ble: long }), 'ble').value, 'k'.repeat(PV.WARNING_CODE_MAX));
    const board = row(rowsOf({ board_source: 'z'.repeat(5000) }), 'board');
    assert.deepEqual([board.value, board.note], ['z'.repeat(PV.WARNING_CODE_MAX), '']);
    const mixed = 'Abé\u{1F600}' + 'x'.repeat(5000);
    assert.equal(row(rowsOf({ ble: mixed }), 'ble').value, Array.from(mixed).slice(0, PV.WARNING_CODE_MAX).join(''));
});

test('an unknown Bluetooth word: the view trims surrounding whitespace only, then cuts; inner spaces and case are kept', () => {
    // Decision on the earlier observation: the view trims whitespace at both ends and nothing else (diagWord), so an
    // unknown word is "as received" modulo surrounding whitespace. Inner spaces, case and punctuation stay as sent.
    assert.equal(row(rowsOf({ ble: '\t Mesh  Pairing \n' }), 'ble').value, 'Mesh  Pairing');
    assert.equal(row(rowsOf({ ble: 'MESH-Pairing!' }), 'ble').value, 'MESH-Pairing!');
});

test('numbers and booleans sent as text are unusable: their rows are hidden, the words of the others stay', () => {
    const r = rowsOf({ calibrated: 'true', cal_factor: '1234.56', motor_latency_ms: '50', wifi_rssi_dbm: '-58', sample_rate_sps: '10',
        push_armed: 'false', std_dev_g: '0.012', noise_ok: 1 });
    assert.deepEqual(r.map((x) => x.id), ['ble', 'board']);
    assert.equal(row(r, 'ble').value, 'Off');
    assert.equal(row(r, 'board').value, 'Detected at start-up');
});

test('the magnitude cut is 1e12 and the signal rounds to whole dBm before its range is checked', () => {
    const latency = row(rowsOf({ motor_latency_ms: 1e12 }), 'latency');
    assert.equal(latency.value, '1,000,000,000,000 ms');
    assert.equal(row(rowsOf({ motor_latency_ms: 1.5e12 }), 'latency'), undefined);
    assert.equal(row(rowsOf({ wifi_rssi_dbm: -127.4 }), 'wifi').value, 'Weak, -127 dBm');
    assert.equal(row(rowsOf({ wifi_rssi_dbm: -127.6 }), 'wifi'), undefined); // rounds to -128: out of the contract's range
});

test('a rate or a signal of exactly the limits: 1 sample/s and -1 dBm are shown, 0 is not', () => {
    assert.equal(row(rowsOf({ sample_rate_sps: 1 }), 'rate').value, '1.0 samples/s');
    assert.equal(row(rowsOf({ wifi_rssi_dbm: -1 }), 'wifi').value, 'Excellent, -1 dBm');
    assert.equal(row(rowsOf({ sample_rate_sps: 0 }), 'rate'), undefined);
});

// ---------------------------------------------------------------------------------------------- session simulation

// A model of the page's polling loop on a 100 ms tick. Replies are instant; the page's own functions decide everything.
// o.events: [{t, tab}] (a tab click; the Device tab is "arrived at" only when it was not shown) and [{t, hidden}].
// o.withDiag: false leaves the Health card out (the diagnostics-less run). o.busy(t), o.offline(t): windows.
// o.diagReply(t): the diagnostics answer of the grinder (default: the example reply).
// Insights (statistics, history) is treated as unavailable so its own probes do not share the loop.
function session(o) {
    let diag = PV.newStats();
    let needSchema = true;
    let needSettings = true;
    let needInfo = true;
    let forceStatus = false;
    let quick = false;
    let haveStatus = false;
    let status = null;
    let conn = PV.newConnection();
    let nextAt = 0;
    let hidden = false;
    let tab = 'grind';
    const diagAt = [];
    const statusAt = [];
    const events = o.events || [];
    const answer = (task, t) => {
        if (task === 'status') return { ok: { ui_state: 'ready', busy: false } };
        if (task === 'schema') return { ok: { fields: [] } };
        const busy = typeof o.settingsBusy === 'function' ? o.settingsBusy(t) : !!o.settingsBusy;
        if (task === 'settings') return busy ? { error: 'ui_busy' } : { ok: { values: {} } };
        if (task === 'info') return { ok: { version: '1' } };
        if (task === 'diagnostics') return o.diagReply ? o.diagReply(t) : { ok: doc() };
        return { error: 'not_supported' };
    };
    for (let t = 0; t <= o.end; t += 100) {
        events.filter((e) => e.t === t).forEach((e) => {
            if (e.tab !== undefined) {
                const wasDevice = tab === 'device';
                tab = e.tab;
                if (tab === 'device' && !wasDevice) {
                    diag = PV.statsShown(diag, t); // showTab: opening the section asks at once ...
                    nextAt = t;                    // ... and pollNow() schedules the loop at once
                }
            }
            if (e.hidden !== undefined) {
                hidden = e.hidden;
                nextAt = hidden ? null : t; // visibilitychange: schedule(null), then pollNow()
            }
        });
        if (nextAt === null || nextAt > t) continue;
        nextAt = null;
        const online = conn.state === 'online';
        const due = o.withDiag === false ? false : PV.diagnosticsDue(diag, {
            online: online, haveStatus: haveStatus, hidden: hidden, tabShown: tab === 'device',
            commandBusy: !!(o.busy && o.busy(t)), nowMs: t
        });
        const task = PV.nextPollTask({ haveStatus: haveStatus, needSchema: needSchema, needSettings: needSettings, needInfo: needInfo,
            online: online, forceStatus: forceStatus, statsDue: false, historyDue: false, diagnosticsDue: due });
        forceStatus = false;
        quick = false;
        const before = status ? status.ui_state : '';
        const out = o.offline && o.offline(t) ? { error: 'network' } : answer(task, t);
        if (task === 'status') statusAt.push(t);
        if (task === 'diagnostics') diagAt.push(t);
        if (out.ok !== undefined) {
            const wasOnline = conn.state === 'online';
            conn = PV.connectionOk(conn, t);
            const next = PV.pollSucceeded(task, { needSchema: needSchema, needSettings: needSettings, needInfo: needInfo }, out.ok, wasOnline, before);
            needSchema = next.needSchema;
            needSettings = next.needSettings;
            needInfo = next.needInfo;
            forceStatus = next.forceStatus;
            quick = next.quick;
            if (task === 'status') { status = out.ok; haveStatus = true; }
            else if (task === 'diagnostics') diag = PV.diagnosticsLoaded(diag, out.ok, t);
        } else {
            const failed = PV.pollFailed(task, out.error);
            if (task === 'diagnostics') diag = PV.diagnosticsFailed(diag, out.error, t);
            conn = failed.connectionLost ? PV.connectionFailed(conn, out.error, t) : PV.connectionOk(conn, t);
            if (failed.forceStatus) forceStatus = true;
        }
        nextAt = hidden ? null : quick ? t + 100 : t + PV.pollDelay(conn, status, false, hidden);
    }
    return { diag: diag, diagAt: diagAt, statusAt: statusAt };
}

function gaps(times) {
    return times.slice(1).map((x, i) => x - times[i]);
}

const GRIND_ONLY = { end: 40000 };
const DEVICE_FROM_12S = { end: 40000, events: [{ t: 12000, tab: 'device' }] };

test('simulation: a session that stays on the grind section requests no health numbers at all', () => {
    const r = session(GRIND_ONLY);
    assert.deepEqual(r.diagAt, []);
    assert.ok(r.statusAt.length > 30); // the loop itself runs
});

test('simulation: arriving at Device asks at once, then every DIAG_PERIOD_MS exactly, and never earlier', () => {
    const r = session(DEVICE_FROM_12S);
    assert.equal(r.diagAt[0], 12000);
    assert.ok(r.diagAt.length >= 5);
    for (let i = 1; i < r.diagAt.length; i++) assert.equal(r.diagAt[i] - r.diagAt[i - 1], PV.DIAG_PERIOD_MS, String(i));
});

test('simulation: clicking the already-open Device tab asks nothing extra and changes no status turn', () => {
    const plain = session(DEVICE_FROM_12S);
    const clicked = session({ end: 40000, events: [{ t: 12000, tab: 'device' }, { t: 15000, tab: 'device' }, { t: 21000, tab: 'device' }] });
    assert.deepEqual(clicked.diagAt, plain.diagAt);
    assert.deepEqual(clicked.statusAt, plain.statusAt);
});

test('simulation: while the tab is hidden nothing is requested; on showing again the first look is at once', () => {
    const r = session({ end: 40000, events: [{ t: 12000, tab: 'device' }, { t: 16000, hidden: true }, { t: 26000, hidden: false }] });
    assert.ok(r.diagAt.every((t) => t < 16000 || t >= 26000), JSON.stringify(r.diagAt));
    assert.equal(r.diagAt.filter((t) => t >= 26000)[0], 26000);
    assert.ok(r.statusAt.every((t) => t < 16000 || t >= 26000), 'the whole loop pauses with the tab'); // pollDelay(hidden) is null
});

test('simulation: nothing during a command; the first look after it is at its end', () => {
    const r = session({ end: 40000, events: [{ t: 12000, tab: 'device' }], busy: (t) => t >= 14000 && t < 24000 });
    assert.ok(r.diagAt.every((t) => t < 14000 || t >= 24000), JSON.stringify(r.diagAt));
    assert.equal(r.diagAt.filter((t) => t >= 24000)[0], 24000);
});

test('simulation: nothing while the grinder is unreachable; a look after it answers again', () => {
    const r = session({ end: 40000, events: [{ t: 12000, tab: 'device' }], offline: (t) => t >= 14000 && t < 24000 });
    assert.ok(r.diagAt.every((t) => t < 14000 || t >= 24000), JSON.stringify(r.diagAt));
    assert.ok(r.statusAt.some((t) => t >= 14000 && t < 24000), 'status keeps asking while offline');
    assert.ok(r.diagAt.some((t) => t >= 24000));
});

test('simulation: a 404 ends the health requests for good, even after the section is opened again', () => {
    const r = session({ end: 60000, events: [{ t: 12000, tab: 'device' }, { t: 20000, tab: 'grind' }, { t: 30000, tab: 'device' }],
        diagReply: () => ({ error: 'not_supported' }) });
    assert.deepEqual(r.diagAt, [12000]);
    assert.equal(PV.diagnosticsView(r.diag, ONLINE, 60000).visible, false);
});

test('simulation: ui_busy is retried exactly DIAG_RETRY_MS after each failure, with a status turn in between', () => {
    let n = 0;
    const r = session({ end: 40000, events: [{ t: 12000, tab: 'device' }],
        diagReply: () => (++n <= 3 ? { error: 'ui_busy' } : { ok: doc() }) });
    assert.equal(r.diagAt.length >= 4, true, JSON.stringify(r.diagAt));
    for (let i = 1; i <= 3; i++) {
        assert.equal(r.diagAt[i] - r.diagAt[i - 1], PV.DIAG_RETRY_MS, String(i));
        assert.ok(r.statusAt.includes(r.diagAt[i - 1] + 1000), 'a status turn follows the failed look');
    }
    assert.equal(PV.diagnosticsView(r.diag, ONLINE, 40000).kind, 'ready');
});

// Accepted behaviour (decision of the Parent, documented in docs/DESIGN-NOTES.md G3c): the Health card takes one turn of
// the single poll loop about every DIAG_PERIOD_MS while Device is shown. A turn is one POLL_IDLE_MS slot, so a status
// request can wait one extra slot: the status gap is at most two turns, and about 48 status requests run per minute
// instead of 60. Pinned here with a 100 ms margin; the gap on every other section is the same as without the card.
test('Device shown: the status gap is at most one status turn plus one diagnostics turn (2 x POLL_IDLE_MS, margin 100 ms)', () => {
    const withCard = session({ end: 60000, events: [{ t: 0, tab: 'device' }] });
    const without = session({ end: 60000, events: [{ t: 0, tab: 'device' }], withDiag: false });
    const bound = 2 * PV.POLL_IDLE_MS + 100;
    // Steady state (from 10 s on): the bound. The first seconds also fetch schema, settings and info, which the page
    // does without the card too; there the card may add one diagnostics turn over the run without it.
    const steady = (times) => times.filter((t) => t >= 10000);
    const startup = (times) => times.filter((t) => t < 10000);
    const longest = Math.max(...gaps(steady(withCard.statusAt)));
    assert.ok(longest <= bound, 'longest steady status gap ' + longest + ' ms, bound ' + bound + ' ms; gaps ' +
        JSON.stringify([...new Set(gaps(steady(withCard.statusAt)))]));
    const startWith = Math.max(...gaps(startup(withCard.statusAt)));
    const startWithout = Math.max(...gaps(startup(without.statusAt)));
    assert.ok(startWith <= startWithout + PV.POLL_IDLE_MS + 100, 'start-up gap with the card ' + startWith + ' ms, without ' + startWithout + ' ms');
    assert.ok(Math.abs(withCard.statusAt.length - 48) <= 2, 'status requests in 60 s: ' + withCard.statusAt.length + ' (documented ~48, +-2)');
    assert.ok(withCard.diagAt.length >= 11 && withCard.diagAt.length <= 13, 'health requests in 60 s: ' + withCard.diagAt.length);
});

test('other sections: the status gap sequence equals a run without the Health card (grind, settings, and before Device opens)', () => {
    const cases = [
        { end: 60000, events: [] },                                          // grind shown
        { end: 60000, events: [{ t: 0, tab: 'settings' }] },                 // settings shown
        { end: 60000, events: [{ t: 30000, tab: 'device' }] }                // Device opened at 30 s: identical before that
    ];
    for (const c of cases) {
        const withCard = session(c);
        const without = session(Object.assign({}, c, { withDiag: false }));
        const upTo = c.events.length && c.events[0].tab === 'device' ? 30000 : Infinity;
        const cut = (times) => times.filter((t) => t < upTo);
        assert.deepEqual(cut(withCard.statusAt), cut(without.statusAt), JSON.stringify(c.events));
        if (upTo === Infinity) assert.deepEqual(withCard.diagAt, []);
    }
});

test('Settings answers ui_busy: the card does not ask, stays on its loading text, and asks within one period once Settings is served', () => {
    // Accepted behaviour (see the note above): settings is asked before the health numbers, so an unanswered settings
    // request keeps the card on "Reading the health values." until settings is served. Same as the statistics card.
    const busyUntil = 20000;
    const busy = (t) => t < busyUntil;
    const r = session({ end: 40000, events: [{ t: 0, tab: 'device' }], settingsBusy: busy });
    assert.deepEqual(r.diagAt.filter((t) => t < busyUntil), [], 'no health request while Settings is busy');
    const early = session({ end: 19500, events: [{ t: 0, tab: 'device' }], settingsBusy: busy });
    const v = PV.diagnosticsView(early.diag, ONLINE, 19500);
    assert.deepEqual([v.kind, v.message], ['loading', 'Reading the health values.']);
    const first = r.diagAt.find((t) => t >= busyUntil);
    assert.ok(first !== undefined && first <= busyUntil + PV.DIAG_PERIOD_MS, 'first health request after Settings: ' + first);
});

test('the poll loop keeps one request in flight: poll() returns while a poll is running, and only poll() starts a request', () => {
    const app = fs.readFileSync(path.join(ROOT, 'web', 'portal', 'portal-app.js'), 'utf8');
    assert.match(app, /function poll\(\) \{\s*pollTimer = null;\s*if \(polling\) return;\s*polling = true;/);
    assert.equal((app.match(/client\.getDiagnostics\(\)/g) || []).length, 1);
});
