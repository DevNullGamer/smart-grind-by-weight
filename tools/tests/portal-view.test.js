// node --test tools/tests/portal-view.test.js
// Checks the pure view helpers of the web panel page (web/portal/portal-view.js).
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const PV = require('../../web/portal/portal-view.js');

test('tabForViewport keeps the picked tab on narrow screens', () => {
    for (const tab of PV.TABS) assert.equal(PV.tabForViewport(tab, false), tab);
});

test('tabForViewport replaces Grind by Settings from 900 px', () => {
    assert.equal(PV.tabForViewport('grind', true), 'settings');
    assert.equal(PV.tabForViewport('settings', true), 'settings');
    assert.equal(PV.tabForViewport('insights', true), 'insights');
});

test('tabForViewport falls back for an unknown tab', () => {
    assert.equal(PV.tabForViewport('nope', false), 'grind');
    assert.equal(PV.tabForViewport(undefined, true), 'settings');
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
    for (const ui of uiStates) for (const busy of [false, true]) for (const b of blocks) for (const c of conns) {
        for (const extra of [undefined, { leadIn: true }, { commandPending: true }]) {
            const d = PV.deriveControls(status({ ui_state: ui, busy: busy, start_block: b, motor_test_block: b }), c, extra);
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
    assert.match(fault.banner.text, /Restart the grinder/);
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
