// node --test tools/tests/portal-lastgrind.test.js
// chg021 G1 (About card) and G2 (Last grind card, weight curve) of the web panel: web/portal/portal-view.js.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const PV = require('../../web/portal/portal-view.js');

const ROOT = path.join(__dirname, '..', '..', 'web', 'portal');

// ------------------------------------------------------------------------------------------------ G1: About card

const INFO = { name: 'grindbyweight', version: '1.4.0', build: 123, board: 'V2', busy: false, confirm: true };

function rowsOf(view) {
    const byId = {};
    view.rows.forEach((r) => { byId[r.id] = r; });
    return byId;
}

test('aboutView: every known member has its row, in order, with the uptime formatted', () => {
    const view = PV.aboutView(INFO, { uptime_s: 123 }, { address: 'grindbyweight.local', online: true });
    assert.equal(view.loading, false);
    assert.deepEqual(view.rows.map((r) => r.id), ['name', 'address', 'firmware', 'build', 'board', 'uptime', 'confirm']);
    assert.deepEqual(view.rows.map((r) => r.visible), [true, true, true, true, true, true, true]);
    const rows = rowsOf(view);
    assert.equal(rows.name.value, 'grindbyweight');
    assert.equal(rows.address.value, 'grindbyweight.local');
    assert.equal(rows.firmware.value, '1.4.0');
    assert.equal(rows.build.value, '123');
    assert.equal(rows.board.value, 'V2');
    assert.equal(rows.uptime.value, PV.formatDuration(123));
    assert.equal(rows.uptime.value, '2 min 3 s');
    assert.equal(rows.confirm.label, 'Confirm on the grinder');
    assert.equal(rows.confirm.value, 'On');
    assert.match(rows.confirm.note, /tap on the grinder/);
});

test('aboutView: Confirm off says so, with its own note', () => {
    const rows = rowsOf(PV.aboutView(Object.assign({}, INFO, { confirm: false }), null, {}));
    assert.equal(rows.confirm.value, 'Off');
    assert.match(rows.confirm.note, /without a tap/);
});

test('aboutView: before the info arrives only the address shows and the card says it is loading', () => {
    const view = PV.aboutView(null, null, { address: '127.0.0.1:8080' });
    assert.equal(view.loading, true);
    assert.deepEqual(view.rows.filter((r) => r.visible).map((r) => r.id), ['address']);
    assert.equal(PV.aboutView(undefined, null).loading, true);
    assert.equal(PV.aboutView({}, null).loading, false); // arrived, but with nothing usable
});

test('aboutView: a member that is missing (older firmware) hides only its row', () => {
    for (const key of ['name', 'version', 'build', 'board', 'confirm']) {
        const info = Object.assign({}, INFO);
        delete info[key];
        const visible = PV.aboutView(info, { uptime_s: 5 }, { address: 'x' }).rows.filter((r) => r.visible).length;
        assert.equal(visible, 6, key);
    }
    const noUptime = rowsOf(PV.aboutView(INFO, {}, { address: 'x' }));
    assert.equal(noUptime.uptime.visible, false);
    assert.equal(rowsOf(PV.aboutView(INFO, null, { address: 'x' })).uptime.visible, false); // no status yet
});

test('aboutView: null, wrong types and odd values hide their row and never print undefined, null or NaN', () => {
    const bad = [null, undefined, 5, true, {}, [], 'x'.repeat(0), '   ', NaN, Infinity, -1];
    for (const v of bad) {
        for (const key of ['name', 'version', 'board']) {
            if (typeof v === 'string' && v.trim() !== '') continue;
            const rows = rowsOf(PV.aboutView(Object.assign({}, INFO, { [key]: v }), null, { address: 'x' }));
            const id = key === 'version' ? 'firmware' : key;
            assert.equal(rows[id].visible, false, key + '=' + String(v));
        }
    }
    for (const v of [null, '12', NaN, Infinity, -Infinity, -1, true, {}, []]) {
        assert.equal(rowsOf(PV.aboutView(Object.assign({}, INFO, { build: v }), null, {})).build.visible, false, 'build=' + String(v));
    }
    for (const v of [null, 'true', 1, 0, 'yes', {}, undefined]) {
        const row = rowsOf(PV.aboutView(Object.assign({}, INFO, { confirm: v }), null, {})).confirm;
        assert.equal(row.visible, false, 'confirm=' + String(v));
        assert.equal(row.note, '');
    }
    for (const v of [null, '5', NaN, Infinity, -1, {}, undefined]) {
        assert.equal(rowsOf(PV.aboutView(INFO, { uptime_s: v }, {})).uptime.visible, false, 'uptime=' + String(v));
    }
    for (const info of [null, 5, 'text', [], [INFO], {}, { name: 5, version: [], build: 'x', board: null, confirm: 'on' }]) {
        const view = PV.aboutView(info, { uptime_s: 'x' }, { address: 'host' });
        view.rows.forEach((r) => assert.doesNotMatch(r.value + r.note, /undefined|null|NaN|\[object/));
        assert.deepEqual(view.rows.filter((r) => r.visible).map((r) => r.id), ['address']);
    }
});

test('aboutView: zero is a real build and a real uptime; a huge uptime stays readable', () => {
    const rows = rowsOf(PV.aboutView(Object.assign({}, INFO, { build: 0 }), { uptime_s: 0 }, {}));
    assert.equal(rows.build.value, '0');
    assert.equal(rows.uptime.value, '0 s');
    assert.equal(rowsOf(PV.aboutView(INFO, { uptime_s: 4294967 }, {})).uptime.value, '49 d 17 h');
});

test('aboutView: the uptime is hidden while the grinder is not reachable, shown again online', () => {
    assert.equal(rowsOf(PV.aboutView(INFO, { uptime_s: 9 }, { online: false })).uptime.visible, false);
    assert.equal(rowsOf(PV.aboutView(INFO, { uptime_s: 9 }, { online: true })).uptime.visible, true);
    assert.equal(rowsOf(PV.aboutView(INFO, { uptime_s: 9 })).uptime.visible, true);
});

test('aboutView: texts are trimmed and cut to 64 characters on code points', () => {
    assert.equal(rowsOf(PV.aboutView(Object.assign({}, INFO, { name: '  kitchen  ' }), null, {})).name.value, 'kitchen');
    const long = rowsOf(PV.aboutView(Object.assign({}, INFO, { board: 'x'.repeat(200) }), null, {})).board.value;
    assert.equal(long.length, 64);
    const emoji = rowsOf(PV.aboutView(Object.assign({}, INFO, { board: '\u{1F600}'.repeat(100) }), null, {})).board.value;
    assert.equal(Array.from(emoji).length, 64);
    assert.doesNotMatch(emoji, /[\ud800-\udbff](?![\udc00-\udfff])/);
});

test('deviceName and buildLine use only what is known', () => {
    assert.equal(PV.deviceName(INFO), 'grindbyweight');
    assert.equal(PV.deviceName(null), 'Grinder');
    assert.equal(PV.deviceName({ name: 5 }), 'Grinder');
    assert.equal(PV.buildLine(INFO), 'Firmware 1.4.0, build 123');
    assert.equal(PV.buildLine({ version: '1.4.0' }), 'Firmware 1.4.0');
    assert.equal(PV.buildLine({ build: 7 }), 'build 7');
    assert.equal(PV.buildLine({ version: null, build: 'x' }), '');
    assert.equal(PV.buildLine(null), '');
    assert.doesNotMatch(PV.buildLine({ version: undefined, build: NaN }), /undefined|NaN/);
});

// ------------------------------------------------------------------------------------------------ G2: sparkline

function samplesOf(weights, stepMs) {
    return weights.map((g, i) => ({ t: i * (stepMs || 500), g }));
}

function parsePoints(text) {
    return text.split(' ').map((p) => p.split(',').map(Number));
}

test('sparklinePoints: under three samples there is no curve', () => {
    assert.equal(PV.sparklinePoints([], 240, 64).points, '');
    assert.equal(PV.sparklinePoints(null, 240, 64).points, '');
    assert.equal(PV.sparklinePoints(samplesOf([1, 2]), 240, 64).points, '');
    assert.equal(PV.sparklinePoints(samplesOf([1, 2, 3]), 240, 64).points !== '', true);
    assert.equal(PV.sparklinePoints(samplesOf([1, 2]), 240, 64, 18).targetY, null);
});

test('sparklinePoints: x runs over the box by time, y rises with the weight and stays inside the box', () => {
    const sp = PV.sparklinePoints(samplesOf([0, 5, 10, 18]), 240, 64);
    const pts = parsePoints(sp.points);
    assert.equal(pts.length, 4);
    assert.equal(pts[0][0], 2);
    assert.equal(pts[3][0], 238);
    for (let i = 1; i < pts.length; i++) {
        assert.ok(pts[i][0] > pts[i - 1][0]);
        assert.ok(pts[i][1] < pts[i - 1][1], 'a heavier weight is higher up (smaller y)');
    }
    pts.forEach(([x, y]) => { assert.ok(x >= 0 && x <= 240 && y >= 0 && y <= 64); });
    assert.equal(pts[0][1], 60); // 0 g at the bottom padding
    assert.equal(pts[3][1], 4);  // the highest reading at the top padding
    assert.equal(sp.count, 4);
});

test('sparklinePoints: NaN, infinite and non-number samples are left out; too few left is no curve', () => {
    const dirty = [{ t: 0, g: 1 }, { t: 100, g: NaN }, { t: 200, g: Infinity }, { t: NaN, g: 3 }, { t: 300, g: '4' },
        null, 7, { t: 400, g: 5 }, { t: 500, g: 6 }, { t: 600, g: 7 }];
    const sp = PV.sparklinePoints(dirty, 240, 64);
    assert.equal(sp.count, 4);
    assert.doesNotMatch(sp.points, /NaN|Infinity|undefined/);
    assert.equal(sp.points.split(' ').length, 4);
    assert.equal(PV.sparklinePoints([{ t: 0, g: NaN }, { t: 1, g: 2 }, { t: 2, g: 3 }], 240, 64).points, '');
});

test('sparklinePoints: negative weights get room below the zero line', () => {
    const sp = PV.sparklinePoints(samplesOf([0, -2, -1, 4]), 240, 64);
    const pts = parsePoints(sp.points);
    assert.ok(pts[1][1] > pts[0][1], 'a negative reading is under the zero reading');
    assert.equal(pts[1][1], 60); // the lowest reading sits at the bottom
    pts.forEach(([, y]) => assert.ok(y >= 0 && y <= 64));
    // all negative: still inside the box
    const neg = parsePoints(PV.sparklinePoints(samplesOf([-1, -3, -2]), 240, 64).points);
    neg.forEach(([, y]) => assert.ok(y >= 0 && y <= 64));
});

test('sparklinePoints: a flat curve is not stretched and never gives NaN', () => {
    const sp = PV.sparklinePoints(samplesOf([0, 0, 0, 0]), 240, 64);
    assert.doesNotMatch(sp.points, /NaN/);
    parsePoints(sp.points).forEach(([, y]) => assert.equal(y, 60));
    const tiny = parsePoints(PV.sparklinePoints(samplesOf([0, 0.01, 0.02]), 240, 64).points);
    assert.ok(tiny[2][1] > 55, 'a 0.02 g change stays a flat line (the scale is at least 1 g)');
});

test('sparklinePoints: x follows the time, so a gap in the samples is one long stretch', () => {
    const gap = [{ t: 0, g: 0 }, { t: 500, g: 2 }, { t: 10000, g: 9 }, { t: 10500, g: 10 }];
    const pts = parsePoints(PV.sparklinePoints(gap, 240, 64).points);
    assert.ok(Math.abs(pts[1][0] - (2 + 500 / 10500 * 236)) < 0.11);
    assert.ok(pts[2][0] > 220);
    // all samples at the same time: spread by order, not divided by zero
    const same = parsePoints(PV.sparklinePoints([{ t: 5, g: 1 }, { t: 5, g: 2 }, { t: 5, g: 3 }], 240, 64).points);
    assert.deepEqual(same.map((p) => p[0]), [2, 120, 238]);
});

test('sparklinePoints: the target line is inside the box, above the curve when the grind stayed under it', () => {
    const sp = PV.sparklinePoints(samplesOf([0, 4, 8, 12]), 240, 64, 18);
    assert.equal(sp.targetY, 4);
    parsePoints(sp.points).forEach(([, y]) => assert.ok(y > sp.targetY));
    // overshoot: the highest reading is above the target and still fits
    const over = PV.sparklinePoints(samplesOf([0, 10, 20, 22]), 240, 64, 18);
    assert.ok(over.targetY > 4 && over.targetY < 60);
    assert.equal(parsePoints(over.points)[3][1], 4);
    // no usable target: no line
    for (const t of [undefined, null, 0, -3, NaN, Infinity, '18']) {
        assert.equal(PV.sparklinePoints(samplesOf([0, 4, 8]), 240, 64, t).targetY, null, String(t));
    }
});

test('sparklineSummary: start, end, time and target in words; empty under three samples', () => {
    assert.equal(PV.sparklineSummary(samplesOf([0, 8, 18.1], 4900), 18), 'Weight from 0.0 g to 18.1 g in 9.8 s. Target 18.0 g (dashed line).');
    assert.equal(PV.sparklineSummary(samplesOf([0, 8, 18.1], 4900), null), 'Weight from 0.0 g to 18.1 g in 9.8 s.');
    assert.equal(PV.sparklineSummary(samplesOf([0, 1]), 18), '');
    assert.equal(PV.sparklineSummary([], 18), '');
    assert.match(PV.sparklineSummary(samplesOf([-0.04, 2, 3]), null), /^Weight from 0\.0 g /); // no "-0.0"
    assert.doesNotMatch(PV.sparklineSummary(samplesOf([1, NaN, 2, 3]), null), /NaN/);
});

// ------------------------------------------------------------------------------------------------ G2: the track

const SETTINGS = { values: { grind_mode: 'weight', single_weight_g: 9, double_weight_g: 18, custom_weight_g: 21.5,
    single_time_s: 5, double_time_s: 10, custom_time_s: 12 }, current_profile: 1 };

function grinding(weight, phase) {
    return { ui_state: 'grinding', grind_phase: phase || 'predictive', busy: true, weight_g: weight };
}

function readyStatus() {
    return { ui_state: 'ready', grind_phase: 'idle', busy: false, weight_g: 0 };
}

function ctx(nowMs, over) {
    return Object.assign({ nowMs, leadIn: null, settings: SETTINGS, history: PV.newStats() }, over || {});
}

// feeds one status per entry at 500 ms steps from startMs; returns the track
function run(track, weights, startMs, over) {
    let t = track;
    weights.forEach((g, i) => { t = PV.grindTrackUpdate(t, grinding(g), ctx(startMs + i * 500, over)); });
    return t;
}

function loadedHistory(grinds, fetchedAt) {
    return PV.historyLoaded(PV.newStats(), { count: grinds.length, persistent: true, grinds }, fetchedAt);
}

function entry(over) {
    return Object.assign({ id: 57, age_s: 1, profile: 1, mode: 'weight', target_g: 18, target_s: null, final_g: 18.1,
        time_ms: 14230, motor_ms: 9800, pulses: 2, result: 'complete' }, over || {});
}

test('the track: idle statuses create nothing; a null status changes nothing', () => {
    let t = PV.newGrindTrack();
    t = PV.grindTrackUpdate(t, readyStatus(), ctx(1000));
    assert.equal(t.phase, 'none');
    assert.equal(PV.grindTrackUpdate(t, null, ctx(1000)), t);
    assert.equal(PV.lastGrindView(t, 2000).visible, false);
});

test('the track: samples are collected while grinding, with the time since the start', () => {
    const t = run(PV.newGrindTrack(), [0, 1.5, 4], 10000);
    assert.equal(t.phase, 'running');
    assert.deepEqual(t.samples, [{ t: 0, g: 0 }, { t: 500, g: 1.5 }, { t: 1000, g: 4 }]);
    assert.equal(t.startedAt, 10000);
});

test('the track: the buffer resets when the next grind starts', () => {
    let t = run(PV.newGrindTrack(), [0, 3, 6, 9], 1000);
    t = PV.grindTrackUpdate(t, readyStatus(), ctx(4000));
    assert.equal(t.phase, 'ended');
    assert.equal(t.samples.length, 4);
    t = PV.grindTrackUpdate(t, grinding(0.5), ctx(20000));
    assert.equal(t.phase, 'running');
    assert.deepEqual(t.samples, [{ t: 0, g: 0.5 }]);
    assert.equal(t.result, null);
    assert.equal(t.endedAt, null);
});

test('the track: the buffer holds the newest 120 samples and the oldest drop out', () => {
    const weights = Array.from({ length: 150 }, (_, i) => i);
    const t = run(PV.newGrindTrack(), weights, 0);
    assert.equal(PV.GRIND_SAMPLES_MAX, 120);
    assert.equal(t.samples.length, 120);
    assert.equal(t.samples[0].g, 30);
    assert.equal(t.samples[119].g, 149);
    assert.equal(t.startedAt, 0);
});

test('the track: NaN, null, string and infinite weights are not kept; negative weights are', () => {
    let t = PV.newGrindTrack();
    [NaN, null, undefined, '5', Infinity, -Infinity].forEach((g, i) => {
        t = PV.grindTrackUpdate(t, { ui_state: 'grinding', grind_phase: 'predictive', weight_g: g }, ctx(i * 500));
    });
    assert.equal(t.phase, 'running');
    assert.equal(t.samples.length, 0);
    t = PV.grindTrackUpdate(t, grinding(-0.4), ctx(5000));
    t = PV.grindTrackUpdate(t, grinding(2), ctx(5500));
    assert.deepEqual(t.samples.map((s) => s.g), [-0.4, 2]);
});

test('the track: two statuses at the same moment give one sample; a time that goes backwards is ignored', () => {
    let t = PV.grindTrackUpdate(PV.newGrindTrack(), grinding(1), ctx(1000));
    t = PV.grindTrackUpdate(t, grinding(2), ctx(1500));
    t = PV.grindTrackUpdate(t, grinding(3), ctx(1500));
    t = PV.grindTrackUpdate(t, grinding(4), ctx(1400));
    assert.deepEqual(t.samples.map((s) => s.g), [1, 2]);
});

test('the track: priming and the purge wait draw nothing; a purge pause does not end or reset the grind', () => {
    let t = PV.grindTrackUpdate(PV.newGrindTrack(), grinding(0.2, 'prime'), ctx(0));
    t = PV.grindTrackUpdate(t, grinding(0.3, 'prime_settling'), ctx(500));
    t = PV.grindTrackUpdate(t, grinding(0.4, 'purge_confirm'), ctx(1000));
    assert.equal(t.samples.length, 0);
    t = PV.grindTrackUpdate(t, grinding(1, 'predictive'), ctx(1500));
    t = PV.grindTrackUpdate(t, { ui_state: 'purge_confirm', grind_phase: 'purge_confirm', weight_g: 2 }, ctx(2000));
    assert.equal(t.phase, 'running');
    t = PV.grindTrackUpdate(t, grinding(3), ctx(2500));
    assert.deepEqual(t.samples.map((s) => s.g), [1, 3]);
    assert.equal(t.startedAt, 0);
});

test('the track: any other screen ends the grind once; later statuses leave it alone', () => {
    let t = run(PV.newGrindTrack(), [0, 5, 10], 0);
    const done = PV.grindTrackUpdate(t, { ui_state: 'grind_complete', grind_phase: 'completed', weight_g: 18 }, ctx(2000));
    assert.equal(done.phase, 'ended');
    assert.equal(done.endedAt, 2000);
    assert.equal(done.lastSeenAt, 1000);
    const later = PV.grindTrackUpdate(done, readyStatus(), ctx(9000));
    assert.equal(later, done);
    assert.equal(t.phase, 'running'); // the earlier object is not changed in place
    assert.equal(t.samples.length, 3);
});

test('the track: the profile comes only from a lead-in this page started, with its target', () => {
    const leadIn = PV.leadInStart(1, 3000, 0);
    let t = PV.grindTrackUpdate(PV.newGrindTrack(), grinding(0), ctx(3100, { leadIn }));
    assert.equal(t.profile, 1);
    assert.deepEqual(t.target, { mode: 'weight', value: 18 });
    const timeSettings = { values: Object.assign({}, SETTINGS.values, { grind_mode: 'time' }), current_profile: 1 };
    t = PV.grindTrackUpdate(PV.newGrindTrack(), grinding(0), ctx(3100, { leadIn, settings: timeSettings }));
    assert.deepEqual(t.target, { mode: 'time', value: 10 });
    // a grind started on the grinder: no profile, no target
    t = PV.grindTrackUpdate(PV.newGrindTrack(), grinding(0), ctx(3100));
    assert.equal(t.profile, null);
    assert.equal(t.target, null);
    // a dropped start shows no grind, but if one starts later it is not this page's
    const dropped = Object.assign({}, leadIn, { phase: 'dropped', droppedAt: 6000 });
    t = PV.grindTrackUpdate(PV.newGrindTrack(), grinding(0), ctx(7000, { leadIn: dropped }));
    assert.equal(t.profile, null);
    // settings not read yet
    t = PV.grindTrackUpdate(PV.newGrindTrack(), grinding(0), ctx(3100, { leadIn, settings: null }));
    assert.equal(t.profile, 1);
    assert.equal(t.target, null);
});

test('profileTarget: only a usable number of a real profile', () => {
    assert.deepEqual(PV.profileTarget(SETTINGS, 0), { mode: 'weight', value: 9 });
    assert.equal(PV.profileTarget(SETTINGS, 3), null);
    assert.equal(PV.profileTarget(SETTINGS, -1), null);
    assert.equal(PV.profileTarget(SETTINGS, 1.5), null);
    assert.equal(PV.profileTarget(SETTINGS, '1'), null);
    assert.equal(PV.profileTarget(SETTINGS, null), null);
    assert.equal(PV.profileTarget(null, 0), null);
    assert.equal(PV.profileTarget({ values: { double_weight_g: NaN } }, 1), null);
    assert.equal(PV.profileTarget({ values: { double_weight_g: 0 } }, 1), null);
});

// ------------------------------------------------------------------------------------------------ G2: the result

function endedTrack(opts) {
    const o = opts || {};
    let t = run(PV.newGrindTrack(), [0, 4, 9, 14, 18], 1000, o.ctx);
    t = PV.grindTrackUpdate(t, { ui_state: 'grind_complete', grind_phase: 'completed', weight_g: 18 }, ctx(3500));
    return t; // ended at 3500, last grinding status at 3000
}

test('the result: a look from before the end settles nothing', () => {
    const t = endedTrack();
    assert.equal(PV.grindTrackResolve(t, loadedHistory([entry()], 3000), 3000), t);
    assert.equal(PV.grindTrackResolve(t, PV.newStats(), 3600), t);
});

test('the result: a new entry (it ended after the last grinding status) is recorded and stays', () => {
    const t = endedTrack();
    const r = PV.grindTrackResolve(t, loadedHistory([entry({ age_s: 0 })], 3600), 3600);
    assert.equal(r.result.state, 'recorded');
    assert.equal(r.result.grind.final, 18.1);
    assert.equal(t.result, null); // not changed in place
    // a later look that shows another, newer grind must not replace it
    const later = PV.grindTrackResolve(r, loadedHistory([entry({ id: 58, age_s: 0 }), entry({ age_s: 40 })], 9000), 9000);
    assert.equal(later, r);
});

test('the result: an older entry right after the end is still "waiting"; after the settle time it is "not recorded"', () => {
    const t = endedTrack();
    const early = loadedHistory([entry({ age_s: 600 })], 3700);
    assert.equal(PV.grindTrackResolve(t, early, 3700), t);
    assert.equal(PV.lastGrindWaiting(t, 3700), true);
    assert.equal(PV.lastGrindView(t, 3700).message, 'Reading the result from the grinder.');
    const late = PV.grindTrackResolve(t, loadedHistory([entry({ age_s: 606 })], 9500), 9500);
    assert.equal(late.result.state, 'unrecorded');
    assert.equal(PV.lastGrindWaiting(late, 9500), false);
    // an empty list: nothing was recorded either
    const empty = PV.grindTrackResolve(t, loadedHistory([], 9500), 9500);
    assert.equal(empty.result.state, 'unrecorded');
    // an entry from before the last restart (age null) cannot be this grind
    const restarted = PV.grindTrackResolve(t, loadedHistory([entry({ age_s: null })], 9500), 9500);
    assert.equal(restarted.result.state, 'unrecorded');
});

test('the result: the entry the page already knew at the start is never this grind', () => {
    const known = loadedHistory([entry({ id: 57, age_s: 300 })], 500);
    const t = endedTrack({ ctx: { history: known } });
    assert.deepEqual(t.before, { known: true, id: 57 });
    // the same id with a small age (a clock quirk) is still not new
    const same = PV.grindTrackResolve(t, loadedHistory([entry({ id: 57, age_s: 1 })], 9500), 9500);
    assert.equal(same.result.state, 'unrecorded');
    const fresh = PV.grindTrackResolve(t, loadedHistory([entry({ id: 58, age_s: 1 }), entry({ id: 57, age_s: 300 })], 9500), 9500);
    // age 1 s at 9500 ms: the grinding status was at 3000, so 6.5 s + slack allows it
    assert.equal(fresh.result.state, 'recorded');
    // a known empty list: any entry that fits the age is new
    const emptyBefore = endedTrack({ ctx: { history: loadedHistory([], 500) } });
    assert.deepEqual(emptyBefore.before, { known: true, id: null });
    assert.equal(PV.grindTrackResolve(emptyBefore, loadedHistory([entry({ age_s: 2 })], 3600), 3600).result.state, 'recorded');
});

test('the result: an entry that ended before the last grinding status is not this grind (a stopped grind is not listed)', () => {
    const t = endedTrack();
    // fetched at 9500; the grinding status was at 3000: 6.5 s ago. An entry 30 s old ended long before the grind
    const r = PV.grindTrackResolve(t, loadedHistory([entry({ age_s: 30 })], 9500), 9500);
    assert.equal(r.result.state, 'unrecorded');
    assert.equal(PV.lastGrindView(r, 9500).badge.label, 'Not recorded');
    assert.match(PV.lastGrindView(r, 9500).message, /did not finish/);
    assert.doesNotMatch(PV.lastGrindView(r, 9500).message, /complete|final/i);
});

test('the result: a 404 on the history ends the wait and keeps the curve; no result text claims anything', () => {
    const t = endedTrack();
    const gone = PV.statsFailed(PV.newStats(), 'not_supported', 3600);
    const r = PV.grindTrackResolve(t, gone, 3600);
    assert.equal(r.result.state, 'unavailable');
    const v = PV.lastGrindView(r, 3700);
    assert.equal(v.visible, true);
    assert.equal(v.spark.visible, true);
    assert.equal(v.result.visible, false);
    assert.equal(v.badge.visible, false);
    assert.match(v.message, /does not list finished grinds/);
});

test('the result: without a usable look the card says so after 30 s and stops waiting', () => {
    const t = endedTrack();
    assert.equal(PV.lastGrindWaiting(t, 3500), true);
    assert.equal(PV.lastGrindWaiting(t, 3500 + PV.LAST_GRIND_WAIT_MS - 1), true);
    assert.equal(PV.lastGrindWaiting(t, 3500 + PV.LAST_GRIND_WAIT_MS), false);
    assert.equal(PV.lastGrindView(t, 3500 + PV.LAST_GRIND_WAIT_MS).message, 'The result could not be read from the grinder.');
    assert.equal(PV.lastGrindWaiting(PV.newGrindTrack(), 0), false);
    assert.equal(PV.lastGrindWaiting(run(PV.newGrindTrack(), [1, 2], 0), 0), false); // running: not yet
});

// ------------------------------------------------------------------------------------------------ G2: the view

test('lastGrindView: hidden before any grind and while a running grind has fewer than three samples', () => {
    assert.equal(PV.lastGrindView(PV.newGrindTrack(), 0).visible, false);
    assert.equal(PV.lastGrindView(run(PV.newGrindTrack(), [0, 1], 0), 1000).visible, false);
    const three = PV.lastGrindView(run(PV.newGrindTrack(), [0, 1, 2], 0), 1000);
    assert.equal(three.visible, true);
    assert.equal(three.running, true);
    assert.equal(three.title, 'This grind');
    assert.equal(three.badge.visible, false);
    assert.equal(three.result.visible, false);
    assert.equal(three.message, '');
});

test('lastGrindView: the target line shows only when the page knows the profile it started (weight mode)', () => {
    const leadIn = PV.leadInStart(1, 3000, 0);
    const own = run(PV.newGrindTrack(), [0, 4, 8], 3100, { leadIn });
    const v = PV.lastGrindView(own, 5000);
    assert.notEqual(v.spark.targetY, null);
    assert.match(v.spark.caption, /Target 18\.0 g/);
    const foreign = PV.lastGrindView(run(PV.newGrindTrack(), [0, 4, 8], 3100), 5000);
    assert.equal(foreign.spark.targetY, null);
    assert.doesNotMatch(foreign.spark.caption, /Target/);
    const timeSettings = { values: Object.assign({}, SETTINGS.values, { grind_mode: 'time' }), current_profile: 1 };
    const time = PV.lastGrindView(run(PV.newGrindTrack(), [0, 4, 8], 3100, { leadIn, settings: timeSettings }), 5000);
    assert.equal(time.spark.targetY, null); // a time target is not a weight
});

test('lastGrindView: a recorded grind shows the result and the badge, with the entry\'s own target on the curve', () => {
    const t = PV.grindTrackResolve(endedTrack(), loadedHistory([entry({ age_s: 0, final_g: 19.6, result: 'overshoot' })], 3600), 3600);
    const v = PV.lastGrindView(t, 3600 + 125000);
    assert.equal(v.visible, true);
    assert.equal(v.running, false);
    assert.equal(v.title, 'Last grind');
    assert.equal(v.result.final, '19.6 g');
    assert.equal(v.result.target, 'target 18.0 g');
    assert.equal(v.result.error, '+1.6 g over');
    assert.equal(v.result.age, '2 min ago'); // the age keeps growing after the look
    assert.equal(v.result.name, 'Double');
    assert.deepEqual(v.badge, { visible: true, label: 'Overshoot', level: 'warning' });
    assert.notEqual(v.spark.targetY, null); // the page did not know the profile, the entry has the target
    assert.equal(v.message, '');
    const ok = PV.lastGrindView(PV.grindTrackResolve(endedTrack(), loadedHistory([entry({ age_s: 0 })], 3600), 3600), 3700);
    assert.deepEqual(ok.badge, { visible: true, label: 'Complete', level: 'ok' });
    assert.equal(ok.result.age, 'just now');
    assert.equal(ok.result.detail, '14.2 s · 2 pulses');
    assert.equal(ok.result.motor, 'motor 9.8 s');
});

test('lastGrindView: a recorded time-mode grind has no weight target line', () => {
    const timed = entry({ age_s: 0, mode: 'time', target_g: null, target_s: 10, final_g: 17.5 });
    const v = PV.lastGrindView(PV.grindTrackResolve(endedTrack(), loadedHistory([timed], 3600), 3600), 3700);
    assert.equal(v.spark.targetY, null);
    assert.equal(v.result.target, 'target 10.0 s');
    assert.equal(v.result.error, '');
});

test('lastGrindView: a stopped grind says it was not recorded and never shows the older grind as its result', () => {
    const older = entry({ id: 56, age_s: 900, final_g: 9.0, target_g: 9 });
    let t = endedTrack({ ctx: { history: loadedHistory([older], 100) } });
    assert.equal(PV.lastGrindView(t, 3600).message, 'Reading the result from the grinder.');
    t = PV.grindTrackResolve(t, loadedHistory([entry({ id: 56, age_s: 906 })], 9600), 9600);
    const v = PV.lastGrindView(t, 9700);
    assert.equal(v.result.visible, false);
    assert.equal(v.result.final, '');
    assert.equal(v.badge.label, 'Not recorded');
    assert.equal(v.badge.level, 'neutral');
    assert.equal(v.spark.visible, true); // the curve is what the page saw: honest
});

test('lastGrindView: a very short grind keeps its result without a curve', () => {
    let t = run(PV.newGrindTrack(), [0, 2], 1000);
    t = PV.grindTrackUpdate(t, readyStatus(), ctx(2000));
    t = PV.grindTrackResolve(t, loadedHistory([entry({ age_s: 0 })], 2100), 2100);
    const v = PV.lastGrindView(t, 2200);
    assert.equal(v.spark.visible, false);
    assert.equal(v.spark.points, '');
    assert.equal(v.result.visible, true);
});

test('lastGrindView: a dropped remote start leaves the card as it was (nothing starts, nothing is claimed)', () => {
    const before = PV.grindTrackResolve(endedTrack(), loadedHistory([entry({ age_s: 0 })], 3600), 3600);
    let t = before;
    const dropped = Object.assign({}, PV.leadInStart(0, 3000, 100000), { phase: 'dropped', droppedAt: 105000 });
    for (let i = 0; i < 6; i++) t = PV.grindTrackUpdate(t, readyStatus(), ctx(100000 + i * 1000, { leadIn: dropped }));
    assert.equal(t, before);
    assert.equal(PV.lastGrindView(t, 110000).title, 'Last grind');
});

test('lead-in interplay: the lead-in, then a grind from this page, then complete, then the history', () => {
    let leadIn = PV.leadInStart(0, 3000, 0);
    let t = PV.newGrindTrack();
    for (const at of [500, 1000, 1500, 2000, 2500]) t = PV.grindTrackUpdate(t, readyStatus(), ctx(at, { leadIn })); // still ready
    assert.equal(t.phase, 'none');
    t = PV.grindTrackUpdate(t, grinding(0), ctx(3500, { leadIn }));
    leadIn = PV.leadInTick(leadIn, grinding(0), 3500, 3500); // the page clears the lead-in after the track update
    assert.equal(leadIn, null);
    t = PV.grindTrackUpdate(t, grinding(5), ctx(4000));
    t = PV.grindTrackUpdate(t, grinding(9), ctx(4500));
    assert.equal(t.profile, 0);
    assert.equal(PV.grindTrackUpdate(t, grinding(9), ctx(4500)).samples.length, 3);
    assert.equal(PV.lastGrindView(t, 4500).spark.targetY !== null, true);
    t = PV.grindTrackUpdate(t, { ui_state: 'grind_complete', grind_phase: 'completed', weight_g: 9 }, ctx(5000));
    t = PV.grindTrackResolve(t, loadedHistory([entry({ age_s: 0, profile: 0, target_g: 9, final_g: 9.1 })], 5100), 5100);
    assert.equal(PV.lastGrindView(t, 5200).result.final, '9.1 g');
});

// ------------------------------------------------------------------------------------------------ G2: history polling

const CTX = { online: true, haveStatus: true, hidden: false, commandBusy: false, tabShown: false, nowMs: 100000, lastGrindWaiting: true };

test('historyDue outside the Insights tab: only for the one look the Last grind card waits for', () => {
    const ended = PV.statsOnStatus(PV.newStats(), 'grinding', 'grind_complete', 100000);
    assert.equal(PV.historyDue(ended, CTX), true);
    assert.equal(PV.historyDue(ended, Object.assign({}, CTX, { lastGrindWaiting: false })), false);
    assert.equal(PV.historyDue(PV.newStats(), CTX), false); // no probe, no refresh without an ask
    assert.equal(PV.historyDue(ended, Object.assign({}, CTX, { commandBusy: true })), false);
    assert.equal(PV.historyDue(ended, Object.assign({}, CTX, { hidden: true })), false);
    assert.equal(PV.historyDue(ended, Object.assign({}, CTX, { online: false })), false);
    assert.equal(PV.historyDue(ended, Object.assign({}, CTX, { haveStatus: false })), false);
    assert.equal(PV.historyDue(PV.statsFailed(ended, 'not_supported', 100000), CTX), false);
});

test('historyDue outside the tab: the follow-up look comes after its delay, a failed look after its retry time', () => {
    let h = PV.statsOnStatus(PV.newStats(), 'grinding', 'grind_complete', 100000);
    h = PV.historyLoaded(h, { grinds: [] }, 100100); // the first look; the follow-up is still asked for
    assert.equal(PV.historyDue(h, Object.assign({}, CTX, { nowMs: 100200 })), false);
    assert.equal(PV.historyDue(h, Object.assign({}, CTX, { nowMs: 100000 + PV.STATS_FOLLOW_UP_MS })), true);
    const failed = PV.statsFailed(PV.statsOnStatus(PV.newStats(), 'grinding', 'grind_complete', 100000), 'ui_busy', 100100);
    assert.equal(PV.historyDue(failed, Object.assign({}, CTX, { nowMs: 100200 })), false);
    assert.equal(PV.historyDue(failed, Object.assign({}, CTX, { nowMs: 100100 + PV.STATS_RETRY_MS })), true);
});

test('historyDue inside the tab is unchanged by the Last grind wait', () => {
    const fresh = PV.newStats();
    assert.equal(PV.historyDue(fresh, Object.assign({}, CTX, { tabShown: true, lastGrindWaiting: false })), true);
    assert.equal(PV.historyDue(fresh, Object.assign({}, CTX, { tabShown: false, lastGrindWaiting: false })), false);
});

// ------------------------------------------------------------------------------------------------ the page

test('the page: the curve is an HTML-embedded svg with a text alternative, no namespace, no script-made SVG', () => {
    const html = fs.readFileSync(path.join(ROOT, 'page.src.html'), 'utf8');
    const app = fs.readFileSync(path.join(ROOT, 'portal-app.js'), 'utf8');
    const spark = html.match(/<svg class="spark"[^>]*>/);
    assert.ok(spark);
    assert.match(spark[0], /aria-hidden="true"/);
    assert.doesNotMatch(spark[0], /xmlns/);
    assert.match(html, /<polyline id="lg-line"/);
    assert.match(html, /<p id="lg-caption"[^>]*>/);
    assert.equal(app.includes('createElementNS'), false);
    assert.match(app, /setAttr\(\$\('lg-line'\), 'points'/);
    // the caption is outside the aria-hidden drawing and the card is no live region
    const card = html.match(/<section id="lastgrind"[\s\S]*?<\/section>/)[0];
    assert.doesNotMatch(card, /aria-live|role="status"|role="alert"/);
    assert.match(card, /aria-labelledby="lg-h"/);
    assert.match(card, /<section id="lastgrind"[^>]*\shidden>/);
});

test('the page: every about row of the view has its elements; the rows that may be missing start hidden', () => {
    const html = fs.readFileSync(path.join(ROOT, 'page.src.html'), 'utf8');
    for (const row of PV.aboutView(INFO, { uptime_s: 1 }, { address: 'x' }).rows) {
        assert.match(html, new RegExp('id="about-row-' + row.id + '"'), row.id);
        assert.match(html, new RegExp('id="about-' + row.id + '"'), row.id);
        if (row.id !== 'address') assert.match(html, new RegExp('id="about-row-' + row.id + '"[^>]*\\shidden'), row.id);
    }
    assert.match(html, /id="about-confirm-note"/);
});

test('the page: the new app ids all exist in the page', () => {
    const html = fs.readFileSync(path.join(ROOT, 'page.src.html'), 'utf8');
    const app = fs.readFileSync(path.join(ROOT, 'portal-app.js'), 'utf8');
    for (const m of app.matchAll(/\$\('((?:lg|about)-[a-z-]+|grind-grid|lastgrind)'\)/g)) {
        assert.match(html, new RegExp('id="' + m[1] + '"'), m[1]);
    }
});

test('the page: the history request still asks about one thing at a time (the polling loop is unchanged)', () => {
    const app = fs.readFileSync(path.join(ROOT, 'portal-app.js'), 'utf8');
    assert.match(app, /lastGrindWaiting: lastGrindWaiting/);
    assert.match(app, /V\.nextPollTask\(\{/);
    assert.equal((app.match(/client\.getHistory\(\)/g) || []).length, 1);
});

// ------------------------------------------------------------------------------------------------ review fixes

test('the result: a stopped grind right after a completed one never resolves to the older entry (ids only grow)', () => {
    const known = loadedHistory([entry({ id: 57, age_s: 2 })], 500);
    const t = endedTrack({ ctx: { history: known } });
    // the older entry is only 2 s old (inside the age slack) but it is not newer than the one known before
    for (const older of [entry({ id: 57, age_s: 2 }), entry({ id: 56, age_s: 1 })]) {
        const r = PV.grindTrackResolve(t, loadedHistory([older], 9500), 9500);
        assert.equal(r.result.state, 'unrecorded');
    }
    assert.equal(PV.grindTrackResolve(t, loadedHistory([entry({ id: 58, age_s: 1 })], 9500), 9500).result.state, 'recorded');
});

test('sparkline: extreme weights and times never put NaN or Infinity into the points or the summary', () => {
    const extreme = [{ t: 0, g: -1e308 }, { t: 500, g: 1e308 }, { t: 1000, g: 5 }, { t: 1e308, g: 1 }];
    const sp = PV.sparklinePoints(extreme, 240, 64, 1e308);
    assert.doesNotMatch(sp.points, /NaN|Infinity/);
    assert.equal(Number.isFinite(sp.targetY), true);
    parsePoints(sp.points).forEach(([x, y]) => { assert.ok(x >= 0 && x <= 240 && y >= 0 && y <= 64); });
    assert.doesNotMatch(PV.sparklineSummary(extreme, 1e308), /NaN|Infinity|e\+/);
});

test('the wait for the result counts from the moment the page is visible again', () => {
    const t = endedTrack();
    const late = 3500 + 10 * PV.LAST_GRIND_WAIT_MS; // the tab was hidden all that time
    assert.equal(PV.lastGrindWaiting(t, late), false);
    assert.equal(PV.lastGrindView(t, late).message, 'The result could not be read from the grinder.');
    const back = PV.grindTrackResume(t, late);
    assert.equal(PV.lastGrindWaiting(back, late), true);
    assert.equal(PV.lastGrindView(back, late).message, 'Reading the result from the grinder.');
    assert.equal(PV.lastGrindWaiting(back, late + PV.LAST_GRIND_WAIT_MS), false);
    assert.equal(t.waitStart, null); // not changed in place
    // a resolved or running track is returned as it is
    const done = PV.grindTrackResolve(t, loadedHistory([entry({ age_s: 0 })], 3600), 3600);
    assert.equal(PV.grindTrackResume(done, late), done);
    const running = run(PV.newGrindTrack(), [1, 2, 3], 0);
    assert.equal(PV.grindTrackResume(running, late), running);
    // the look after the return can still settle it
    assert.equal(PV.grindTrackResolve(back, loadedHistory([entry({ age_s: 0 })], late), late).result.state, 'recorded');
});

test('the info is read again after the connection came back (Confirm may have changed)', () => {
    const none = { needSchema: false, needSettings: false, needInfo: false };
    assert.equal(PV.pollSucceeded('status', none, {}, false, 'ready').needInfo, true);
    assert.equal(PV.pollSucceeded('status', none, {}, true, 'ready').needInfo, false);
    assert.equal(PV.nextPollTask({ haveStatus: true, online: true, needInfo: true }), 'info');
});

// ------------------------------------------------------------------------------------------------ tester additions (G2)

// Runs the page's polling decisions (nextPollTask + historyDue + the app's state updates) against a scripted grinder.
// script(ms) -> status; listAt(ms) -> {grinds} | {err}. Returns the history requests and the most in flight at once.
function simulatePage(script, listAt, endMs, over) {
    const o = over || {};
    let history = PV.newStats();
    let track = PV.newGrindTrack();
    let status = null;
    let leadIn = o.leadIn || null;
    let prevUi = null;
    const requests = [];
    let inFlight = 0;
    let maxInFlight = 0;
    for (let now = 0; now <= endMs; now += 500) {
        const waiting = PV.lastGrindWaiting(track, now);
        const due = PV.historyDue(history, { online: true, haveStatus: status !== null, hidden: false, tabShown: false,
            commandBusy: false, nowMs: now, lastGrindWaiting: waiting });
        const task = PV.nextPollTask({ haveStatus: status !== null, online: true, needSchema: false, needSettings: false,
            needInfo: false, forceStatus: false, statsDue: false, historyDue: due });
        inFlight += 1; // the loop starts one request and waits for its reply (here in the same tick)
        maxInFlight = Math.max(maxInFlight, inFlight);
        if (task === 'history') {
            requests.push(now);
            const r = listAt(now);
            history = r.err ? PV.statsFailed(history, r.err, now)
                : PV.historyLoaded(history, { count: r.grinds.length, persistent: true, grinds: r.grinds }, now);
            track = PV.grindTrackResolve(track, history, now);
        } else {
            const s = script(now);
            history = PV.statsOnStatus(history, prevUi, s.ui_state, now);
            track = PV.grindTrackUpdate(track, s, { nowMs: now, leadIn: leadIn, settings: SETTINGS, history: history });
            if (leadIn) leadIn = PV.leadInTick(leadIn, s, now, now);
            status = s;
            prevUi = s.ui_state;
        }
        inFlight -= 1;
    }
    return { requests, maxInFlight, track, history };
}

function normalScript(ms) {
    if (ms < 2000) return readyStatus();
    if (ms < 7000) return grinding((ms - 2000) / 250);
    if (ms < 12000) return { ui_state: 'grind_complete', grind_phase: 'completed', busy: false, weight_g: 18 };
    return readyStatus();
}
function stoppedScript(ms) {
    if (ms < 2000) return readyStatus();
    if (ms < 5000) return grinding((ms - 2000) / 250);
    return readyStatus();
}

test('requests: a normal grind asks the history once, never two at a time', () => {
    const list = (ms) => ({ grinds: ms >= 7000
        ? [entry({ id: 58, age_s: Math.floor((ms - 7000) / 1000) }), entry({ id: 57, age_s: 300 })]
        : [entry({ id: 57, age_s: 300 + Math.floor(ms / 1000) })] });
    const r = simulatePage(normalScript, list, 60000);
    assert.equal(r.requests.length, 1, JSON.stringify(r.requests));
    assert.equal(r.maxInFlight, 1);
    assert.equal(r.track.result.state, 'recorded');
    assert.equal(r.track.result.grind.final, 18.1);
});

test('requests: a stopped grind asks the history at most twice and says "not recorded", not the older result', () => {
    const list = (ms) => ({ grinds: [entry({ id: 57, age_s: 2 + Math.floor(ms / 1000) })] });
    const r = simulatePage(stoppedScript, list, 60000);
    assert.equal(r.requests.length, 2, JSON.stringify(r.requests));
    assert.equal(r.maxInFlight, 1);
    assert.equal(r.track.result.state, 'unrecorded');
    const v = PV.lastGrindView(r.track, 60000);
    assert.equal(v.badge.label, 'Not recorded');
    assert.equal(v.result.visible, false);
    assert.equal(PV.lastGrindWaiting(r.track, 60000), false);
});

test('requests: a stop right after a completed grind never shows the completed grind as its result', () => {
    const known = loadedHistory([entry({ id: 57, age_s: 2 })], 500);
    let t = run(PV.newGrindTrack(), [0, 3, 6], 1000, { history: known });
    t = PV.grindTrackUpdate(t, readyStatus(), ctx(2500));
    for (const at of [2600, 4000, 9000, 20000]) {
        t = PV.grindTrackResolve(t, loadedHistory([entry({ id: 57, age_s: 2 + Math.floor((at - 500) / 1000) })], at), at);
    }
    assert.equal(t.result.state, 'unrecorded');
    assert.equal(PV.lastGrindView(t, 20000).result.visible, false);
});

test('requests: no grind at all (a dropped start) asks the history zero times and shows no card', () => {
    const dropped = Object.assign({}, PV.leadInStart(0, 3000, 0), { phase: 'dropped', droppedAt: 1000 });
    const r = simulatePage(readyStatus, () => ({ grinds: [] }), 60000, { leadIn: dropped });
    assert.equal(r.requests.length, 0);
    assert.equal(r.maxInFlight, 1);
    assert.equal(PV.lastGrindView(r.track, 60000).visible, false);
});

test('requests: a 404 on the history ends the asking after one request and the curve stays', () => {
    const r = simulatePage(normalScript, () => ({ err: 'not_supported' }), 60000);
    assert.equal(r.requests.length, 1);
    assert.equal(r.track.result.state, 'unavailable');
    const v = PV.lastGrindView(r.track, 60000);
    assert.equal(v.visible, true);
    assert.equal(v.spark.visible, true);
    assert.equal(v.result.visible, false);
});

test('requests: a busy grinder (503 ui_busy) is asked again after the retry delay, then settles', () => {
    let calls = 0;
    const list = (ms) => (calls++ < 2 ? { err: 'ui_busy' } : { grinds: [entry({ id: 58, age_s: Math.floor((ms - 7000) / 1000) })] });
    const r = simulatePage(normalScript, list, 60000);
    assert.equal(r.requests.length, 3, JSON.stringify(r.requests));
    r.requests.slice(1).forEach((at, i) => assert.ok(at - r.requests[i] >= PV.STATS_RETRY_MS, 'spacing ' + (at - r.requests[i])));
    assert.equal(r.track.result.state, 'recorded');
});

test('requests: a busy history that never recovers is given up after the wait', () => {
    const r = simulatePage(normalScript, () => ({ err: 'ui_busy' }), 120000);
    assert.ok(r.requests.length <= Math.ceil(PV.LAST_GRIND_WAIT_MS / PV.STATS_RETRY_MS) + 1, String(r.requests.length));
    assert.equal(PV.lastGrindView(r.track, 120000).message, 'The result could not be read from the grinder.');
});

test('sparkline: hidden at 0, 1 and 2 samples, shown from 3 (running and ended)', () => {
    [0, 1, 2].forEach((n) => {
        const t = run(PV.newGrindTrack(), [1, 2, 3].slice(0, n), 0);
        assert.equal(PV.lastGrindView(t, 5000).spark.visible, false, 'n=' + n);
        const ended = n === 0 ? t : PV.grindTrackUpdate(t, readyStatus(), ctx(5000));
        assert.equal(PV.lastGrindView(ended, 5000).spark.visible, false, 'ended n=' + n);
    });
    assert.equal(PV.lastGrindView(run(PV.newGrindTrack(), [1, 2, 3], 0), 5000).spark.visible, true);
});

test('the track: exactly 120 samples fit, the 121st drops the oldest, and the next grind resets the buffer', () => {
    let t = run(PV.newGrindTrack(), Array.from({ length: 120 }, (_, i) => i), 0);
    assert.equal(t.samples.length, 120);
    assert.equal(t.samples[0].g, 0);
    t = PV.grindTrackUpdate(t, grinding(120), ctx(120 * 500));
    assert.equal(t.samples.length, 120);
    assert.equal(t.samples[0].g, 1);
    assert.equal(t.samples[119].g, 120);
    t = PV.grindTrackUpdate(t, readyStatus(), ctx(70000));
    t = run(t, [3], 80000);
    assert.equal(t.samples.length, 1);
    assert.equal(t.samples[0].g, 3);
});
