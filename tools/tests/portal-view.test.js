// node --test tools/tests/portal-view.test.js
// Checks the pure view helpers of the web panel page (web/portal/portal-view.js).
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const PV = require('../../web/portal/portal-view.js');

test('the page has four sections, Device last', () => {
    assert.deepEqual(PV.TABS, ['grind', 'settings', 'insights', 'device']);
});

test('tabForViewport keeps the picked tab at every width', () => {
    for (const tab of PV.TABS) {
        assert.equal(PV.tabForViewport(tab, false), tab);
        assert.equal(PV.tabForViewport(tab, true), tab); // a second argument (the old wide flag) changes nothing
    }
});

test('tabForViewport no longer replaces Grind by Settings on a wide screen', () => {
    assert.equal(PV.tabForViewport('grind', true), 'grind');
    assert.equal(PV.tabForViewport('settings', true), 'settings');
    assert.equal(PV.tabForViewport('insights', true), 'insights');
});

test('tabForViewport falls back to Grind for an unknown tab', () => {
    assert.equal(PV.tabForViewport('nope', false), 'grind');
    assert.equal(PV.tabForViewport(undefined, true), 'grind');
});

test('tabForViewport keeps Device, the new fourth section, at every width', () => {
    assert.equal(PV.tabForViewport('device'), 'device');
    assert.equal(PV.tabForViewport('device', false), 'device');
    assert.equal(PV.tabForViewport('device', true), 'device');
});

test('tabForViewport always answers one of the four sections, Grind for anything else', () => {
    const odd = [null, undefined, '', ' ', 0, 1, 3, NaN, true, false, {}, [], ['settings'], 'Grind', 'SETTINGS', ' device',
        'device ', 'tab-device', 'panel-grind', 'length', 'constructor', '__proto__', 'toString', 'indexOf'];
    for (const value of odd) {
        for (const wide of [undefined, false, true]) {
            assert.equal(PV.tabForViewport(value, wide), 'grind', String(value));
        }
    }
    assert.equal(PV.tabForViewport(), 'grind');
    for (const value of odd.concat(PV.TABS)) {
        assert.ok(PV.TABS.includes(PV.tabForViewport(value)));
    }
});

test('tabForViewport does not change the list of sections', () => {
    const before = PV.TABS.slice();
    PV.tabForViewport('device', true);
    PV.tabForViewport('nope', true);
    assert.deepEqual(PV.TABS, before);
    assert.equal(PV.TABS.length, 4);
});

test('formatWeight shows one decimal and a placeholder without a number', () => {
    assert.equal(PV.formatWeight(18), '18.0 g');
    assert.equal(PV.formatWeight(null), '-- g');
    assert.equal(PV.formatWeight(NaN), '-- g');
});

// ---------------------------------------------------------------------------------------------- step 2
const PC = require('../../web/portal/portal-client.js');

function status(over) {
    return Object.assign({
        ui_state: 'ready', grind_phase: 'idle', busy: false, weight_g: 0.0, motor_available: true,
        start_block: null, motor_test_block: null, uptime_s: 5, build: 1, web_interface: true
    }, over || {});
}
const online = () => PV.connectionOk(PV.newConnection(), 1000);
let offlineConn = PV.newConnection();
for (let i = 0; i < 3; i++) offlineConn = PV.connectionFailed(offlineConn, 'network', 2000);

test('formatters: weight rounding, negatives, large values, durations and ages', () => {
    assert.equal(PV.formatWeight(18.04), '18.0 g');
    assert.equal(PV.formatWeight(18.06), '18.1 g');
    assert.equal(PV.formatWeight(-0.3), '-0.3 g');
    assert.equal(PV.formatWeight(1234.5), '1234.5 g');
    assert.equal(PV.formatWeight(undefined), '-- g');
    assert.equal(PV.formatWeight(Infinity), '-- g');
    assert.equal(PV.formatSeconds(10), '10.0 s');
    assert.equal(PV.formatSeconds(null), '-- s');
    assert.equal(PV.formatAge(0), '0 s');
    assert.equal(PV.formatAge(59999), '59 s');
    assert.equal(PV.formatAge(60000), '1 min');
    assert.equal(PV.formatAge(3600000), '1 h');
    assert.equal(PV.formatAge(-5000), '0 s');
    assert.equal(PV.formatAge(NaN), '');
});

test('state and phase labels cover every ui_state and grind_phase of the contract', () => {
    const states = ['ready', 'grinding', 'grind_complete', 'grind_timeout', 'edit', 'menu', 'calibration', 'confirm',
        'purge_confirm', 'autotuning', 'ota_update', 'ota_update_failed'];
    for (const s of states) assert.notEqual(PV.stateLabel(s), s, s);
    assert.equal(PV.stateLabel('grind_complete'), 'Grind complete (waiting on the grinder screen)');
    assert.equal(PV.stateLabel('something_new'), 'something_new');
    const phases = ['initializing', 'setup', 'taring', 'tare_confirm', 'predictive', 'pulse_decision', 'pulse_execute',
        'pulse_settling', 'final_settling', 'time_grinding', 'time_additional_pulse', 'completed', 'timeout', 'prime',
        'prime_settling', 'purge_confirm'];
    for (const p of phases) assert.notEqual(PV.phaseLabel(p), p, p);
    assert.equal(PV.phaseLabel('idle'), '');
});

test('backoff schedule is 1.5, 3, 6 and 10 s, capped', () => {
    assert.deepEqual([1, 2, 3, 4, 5, 20].map(PV.backoffDelay), [1500, 3000, 6000, 10000, 10000, 10000]);
    assert.equal(PV.backoffDelay(0), 0);
});

test('connection: reconnecting, then offline, reset by one good reply', () => {
    let c = PV.newConnection();
    assert.equal(c.state, 'connecting');
    c = PV.connectionFailed(c, 'timeout', 100);
    assert.equal(c.state, 'reconnecting');
    assert.equal(c.failures, 1);
    assert.equal(PV.pollDelay(c, null, false, false), 1500);
    c = PV.connectionFailed(c, 'network', 200);
    assert.equal(c.state, 'offline');
    assert.equal(PV.pollDelay(c, null, false, false), 3000);
    c = PV.connectionFailed(c, 'network', 300);
    assert.equal(PV.pollDelay(c, null, false, false), 6000);
    c = PV.connectionOk(c, 400);
    assert.equal(c.state, 'online');
    assert.equal(c.failures, 0);
    assert.equal(c.lastOkAt, 400);
    assert.equal(PV.pollDelay(c, status(), false, false), 1000);
});

test('connection: web_interface_off slows polling and recovers by itself', () => {
    const c = PV.connectionFailed(online(), 'web_interface_off', 5);
    assert.equal(c.state, 'web_off');
    assert.equal(c.failures, 0);
    assert.equal(PV.pollDelay(c, null, false, false), 5000);
    assert.equal(PV.connectionOk(c, 9).state, 'online');
    assert.equal(PV.connectionView(c, 6).label, 'Web interface off');
});

test('connection: ui_busy is an answer, not a lost connection', () => {
    const c = PV.connectionFailed(online(), 'ui_busy', 50);
    assert.equal(c.state, 'online');
    assert.equal(c.failures, 0);
    assert.equal(c.uiBusy, true);
    assert.match(PV.connectionView(c, 60).detail, /busy/);
    assert.equal(PV.connectionOk(c, 70).uiBusy, false);
});

test('connection transitions do not change the object they were given', () => {
    const c = online();
    const copy = JSON.stringify(c);
    PV.connectionFailed(c, 'network', 5);
    PV.connectionOk(c, 6);
    assert.equal(JSON.stringify(c), copy);
});

test('connectionView: chip words, last seen and the two-connection hint', () => {
    assert.equal(PV.connectionView(online(), 1000).label, 'Online');
    assert.equal(PV.connectionView(PV.newConnection(), 0).label, 'Connecting');
    const re = PV.connectionFailed(online(), 'network', 4000);
    const v = PV.connectionView(re, 1000 + 10000);
    assert.equal(v.label, 'Reconnecting');
    assert.equal(v.retry, true);
    assert.match(v.detail, /Last seen 10 s ago/);
    const off = PV.connectionView(offlineConn, 61000);
    assert.equal(off.label, 'Offline');
    assert.match(off.detail, /two connections/);
    assert.equal(off.retry, true);
    const host = PV.connectionFailed(PV.connectionFailed(online(), 'bad_host', 1), 'bad_host', 2);
    assert.equal(PV.connectionView(host, 3).detail.startsWith(PC.describeError('bad_host')), true);
});

test('pollDelay: 500 ms busy or lead-in, 1 s idle, paused while hidden', () => {
    const c = online();
    assert.equal(PV.pollDelay(c, status(), false, false), 1000);
    assert.equal(PV.pollDelay(c, status({ busy: true }), false, false), 500);
    assert.equal(PV.pollDelay(c, status(), true, false), 500);
    assert.equal(PV.pollDelay(c, status({ busy: true }), true, true), null);
    assert.equal(PV.pollDelay(offlineConn, null, false, true), null);
});

test('nextPollTask: status first, settings and info only while online, one at a time', () => {
    const f = { haveStatus: true, needSettings: true, needInfo: true, online: true };
    assert.equal(PV.nextPollTask(Object.assign({}, f, { haveStatus: false })), 'status');
    assert.equal(PV.nextPollTask(f), 'settings');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { needSettings: false })), 'info');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { needSettings: false, needInfo: false })), 'status');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { online: false })), 'status');
});

test('deriveControls: STOP is enabled in every combination of state, block and connection', () => {
    const uiStates = ['ready', 'grinding', 'grind_complete', 'grind_timeout', 'edit', 'menu', 'calibration', 'confirm',
        'purge_confirm', 'autotuning', 'ota_update', 'ota_update_failed'];
    const blocks = [null, 'motor_disabled', 'update_in_progress', 'load_cell_not_connected', 'load_cell_no_data',
        'load_cell_rate_invalid', 'busy', 'motor_fault'];
    const conns = [PV.newConnection(), online(), offlineConn, PV.connectionFailed(online(), 'web_interface_off', 1),
        PV.connectionFailed(online(), 'network', 1)];
    const warningSets = [[], ['motor_fault'], ['motor_disabled', 'load_cell_noisy'], ['future_code']];
    for (const ui of uiStates) for (const busy of [false, true]) for (const b of blocks) for (const c of conns) {
        for (const extra of [undefined, { leadIn: true }, { commandPending: true }]) for (const w of warningSets) {
            const d = PV.deriveControls(status({ ui_state: ui, busy: busy, start_block: b, motor_test_block: b, warnings: w }), c, extra);
            assert.equal(d.stop.enabled, true);
            for (const k of ['start', 'tare', 'motorTest']) {
                assert.equal(d[k].enabled, d[k].reason === '', k);
            }
        }
    }
    assert.equal(PV.deriveControls(null, offlineConn).stop.enabled, true);
});

test('deriveControls: a ready, idle grinder enables everything and STOP is calm', () => {
    const d = PV.deriveControls(status(), online());
    assert.equal(d.start.enabled && d.tare.enabled && d.motorTest.enabled, true);
    assert.equal(d.stop.emphasis, false);
    assert.equal(d.banner, null);
});

test('deriveControls: each start_block code disables Start with its own reason', () => {
    const seen = {};
    for (const code of ['motor_disabled', 'update_in_progress', 'load_cell_not_connected', 'load_cell_no_data',
        'load_cell_rate_invalid', 'busy', 'motor_fault']) {
        const d = PV.deriveControls(status({ start_block: code }), online());
        assert.equal(d.start.enabled, false, code);
        assert.notEqual(d.start.reason, '', code);
        seen[d.start.reason] = true;
        assert.equal(d.tare.enabled, code !== 'update_in_progress', code); // an update also blocks Tare
        assert.equal(d.motorTest.enabled, true);
    }
    assert.equal(Object.keys(seen).length, 7);
    assert.match(PV.deriveControls(status({ start_block: 'load_cell_no_data' }), online()).start.reason, /not delivering data/);
    assert.match(PV.blockText('something_new'), /something_new/);
});

test('deriveControls: motor_test_block only blocks the motor test', () => {
    const d = PV.deriveControls(status({ motor_test_block: 'motor_fault' }), online());
    assert.equal(d.motorTest.enabled, false);
    assert.match(d.motorTest.reason, /fault/);
    assert.equal(d.start.enabled, true);
});

test('deriveControls: busy disables the actions and emphasises STOP', () => {
    const d = PV.deriveControls(status({ ui_state: 'grinding', grind_phase: 'predictive', busy: true }), online());
    for (const k of ['start', 'tare', 'motorTest']) {
        assert.equal(d[k].enabled, false);
        assert.equal(d[k].reason, 'A grind or motor run is active.');
    }
    assert.equal(d.stop.emphasis, true);
});

test('deriveControls: not-ready screens ask to go back to the main screen; the menu allows the motor test', () => {
    for (const ui of ['edit', 'calibration', 'confirm', 'purge_confirm', 'autotuning', 'grind_complete', 'grind_timeout', 'ota_update_failed']) {
        const d = PV.deriveControls(status({ ui_state: ui }), online());
        assert.equal(d.start.reason, 'Go back to the main screen on the grinder.', ui);
        assert.equal(d.tare.reason, 'Go back to the main screen on the grinder.', ui);
        assert.equal(d.motorTest.reason, 'Go back to the main screen on the grinder.', ui);
    }
    const menu = PV.deriveControls(status({ ui_state: 'menu' }), online());
    assert.equal(menu.start.enabled, false);
    assert.equal(menu.tare.enabled, false);
    assert.equal(menu.motorTest.enabled, true);
});

test('deriveControls: a running update disables everything but STOP and shows a banner', () => {
    const d = PV.deriveControls(status({ ui_state: 'ota_update' }), online());
    for (const k of ['start', 'tare', 'motorTest']) assert.equal(d[k].reason, 'A firmware update is running.');
    assert.equal(d.banner.level, 'warning');
    assert.match(d.banner.text, /Firmware update running/);
    assert.equal(d.stop.enabled, true);
    const held = PV.deriveControls(status({ start_block: 'update_in_progress', motor_test_block: 'update_in_progress' }), online());
    assert.equal(held.banner.level, 'warning');
    assert.equal(held.start.reason, 'A firmware update is running.');
    assert.equal(PV.deriveControls(status({ ui_state: 'ota_update_failed' }), online()).banner.level, 'error');
});

test('deriveControls: motor fault and motor disabled give a persistent error banner', () => {
    const fault = PV.deriveControls(status({ start_block: 'motor_fault', motor_test_block: 'motor_fault' }), online());
    assert.equal(fault.banner.level, 'error');
    assert.match(fault.banner.text, /restart it/);
    const dis = PV.deriveControls(status({ start_block: 'motor_disabled', motor_available: false }), online());
    assert.equal(dis.banner.level, 'error');
    assert.match(dis.banner.text, /Hardware board/);
    assert.match(dis.start.reason, /Hardware board/);
});

test('deriveControls: offline, reconnecting, connecting and web off disable everything except STOP', () => {
    const cases = [
        [offlineConn, 'The grinder is not reachable.'],
        [PV.connectionFailed(online(), 'network', 5), 'The grinder is not reachable.'],
        [PV.connectionFailed(online(), 'web_interface_off', 5), 'The web interface is switched off on the grinder.']
    ];
    for (const [c, text] of cases) {
        for (const st of [null, status()]) {
            const d = PV.deriveControls(st, c);
            for (const k of ['start', 'tare', 'motorTest']) {
                assert.equal(d[k].enabled, false);
                assert.equal(d[k].reason, text);
            }
            assert.equal(d.stop.enabled, true);
            assert.equal(d.banner, null);
        }
    }
    const first = PV.deriveControls(null, PV.newConnection());
    assert.equal(first.start.reason, 'Connecting to the grinder.');
    assert.equal(first.stop.enabled, true);
});

test('deriveControls: lead-in and a command on the wire block the actions with their own reason', () => {
    const lead = PV.deriveControls(status(), online(), { leadIn: true });
    assert.equal(lead.start.enabled, false);
    assert.match(lead.start.reason, /STOP/);
    assert.equal(lead.stop.emphasis, true);
    const pend = PV.deriveControls(status(), online(), { commandPending: true });
    assert.match(pend.tare.reason, /previous request/);
    assert.equal(pend.stop.emphasis, false);
});

test('deriveControls: online without a status yet waits', () => {
    assert.equal(PV.deriveControls(null, online()).start.reason, 'Waiting for the grinder to answer.');
});

test('lead-in: accepted, counting down, then watching, then dropped after lead_in_ms + 2 s', () => {
    let li = PV.leadInStart(1, 3000, 10000);
    assert.equal(li.phase, 'countdown');
    assert.equal(PV.leadInActive(li), true);
    assert.equal(PV.leadInView(li, 10000).seconds, 3);
    assert.equal(PV.leadInView(li, 10000).text, 'Starting DOUBLE in 3 s. Touch the grinder or press STOP to cancel.');
    assert.equal(PV.leadInView(li, 11001).seconds, 2);
    assert.equal(PV.leadInView(li, 12999).seconds, 1);
    li = PV.leadInTick(li, status(), 12999);
    assert.equal(li.phase, 'countdown');
    li = PV.leadInTick(li, status(), 13000);
    assert.equal(li.phase, 'watching');
    assert.equal(PV.leadInActive(li), true);
    assert.match(PV.leadInView(li, 13500).text, /Waiting for the grinder/);
    li = PV.leadInTick(li, status(), 14999);
    assert.equal(li.phase, 'watching');
    li = PV.leadInTick(li, status(), 15000, 14000); // status received 1 s after the lead-in ended
    assert.equal(li.phase, 'dropped');
    assert.equal(PV.leadInActive(li), false);
    assert.equal(PV.leadInView(li, 15000).text, 'The start was dropped on the grinder (check its screen).');
});

test('lead-in: ends when the grind shows up, in any phase', () => {
    const grinding = status({ ui_state: 'grinding', grind_phase: 'predictive', busy: true });
    assert.equal(PV.leadInTick(PV.leadInStart(0, 3000, 0), grinding, 3100), null);
    const watching = PV.leadInTick(PV.leadInStart(0, 3000, 0), status(), 3100);
    assert.equal(PV.leadInTick(watching, grinding, 3200), null);
    const dropped = PV.leadInTick(PV.leadInStart(0, 3000, 0), status(), 6000, 5900);
    assert.equal(dropped.phase, 'dropped');
    assert.equal(PV.leadInTick(dropped, grinding, 6100), null); // a late status heals a false "dropped"
    assert.equal(PV.grindVisible(status({ ui_state: 'purge_confirm' })), true);
    assert.equal(PV.grindVisible(status({ grind_phase: 'completed', ui_state: 'grind_complete' })), true);
    assert.equal(PV.grindVisible(status()), false);
    assert.equal(PV.grindVisible(null), false);
});

test('lead-in: the dropped notice goes away by itself, STOP cancels, bad input is safe', () => {
    const dropped = PV.leadInTick(PV.leadInStart(2, 3000, 0), status(), 5000, 4500);
    assert.equal(PV.leadInTick(dropped, status(), 5000 + PV.LEAD_IN_DROPPED_SHOW_MS - 1).phase, 'dropped');
    assert.equal(PV.leadInTick(dropped, status(), 5000 + PV.LEAD_IN_DROPPED_SHOW_MS), null);
    assert.equal(PV.leadInCancel(PV.leadInStart(0, 3000, 0)), null);
    assert.equal(PV.leadInTick(null, status(), 1), null);
    assert.equal(PV.leadInView(null, 1).visible, false);
    assert.equal(PV.leadInStart(0, undefined, 0).leadInMs, 3000);
    assert.equal(PV.leadInStart(0, 1000, 0).leadInMs, 1000);
    assert.equal(PV.leadInView(PV.leadInStart(0, 3000, 0), 99999).seconds, 0);
});

test('lead-in: a long gap (hidden tab) stays watching until a fresh status arrives', () => {
    const li = PV.leadInStart(0, 3000, 0);
    // The last status is from before the start (received at 0): it proves nothing.
    const watching = PV.leadInTick(li, status(), 60000, 0);
    assert.equal(watching.phase, 'watching');
    assert.equal(PV.leadInTick(watching, status(), 60100, 0).phase, 'watching');
    // A fresh idle status after the gap: now the start counts as dropped.
    assert.equal(PV.leadInTick(watching, status(), 60200, 60100).phase, 'dropped');
    // A fresh status that shows the grind ends the lead-in instead.
    const grinding = status({ ui_state: 'grinding', grind_phase: 'predictive', busy: true });
    assert.equal(PV.leadInTick(watching, grinding, 60200, 60100), null);
});

test('lead-in: offline or a stale status never gives dropped', () => {
    const li = PV.leadInStart(0, 3000, 0);
    assert.equal(PV.leadInTick(li, status(), 100000, undefined).phase, 'watching');
    assert.equal(PV.leadInTick(li, status(), 100000, null).phase, 'watching');
    assert.equal(PV.leadInTick(li, null, 100000, 500).phase, 'watching');
    // received before the lead-in ended, or less than 1 s after it
    assert.equal(PV.leadInTick(li, status(), 100000, 2999).phase, 'watching');
    assert.equal(PV.leadInTick(li, status(), 100000, 3000 + PV.LEAD_IN_STATUS_AFTER_MS - 1).phase, 'watching');
    assert.equal(PV.leadInTick(li, status(), 100000, 3000 + PV.LEAD_IN_STATUS_AFTER_MS).phase, 'dropped');
});

test('deriveControls: Tare is disabled while an update holds the guard', () => {
    for (const key of ['start_block', 'motor_test_block']) {
        const d = PV.deriveControls(status({ [key]: 'update_in_progress' }), online());
        assert.equal(d.tare.enabled, false, key);
        assert.equal(d.tare.reason, 'A firmware update is running.', key);
    }
});

test('nextPollTask: a failed settings or info request forces status next', () => {
    const f = { haveStatus: true, needSettings: true, needInfo: true, online: true };
    assert.equal(PV.nextPollTask(Object.assign({}, f, { forceStatus: true })), 'status');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { forceStatus: false })), 'settings');
});

test('readoutView: normal, null weight, hints, stale and unknown states', () => {
    let r = PV.readoutView(status({ weight_g: 12.34 }), online(), 2000);
    assert.deepEqual([r.state, r.weight, r.phase, r.hint, r.stale], ['Ready', '12.3 g', '', '', false]);
    r = PV.readoutView(status({ weight_g: null }), online(), 2000);
    assert.equal(r.weight, '-- g');
    r = PV.readoutView(status({ ui_state: 'grinding', grind_phase: 'predictive', busy: true, weight_g: 5 }), online(), 2000);
    assert.equal(r.state, 'Grinding');
    assert.equal(r.phase, 'Grinding to the target weight');
    r = PV.readoutView(status({ ui_state: 'grind_complete', grind_phase: 'completed' }), online(), 2000);
    assert.match(r.hint, /grinder screen/);
    assert.notEqual(PV.readoutView(status({ ui_state: 'grind_timeout' }), online(), 2000).hint, '');
    r = PV.readoutView(status({ weight_g: 7 }), offlineConn, 2000 + 42000);
    assert.equal(r.stale, true);
    assert.equal(r.weight, '7.0 g');
    assert.equal(r.seen, ''); // never answered: no "last seen"
    const lost = PV.connectionFailed(PV.connectionFailed(online(), 'network', 2), 'network', 3);
    assert.equal(PV.readoutView(status(), lost, 1000 + 42000).seen, 'Last seen 42 s ago');
    assert.equal(PV.readoutView(null, PV.newConnection(), 0).state, 'Connecting');
    assert.equal(PV.readoutView(null, offlineConn, 0).state, 'Offline');
    assert.equal(PV.readoutView(null, offlineConn, 0).weight, '-- g');
    assert.equal(PV.readoutView(status({ ui_state: 'new_thing' }), online(), 0).state, 'new_thing');
});

test('readoutView: stale without a status and while the web interface is off, live only online with a status', () => {
    // before any status arrives the bar must not show a live (green) dot
    assert.equal(PV.readoutView(null, PV.newConnection(), 0).stale, true);
    assert.equal(PV.readoutView(null, online(), 0).stale, true);
    assert.equal(PV.readoutView(null, offlineConn, 0).stale, true);
    // web interface switched off: the last values stay but are marked old, without a "last seen" line
    const webOff = PV.connectionFailed(online(), 'web_interface_off', 5000);
    assert.equal(webOff.state, 'web_off');
    let r = PV.readoutView(status({ weight_g: 18 }), webOff, 9000);
    assert.deepEqual([r.state, r.weight, r.stale, r.seen], ['Ready', '18.0 g', true, '']);
    r = PV.readoutView(null, webOff, 9000);
    assert.deepEqual([r.state, r.weight, r.stale, r.seen], ['Web interface off', '-- g', true, '']);
    // a lost connection stays stale; back online with a status it is live again, also while the screen is busy
    assert.equal(PV.readoutView(status(), PV.connectionFailed(online(), 'network', 2), 3).stale, true);
    assert.equal(PV.readoutView(status(), PV.connectionOk(webOff, 9500), 9600).stale, false);
    assert.equal(PV.readoutView(status(), PV.connectionFailed(online(), 'ui_busy', 2), 3).stale, false);
    assert.equal(PV.readoutView(status({ ui_state: 'grinding', busy: true }), online(), 0).stale, false);
});

test('leadInView: the ring fraction runs from 0 to 1 over the lead-in, with the seconds as its count', () => {
    const li = PV.leadInStart(1, 3000, 10000);
    assert.deepEqual([PV.leadInView(li, 10000).fraction, PV.leadInView(li, 10000).count], [0, '3']);
    assert.deepEqual([PV.leadInView(li, 11500).fraction, PV.leadInView(li, 11500).count], [0.5, '2']);
    assert.deepEqual([PV.leadInView(li, 13000).fraction, PV.leadInView(li, 13000).count], [1, '']);
    assert.equal(PV.leadInView(li, 99999).fraction, 1);
    assert.equal(PV.leadInView(li, 9000).fraction, 0); // a clock that stepped back never gives a negative ring
    const watching = PV.leadInView(PV.leadInTick(li, status(), 13000), 13500);
    assert.deepEqual([watching.fraction, watching.count, watching.dropped], [1, '', false]);
    const dropped = PV.leadInView(PV.leadInTick(li, status(), 15000, 14000), 15000);
    assert.deepEqual([dropped.fraction, dropped.count, dropped.dropped], [0, '', true]);
    assert.deepEqual(PV.leadInView(null, 1), { visible: false, seconds: 0, fraction: 0, count: '', dropped: false, text: '' });
    assert.equal(PV.leadInView(PV.leadInStart(0, 0, 0), 0).fraction, 1); // no lead-in time: full at once
});

test('readoutView: short words for the status bar, the full words as detail, the weight without its unit', () => {
    let r = PV.readoutView(status({ weight_g: 12.34 }), online(), 0);
    assert.deepEqual([r.short, r.detail, r.value], ['Ready', '', '12.3']);
    r = PV.readoutView(status({ ui_state: 'menu' }), online(), 0);
    assert.deepEqual([r.short, r.detail, r.state], ['Menu open', 'Menu open on the grinder screen', 'Menu open on the grinder screen']);
    r = PV.readoutView(status({ ui_state: 'confirm' }), online(), 0);
    assert.deepEqual([r.short, r.detail], ['Confirm on grinder', 'Waiting for a confirmation on the grinder screen']);
    // a hint already says where to look: no second sentence
    r = PV.readoutView(status({ ui_state: 'grind_complete', grind_phase: 'completed' }), online(), 0);
    assert.deepEqual([r.short, r.detail], ['Grind complete', '']);
    assert.notEqual(r.hint, '');
    // an unknown state is shown as it came, in both places
    r = PV.readoutView(status({ ui_state: 'new_thing' }), online(), 0);
    assert.deepEqual([r.short, r.detail], ['new_thing', '']);
    r = PV.readoutView(status({ weight_g: null }), online(), 0);
    assert.equal(r.value, '--');
    r = PV.readoutView(null, offlineConn, 0);
    assert.deepEqual([r.short, r.detail, r.value], ['Offline', '', '--']);
    // every known state has short words that are no longer than the full ones
    for (const ui of Object.keys(PC.STATE_TEXT)) {
        const short = PV.shortStateLabel(ui);
        assert.ok(short.length > 0 && short.length <= 18 && short.length <= PC.STATE_TEXT[ui].length, ui);
    }
    // the lead-in of a remote start: the grinder still says ready, the short words say what happens
    r = PV.readoutView(status(), online(), 0, { leadIn: true });
    assert.deepEqual([r.short, r.state, r.detail], ['Starting', 'Ready', '']);
});

test('readoutView: the tone of the state dot is ready, busy, neutral or stale', () => {
    const tone = (st, conn, extra) => PV.readoutView(st, conn, 0, extra).tone;
    assert.equal(tone(status(), online()), 'ready');
    assert.equal(tone(status({ ui_state: 'grinding', busy: true }), online()), 'busy');
    assert.equal(tone(status({ busy: true }), online()), 'busy'); // a motor test on the main screen
    assert.equal(tone(status(), online(), { leadIn: true }), 'busy');
    assert.equal(tone(status(), online(), { leadIn: false }), 'ready');
    // neither ready nor running: never the green dot
    for (const ui of ['menu', 'confirm', 'edit', 'calibration', 'grind_complete', 'grind_timeout', 'purge_confirm',
        'autotuning', 'ota_update', 'ota_update_failed', 'new_thing']) {
        assert.equal(tone(status({ ui_state: ui }), online()), 'neutral', ui);
    }
    // the main screen, but a start would be refused: not green under the red banner
    for (const over of [{ start_block: 'motor_fault' }, { start_block: 'motor_disabled' }, { start_block: 'load_cell_no_data' },
        { start_block: 'update_in_progress' }, { motor_test_block: 'motor_fault' }, { motor_test_block: 'motor_disabled' },
        { motor_available: false }, { warnings: ['motor_fault'] }, { warnings: ['motor_disabled'] }]) {
        assert.equal(tone(status(over), online()), 'neutral', JSON.stringify(over));
    }
    // a warning that does not stop a start keeps the green dot; a running grind stays busy whatever is blocked
    assert.equal(tone(status({ warnings: ['load_cell_not_calibrated'] }), online()), 'ready');
    assert.equal(tone(status({ start_block: 'busy', busy: true }), online()), 'busy');
    assert.equal(tone(null, PV.newConnection()), 'stale');
    assert.equal(tone(null, online()), 'stale');
    assert.equal(tone(status(), offlineConn), 'stale');
    assert.equal(tone(status({ busy: true }), offlineConn, { leadIn: true }), 'stale');
    assert.equal(tone(status(), PV.connectionFailed(online(), 'web_interface_off', 1)), 'stale');
});

test('profileCards: targets follow the grind mode, the selected profile is flagged', () => {
    const values = { grind_mode: 'weight', single_weight_g: 9, double_weight_g: 18, custom_weight_g: 21.5,
        single_time_s: 5, double_time_s: 10, custom_time_s: 12 };
    let cards = PV.profileCards({ values: values, current_profile: 1 });
    assert.deepEqual(cards.map((c) => c.name), ['Single', 'Double', 'Custom']);
    assert.deepEqual(cards.map((c) => c.target), ['9.0 g', '18.0 g', '21.5 g']);
    assert.deepEqual(cards.map((c) => c.selected), [false, true, false]);
    cards = PV.profileCards({ values: Object.assign({}, values, { grind_mode: 'time' }), current_profile: 2 });
    assert.deepEqual(cards.map((c) => c.target), ['5.0 s', '10.0 s', '12.0 s']);
    assert.deepEqual(cards.map((c) => c.selected), [false, false, true]);
    cards = PV.profileCards(null);
    assert.deepEqual(cards.map((c) => c.target), ['--', '--', '--']);
    assert.deepEqual(cards.map((c) => c.selected), [false, false, false]);
});

test('the view module has no DOM, timer, storage or network use', () => {
    const fs = require('node:fs');
    const path = require('node:path');
    const src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'portal', 'portal-view.js'), 'utf8');
    for (const word of ['document', 'window', 'setTimeout', 'setInterval', 'fetch(', 'localStorage', 'innerHTML', 'Date.now']) {
        assert.equal(src.includes(word), false, word);
    }
});

// ---------------------------------------------------------------------------------------------- step 3: settings form
const fs3 = require('node:fs');
const path3 = require('node:path');

// The field table of the schema, read from the dev-server model so the test follows the contract.
function schemaFields() {
    const src = fs3.readFileSync(path3.join(__dirname, '..', 'portal', 'portal_model.py'), 'utf8');
    const fields = [];
    const re = /_f\("(\w+)", "(\w+)", "(\w+)"([^)]*)\)/g;
    let m;
    while ((m = re.exec(src)) !== null) {
        const f = { key: m[1], type: m[2], group: m[3] };
        const unit = /unit="([^"]+)"/.exec(m[4]);
        const min = /min=([\d.]+)/.exec(m[4]);
        const max = /max=([\d.]+)/.exec(m[4]);
        const step = /step=([\d.]+)/.exec(m[4]);
        const choices = /choices=\[([^\]]*)\]/.exec(m[4]);
        if (unit) f.unit = unit[1];
        if (min) f.min = parseFloat(min[1]);
        if (max) f.max = parseFloat(max[1]);
        if (step) f.step = parseFloat(step[1]);
        if (choices) f.choices = choices[1].split(',').map((c) => c.trim().replace(/"/g, ''));
        fields.push(f);
    }
    return fields;
}
const FIELDS = schemaFields();
const VALUES = {
    ble_startup: true, logging: false, swipe: false, auto_start: false, auto_return: false,
    brightness_normal: 100, brightness_screensaver: 35, purge_mode: 'remove', purge_amount_g: 1.0,
    freshness_hours: 8.0, screen_dim_min: 5, screen_off_min: 60, grind_mode: 'weight', single_weight_g: 9.0, single_time_s: 5.0, double_weight_g: 18.0,
    double_time_s: 10.0, custom_weight_g: 21.5, custom_time_s: 12.0, device_name: 'grindbyweight'
};

test('the schema model has the 20 keys and the key table references each exactly once', () => {
    assert.equal(FIELDS.length, 20);
    assert.deepEqual(FIELDS.map((f) => f.key).sort(), Object.keys(VALUES).sort());
    const grouped = PV.groupedKeys();
    assert.deepEqual(grouped.slice().sort(), FIELDS.map((f) => f.key).sort());
    assert.equal(new Set(grouped).size, grouped.length);
});

test('settingsGroups: all 20 keys exactly once, no Other for a known schema, order Profiles to Device', () => {
    const groups = PV.settingsGroups(FIELDS);
    assert.deepEqual(groups.map((g) => g.title), ['Profiles', 'Display', 'Grinding', 'Device']);
    const keys = [].concat(...groups.map((g) => g.fields.map((f) => f.key)));
    assert.equal(keys.length, 20);
    assert.equal(new Set(keys).size, 20);
});

test('settingsGroups: an unknown key lands in Other, an empty group is left out', () => {
    const extra = { key: 'new_thing', type: 'number', group: 'settings', min: 0, max: 5 };
    const groups = PV.settingsGroups(FIELDS.concat([extra]));
    const other = groups[groups.length - 1];
    assert.equal(other.title, 'Other');
    assert.deepEqual(other.fields.map((f) => f.key), ['new_thing']);
    assert.equal(other.fields[0].kind, 'stepper');
    const onlyDevice = PV.settingsGroups(FIELDS.filter((f) => f.key === 'device_name'));
    assert.deepEqual(onlyDevice.map((g) => g.title), ['Device']);
    assert.deepEqual(PV.settingsGroups([]), []);
});

test('chg021 V5: every settings group icon (Other included) is a symbol in page.src.html', () => {
    const html = fs3.readFileSync(path3.join(__dirname, '..', '..', 'web', 'portal', 'page.src.html'), 'utf8');
    const ids = new Set([...html.matchAll(/id="i-([a-z-]+)"/g)].map((m) => m[1]));
    const extra = { key: 'new_thing', type: 'number', group: 'settings', min: 0, max: 5 };
    const groups = PV.settingsGroups(FIELDS.concat([extra]));
    assert.deepEqual(groups.map((g) => g.id), ['profiles', 'display', 'grinding', 'device', 'other']);
    for (const g of groups) {
        assert.equal(typeof g.icon, 'string', g.id);
        assert.ok(ids.has(g.icon), `group ${g.id}: icon "${g.icon}" has no id="i-${g.icon}" symbol`);
    }
});

test('fieldView: widget per key and type', () => {
    const kinds = {};
    for (const g of PV.settingsGroups(FIELDS)) for (const f of g.fields) kinds[f.key] = f.kind;
    assert.equal(kinds.logging, 'switch');
    assert.equal(kinds.grind_mode, 'segmented');
    assert.equal(kinds.purge_mode, 'segmented');
    assert.equal(kinds.brightness_normal, 'slider');
    assert.equal(kinds.purge_amount_g, 'slider');
    assert.equal(kinds.freshness_hours, 'select');
    assert.equal(kinds.screen_dim_min, 'select');
    assert.equal(kinds.screen_off_min, 'select');
    assert.equal(kinds.single_weight_g, 'stepper');
    assert.equal(kinds.device_name, 'text');
    const mode = PV.settingsGroups(FIELDS)[2].fields[1];
    assert.deepEqual(mode.choices.map((c) => c.label), ['Prime (keep)', 'Purge (remove)']);
    assert.equal(PV.rangeText(FIELDS.find((f) => f.key === 'single_weight_g')), '5 to 1000 g');
    assert.equal(PV.rangeText(FIELDS.find((f) => f.key === 'single_time_s')), '0.5 to 25 s');
});

test('selectOptions: the steps per key, plus a stored off-step value', () => {
    const fr = (v) => PV.selectOptions('freshness_hours', v);
    assert.deepEqual(fr(8).map((o) => o.value), [0.5, 1, 2, 3, 4, 8, 12, 24, 48]);
    assert.equal(fr(8)[0].label, '30 minutes');
    assert.equal(fr(8)[1].label, '1 hour');
    assert.equal(fr(8)[5].label, '8 hours');
    assert.deepEqual(fr(6).map((o) => o.value), [0.5, 1, 2, 3, 4, 6, 8, 12, 24, 48]);
    const dim = PV.selectOptions('screen_dim_min', 5);
    assert.deepEqual(dim.map((o) => o.value), [1, 2, 5, 10, 15, 30, 60, 0]);
    assert.deepEqual(dim.map((o) => o.label), ['1 minute', '2 minutes', '5 minutes', '10 minutes', '15 minutes', '30 minutes', '1 hour', 'Never']);
    const off = PV.selectOptions('screen_off_min', 60);
    assert.deepEqual(off.map((o) => o.label), ['10 minutes', '30 minutes', '1 hour', '2 hours', '4 hours', '8 hours', 'Never']);
    assert.deepEqual(PV.selectOptions('screen_off_min', 90).map((o) => o.value), [10, 30, 60, 90, 120, 240, 480, 0]);
    assert.equal(PV.selectOptions('screen_off_min', 90)[3].label, '1.5 hours');
    assert.deepEqual(PV.selectOptions('screen_dim_min', 7).map((o) => o.value), [1, 2, 5, 7, 10, 15, 30, 60, 0]);
    assert.equal(PV.selectOptions('screen_dim_min', 7)[3].label, '7 minutes');
});

test('adjustedNotes: screen timeouts read Never for 0 and minutes otherwise', () => {
    const notes = PV.adjustedNotes(['screen_dim_min', 'screen_off_min'], { screen_dim_min: 0, screen_off_min: 30 }, FIELDS);
    assert.equal(notes.screen_dim_min, 'The grinder adjusted this to Never.');
    assert.equal(notes.screen_off_min, 'The grinder adjusted this to 30 minutes.');
    const c = PC.describeAdjusted(['screen_dim_min'], { screen_dim_min: 0 }, FIELDS);
    assert.equal(c[0], 'Dim screen after was adjusted by the grinder to Never.');
});

test('formPatch: nothing changed gives an empty patch, only changed keys are sent, in their own types', () => {
    let form = PV.formNew(VALUES);
    assert.deepEqual(PV.formPatch(form, FIELDS), {});
    assert.equal(PV.formChangeCount(form, FIELDS), 0);
    form = PV.formSet(form, 'brightness_normal', '80');       // slider: text
    form = PV.formSet(form, 'grind_mode', 'time');             // enum: string
    form = PV.formSet(form, 'auto_start', true);               // bool
    form = PV.formSet(form, 'double_time_s', '9.5');           // number as text
    form = PV.formSet(form, 'single_weight_g', 9);             // same value as stored 9.0: not a change
    form = PV.formSet(form, 'device_name', 'grindbyweight');   // same: not a change
    const patch = PV.formPatch(form, FIELDS);
    assert.deepEqual(patch, { auto_start: true, brightness_normal: 80, grind_mode: 'time', double_time_s: 9.5 });
    assert.equal(typeof patch.brightness_normal, 'number');
    assert.equal(PV.formChangeCount(form, FIELDS), 4);
    assert.equal(PV.changeCountText(1), '1 change');
    assert.equal(PV.changeCountText(3), '3 changes');
});

test('formPatch: an empty or unreadable number is left out, never NaN; an empty text is sent as typed', () => {
    let form = PV.formNew(VALUES);
    form = PV.formSet(form, 'single_weight_g', '');
    form = PV.formSet(form, 'double_weight_g', 'abc');
    form = PV.formSet(form, 'custom_weight_g', '1e3');
    form = PV.formSet(form, 'device_name', '');
    const patch = PV.formPatch(form, FIELDS);
    assert.deepEqual(patch, { device_name: '' });
    assert.deepEqual(Object.keys(PV.entryNotes(form, FIELDS)), ['single_weight_g', 'double_weight_g', 'custom_weight_g']);
    assert.equal(PV.parseNumberText('9,5'), 9.5);
    assert.equal(PV.parseNumberText(' 9.5 '), 9.5);
    assert.equal(PV.parseNumberText('9.5.1'), undefined);
    assert.equal(PV.parseNumberText('1,000.5'), undefined);
    assert.equal(PV.parseNumberText('.5'), 0.5);
    assert.equal(PV.parseNumberText('-3'), -3);
});

test('formPatch: the page sends an uppercase device name as typed and never current_profile', () => {
    let form = PV.formNew(Object.assign({ current_profile: 1 }, VALUES));
    form = PV.formSet(form, 'device_name', 'MyGrinder');
    assert.deepEqual(PV.formPatch(form, FIELDS), { device_name: 'MyGrinder' });
    form = PV.formSet(form, 'current_profile', 2);
    assert.equal('current_profile' in PV.formPatch(form, FIELDS), false);
});

test('formDiscard restores the stored values', () => {
    let form = PV.formNew(VALUES);
    form = PV.formSet(form, 'single_weight_g', '12');
    form = PV.formSet(form, 'logging', true);
    form = PV.formDiscard(form);
    assert.equal(PV.formChangeCount(form, FIELDS), 0);
    assert.deepEqual(form.draft, VALUES);
});

test('formSaved: saved keys show what the grinder applied, other entries stay', () => {
    let form = PV.formNew(VALUES);
    form = PV.formSet(form, 'brightness_normal', '5');
    form = PV.formSet(form, 'single_weight_g', '');
    const patch = PV.formPatch(form, FIELDS);
    const applied = Object.assign({}, VALUES, { brightness_normal: 15 });
    const saved = PV.formSaved(form, applied, patch, FIELDS);
    assert.equal(saved.draft.brightness_normal, 15);
    assert.equal(saved.stored.brightness_normal, 15);
    assert.equal(saved.draft.single_weight_g, '');
    assert.equal(PV.formChangeCount(saved, FIELDS), 0);
});

test('formRefresh: fresh values never overwrite edits, other fields follow the grinder', () => {
    let form = PV.formNew(VALUES);
    form = PV.formSet(form, 'single_weight_g', '12');
    const fresh = Object.assign({}, VALUES, { single_weight_g: 10, logging: true, brightness_normal: 70 });
    const r = PV.formRefresh(form, fresh, FIELDS);
    assert.equal(r.form.draft.single_weight_g, '12');
    assert.equal(r.form.stored.single_weight_g, 10);
    assert.equal(r.form.draft.logging, true);
    assert.equal(r.form.draft.brightness_normal, 70);
    assert.deepEqual(PV.formPatch(r.form, FIELDS), { single_weight_g: 12 });
    assert.match(r.notice, /unsaved edits are kept/);
    assert.match(r.notice, /Single, weight/);
});

test('formRefresh: no edits means no notice; an edit equal to the new value becomes clean; a blank entry is kept', () => {
    let form = PV.formNew(VALUES);
    let r = PV.formRefresh(form, Object.assign({}, VALUES, { logging: true }), FIELDS);
    assert.equal(r.notice, '');
    assert.equal(r.form.draft.logging, true);
    form = PV.formSet(form, 'single_weight_g', '10');
    r = PV.formRefresh(form, Object.assign({}, VALUES, { single_weight_g: 10, logging: true }), FIELDS);
    assert.equal(PV.formChangeCount(r.form, FIELDS), 0);
    form = PV.formSet(PV.formNew(VALUES), 'single_weight_g', '');
    r = PV.formRefresh(form, Object.assign({}, VALUES, { single_weight_g: 11 }), FIELDS);
    assert.equal(r.form.draft.single_weight_g, '');
});

test('stepValue: no float drift, 0.1 g and 0.5 s steps, clamped to the limits', () => {
    const g = FIELDS.find((f) => f.key === 'single_weight_g');
    const s = FIELDS.find((f) => f.key === 'single_time_s');
    assert.equal(PV.stepValue(9.0, 1, g, 9), 9.1);
    assert.equal(PV.stepValue('9.1', 1, g, 9), 9.2);
    assert.equal(PV.stepValue(0.7, 1, { type: 'number', unit: 'g' }, 0), 0.8);
    assert.equal(PV.stepValue(0.3, -1, { type: 'number', unit: 'g' }, 0), 0.2);
    let v = 0;
    for (let i = 0; i < 10; i++) v = PV.stepValue(v, 1, { type: 'number', unit: 'g' }, 0);
    assert.equal(v, 1);
    assert.equal(PV.stepValue(5.0, 1, s, 5), 5.5);
    assert.equal(PV.stepValue(5.5, -1, s, 5), 5);
    assert.equal(PV.stepValue(5, -1, g, 5), 5);        // at the lower limit
    assert.equal(PV.stepValue(1000, 1, g, 9), 1000);   // at the upper limit
    assert.equal(PV.stepValue(2000, 1, g, 9), 1000);
    assert.equal(PV.stepValue('', 1, g, 9), 9.1);      // blank starts from the stored value
    assert.equal(PV.stepValue('', 1, g), 5.1);         // ... or the lower limit
    assert.equal(PV.stepValue(0.5, -1, s, 5), 0.5);
});

test('saveBarView: hidden and disabled without changes, count with changes, reasons while it cannot write', () => {
    let form = PV.formNew(VALUES);
    let bar = PV.saveBarView(form, FIELDS, status(), online(), {});
    assert.deepEqual([bar.visible, bar.canSave], [false, false]);
    form = PV.formSet(form, 'single_weight_g', '10');
    form = PV.formSet(form, 'logging', true);
    form = PV.formSet(form, 'swipe', true);
    bar = PV.saveBarView(form, FIELDS, status(), online(), {});
    assert.deepEqual([bar.visible, bar.canSave, bar.countText, bar.reason], [true, true, '3 changes', '']);
    const cases = [
        [status({ busy: true, ui_state: 'grinding' }), online(), {}, PV.REASON.busy],
        [status({ ui_state: 'edit' }), online(), {}, PV.REASON.notReady],
        [status({ ui_state: 'confirm' }), online(), {}, PV.REASON.notReady],
        [status({ ui_state: 'ota_update' }), online(), {}, PV.REASON.update],
        [status(), online(), { leadIn: true }, PV.REASON.leadIn],
        [status(), online(), { commandPending: true }, PV.REASON.pending],
        [status(), offlineConn, {}, PV.REASON.offline],
        [status(), PV.connectionFailed(online(), 'web_interface_off', 1), {}, PV.REASON.webOff],
        [null, PV.newConnection(), {}, PV.REASON.connecting]
    ];
    for (const [st, c, extra, reason] of cases) {
        bar = PV.saveBarView(form, FIELDS, st, c, extra);
        assert.equal(bar.canSave, false, reason);
        assert.equal(bar.reason, reason);
        assert.equal(bar.visible, true);
    }
    assert.equal(PV.saveBarView(form, FIELDS, status({ ui_state: 'menu' }), online(), {}).canSave, true);
});

test('adjustedNotes: whole percent, grams, hours snapped', () => {
    const values = Object.assign({}, VALUES, { brightness_normal: 15, purge_amount_g: 2.5, freshness_hours: 0.5 });
    const notes = PV.adjustedNotes(['brightness_normal', 'purge_amount_g', 'freshness_hours'], values, FIELDS);
    assert.equal(notes.brightness_normal, 'The grinder adjusted this to 15 %.');
    assert.equal(notes.purge_amount_g, 'The grinder adjusted this to 2.5 g.');
    assert.equal(notes.freshness_hours, 'The grinder adjusted this to 0.5 h.');
    assert.deepEqual(PV.adjustedNotes([], values, FIELDS), {});
    assert.deepEqual(PV.adjustedNotes(undefined, values, FIELDS), {});
    const top = PV.adjustedNotes(['freshness_hours'], Object.assign({}, VALUES, { freshness_hours: 48 }), FIELDS);
    assert.equal(top.freshness_hours, 'The grinder adjusted this to 48 h.');
});

test('saveErrorView: field text for out_of_range and invalid_device_name, form text for every other 400', () => {
    const patch = { single_weight_g: 2000, single_time_s: 5.5, double_weight_g: 4 };
    let v = PV.saveErrorView('out_of_range', patch, FIELDS);
    assert.deepEqual(Object.keys(v.fields), ['single_weight_g', 'double_weight_g']);
    assert.match(v.fields.single_weight_g, /5 to 1000 g/);
    assert.match(v.form, /Nothing was saved/);
    v = PV.saveErrorView('invalid_device_name', { device_name: 'Bad Name' }, FIELDS);
    assert.deepEqual(Object.keys(v.fields), ['device_name']);
    assert.match(v.fields.device_name, /lowercase a-z/);
    for (const code of ['empty_body', 'body_too_large', 'malformed_json', 'nested_value', 'duplicate_key', 'unknown_key',
        'unexpected_field', 'missing_field', 'empty_patch', 'unknown_command']) {
        v = PV.saveErrorView(code, patch, FIELDS);
        assert.deepEqual(v.fields, {}, code);
        assert.equal(v.form, PC.describeError(code) + ' Your edits are kept.', code);
        assert.equal(v.reload, false, code);
    }
    // a wrong type with one changed key can be pinned to it, with several it cannot
    assert.deepEqual(Object.keys(PV.saveErrorView('wrong_type', { logging: true }, FIELDS).fields), ['logging']);
    assert.deepEqual(PV.saveErrorView('wrong_type', patch, FIELDS).fields, {});
    // out_of_range that names no key of the patch stays a form text
    v = PV.saveErrorView('out_of_range', { single_weight_g: 9 }, FIELDS);
    assert.deepEqual(v.fields, {});
    assert.match(v.form, /outside the range/);
});

test('saveErrorView: refusals that keep edits, and those that ask for a settings reload', () => {
    for (const code of ['busy', 'not_ready', 'command_in_flight', 'update_in_progress', 'web_interface_off']) {
        const v = PV.saveErrorView(code, { logging: true }, FIELDS);
        assert.equal(v.reload, false, code);
        assert.match(v.form, /Your edits are kept\.$/, code);
    }
    for (const code of ['ui_busy', 'store_failed', 'network', 'timeout']) {
        assert.equal(PV.saveErrorView(code, { logging: true }, FIELDS).reload, true, code);
    }
});

test('renameNotice and renameLink: only for a plain .local name, link only over http', () => {
    assert.match(PV.renameNotice('espresso-2.local'), /espresso-2\.local/);
    assert.match(PV.renameNotice('espresso-2.local'), /old name stops answering/);
    assert.equal(PV.renameNotice('evil.com'), '');
    assert.equal(PV.renameNotice('a b.local'), '');
    assert.equal(PV.renameNotice(undefined), '');
    assert.equal(PV.renameLink('espresso.local', 'http:', ''), 'http://espresso.local/');
    assert.equal(PV.renameLink('espresso.local', 'http:', '8080'), 'http://espresso.local:8080/');
    assert.equal(PV.renameLink('espresso.local', 'file:', ''), '');
    assert.equal(PV.renameLink('evil.com', 'http:', ''), '');
});

test('currentProfileText and settingsPlaceholder', () => {
    assert.match(PV.currentProfileText({ current_profile: 1 }), /Double/);
    assert.equal(PV.currentProfileText({ current_profile: 7 }), '');
    assert.equal(PV.currentProfileText(null), '');
    assert.equal(PV.settingsPlaceholder(true, true, online(), false), '');
    assert.equal(PV.settingsPlaceholder(false, false, online(), false), 'Loading the settings.');
    assert.match(PV.settingsPlaceholder(false, false, online(), true), /Update the firmware/);
    assert.equal(PV.settingsPlaceholder(false, true, offlineConn, false), PV.REASON.offline);
});

test('nextPollTask: the schema is fetched before the settings', () => {
    const f = { haveStatus: true, needSchema: true, needSettings: true, needInfo: true, online: true };
    assert.equal(PV.nextPollTask(f), 'schema');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { needSchema: false })), 'settings');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { forceStatus: true })), 'status');
});

// ---------------------------------------------------------------------------------------------- step 3: audit round 1
test('saveReason: Save is blocked while a WiFi update holds the grinder (start or motor test block)', () => {
    let form = PV.formSet(PV.formNew(VALUES), 'logging', true);
    for (const key of ['start_block', 'motor_test_block']) {
        const bar = PV.saveBarView(form, FIELDS, status({ [key]: 'update_in_progress' }), online(), {});
        assert.equal(bar.canSave, false, key);
        assert.equal(bar.reason, PV.REASON.update, key);
        assert.equal(bar.visible, true);
    }
});

test('formSaved: a key the user did not edit follows the reply and makes no pending change', () => {
    let form = PV.formNew(VALUES);
    form = PV.formSet(form, 'logging', true);
    const patch = PV.formPatch(form, FIELDS);
    const saved = PV.formSaved(form, Object.assign({}, VALUES, { single_weight_g: 12, logging: true }), patch, FIELDS);
    assert.equal(saved.draft.single_weight_g, 12);
    assert.equal(saved.stored.single_weight_g, 12);
    assert.deepEqual(PV.formPatch(saved, FIELDS), {});
    assert.equal(PV.formChangeCount(saved, FIELDS), 0);
});

test('formRefresh: the typed text is kept when it already equals the new value', () => {
    let form = PV.formSet(PV.formNew(VALUES), 'single_weight_g', '9.0'); // stored 9.0, typed "9.0"
    let r = PV.formRefresh(form, Object.assign({}, VALUES, { logging: true }), FIELDS);
    assert.equal(r.form.draft.single_weight_g, '9.0');
    form = PV.formSet(PV.formNew(VALUES), 'single_weight_g', '10');
    r = PV.formRefresh(form, Object.assign({}, VALUES, { single_weight_g: 10 }), FIELDS);
    assert.equal(r.form.draft.single_weight_g, '10'); // not rewritten to 10.0
    assert.equal(r.notice, '');
    form = PV.formSet(PV.formNew(VALUES), 'single_weight_g', '5.');
    r = PV.formRefresh(form, Object.assign({}, VALUES, { single_weight_g: 5 }), FIELDS);
    assert.equal(r.form.draft.single_weight_g, '5.');
    assert.equal(PV.formChangeCount(r.form, FIELDS), 0);
});

test('saveErrorView: malformed_json with a device name is shown at the field', () => {
    const v = PV.saveErrorView('malformed_json', { device_name: 'grïnder' }, FIELDS);
    assert.deepEqual(Object.keys(v.fields), ['device_name']);
    assert.match(v.form, /Nothing was saved/);
});

test('fieldView: the device name carries its 24 character limit; firstInvalidKey follows the form order', () => {
    const name = PV.settingsGroups(FIELDS)[3].fields.find((f) => f.key === 'device_name');
    assert.equal(name.max, 24);
    // patch order is schema order, the form shows Profiles before Device
    assert.equal(PV.firstInvalidKey({ device_name: 'x', double_weight_g: 'y', single_weight_g: 'z' }, FIELDS), 'single_weight_g');
    assert.equal(PV.firstInvalidKey({ device_name: 'x' }, FIELDS), 'device_name');
    assert.equal(PV.firstInvalidKey({}, FIELDS), '');
});

test('validSettingsDoc: only an object with a values object is usable', () => {
    assert.equal(PV.validSettingsDoc({ values: {}, current_profile: 0 }), true);
    for (const bad of [null, undefined, {}, 'x', { values: null }, { values: [] }, { values: 'x' }]) {
        assert.equal(PV.validSettingsDoc(bad), false);
    }
});

test('pollSucceeded: the schema is fetched once, settings only again when something asks for them', () => {
    let needs = { needSchema: true, needSettings: true, needInfo: true };
    let n = PV.pollSucceeded('status', needs, status(), false, '');
    assert.equal(n.needSchema, true);
    n = PV.pollSucceeded('schema', needs, { fields: [] }, true, 'ready');
    assert.equal(n.needSchema, false);
    assert.equal(n.quick, true);
    needs = { needSchema: false, needSettings: true, needInfo: true };
    n = PV.pollSucceeded('settings', needs, { values: {} }, true, 'ready');
    assert.deepEqual([n.needSchema, n.needSettings, n.needInfo, n.forceStatus], [false, false, true, false]);
    needs = { needSchema: false, needSettings: false, needInfo: false };
    n = PV.pollSucceeded('status', needs, status(), true, 'ready');
    assert.deepEqual([n.needSchema, n.needSettings, n.quick], [false, false, false]);
    n = PV.pollSucceeded('status', needs, status(), true, 'edit'); // back to the main screen: settings may have changed
    assert.equal(n.needSettings, true);
    n = PV.pollSucceeded('status', needs, status(), false, 'ready'); // back after a gap
    assert.equal(n.needSettings, true);
    n = PV.pollSucceeded('info', { needSchema: false, needSettings: false, needInfo: true }, {}, true, 'ready');
    assert.equal(n.needInfo, false);
});

test('pollSucceeded: an unusable settings reply keeps the need and sends status next', () => {
    const needs = { needSchema: false, needSettings: true, needInfo: false };
    const n = PV.pollSucceeded('settings', needs, { nope: 1 }, true, 'ready');
    assert.equal(n.needSettings, true);
    assert.equal(n.forceStatus, true);
    assert.equal(n.quick, false); // the normal poll delay applies, no request storm on a mismatch
});

test('pollFailed: a 404 on the schema means no form, any other failure loses the connection', () => {
    assert.deepEqual(PV.pollFailed('schema', 'not_supported'), { schemaMissing: true, connectionLost: false, forceStatus: false });
    assert.deepEqual(PV.pollFailed('settings', 'not_supported'), { schemaMissing: false, connectionLost: true, forceStatus: true });
    assert.deepEqual(PV.pollFailed('schema', 'timeout'), { schemaMissing: false, connectionLost: true, forceStatus: true });
    assert.deepEqual(PV.pollFailed('status', 'network'), { schemaMissing: false, connectionLost: true, forceStatus: false });
});

test('the page source keeps the settings form rules (no markup writes, switches and radio groups)', () => {
    const app = fs3.readFileSync(path3.join(__dirname, '..', '..', 'web', 'portal', 'portal-app.js'), 'utf8');
    assert.equal(app.includes('innerHTML'), false);
    assert.match(app, /setAttribute\('role', 'switch'\)/);
    assert.match(app, /setAttribute\('role', 'radiogroup'\)/);
    assert.match(app, /type = 'checkbox'/);
    assert.match(app, /type = 'radio'/);
});

test('the page source wires the stale-reply guard, the name length limit and the save bar height', () => {
    const app = fs3.readFileSync(path3.join(__dirname, '..', '..', 'web', 'portal', 'portal-app.js'), 'utf8');
    assert.match(app, /settingsGen\+\+/);
    assert.match(app, /gen !== settingsGen/);
    assert.match(app, /\.maxLength = view\.max/);
    assert.match(app, /setProperty\('--savebar-h'/);
    // the body of showTab only: it ends at the first closing brace at function indent
    const showTabBody = app.match(/function showTab\([^)]*\) \{[\s\S]*?\r?\n  \}\r?\n/);
    assert.ok(showTabBody, 'showTab found');
    assert.match(showTabBody[0], /setSavebarHeight\(\)/);
    assert.match(app, /addEventListener\('resize', setSavebarHeight\)/);
});

// ---------------------------------------------------------------------------------------------- chg016 step 7: warnings
const WARNING_CODES = ['board_unrecognized', 'motor_fault', 'motor_disabled', 'hx711_not_connected', 'hx711_no_data',
    'hx711_sample_rate_invalid', 'mechanical_instability', 'load_cell_noisy', 'load_cell_not_calibrated'];

test('every firmware warning code has text, an action and a level; the table has no other codes', () => {
    assert.deepEqual(Object.keys(PV.WARNING_TEXT), WARNING_CODES);
    const seen = {};
    for (const code of WARNING_CODES) {
        const t = PV.WARNING_TEXT[code];
        assert.ok(t.title.length > 0 && t.action.length > 0, code);
        assert.ok(t.level === 'error' || t.level === 'warning', code);
        assert.equal(seen[t.title], undefined, 'titles are distinct: ' + code);
        seen[t.title] = true;
        const row = PV.warningsView(status({ warnings: [code] }), online()).rows;
        // motor_fault and motor_disabled are the banner's job; every other code is a row
        if (code === 'motor_fault' || code === 'motor_disabled') assert.equal(row.length, 0, code);
        else {
            assert.equal(row.length, 1, code);
            assert.equal(row[0].title, t.title);
            assert.equal(row[0].action, t.action);
            assert.equal(row[0].icon, t.level);
            assert.ok(row[0].label.length > 0);
        }
    }
});

test('warningsView lists the rows in the order received, each code once', () => {
    const v = PV.warningsView(status({ warnings: ['hx711_no_data', 'load_cell_noisy', 'hx711_no_data', 'load_cell_not_calibrated'] }), online());
    assert.equal(v.visible, true);
    assert.deepEqual(v.rows.map((r) => r.code), ['hx711_no_data', 'load_cell_noisy', 'load_cell_not_calibrated']);
    // no re-sorting: the contract already sends them by priority
    const rev = PV.warningsView(status({ warnings: ['load_cell_not_calibrated', 'hx711_no_data'] }), online());
    assert.deepEqual(rev.rows.map((r) => r.code), ['load_cell_not_calibrated', 'hx711_no_data']);
});

test('warningsView: all nine codes give seven rows (the banner takes motor_fault and motor_disabled) and the error banner', () => {
    const st = status({ warnings: WARNING_CODES });
    const v = PV.warningsView(st, online());
    assert.equal(v.rows.length, 7);
    assert.deepEqual(v.rows.map((r) => r.code), WARNING_CODES.filter((c) => c !== 'motor_fault' && c !== 'motor_disabled'));
    assert.equal(PV.deriveControls(st, online()).banner.level, 'error');
});

test('warningsView is hidden when empty, absent, malformed or not online', () => {
    for (const w of [[], undefined, null, 'motor_fault', 7, {}]) {
        const v = PV.warningsView(status(w === undefined ? {} : { warnings: w }), online());
        assert.deepEqual(v, { visible: false, rows: [] });
    }
    const st = status({ warnings: ['load_cell_noisy'] });
    for (const c of [offlineConn, PV.newConnection(), PV.connectionFailed(online(), 'web_interface_off', 5)]) {
        assert.equal(PV.warningsView(st, c).visible, false);
    }
    assert.equal(PV.warningsView(null, online()).visible, false);
    assert.equal(PV.warningsView(st, online()).visible, true);
});

test('warningsView shows an unknown code with generic text and the code cut to 40 characters', () => {
    const long = 'future_code_' + 'x'.repeat(60);
    const v = PV.warningsView(status({ warnings: ['load_cell_noisy', 'new_thing', long, 7, null, '', { a: 1 }] }), online());
    assert.deepEqual(v.rows.map((r) => r.code), ['load_cell_noisy', 'new_thing', long.slice(0, 40)]);
    assert.match(v.rows[1].title, /new_thing/);
    assert.match(v.rows[1].action, /does not know/);
    assert.equal(v.rows[1].level, 'warning');
    assert.ok(v.rows[2].title.includes(long.slice(0, 40)));
    assert.equal(v.rows[2].title.includes(long.slice(0, 41)), false);
    // names that exist on every object are not known codes
    const proto = PV.warningsView(status({ warnings: ['constructor', '__proto__', 'toString'] }), online()).rows;
    assert.equal(proto.length, 3);
    assert.match(proto[0].title, /constructor/);
});

test('warning text: wiring advice says to switch off and unplug first; motor fault uses the firmware wording', () => {
    for (const code of ['hx711_not_connected', 'hx711_no_data', 'hx711_sample_rate_invalid']) {
        assert.match(PV.WARNING_TEXT[code].action, /switched off and unplugged, check/, code);
    }
    assert.match(PV.WARNING_TEXT.motor_fault.action, /Check the grinder, then restart it/);
    assert.match(PV.deriveControls(status({ warnings: ['motor_fault'] }), online()).banner.text, /Check the grinder, then restart it/);
});

test('shortCode: unknown codes are cut on code points, then shown once', () => {
    const emoji = '\u{1F600}';
    const long = 'a'.repeat(39) + emoji + 'tail'; // the pair sits across the 40th character
    const row = PV.warningsView(status({ warnings: [long] }), online()).rows[0];
    assert.equal(row.code, 'a'.repeat(39) + emoji);
    assert.equal(Array.from(row.code).length, 40);
    assert.doesNotMatch(row.code, /[\uD800-\uDBFF](?![\uDC00-\uDFFF])/);
    // two different long codes that are equal after the cut give one row
    const a = 'x'.repeat(40) + '_one';
    const b = 'x'.repeat(40) + '_two';
    const v = PV.warningsView(status({ warnings: [a, b, 'load_cell_noisy'] }), online());
    assert.deepEqual(v.rows.map((r) => r.code), ['x'.repeat(40), 'load_cell_noisy']);
});

test('motor_fault in warnings alone gives the fault banner and disables Start and Motor test with the reason', () => {
    const d = PV.deriveControls(status({ warnings: ['motor_fault', 'load_cell_noisy'] }), online());
    assert.equal(d.banner.level, 'error');
    assert.match(d.banner.text, /restart it/);
    assert.equal(d.start.enabled, false);
    assert.equal(d.motorTest.enabled, false);
    assert.match(d.start.reason, /restarted/);
    assert.match(d.motorTest.reason, /restarted/);
    assert.equal(d.tare.enabled, true);
    assert.equal(d.stop.enabled, true);
});

test('motor_disabled in warnings alone gives the disabled banner and disables Start and Motor test', () => {
    const d = PV.deriveControls(status({ warnings: ['motor_disabled'] }), online());
    assert.match(d.banner.text, /^Motor disabled\./);
    assert.match(d.start.reason, /Hardware board/);
    assert.match(d.motorTest.reason, /Hardware board/);
});

test('the banner and the list never say the same thing twice or contradict each other', () => {
    // fault from both the start_block and the warning: one banner, no row for it
    const both = status({ start_block: 'motor_fault', motor_test_block: 'motor_fault', warnings: ['motor_fault', 'hx711_no_data'] });
    const d = PV.deriveControls(both, online());
    assert.match(d.banner.text, /^Motor fault\./);
    assert.deepEqual(PV.warningsView(both, online()).rows.map((r) => r.code), ['hx711_no_data']);
    // fault wins over disabled in the banner and covers it: the board advice of the disabled row would mislead
    const mixed = status({ warnings: ['motor_fault', 'motor_disabled'] });
    assert.match(PV.deriveControls(mixed, online()).banner.text, /^Motor fault\./);
    assert.deepEqual(PV.warningsView(mixed, online()).rows, []);
    // disabled alone keeps its banner and no row
    assert.deepEqual(PV.warningsView(status({ warnings: ['motor_disabled'] }), online()).rows, []);
    // a firmware update banner does not hide the motor rows
    const ota = status({ ui_state: 'ota_update', warnings: ['motor_fault'] });
    assert.match(PV.deriveControls(ota, online()).banner.text, /Firmware update/);
    assert.deepEqual(PV.warningsView(ota, online()).rows.map((r) => r.code), ['motor_fault']);
    // a start_block of its own wins over the warning for the Start reason
    assert.match(PV.deriveControls(status({ start_block: 'busy', warnings: ['motor_fault'] }), online()).start.reason, /active/);
});

test('an older firmware without the warnings member works as before', () => {
    const st = status();
    assert.equal('warnings' in st, false);
    const d = PV.deriveControls(st, online());
    assert.equal(d.banner, null);
    assert.equal(d.start.enabled && d.motorTest.enabled, true);
    assert.equal(PV.warningsView(st, online()).visible, false);
});

test('the warnings list is wired as a named region built from the template without markup or storage', () => {
    const html = fs3.readFileSync(path3.join(__dirname, '..', '..', 'web', 'portal', 'page.src.html'), 'utf8');
    const app = fs3.readFileSync(path3.join(__dirname, '..', '..', 'web', 'portal', 'portal-app.js'), 'utf8');
    assert.match(html, /<section id="warnings"[^>]*aria-labelledby="warnings-h"[^>]*hidden>/);
    assert.match(html, /<h2 id="warnings-h">Grinder warnings<\/h2>/);
    assert.match(html, /<ul id="warnings-list"/);
    assert.match(html, /<symbol id="i-error"/);
    assert.equal(app.includes('createElementNS'), false);
    assert.equal(app.includes('http'), false);
    assert.match(app, /V\.warningsView\(status, conn\)/);
});

// ---------------------------------------------------------------------------------------------- step 9 (Insights)
const statsDoc = (over) => Object.assign({
    total_grinds: 412, single: 180, double: 200, custom: 32, weight_mode: 390, time_mode: 22, total_weight_kg: 8.43,
    motor_runtime_s: 5120, avg_abs_error_g: 0.12, total_pulses: 1100, avg_pulses: 2.7, time_pulses: 14, powered_minutes: 22890
}, over || {});
const loadedStats = (over, at) => PV.statsLoaded(PV.newStats(), statsDoc(over), at === undefined ? 1000 : at);
const tileOf = (view, id) => view.tiles.find((t) => t.id === id);
const lostConn = () => PV.connectionFailed(PV.connectionFailed(online(), 'network', 1), 'network', 2);

test('formatDuration: seconds, minutes, hours and days, 0 is "0 s"', () => {
    assert.equal(PV.formatDuration(0), '0 s');
    assert.equal(PV.formatDuration(59), '59 s');
    assert.equal(PV.formatDuration(60), '1 min');
    assert.equal(PV.formatDuration(125), '2 min 5 s');
    assert.equal(PV.formatDuration(3599), '59 min 59 s');
    assert.equal(PV.formatDuration(3720), '1 h 02 min');
    assert.equal(PV.formatDuration(86399), '23 h 59 min');
    assert.equal(PV.formatDuration(3 * 86400 + 4 * 3600 + 59), '3 d 4 h');
    assert.equal(PV.formatDuration(5120.9), '1 h 25 min');
});

test('formatDuration: very large and unusable values stay readable', () => {
    assert.match(PV.formatDuration(257698037759 * 60), /^[\d,]+ d \d{1,2} h$/);
    for (const bad of [null, undefined, NaN, Infinity, -1, '5']) assert.equal(PV.formatDuration(bad), '--');
});

test('formatCount groups thousands without a locale and rejects non-numbers', () => {
    assert.equal(PV.formatCount(0), '0');
    assert.equal(PV.formatCount(999), '999');
    assert.equal(PV.formatCount(1000), '1,000');
    assert.equal(PV.formatCount(4294967295), '4,294,967,295');
    assert.equal(PV.formatCount(1234.9), '1,234');
    for (const bad of [null, undefined, NaN, Infinity, -5, '7']) assert.equal(PV.formatCount(bad), '--');
});

test('formatKg: grams below a kilogram, then kg with fewer decimals as it grows', () => {
    assert.equal(PV.formatKg(0), '0 g');
    assert.equal(PV.formatKg(0.018), '18 g');
    assert.equal(PV.formatKg(8.43), '8.43 kg');
    assert.equal(PV.formatKg(123.456), '123.5 kg');
    assert.equal(PV.formatKg(12345.6), '12,346 kg');
    assert.equal(PV.formatKg(1000000000), '1,000,000,000 kg');
    assert.equal(PV.formatKg(0.9996), '1.00 kg'); // the unit follows the rounded value
    assert.equal(PV.formatKg(0.9994), '999 g');
    assert.equal(PV.formatKg(9.999), '10.0 kg');
    assert.equal(PV.formatKg(9.994), '9.99 kg');
    assert.equal(PV.formatKg(999.96), '1,000 kg');
    assert.equal(PV.formatKg(999.94), '999.9 kg');
    assert.equal(PV.formatKg(NaN), '--');
    assert.equal(PV.formatKg(-1), '--');
});

test('parseStats: numbers only, missing members are unknown (null), anything else is not a statistics reply', () => {
    const ok = PV.parseStats(statsDoc());
    assert.equal(ok.total_grinds, 412);
    assert.deepEqual(Object.keys(ok), PV.STATS_KEYS);
    const partial = PV.parseStats({ total_grinds: 3, powered_minutes: 'x', avg_pulses: -1, time_pulses: Infinity, extra: 1 });
    assert.equal(partial.total_grinds, 3);
    assert.equal(partial.powered_minutes, null);
    assert.equal(partial.avg_pulses, null);
    assert.equal(partial.time_pulses, null);
    assert.equal(partial.single, null);
    assert.equal('extra' in partial, false);
    for (const bad of [null, undefined, 5, 'x', [], {}, { total_grinds: 'many' }, { total_grinds: -1 }, { total_grinds: NaN }]) {
        assert.equal(PV.parseStats(bad), null);
    }
});

test('statsView: the tiles of a normal reply', () => {
    const view = PV.statsView(loadedStats(), online(), 1000);
    assert.equal(view.kind, 'ready');
    assert.equal(view.tabVisible, true);
    assert.deepEqual(view.tiles.map((t) => t.id), ['grinds', 'coffee', 'motor', 'powered', 'error', 'pulses', 'timepulses']);
    assert.equal(tileOf(view, 'grinds').value, '412');
    assert.match(tileOf(view, 'grinds').detail, /390 by weight, 22 by time/);
    assert.equal(tileOf(view, 'coffee').value, '8.43 kg');
    assert.equal(tileOf(view, 'motor').value, '1 h 25 min');
    assert.equal(tileOf(view, 'powered').value, '15 d 21 h');
    assert.equal(tileOf(view, 'error').value, '\u00b10.12 g');
    assert.equal(tileOf(view, 'pulses').value, '2.7');
    assert.match(tileOf(view, 'pulses').detail, /1,100 correction pulses in total/);
    assert.equal(tileOf(view, 'timepulses').value, '14');
    assert.equal(view.note, 'Updated 0 s ago.');
    for (const t of view.tiles) assert.ok(t.label && t.value && t.detail, t.id); // every tile says what it is in words
});

test('statsView: the weight split names the buckets by weight, with counts and percentages', () => {
    const mix = PV.statsView(loadedStats(), online(), 1000).mix;
    assert.equal(mix.visible, true);
    assert.deepEqual(mix.segments.map((s) => s.key), ['single', 'double', 'custom']);
    assert.deepEqual(mix.segments.map((s) => s.label), ['Up to 10 g', 'Over 10 g, up to 22 g', 'Over 22 g']);
    assert.deepEqual(mix.segments.map((s) => s.text), ['180 grinds (44 %)', '200 grinds (48 %)', '32 grinds (8 %)']);
    assert.deepEqual(mix.segments.map((s) => s.flex), [180, 200, 32]);
    assert.deepEqual(mix.segments.map((s) => s.drawn), [true, true, true]);
    const tiny = PV.statsView(loadedStats({ single: 1, double: 0, custom: 100000 }), online(), 1000).mix;
    assert.deepEqual(tiny.segments.map((s) => s.text), ['1 grind (under 1 %)', '0 grinds (0 %)', '100,000 grinds (100 %)']);
    assert.deepEqual(tiny.segments.map((s) => s.drawn), [true, false, true]); // no grinds: no bar segment, no divider
    assert.equal(PV.statsView(loadedStats({ single: 0, double: 0, custom: 0 }), online(), 1000).mix.visible, false);
    const unknown = PV.statsLoaded(PV.newStats(), { total_grinds: 5, single: 5 }, 0);
    assert.equal(PV.statsView(unknown, online(), 0).mix.visible, false); // one bucket unknown: no bar
});

test('statsView: the percentages of the weight split always add up to 100 (largest remainder)', () => {
    const sum = (counts) => {
        const mix = PV.statsView(loadedStats({ single: counts[0], double: counts[1], custom: counts[2] }), online(), 1000).mix;
        return mix.segments.map((s) => (/\(under 1 %\)/.test(s.text) ? 0 : Number(/\((\d+) %\)/.exec(s.text)[1]))).reduce((a, b) => a + b, 0);
    };
    assert.equal(sum([1, 1, 1]), 100); // 33 + 33 + 33 would leave one over
    assert.equal(sum([1, 1, 0]), 100);
    assert.equal(sum([180, 200, 32]), 100);
    assert.equal(sum([5, 5, 5]), 100);
    assert.equal(sum([1, 0, 100000]), 100);
    const third = PV.statsView(loadedStats({ single: 1, double: 1, custom: 1 }), online(), 1000).mix;
    assert.deepEqual(third.segments.map((s) => s.text), ['1 grind (34 %)', '1 grind (33 %)', '1 grind (33 %)']);
});

test('statsView: zero data is an empty state without tiles', () => {
    const zero = PV.statsLoaded(PV.newStats(), statsDoc({ total_grinds: 0, single: 0, double: 0, custom: 0, weight_mode: 0, time_mode: 0 }), 0);
    const view = PV.statsView(zero, online(), 0);
    assert.equal(view.kind, 'empty');
    assert.match(view.message, /No grinds recorded yet/);
    assert.deepEqual(view.tiles, []);
    assert.equal(view.tabVisible, true);
});

test('statsView: no weight-mode samples and unknown members show a dash with a reason, never NaN', () => {
    const view = PV.statsView(loadedStats({ weight_mode: 0, avg_abs_error_g: 0, avg_pulses: 0, total_pulses: 0 }), online(), 1000);
    assert.equal(tileOf(view, 'error').value, '--');
    assert.match(tileOf(view, 'error').detail, /No weight-mode grinds yet/);
    assert.equal(tileOf(view, 'pulses').value, '0.0');
    const partial = PV.statsView(PV.statsLoaded(PV.newStats(), { total_grinds: 7 }, 0), online(), 0);
    assert.equal(tileOf(partial, 'coffee').value, '--');
    assert.equal(tileOf(partial, 'pulses').value, '--');
    assert.equal(tileOf(partial, 'powered').value, '--');
    for (const t of partial.tiles) assert.doesNotMatch(t.value + t.detail, /NaN|undefined|null/);
});

test('statsView: very large numbers are grouped and keep their shape', () => {
    const max = 4294967295;
    const view = PV.statsView(loadedStats({
        total_grinds: max, single: max, double: max, custom: max, weight_mode: max, time_mode: max, total_weight_kg: 1e9,
        motor_runtime_s: max, avg_abs_error_g: 1e9, total_pulses: max, avg_pulses: 255, time_pulses: max, powered_minutes: 257698037759
    }), online(), 1000);
    assert.equal(tileOf(view, 'grinds').value, '4,294,967,295');
    assert.equal(tileOf(view, 'coffee').value, '1,000,000,000 kg');
    assert.match(tileOf(view, 'motor').value, /^49,710 d \d{1,2} h$/);
    assert.match(tileOf(view, 'powered').value, /^[\d,]+ d \d{1,2} h$/);
    assert.equal(tileOf(view, 'error').value, '\u00b11,000,000,000 g');
    assert.equal(tileOf(PV.statsView(loadedStats({ avg_abs_error_g: 999.994 }), online(), 1000), 'error').value, '\u00b1999.99 g');
    assert.equal(tileOf(view, 'timepulses').value, '4,294,967,295');
    assert.equal(view.mix.segments[0].text, '4,294,967,295 grinds (34 %)');
});

test('statsView: loading, error, offline and not available', () => {
    assert.equal(PV.statsView(PV.newStats(), online(), 0).kind, 'loading');
    assert.equal(PV.statsView(PV.newStats(), PV.newConnection(), 0).kind, 'loading');
    assert.equal(PV.statsView(PV.newStats(), PV.newConnection(), 0).tabVisible, false);
    const errorView = PV.statsView(PV.statsFailed(PV.newStats(), 'ui_busy', 0), online(), 0);
    assert.equal(errorView.kind, 'error');
    assert.match(errorView.message, /could not be read/);
    assert.equal(errorView.tabVisible, false);
    assert.equal(PV.statsView(PV.newStats(), lostConn(), 0).kind, 'error');
    const view = PV.statsView(PV.statsFailed(PV.newStats(), 'not_supported', 0), online(), 0);
    assert.equal(view.kind, 'unavailable');
    assert.equal(view.message, PV.STATS_NOT_AVAILABLE);
    assert.equal(view.tabVisible, false);
});

test('statsView: stale numbers stay with a note when the grinder is offline or a refresh failed', () => {
    const loaded = loadedStats({}, 0);
    const offView = PV.statsView(loaded, lostConn(), 125000);
    assert.equal(offView.kind, 'ready');
    assert.equal(offView.tiles.length, 7);
    assert.match(offView.note, /not reachable.*2 min ago/);
    const failed = PV.statsFailed(loaded, 'ui_busy', 40000);
    assert.match(PV.statsView(failed, online(), 45000).note, /could not be refreshed.*45 s ago/);
    assert.equal(PV.statsView(failed, online(), 45000).tabVisible, true);
});

test('a missing route after data was seen hides the tab again', () => {
    const view = PV.statsView(PV.statsFailed(loadedStats(), 'not_supported', 5000), online(), 5000);
    assert.equal(view.tabVisible, false);
    assert.equal(view.kind, 'unavailable');
});

test('statsLoaded with an unusable reply counts as a failed request and keeps what was shown', () => {
    const bad = PV.statsLoaded(PV.newStats(), '{"total_grinds":1}', 100);
    assert.equal(bad.seen, false);
    assert.match(bad.error, /does not understand/);
    assert.equal(bad.retryAt, 100 + PV.STATS_RETRY_MS);
    const kept = PV.statsLoaded(loadedStats(), [], 200);
    assert.equal(kept.seen, true);
    assert.equal(kept.doc.total_grinds, 412);
});

test('statsDue: probe once online, then only on request, on a grind end, or every 30 s while the tab is shown', () => {
    const ctx = (over) => Object.assign({ online: true, haveStatus: true, hidden: false, tabShown: false, commandBusy: false, nowMs: 1000 }, over || {});
    const fresh = PV.newStats();
    assert.equal(PV.statsDue(fresh, ctx()), true); // the first probe
    assert.equal(PV.statsDue(fresh, ctx({ online: false })), false);
    assert.equal(PV.statsDue(fresh, ctx({ haveStatus: false })), false);
    assert.equal(PV.statsDue(fresh, ctx({ hidden: true })), false);
    assert.equal(PV.statsDue(fresh, ctx({ commandBusy: true })), false); // never while a command is in flight
    const seen = loadedStats({}, 1000);
    assert.equal(PV.statsDue(seen, ctx({ nowMs: 1001 })), false); // tab not shown: no polling
    assert.equal(PV.statsDue(seen, ctx({ nowMs: 1001, tabShown: true })), false);
    assert.equal(PV.statsDue(seen, ctx({ nowMs: 1000 + PV.STATS_PERIOD_MS - 1, tabShown: true })), false);
    assert.equal(PV.statsDue(seen, ctx({ nowMs: 1000 + PV.STATS_PERIOD_MS, tabShown: true })), true);
    const opened = PV.statsShown(seen, 5000); // opening the tab asks for a fetch at once
    assert.equal(PV.statsDue(opened, ctx({ nowMs: 5000, tabShown: true })), true);
    assert.equal(PV.statsDue(PV.statsLoaded(opened, statsDoc(), 5100), ctx({ nowMs: 5200, tabShown: true })), false);
});

test('statsDue: a failed request waits for its retry time, a 404 stops for good', () => {
    const ctx = (nowMs) => ({ online: true, haveStatus: true, hidden: false, tabShown: true, commandBusy: false, nowMs: nowMs });
    const failed = PV.statsFailed(PV.newStats(), 'timeout', 1000);
    assert.equal(PV.statsDue(failed, ctx(1000 + PV.STATS_RETRY_MS - 1)), false);
    assert.equal(PV.statsDue(failed, ctx(1000 + PV.STATS_RETRY_MS)), true);
    const missing = PV.statsFailed(PV.newStats(), 'not_supported', 1000);
    assert.equal(PV.statsDue(missing, ctx(99999999)), false);
    assert.equal(missing.retryAt, 0);
});

test('a grind that ends asks for a fetch now and once more after the grinder refreshed', () => {
    assert.equal(PV.grindEnded('grinding', 'grind_complete'), true);
    assert.equal(PV.grindEnded('grinding', 'ready'), true);
    assert.equal(PV.grindEnded('grind_complete', 'ready'), true);
    assert.equal(PV.grindEnded('grinding', 'grinding'), false);
    assert.equal(PV.grindEnded('ready', 'menu'), false);
    assert.equal(PV.grindEnded('', 'ready'), false);
    const ctx = (nowMs) => ({ online: true, haveStatus: true, hidden: false, tabShown: true, commandBusy: false, nowMs: nowMs });
    let s = loadedStats({}, 1000);
    assert.equal(PV.statsOnStatus(s, 'ready', 'menu', 2000), s); // same object: nothing asked
    s = PV.statsOnStatus(s, 'grinding', 'grind_complete', 2000);
    assert.equal(PV.statsDue(s, ctx(2000)), true);
    s = PV.statsLoaded(s, statsDoc(), 2100);
    assert.equal(PV.statsDue(s, ctx(2200)), false);
    assert.equal(PV.statsDue(s, ctx(2000 + PV.STATS_FOLLOW_UP_MS - 1)), false);
    assert.equal(PV.statsDue(s, ctx(2000 + PV.STATS_FOLLOW_UP_MS)), true); // the follow-up
    s = PV.statsLoaded(s, statsDoc(), 8100);
    assert.equal(PV.statsDue(s, ctx(8200)), false);
    const waiting = PV.statsOnStatus(loadedStats({}, 1000), 'grinding', 'ready', 2000); // tab hidden: waits for the tab
    assert.equal(PV.statsDue(waiting, Object.assign(ctx(2500), { tabShown: false })), false);
    assert.equal(PV.statsDue(waiting, ctx(2500)), true);
});

test('the polling loop gives stats their turn after settings and info, and a stats failure keeps the connection', () => {
    const f = { haveStatus: true, online: true, needSchema: false, needSettings: false, needInfo: false, forceStatus: false, statsDue: true };
    assert.equal(PV.nextPollTask(f), 'stats');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { needInfo: true })), 'info');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { forceStatus: true })), 'status');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { online: false })), 'status');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { statsDue: false })), 'status');
    const needs = { needSchema: false, needSettings: false, needInfo: false };
    const next = PV.pollSucceeded('stats', needs, statsDoc(), true, 'ready');
    assert.deepEqual([next.needSchema, next.needSettings, next.needInfo, next.forceStatus, next.quick], [false, false, false, false, false]);
    for (const code of ['not_supported', 'ui_busy', 'bad_reply', 'http_error']) {
        assert.deepEqual(PV.pollFailed('stats', code), { schemaMissing: false, connectionLost: false, forceStatus: true }, code);
    }
    for (const code of ['network', 'timeout', 'web_interface_off']) assert.equal(PV.pollFailed('stats', code).connectionLost, true, code);
});

test('the Insights tab is wired in the page: definition lists, a named region, no markup written from data', () => {
    const html = fs3.readFileSync(path3.join(__dirname, '..', '..', 'web', 'portal', 'page.src.html'), 'utf8');
    const app = fs3.readFileSync(path3.join(__dirname, '..', '..', 'web', 'portal', 'portal-app.js'), 'utf8');
    assert.match(html, /<button id="tab-insights"[^>]*\bhidden>/);
    assert.match(html, /<dl id="tiles"[^>]*hidden>/);
    assert.match(html, /<dl id="mix-list"/);
    assert.match(html, /<div id="mix-bar"[^>]*aria-hidden="true"/);
    assert.match(app, /V\.statsView\(stats, conn, now\(\)\)/);
    assert.match(app, /client\.getStats\(\)/);
    assert.match(app, /commandBusy: commandPending \|\| client\.pending\(\) > 0/);
    assert.equal(app.includes('innerHTML'), false);
    // the warnings sit above the readout, Profiles and Tools in the Grind section (chg021 V4: alerts first), so they
    // are not behind a long scroll
    assert.ok(html.indexOf('<section id="warnings"') > 0 && html.indexOf('<div id="readout"') > 0);
    assert.ok(html.indexOf('<section id="warnings"') < html.indexOf('<div id="readout"'));
    assert.ok(html.indexOf('<div id="readout"') < html.indexOf('<h2>Profiles</h2>'));
    assert.ok(html.indexOf('<section id="warnings"') < html.indexOf('<h2>Profiles</h2>'));
    assert.ok(html.indexOf('<h2>Profiles</h2>') < html.indexOf('<h2>Tools</h2>'));
});

// ---------------------------------------------------------------------------------------------- step 12 (recent grinds)
function grind(over) {
    return Object.assign({ id: 1, age_s: 120, profile: 1, mode: 'weight', target_g: 18.0, target_s: null, final_g: 18.1,
        time_ms: 14230, motor_ms: 9800, pulses: 2, result: 'complete' }, over || {});
}

function historyDoc(grinds, persistent) {
    return { count: grinds.length, persistent: persistent !== false, grinds: grinds };
}

function historyState(doc, fetchedAt) {
    return PV.historyLoaded(PV.newStats(), doc, fetchedAt === undefined ? 0 : fetchedAt);
}

const ONLINE = { state: 'online' };

test('historyAgeText: just now, minutes, hours with two digit minutes, days and "before the last restart"', () => {
    assert.equal(PV.historyAgeText(0), 'just now');
    assert.equal(PV.historyAgeText(59.9), 'just now');
    assert.equal(PV.historyAgeText(60), '1 min ago');
    assert.equal(PV.historyAgeText(120), '2 min ago');
    assert.equal(PV.historyAgeText(3599), '59 min ago');
    assert.equal(PV.historyAgeText(3600), '1 h 00 min ago');
    assert.equal(PV.historyAgeText(3900), '1 h 05 min ago');
    assert.equal(PV.historyAgeText(86399), '23 h 59 min ago');
    assert.equal(PV.historyAgeText(86400 + 3 * 3600), '1 d 3 h ago');
    assert.equal(PV.historyAgeText(4294967295), '49,710 d 6 h ago');
    assert.equal(PV.historyAgeText(null), 'before the last restart');
    assert.equal(PV.historyAgeText(undefined), '');
});

test('historyErrorText: the sign and the words, "on target" under 0.05 g, no minus sign on a rounded zero', () => {
    assert.equal(PV.historyErrorText(0.1), '+0.1 g over');
    assert.equal(PV.historyErrorText(-0.2), '-0.2 g under');
    assert.equal(PV.historyErrorText(1.46), '+1.5 g over');
    assert.equal(PV.historyErrorText(0), 'on target');
    assert.equal(PV.historyErrorText(-0), 'on target');
    assert.equal(PV.historyErrorText(0.049), 'on target');
    assert.equal(PV.historyErrorText(-0.049), 'on target');
    assert.equal(PV.historyErrorText(0.05), '+0.1 g over'); // 0.05 g rounds up to 0.1
    assert.equal(PV.historyErrorText(-0.05), '-0.1 g under');
    assert.equal(PV.historyErrorText(1234.5), '+1,235 g over');
    assert.equal(PV.historyErrorText(-1000000), '-1,000,000 g under');
    assert.equal(PV.historyErrorText(null), '');
});

test('parseHistory: needs a grinds array; skips what is no object; keeps only usable numbers', () => {
    assert.equal(PV.parseHistory(null), null);
    assert.equal(PV.parseHistory([]), null);
    assert.equal(PV.parseHistory({}), null);
    assert.equal(PV.parseHistory({ grinds: 'x' }), null);
    const parsed = PV.parseHistory({ persistent: true, grinds: [null, 5, [], grind({ final_g: 'x', target_g: NaN, profile: 7, time_ms: null, pulses: 'many', age_s: -4, result: 'bogus' })] });
    assert.equal(parsed.grinds.length, 1);
    assert.deepEqual(parsed.grinds[0], { ageS: undefined, profile: null, time: false, target: null, final: null, timeMs: null,
        pulses: null, result: 'unknown' });
    assert.equal(PV.parseHistory({ grinds: [] }).persistent, true); // a missing flag is not an alarm
    assert.equal(PV.parseHistory({ persistent: false, grinds: [] }).persistent, false);
    assert.equal(PV.parseHistory({ grinds: new Array(25).fill(grind()) }).grinds.length, 10);
    const time = PV.parseHistory({ grinds: [grind({ mode: 'time', target_g: null, target_s: 12 })] }).grinds[0];
    assert.equal(time.time, true);
    assert.equal(time.target, 12);
    assert.equal(PV.parseHistory({ grinds: [grind({ age_s: null })] }).grinds[0].ageS, null);
});

test('every shared history vector parses and shows: rows match the entries, nothing throws', () => {
    const vectors = JSON.parse(fs3.readFileSync(path3.join(__dirname, 'portal_api_vectors.json'), 'utf8'));
    for (const c of vectors.history_cases) {
        const doc = JSON.parse(c.json);
        const view = PV.historyView(historyState(doc), ONLINE, 0);
        assert.equal(view.visible, true, c.name);
        assert.equal(view.rows.length, doc.grinds.length, c.name);
        assert.equal(view.kind, doc.grinds.length === 0 ? 'empty' : 'ready', c.name);
        assert.equal(view.notice !== '', doc.persistent === false, c.name);
        for (const row of view.rows) {
            for (const text of [row.name, row.age, row.final, row.target, row.error, row.detail]) {
                assert.equal(typeof text, 'string', c.name);
                assert.equal(/NaN|undefined|null|Infinity/.test(text), false, c.name + ': ' + text);
            }
        }
    }
});

test('historyView: a weight-mode row shows age, profile, final against target, error, badge and details', () => {
    const view = PV.historyView(historyState(historyDoc([grind()])), ONLINE, 0);
    assert.deepEqual(view.rows, [{ name: 'Double', age: '2 min ago', final: '18.1 g', target: 'target 18.0 g', error: '+0.1 g over',
        badge: { label: 'Complete', level: 'ok' }, detail: '14.2 s \u00b7 2 pulses' }]);
    assert.equal(view.kind, 'ready');
    assert.equal(view.message, '');
    assert.equal(view.note, '');
});

test('historyView: ages keep growing after the reply arrived (fetched at 10 s, shown at 70 s)', () => {
    const state = historyState(historyDoc([grind({ age_s: 30 }), grind({ age_s: null })]), 10000);
    assert.equal(PV.historyView(state, ONLINE, 10000).rows[0].age, 'just now');
    assert.equal(PV.historyView(state, ONLINE, 70000).rows[0].age, '1 min ago');
    assert.equal(PV.historyView(state, ONLINE, 70000).rows[1].age, 'before the last restart');
    assert.equal(PV.historyView(state, ONLINE, 5000).rows[0].age, 'just now'); // a clock step back is not a negative age
});

test('historyView: a time-mode row shows its target in seconds and no error; one pulse is singular', () => {
    const doc = historyDoc([grind({ mode: 'time', target_g: null, target_s: 12.0, final_g: 20.4, pulses: 0, time_ms: 12000 }),
        grind({ pulses: 1, time_ms: 75000 })]);
    const rows = PV.historyView(historyState(doc), ONLINE, 0).rows;
    assert.equal(rows[0].target, 'target 12.0 s');
    assert.equal(rows[0].final, '20.4 g');
    assert.equal(rows[0].error, '');
    assert.equal(rows[0].detail, '12.0 s');
    assert.equal(rows[1].detail, '1 min 15 s \u00b7 1 pulse');
});

test('historyView: every result has a word and a level; overshoot and timeout are not "ok"', () => {
    const wanted = { complete: ['Complete', 'ok'], max_pulses: ['Max pulses', 'warning'], overshoot: ['Overshoot', 'warning'],
        timeout: ['Timeout', 'error'], unknown: ['Unknown result', 'neutral'], 'something new': ['Unknown result', 'neutral'] };
    for (const result of Object.keys(wanted)) {
        const row = PV.historyView(historyState(historyDoc([grind({ result: result })])), ONLINE, 0).rows[0];
        assert.deepEqual([row.badge.label, row.badge.level], wanted[result], result);
    }
});

test('historyView: large, negative and not-a-number weights stay readable and never print NaN', () => {
    const doc = historyDoc([grind({ final_g: 1e6, target_g: 18 }), grind({ final_g: -2.5, target_g: 18 }), grind({ final_g: -0.04 }),
        { mode: 'weight', result: 'complete', final_g: NaN, target_g: Infinity, time_ms: NaN, pulses: NaN, age_s: NaN, profile: 'x' }]);
    const rows = PV.historyView(historyState(doc), ONLINE, 0).rows;
    assert.equal(rows[0].final, '1,000,000 g');
    assert.equal(rows[0].error, '+999,982 g over');
    assert.equal(rows[1].final, '-2.5 g');
    assert.equal(rows[1].error, '-20.5 g under');
    assert.equal(rows[2].final, '0.0 g'); // -0.04 rounds to zero without a minus sign
    assert.deepEqual([rows[3].name, rows[3].age, rows[3].final, rows[3].target, rows[3].error, rows[3].detail], ['Grind', '', '-- g', '', '', '']);
    assert.equal(JSON.stringify(rows).includes('NaN'), false);
});

test('historyView: the chart is scaled to the largest error, at least 0.5 g, oldest bar first', () => {
    const doc = historyDoc([grind({ final_g: 19.0 }), grind({ final_g: 17.9 }), grind({ final_g: 18.0 }), grind({ mode: 'time', target_g: null, target_s: 5 })]);
    const chart = PV.historyView(historyState(doc), ONLINE, 0).chart;
    assert.equal(chart.visible, true);
    // newest first in the list, so the chart (oldest left): time mode, on target, under 0.1, over 1.0
    assert.deepEqual(chart.bars, [{ dir: 'none', pct: 0 }, { dir: 'on', pct: 0 }, { dir: 'under', pct: 10 }, { dir: 'over', pct: 100 }]);
    assert.match(chart.caption, /^Error of the last 4 grinds, oldest on the left\./);
    assert.match(chart.caption, /Full height is 1\.0 g\.$/);
    const small = PV.historyView(historyState(historyDoc([grind({ final_g: 18.1 })])), ONLINE, 0).chart;
    assert.deepEqual(small.bars, [{ dir: 'over', pct: 20 }]); // 0.1 g of the 0.5 g minimum scale
    assert.match(small.caption, /^Error of the last 1 grind,/);
    assert.match(small.caption, /Full height is 0\.5 g\.$/);
    const tiny = PV.historyView(historyState(historyDoc([grind({ final_g: 18.0, target_g: 18.0 }), grind({ final_g: 10, target_g: 18 })])), ONLINE, 0).chart;
    assert.deepEqual(tiny.bars.map((b) => b.dir), ['under', 'on']);
    assert.equal(PV.historyView(historyState(historyDoc([grind({ mode: 'time', target_g: null, target_s: 5 })])), ONLINE, 0).chart.visible, false);
    const big = PV.historyView(historyState(historyDoc([grind({ final_g: 1e6 }), grind({ final_g: 18.1 })])), ONLINE, 0).chart;
    assert.equal(big.bars[0].pct, 4); // a visible sliver for a non-zero error, never a hidden bar
    assert.equal(big.bars[1].pct, 100);
});

test('historyView: an empty list says so; not persistent changes the words and adds the notice', () => {
    let view = PV.historyView(historyState(historyDoc([], true)), ONLINE, 0);
    assert.equal(view.kind, 'empty');
    assert.equal(view.message, 'No grinds recorded yet. Finished grinds are listed here and kept across restarts.');
    assert.equal(view.notice, '');
    assert.deepEqual([view.rows.length, view.chart.visible], [0, false]);
    view = PV.historyView(historyState(historyDoc([], false)), ONLINE, 0);
    assert.equal(view.message, 'No grinds recorded since the grinder started.');
    assert.equal(view.notice, 'History is not being saved on the grinder; it is lost at the next restart.');
    view = PV.historyView(historyState(historyDoc([grind()], false)), ONLINE, 0);
    assert.equal(view.kind, 'ready');
    assert.equal(view.rows.length, 1);
    assert.equal(view.notice, 'History is not being saved on the grinder; it is lost at the next restart.');
});

test('historyView: a firmware without the route hides the card only; other failures keep it and say why', () => {
    const gone = PV.statsFailed(PV.newStats(), 'not_supported', 0);
    assert.equal(PV.historyView(gone, ONLINE, 0).visible, false);
    assert.equal(PV.historyDue(gone, { tabShown: true, online: true, haveStatus: true, hidden: false, commandBusy: false, nowMs: 1e6 }), false);
    let view = PV.historyView(PV.newStats(), { state: 'connecting' }, 0);
    assert.deepEqual([view.visible, view.kind, view.message], [true, 'loading', 'Loading the recent grinds.']);
    view = PV.historyView(PV.statsFailed(PV.newStats(), 'ui_busy', 0), ONLINE, 0);
    assert.equal(view.kind, 'error');
    assert.match(view.message, /^The recent grinds could not be read\. .* This page tries again by itself\.$/);
    view = PV.historyView(PV.newStats(), { state: 'offline' }, 0);
    assert.match(view.message, /^The grinder is not reachable, so the recent grinds cannot be read\./);
    // with a list already shown: it stays, with a note about its age
    const state = historyState(historyDoc([grind()]), 1000);
    assert.equal(PV.historyView(state, ONLINE, 31000).note, '');
    assert.equal(PV.historyView(state, { state: 'offline' }, 31000).note, 'The grinder is not reachable. This list is from 30 s ago.');
    const failed = PV.statsFailed(state, 'ui_busy', 31000);
    assert.match(PV.historyView(failed, ONLINE, 31000).note, /^The list could not be refreshed \(.*\) and is from 30 s ago\.$/);
    assert.equal(PV.historyView(failed, ONLINE, 31000).rows.length, 1);
});

test('historyLoaded: a reply that is not a history is a failure and keeps the old list', () => {
    const good = historyState(historyDoc([grind()]), 1000);
    const bad = PV.historyLoaded(good, { count: 1 }, 2000);
    assert.equal(bad.doc.grinds.length, 1);
    assert.equal(bad.fetchedAt, 1000);
    assert.notEqual(bad.error, '');
    assert.equal(bad.unavailable, false);
    const fresh = PV.historyLoaded(good, historyDoc([]), 3000);
    assert.equal(fresh.doc.grinds.length, 0);
    assert.equal(fresh.fetchedAt, 3000);
    assert.equal(fresh.error, '');
});

test('historyDue: only while the Insights tab is shown, then the statistics rules (30 s, on request, never in a command)', () => {
    const ctx = (over) => Object.assign({ tabShown: true, online: true, haveStatus: true, hidden: false, commandBusy: false, nowMs: 100000 }, over);
    const fresh = PV.newStats();
    assert.equal(PV.historyDue(fresh, ctx()), true); // the first look
    assert.equal(PV.historyDue(fresh, ctx({ tabShown: false })), false);
    assert.equal(PV.historyDue(fresh, ctx({ commandBusy: true })), false);
    assert.equal(PV.historyDue(fresh, ctx({ hidden: true })), false);
    assert.equal(PV.historyDue(fresh, ctx({ online: false })), false);
    assert.equal(PV.historyDue(fresh, ctx({ haveStatus: false })), false);
    const got = historyState(historyDoc([grind()]), 100000);
    assert.equal(PV.historyDue(got, ctx({ nowMs: 129999 })), false);
    assert.equal(PV.historyDue(got, ctx({ nowMs: 130000 })), true);
    assert.equal(PV.historyDue(PV.statsShown(got, 110000), ctx({ nowMs: 110000 })), true); // opening the tab asks at once
    const ended = PV.statsOnStatus(got, 'grinding', 'grind_complete', 105000);
    assert.equal(PV.historyDue(ended, ctx({ nowMs: 105000 })), true); // after a grind ended
    assert.equal(PV.historyDue(PV.statsFailed(got, 'ui_busy', 131000), ctx({ nowMs: 135000 })), false); // waits for its retry time
    assert.equal(PV.historyDue(PV.statsFailed(got, 'ui_busy', 131000), ctx({ nowMs: 141000 })), true);
});

test('nextPollTask: the list comes after the statistics, never starves the status, and a failure forces status', () => {
    const f = { haveStatus: true, needSchema: false, needSettings: false, needInfo: false, online: true, forceStatus: false, statsDue: true, historyDue: true };
    assert.equal(PV.nextPollTask(f), 'stats');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { statsDue: false })), 'history');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { statsDue: false, historyDue: false })), 'status');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { statsDue: false, forceStatus: true })), 'status');
    assert.equal(PV.nextPollTask(Object.assign({}, f, { statsDue: false, online: false })), 'status');
    const needs = { needSchema: false, needSettings: false, needInfo: false };
    const next = PV.pollSucceeded('history', needs, historyDoc([]), true, 'ready');
    assert.deepEqual([next.needSchema, next.needSettings, next.needInfo, next.forceStatus, next.quick], [false, false, false, false, false]);
    for (const code of ['not_supported', 'ui_busy', 'bad_reply', 'http_error']) {
        assert.deepEqual(PV.pollFailed('history', code), { schemaMissing: false, connectionLost: false, forceStatus: true }, code);
    }
    for (const code of ['network', 'timeout', 'web_interface_off']) assert.equal(PV.pollFailed('history', code).connectionLost, true, code);
});

test('the recent-grinds list is wired in the page: a named list, a hidden chart, text written with textContent only', () => {
    const html = fs3.readFileSync(path3.join(__dirname, '..', '..', 'web', 'portal', 'page.src.html'), 'utf8');
    const app = fs3.readFileSync(path3.join(__dirname, '..', '..', 'web', 'portal', 'portal-app.js'), 'utf8');
    const css = fs3.readFileSync(path3.join(__dirname, '..', '..', 'web', 'portal', 'portal.css'), 'utf8');
    assert.match(html, /<section id="history"[^>]*aria-labelledby="history-h"[^>]*hidden>/);
    assert.match(html, /<ul id="history-list"[^>]*role="list"[^>]*aria-labelledby="history-h"/);
    assert.match(html, /<div id="history-chart"[^>]*aria-hidden="true"/);
    assert.match(html, /<template id="history-row">/);
    assert.match(app, /V\.historyView\(history, conn, now\(\)\)/);
    assert.match(app, /client\.getHistory\(\)/);
    assert.match(app, /V\.historyDue\(history, \{[^}]*commandBusy: commandPending \|\| client\.pending\(\) > 0/s);
    assert.equal(app.includes('createElementNS'), false); // the chart is divs, not script-made SVG
    assert.match(app, /fill\.style\.height = bar\.pct \+ '%'/);
    assert.equal(app.includes('innerHTML'), false);
    for (const level of ['ok', 'warning', 'error', 'neutral']) assert.ok(css.includes('.h-badge.' + level), level);
    // each result badge has an icon of its own shape, not only a colour
    assert.match(css, /\.h-badge\.ok \.icon-ok/);
    assert.match(css, /\.h-badge\.error \.icon-error/);
});

test('screen keys sit in Display after the brightness sliders; Never sorts last; freshness labels unchanged', () => {
    const display = PV.settingsGroups(FIELDS).find((g) => g.title === 'Display');
    assert.deepEqual(display.fields.map((f) => f.key), ['brightness_normal', 'brightness_screensaver', 'screen_dim_min', 'screen_off_min']);
    ['screen_dim_min', 'screen_off_min'].forEach((k) => {
        PV.STEP_OPTIONS[k].forEach((v) => {
            const o = PV.selectOptions(k, v);
            assert.equal(o[o.length - 1].label, 'Never');
            assert.equal(o[o.length - 1].value, 0);
        });
        assert.equal(PV.selectOptions(k, 3).slice(-1)[0].value, 0);
    });
    assert.deepEqual(PV.selectOptions('freshness_hours', 8).map((o) => o.label),
        ['30 minutes', '1 hour', '2 hours', '3 hours', '4 hours', '8 hours', '12 hours', '24 hours', '48 hours']);
    assert.equal(PV.selectOptions('screen_dim_min', 120).slice(-2)[0].label, '2 hours');
});
