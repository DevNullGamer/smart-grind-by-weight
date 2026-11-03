// View helpers of the web panel (pure functions, no DOM, no network), inlined by tools/portal/build_page.py and
// loaded unchanged by the node tests (tools/tests/portal-view.test.js). All logic that node can
// test lives here or in portal-client.js; portal-app.js only wires the DOM.
//
// Time is always passed in (nowMs); nothing here reads a clock or starts a timer. State objects are never
// changed in place: every transition returns a new object.
'use strict';

(function (root, factory) {
    if (typeof module !== 'undefined' && module.exports) {
        module.exports = factory(require('./portal-client.js'));
    } else {
        root.PortalView = factory(root.PortalClient);
    }
}(typeof self !== 'undefined' ? self : this, function (PC) {

    // The sections of the page in tab order. Insights stays hidden until the firmware has sent statistics.
    var TABS = ['grind', 'settings', 'insights'];

    var PROFILE_NAMES = ['Single', 'Double', 'Custom'];
    var PROFILE_KEYS = ['single', 'double', 'custom'];

    // Polling (docs: status every 1 s idle, 500 ms while busy or in a lead-in, 5 s while the web interface is off,
    // backoff after errors).
    var POLL_IDLE_MS = 1000;
    var POLL_BUSY_MS = 500;
    var POLL_WEB_OFF_MS = 5000;
    var BACKOFF_MS = [1500, 3000, 6000, 10000];
    // The first failure is "Reconnecting", from the second one on the chip says "Offline".
    var OFFLINE_AFTER_FAILURES = 2;

    // After the lead-in the grind should show up within this long, otherwise the grinder dropped the start.
    var LEAD_IN_GRACE_MS = 2000;
    var LEAD_IN_DROPPED_SHOW_MS = 8000;
    // The status that proves a drop must have been received at least this long after the lead-in ended.
    var LEAD_IN_STATUS_AFTER_MS = 1000;

    var REASON = {
        webOff: 'The web interface is switched off on the grinder.',
        offline: 'The grinder is not reachable.',
        connecting: 'Connecting to the grinder.',
        noStatus: 'Waiting for the grinder to answer.',
        update: 'A firmware update is running.',
        busy: 'A grind or motor run is active.',
        leadIn: 'A remote start is counting down. Press STOP to cancel it.',
        pending: 'Waiting for the grinder to answer the previous request.',
        notReady: 'Go back to the main screen on the grinder.'
    };

    // Reasons for the error codes the grinder reports in start_block / motor_test_block.
    var BLOCK_TEXT = {
        motor_disabled: 'The motor is disabled because the grinder has not confirmed which board it is. Check Menu, Hardware board on the grinder.',
        update_in_progress: REASON.update,
        load_cell_not_connected: 'The load cell is not connected.',
        load_cell_no_data: 'The load cell is not delivering data.',
        load_cell_rate_invalid: 'The load cell sample rate is invalid.',
        busy: REASON.busy,
        motor_fault: 'The motor has a fault that stays until the grinder is restarted.'
    };

    // The grinder's warnings (status.warnings): the wording belongs to the page. `level` picks the icon and the
    // label (error: the grinder cannot work properly; warning: it works but check it). The nine codes of
    // docs/PORTAL-API.md section 7, in the grinder's priority order.
    var WARNING_TEXT = {
        board_unrecognized: {
            level: 'error', title: 'Grinder board not recognised',
            action: 'The grinder cannot tell which hardware version it is, so the motor stays off. Choose the board on the grinder: Menu, Hardware board.'
        },
        motor_fault: {
            level: 'error', title: 'Motor fault',
            action: 'The motor output was cut off for safety and stays off until the grinder is restarted. Check the grinder, then restart it.'
        },
        motor_disabled: {
            level: 'error', title: 'Motor disabled',
            action: 'The motor output is off because the board is not confirmed or the grinder did not start up properly. Check Menu, Hardware board on the grinder, or restart it.'
        },
        hx711_not_connected: {
            level: 'error', title: 'Load cell not connected',
            action: 'The grinder cannot find the load cell amplifier. With the grinder switched off and unplugged, check its cable, then switch it on again.'
        },
        hx711_no_data: {
            level: 'error', title: 'Load cell sends no data',
            action: 'The load cell amplifier is connected but delivers no readings. With the grinder switched off and unplugged, check the wiring, then switch it on again.'
        },
        hx711_sample_rate_invalid: {
            level: 'error', title: 'Load cell sample rate is wrong',
            action: 'The load cell amplifier is wired for the wrong sample rate. With the grinder switched off and unplugged, check its rate pin wiring.'
        },
        mechanical_instability: {
            level: 'warning', title: 'Mechanical instability',
            action: 'The grinder noticed repeated odd movements while grinding. Check that the grinder and the scale sit firmly and that nothing touches the cup.'
        },
        load_cell_noisy: {
            level: 'warning', title: 'Load cell is noisy',
            action: 'The weight reading has been unsteady for over a minute. Keep the scale still, away from vibration, and check the cable.'
        },
        load_cell_not_calibrated: {
            level: 'warning', title: 'Scale not calibrated',
            action: 'Weights may be wrong. Calibrate the scale on the grinder: Menu, Tools, Calibrate.'
        }
    };
    var WARNING_LABEL = { error: 'Problem', warning: 'Check' };
    var WARNING_CODE_MAX = 40; // an unknown code from a later firmware is shown cut to this many characters

    var PHASE_TEXT = {
        initializing: 'Starting up',
        setup: 'Setting up',
        taring: 'Taring',
        tare_confirm: 'Confirm the tare on the grinder',
        predictive: 'Grinding to the target weight',
        pulse_decision: 'Checking the weight',
        pulse_execute: 'Fine-tuning pulse',
        pulse_settling: 'Letting the scale settle',
        final_settling: 'Final settling',
        time_grinding: 'Grinding for the set time',
        time_additional_pulse: 'Extra pulse',
        completed: 'Done',
        timeout: 'Timed out',
        prime: 'Priming',
        prime_settling: 'Settling after priming',
        purge_confirm: 'Waiting for a tap on the grinder (purge)'
    };

    var HINT_TEXT = {
        grind_complete: 'Waiting on the grinder screen: tap there to continue.',
        grind_timeout: 'Waiting on the grinder screen: tap there to continue.',
        purge_confirm: 'Waiting on the grinder screen: tap there so the grind can go on.'
    };

    function has(obj, key) {
        return Object.prototype.hasOwnProperty.call(obj, key);
    }

    // ------------------------------------------------------------------ formatters

    // A weight in grams for the big readout: one decimal, "--" while the grinder reports none.
    function formatWeight(grams) {
        if (typeof grams !== 'number' || !isFinite(grams)) return '-- g';
        return grams.toFixed(1) + ' g';
    }

    function formatSeconds(seconds) {
        if (typeof seconds !== 'number' || !isFinite(seconds)) return '-- s';
        return seconds.toFixed(1) + ' s';
    }

    // "N s", "N min" or "N h" for an age in milliseconds; negative values count as 0.
    function formatAge(ms) {
        if (typeof ms !== 'number' || !isFinite(ms)) return '';
        var s = Math.max(0, Math.floor(ms / 1000));
        if (s < 60) return s + ' s';
        if (s < 3600) return Math.floor(s / 60) + ' min';
        return Math.floor(s / 3600) + ' h';
    }

    // The words for a ui_state; an unknown state is shown as it came.
    function stateLabel(uiState) {
        if (has(PC.STATE_TEXT, uiState)) return PC.STATE_TEXT[uiState];
        return typeof uiState === 'string' && uiState ? uiState : '';
    }

    function phaseLabel(phase) {
        if (phase === 'idle') return '';
        if (has(PHASE_TEXT, phase)) return PHASE_TEXT[phase];
        return typeof phase === 'string' ? phase : '';
    }

    function blockText(code) {
        if (has(BLOCK_TEXT, code)) return BLOCK_TEXT[code];
        return 'The grinder cannot do this now (' + code + ').';
    }

    // Which tab to show for the tab the user picked. From 900 px the live pane sits beside the tabs, so the Grind tab
    // does not exist there and Settings takes its place; an unknown name also falls back to Grind or Settings.
    function tabForViewport(requested, isWide) {
        var name = TABS.indexOf(requested) >= 0 ? requested : 'grind';
        if (name === 'grind' && isWide) return 'settings';
        return name;
    }

    // ------------------------------------------------------------------ connection state machine

    function newConnection() {
        return { state: 'connecting', failures: 0, lastOkAt: null, lastError: '', uiBusy: false };
    }

    function copyConnection(conn) {
        return { state: conn.state, failures: conn.failures, lastOkAt: conn.lastOkAt, lastError: conn.lastError, uiBusy: conn.uiBusy };
    }

    // A reply arrived: online again, backoff forgotten.
    function connectionOk(conn, nowMs) {
        var next = copyConnection(conn);
        next.state = 'online';
        next.failures = 0;
        next.lastOkAt = nowMs;
        next.lastError = '';
        next.uiBusy = false;
        return next;
    }

    // A request failed with `code` (a PortalApiError code). web_interface_off and ui_busy are answers from a
    // reachable grinder; everything else counts as a lost connection.
    function connectionFailed(conn, code, nowMs) {
        var next = copyConnection(conn);
        if (code === 'web_interface_off') {
            next.state = 'web_off';
            next.failures = 0;
            next.lastError = code;
            next.uiBusy = false;
            return next;
        }
        if (code === 'ui_busy') {
            next.state = 'online';
            next.failures = 0;
            next.lastOkAt = nowMs;
            next.lastError = code;
            next.uiBusy = true;
            return next;
        }
        next.failures = conn.failures + 1;
        next.state = next.failures >= OFFLINE_AFTER_FAILURES ? 'offline' : 'reconnecting';
        next.lastError = typeof code === 'string' ? code : 'network';
        next.uiBusy = false;
        return next;
    }

    // Wait before the next retry after `failures` failures in a row: 1.5, 3, 6, then 10 s.
    function backoffDelay(failures) {
        if (!(failures > 0)) return 0;
        return BACKOFF_MS[Math.min(failures, BACKOFF_MS.length) - 1];
    }

    function leadInActive(leadIn) {
        return !!leadIn && (leadIn.phase === 'countdown' || leadIn.phase === 'watching');
    }

    // Delay before the next status request, or null while the page is hidden (polling pauses).
    function pollDelay(conn, status, leadInIsActive, hidden) {
        if (hidden) return null;
        if (conn.state === 'web_off') return POLL_WEB_OFF_MS;
        if (conn.failures > 0) return backoffDelay(conn.failures);
        if ((status && status.busy) || leadInIsActive) return POLL_BUSY_MS;
        return POLL_IDLE_MS;
    }

    // What the single polling loop requests next. The page makes one request at a time, so settings and info are
    // fetched in turns between status requests instead of beside them.
    // forceStatus: the last settings or info request failed, so status goes next and is never starved.
    function nextPollTask(flags) {
        if (!flags.haveStatus || !flags.online || flags.forceStatus) return 'status';
        if (flags.needSchema) return 'schema';
        if (flags.needSettings) return 'settings';
        if (flags.needInfo) return 'info';
        if (flags.statsDue) return 'stats'; // see statsDue: one request at a time, in turn with the status requests
        return 'status';
    }

    // A settings reply (GET or PATCH) the page can use: an object with a values object.
    function validSettingsDoc(doc) {
        return !!doc && typeof doc === 'object' && !!doc.values && typeof doc.values === 'object' && !Array.isArray(doc.values);
    }

    // The needs after a good reply to `task`. `needs`: {needSchema, needSettings, needInfo}. wasOnline / prevUi: the
    // connection state and ui_state before the reply. A settings reply that is not usable keeps needSettings and asks
    // for status next (forceStatus) so the retry cannot starve it. `quick`: more is still missing, ask again at once.
    function pollSucceeded(task, needs, doc, wasOnline, prevUi) {
        var next = { needSchema: needs.needSchema, needSettings: needs.needSettings, needInfo: needs.needInfo, forceStatus: false };
        if (!wasOnline) next.needSettings = true; // back after a gap: the grinder may have changed
        if (task === 'status') {
            if (prevUi && prevUi !== 'ready' && doc && doc.ui_state === 'ready') next.needSettings = true;
        } else if (task === 'schema') {
            next.needSchema = false;
        } else if (task === 'settings') {
            if (validSettingsDoc(doc)) next.needSettings = false;
            else next.forceStatus = true;
        } else if (task === 'info') {
            next.needInfo = false;
        } // 'stats' changes no need: the statistics keep their own state (statsLoaded)
        // an unusable settings reply must not be asked again at once (a firmware/page mismatch would loop)
        next.quick = !next.forceStatus && (next.needSchema || next.needSettings || next.needInfo);
        return next;
    }

    // What a failed request means: a 404 on the schema is an older firmware (reachable, no form), anything else is a
    // lost connection; a failing settings, schema or info request must not starve status.
    function pollFailed(task, code) {
        if (task === 'schema' && code === 'not_supported') return { schemaMissing: true, connectionLost: false, forceStatus: false };
        if (task === 'stats') {
            // The statistics are optional: a 404, a busy screen or an unreadable reply say nothing about the connection
            var lost = code !== 'not_supported' && code !== 'ui_busy' && code !== 'bad_reply' && code !== 'http_error';
            return { schemaMissing: false, connectionLost: lost, forceStatus: true };
        }
        return { schemaMissing: false, connectionLost: true, forceStatus: task !== 'status' };
    }

    // The first field with a message, in the order the form shows them (groups top to bottom), or ''.
    function firstInvalidKey(messages, fields) {
        var found = '';
        settingsGroups(fields).forEach(function (g) {
            g.fields.forEach(function (f) {
                if (!found && has(messages, f.key)) found = f.key;
            });
        });
        return found;
    }

    // Chip, explanation and whether a Retry button makes sense.
    function connectionView(conn, nowMs) {
        var seen = conn.lastOkAt === null ? '' : ' Last seen ' + formatAge(nowMs - conn.lastOkAt) + ' ago.';
        if (conn.state === 'online') {
            return {
                label: 'Online', icon: 'online', retry: false,
                detail: conn.uiBusy ? 'The grinder screen is busy right now. Trying again.' : ''
            };
        }
        if (conn.state === 'web_off') {
            return {
                label: 'Web interface off', icon: 'warning', retry: true,
                detail: 'The web interface is switched off on the grinder. Turn it on: Menu, WiFi, Web interface.'
            };
        }
        if (conn.state === 'reconnecting') {
            return { label: 'Reconnecting', icon: 'warning', retry: true, detail: 'Lost contact with the grinder. Trying again.' + seen };
        }
        if (conn.state === 'offline') {
            var detail;
            if (conn.lastError === 'bad_host' || conn.lastError === 'bad_origin') detail = PC.describeError(conn.lastError);
            else {
                detail = 'The grinder does not answer. Check that it is on and on the same WiFi. ' +
                    'It serves only two connections at a time, so another device using the panel can also block this page.';
            }
            return { label: 'Offline', icon: 'warning', retry: true, detail: detail + seen };
        }
        return { label: 'Connecting', icon: 'online', retry: false, detail: '' };
    }

    // ------------------------------------------------------------------ lead-in countdown

    // After an accepted start (202, lead_in_ms): count down, then watch for the grind to appear.
    function leadInStart(profileIndex, leadInMs, nowMs) {
        var ms = typeof leadInMs === 'number' && leadInMs >= 0 ? leadInMs : 3000;
        return { phase: 'countdown', profile: profileIndex, startedAt: nowMs, leadInMs: ms, droppedAt: null };
    }

    // True when the status shows a grind that is running or just finished (the lead-in itself is invisible).
    function grindVisible(status) {
        if (!status) return false;
        if (status.busy) return true;
        if (status.ui_state === 'grinding' || status.ui_state === 'purge_confirm') return true;
        return typeof status.grind_phase === 'string' && status.grind_phase !== 'idle';
    }

    // Advances the lead-in with the latest status. Returns the new lead-in, or null when it is over (the grind
    // appeared, or the dropped notice has been shown long enough). statusAtMs is when that status was received:
    // "dropped" needs a status that arrived at least LEAD_IN_STATUS_AFTER_MS after the lead-in ended, so a stale
    // status (hidden tab, lost connection) never counts as proof that no grind started.
    function leadInTick(leadIn, status, nowMs, statusAtMs) {
        if (!leadIn) return null;
        if (grindVisible(status)) return null;
        var over = leadIn.startedAt + leadIn.leadInMs;
        function make(phase, droppedAt) {
            return { phase: phase, profile: leadIn.profile, startedAt: leadIn.startedAt, leadInMs: leadIn.leadInMs, droppedAt: droppedAt };
        }
        if (leadIn.phase === 'dropped') {
            return nowMs - leadIn.droppedAt >= LEAD_IN_DROPPED_SHOW_MS ? null : leadIn;
        }
        var fresh = typeof statusAtMs === 'number' && statusAtMs >= over + LEAD_IN_STATUS_AFTER_MS;
        if (nowMs >= over + LEAD_IN_GRACE_MS && fresh) return make('dropped', nowMs);
        if (nowMs >= over) return leadIn.phase === 'watching' ? leadIn : make('watching', null);
        return leadIn;
    }

    // STOP accepted: the start is cancelled.
    function leadInCancel() {
        return null;
    }

    function profileName(index) {
        return PROFILE_NAMES[index] || 'Profile';
    }

    // Words for the lead-in card; `visible` is false when there is nothing to show.
    function leadInView(leadIn, nowMs) {
        if (!leadIn) return { visible: false, seconds: 0, text: '' };
        var name = profileName(leadIn.profile).toUpperCase();
        if (leadIn.phase === 'dropped') {
            return { visible: true, seconds: 0, dropped: true, text: 'The start was dropped on the grinder (check its screen).' };
        }
        if (leadIn.phase === 'watching') {
            return { visible: true, seconds: 0, text: 'Starting ' + name + ' now. Waiting for the grinder.' };
        }
        var remaining = Math.max(0, leadIn.startedAt + leadIn.leadInMs - nowMs);
        var seconds = Math.ceil(remaining / 1000);
        return {
            visible: true,
            seconds: seconds,
            text: 'Starting ' + name + ' in ' + seconds + ' s. Touch the grinder or press STOP to cancel.'
        };
    }

    // ------------------------------------------------------------------ controls and readout

    function isOnline(conn) {
        return !!conn && conn.state === 'online';
    }

    // The reason that blocks every action (not STOP), or '' when none does.
    function commonReason(status, conn, extra) {
        if (!conn || conn.state === 'connecting') return status ? '' : REASON.connecting;
        if (conn.state === 'web_off') return REASON.webOff;
        if (conn.state !== 'online') return REASON.offline;
        if (!status) return REASON.noStatus;
        if (status.ui_state === 'ota_update') return REASON.update;
        if (status.busy) return REASON.busy;
        if (extra && extra.leadIn) return REASON.leadIn;
        if (extra && extra.commandPending) return REASON.pending;
        return '';
    }

    function control(reason) {
        return { enabled: reason === '', reason: reason };
    }

    // Which controls are usable and why not. `extra`: {leadIn: lead-in is counting down or watching,
    // commandPending: a non-STOP command is on the wire}. STOP is always enabled and is never behind a reason; it
    // is emphasised while something runs. The settings do not change any flag here, so they are not a parameter.
    function deriveControls(status, conn, extra) {
        var common = commonReason(status, conn, extra);
        var ui = status ? status.ui_state : '';
        var startReason = common;
        var tareReason = common;
        var motorReason = common;
        if (!common && status) {
            if (ui !== 'ready') startReason = REASON.notReady;
            else if (status.start_block) startReason = blockText(status.start_block);
            if (ui !== 'ready') tareReason = REASON.notReady;
            else if (status.start_block === 'update_in_progress' || status.motor_test_block === 'update_in_progress') {
                tareReason = REASON.update;
            }
            if (ui !== 'ready' && ui !== 'menu') motorReason = REASON.notReady;
            else if (status.motor_test_block) motorReason = blockText(status.motor_test_block);
            var motorWarning = motorWarningBlock(status);
            if (motorWarning && !startReason) startReason = blockText(motorWarning);
            if (motorWarning && !motorReason) motorReason = blockText(motorWarning);
        }
        var emphasis = !!((status && status.busy) || (extra && extra.leadIn));
        return {
            stop: { enabled: true, emphasis: emphasis },
            start: control(startReason),
            tare: control(tareReason),
            motorTest: control(motorReason),
            banner: bannerFor(status, conn)
        };
    }

    // The warning codes of a status, as strings, each once, in the order received. An absent or malformed member
    // (an older firmware) is an empty list.
    function warningCodes(status) {
        var out = [];
        var list = status && Array.isArray(status.warnings) ? status.warnings : [];
        list.forEach(function (code) {
            if (typeof code === 'string' && code !== '' && out.indexOf(code) < 0) out.push(code);
        });
        return out;
    }

    // Which motor warning keeps Start and Motor test off, as a BLOCK_TEXT code, or '' (the grinder's own
    // start_block says the same on a current firmware; this keeps the page safe when only the warning is set).
    function motorWarningBlock(status) {
        var codes = warningCodes(status);
        if (codes.indexOf('motor_fault') >= 0) return 'motor_fault';
        if (codes.indexOf('motor_disabled') >= 0) return 'motor_disabled';
        return '';
    }

    // Persistent banner for update / motor problems, or null. `covers` lists the warning codes the banner already
    // says, so the warnings list does not repeat them.
    function bannerFor(status, conn) {
        if (!status || !isOnline(conn)) return null;
        var blocks = [status.start_block, status.motor_test_block];
        var codes = warningCodes(status);
        if (status.ui_state === 'ota_update' || blocks.indexOf('update_in_progress') >= 0) {
            return { level: 'warning', covers: [], text: 'Firmware update running. Everything except STOP is disabled until it is finished.' };
        }
        if (status.ui_state === 'ota_update_failed') {
            return { level: 'error', covers: [], text: 'The firmware update failed. See the grinder screen.' };
        }
        if (blocks.indexOf('motor_fault') >= 0 || codes.indexOf('motor_fault') >= 0) {
            return {
                level: 'error', covers: ['motor_fault', 'motor_disabled'], // a fault also raises motor_disabled; its board advice would mislead
                text: 'Motor fault. The motor output has a fault that stays until the grinder is restarted. Check the grinder, then restart it.'
            };
        }
        if (blocks.indexOf('motor_disabled') >= 0 || status.motor_available === false || codes.indexOf('motor_disabled') >= 0) {
            return { level: 'error', covers: ['motor_disabled'], text: 'Motor disabled. ' + BLOCK_TEXT.motor_disabled };
        }
        return null;
    }

    // Cut on code points so a surrogate pair is never split.
    function shortCode(code) {
        var points = Array.from(code);
        return points.length > WARNING_CODE_MAX ? points.slice(0, WARNING_CODE_MAX).join('') : code;
    }

    // One row of the warnings list for a code. A code this page does not know (a later firmware) gets generic text
    // with the code itself, cut to WARNING_CODE_MAX characters.
    function warningRow(code) {
        if (has(WARNING_TEXT, code)) {
            var t = WARNING_TEXT[code];
            return { code: code, level: t.level, icon: t.level, label: WARNING_LABEL[t.level], title: t.title, action: t.action };
        }
        return {
            code: shortCode(code), level: 'warning', icon: 'warning', label: WARNING_LABEL.warning,
            title: 'Other warning: ' + shortCode(code),
            action: 'The grinder reports a warning this page does not know. Check the grinder screen.'
        };
    }

    // The warnings card: rows in the order the grinder sent them (it sends them by priority), each code once.
    // Hidden while empty, while the connection is not online, and for a code the banner already shows.
    function warningsView(status, conn) {
        if (!status || !isOnline(conn)) return { visible: false, rows: [] };
        var banner = bannerFor(status, conn);
        var covered = banner ? banner.covers : [];
        var rows = warningCodes(status).filter(function (code) {
            return covered.indexOf(code) < 0;
        }).map(warningRow).filter(function (row, i, all) {
            // unknown codes cut to the same text show once
            return all.findIndex(function (r) { return r.code === row.code; }) === i;
        });
        return { visible: rows.length > 0, rows: rows };
    }

    // Texts of the readout card. While the connection is lost the last values stay, marked stale.
    function readoutView(status, conn, nowMs) {
        var stale = !!conn && (conn.state === 'offline' || conn.state === 'reconnecting');
        var seen = '';
        if (stale && conn.lastOkAt !== null) seen = 'Last seen ' + formatAge(nowMs - conn.lastOkAt) + ' ago';
        if (!status) {
            var label = 'Connecting';
            if (conn && conn.state === 'offline') label = 'Offline';
            else if (conn && conn.state === 'reconnecting') label = 'Reconnecting';
            else if (conn && conn.state === 'web_off') label = 'Web interface off';
            return { state: label, weight: formatWeight(null), phase: '', hint: '', stale: stale, seen: seen };
        }
        return {
            state: stateLabel(status.ui_state),
            weight: formatWeight(status.weight_g),
            phase: phaseLabel(status.grind_phase),
            hint: has(HINT_TEXT, status.ui_state) ? HINT_TEXT[status.ui_state] : '',
            stale: stale,
            seen: seen
        };
    }

    // The three profile cards: name, the target of the current grind mode, and whether the grinder has it selected.
    function profileCards(settings) {
        var values = settings && settings.values ? settings.values : null;
        var timeMode = !!values && values.grind_mode === 'time';
        return PROFILE_KEYS.map(function (key, i) {
            var target = '--';
            if (values) {
                var v = values[key + (timeMode ? '_time_s' : '_weight_g')];
                target = timeMode ? formatSeconds(v) : formatWeight(v);
            }
            return {
                index: i,
                name: PROFILE_NAMES[i],
                target: target,
                selected: !!settings && settings.current_profile === i
            };
        });
    }

    // ------------------------------------------------------------------ Insights: lifetime statistics (chg016 step 9)
    // GET /api/stats (docs/PORTAL-API.md section 7). The page keeps one small state object:
    // {doc: parsed statistics or null, fetchedAt, seen: a reply with data has arrived, unavailable: the route is
    // missing (404), error: words for the latest failure, retryAt, refreshAt / followUpAt: asked-for fetch times}.
    // The Insights tab shows only once data has arrived, so an older firmware never gets an empty tab.

    var STATS_PERIOD_MS = 30000;   // refresh while the tab is shown
    var STATS_RETRY_MS = 10000;    // after a failed request
    var STATS_FOLLOW_UP_MS = 6000; // the grinder republishes its statistics every 5 s: look again after a grind ended
    var STATS_KEYS = ['total_grinds', 'single', 'double', 'custom', 'weight_mode', 'time_mode', 'total_weight_kg',
        'motor_runtime_s', 'avg_abs_error_g', 'total_pulses', 'avg_pulses', 'time_pulses', 'powered_minutes'];
    var STATS_NOT_AVAILABLE = 'Statistics not available. This grinder firmware does not provide them yet.';

    function newStats() {
        return { doc: null, fetchedAt: null, seen: false, unavailable: false, error: '', retryAt: 0, refreshAt: null, followUpAt: null };
    }

    function copyStats(s) {
        return { doc: s.doc, fetchedAt: s.fetchedAt, seen: s.seen, unavailable: s.unavailable, error: s.error,
            retryAt: s.retryAt, refreshAt: s.refreshAt, followUpAt: s.followUpAt };
    }

    function statNumber(v) {
        return typeof v === 'number' && isFinite(v) && v >= 0 ? v : null;
    }

    // The members of a statistics reply as numbers; a member that is missing or not a plain non-negative number is
    // null ("unknown", never 0). A reply that is no object, or has no usable total_grinds, is not a statistics reply.
    function parseStats(doc) {
        if (!doc || typeof doc !== 'object' || Array.isArray(doc)) return null;
        var out = {};
        STATS_KEYS.forEach(function (key) { out[key] = statNumber(doc[key]); });
        return out.total_grinds === null ? null : out;
    }

    function statsLoaded(s, doc, nowMs) {
        var parsed = parseStats(doc);
        if (!parsed) return statsFailed(s, 'bad_reply', nowMs);
        var next = copyStats(s);
        next.doc = parsed;
        next.fetchedAt = nowMs;
        next.seen = true;
        next.unavailable = false;
        next.error = '';
        next.retryAt = 0;
        next.refreshAt = null;
        if (next.followUpAt !== null && nowMs >= next.followUpAt) next.followUpAt = null;
        return next;
    }

    // A failed request. 404 means the firmware has no such route: stop asking. Anything else is retried later.
    function statsFailed(s, code, nowMs) {
        var next = copyStats(s);
        next.refreshAt = null;
        if (next.followUpAt !== null && nowMs >= next.followUpAt) next.followUpAt = null;
        if (code === 'not_supported') {
            next.unavailable = true;
            next.error = '';
            return next;
        }
        next.error = PC.describeError(code);
        next.retryAt = nowMs + STATS_RETRY_MS;
        return next;
    }

    // Ask for a fetch at nowMs + afterMs (the earlier request wins).
    function statsRefresh(s, afterMs, nowMs) {
        var next = copyStats(s);
        var at = nowMs + afterMs;
        if (next.refreshAt === null || at < next.refreshAt) next.refreshAt = at;
        return next;
    }

    // The Insights tab was opened: fetch now.
    function statsShown(s, nowMs) {
        return statsRefresh(s, 0, nowMs);
    }

    // The screen state of the grinder changed: a grind that just ended changes the totals. Fetch now and once more
    // after the grinder's own refresh. ui_state values: 'grinding' and 'grind_complete' are the grind screens.
    function grindEnded(prevUi, nextUi) {
        var grindScreens = ['grinding', 'grind_complete'];
        return grindScreens.indexOf(prevUi) >= 0 && nextUi !== prevUi;
    }

    function statsOnStatus(s, prevUi, nextUi, nowMs) {
        if (!grindEnded(prevUi, nextUi)) return s;
        var next = statsRefresh(s, 0, nowMs);
        next.followUpAt = nowMs + STATS_FOLLOW_UP_MS;
        return next;
    }

    // Whether the polling loop should fetch the statistics now. ctx: {online, haveStatus, hidden, tabShown,
    // commandBusy, nowMs}. Never while a command is on the wire (the grinder serves two connections), never after a
    // 404, and not before the status is known. Until data has arrived the page probes once (a failed probe waits for
    // its retry time); once the tab is shown it refreshes on request and every STATS_PERIOD_MS.
    function statsDue(s, ctx) {
        if (s.unavailable || ctx.hidden || !ctx.online || !ctx.haveStatus || ctx.commandBusy) return false;
        if (ctx.nowMs < s.retryAt) return false;
        if (!s.seen) return true;
        var asked = (s.refreshAt !== null && ctx.nowMs >= s.refreshAt) || (s.followUpAt !== null && ctx.nowMs >= s.followUpAt);
        if (!ctx.tabShown) return false;
        return asked || s.fetchedAt === null || ctx.nowMs - s.fetchedAt >= STATS_PERIOD_MS;
    }

    // Whole numbers with a comma every three digits ("4,294,967,295"). Not locale based: the page is English and the
    // result must not depend on the browser.
    function formatCount(n) {
        if (typeof n !== 'number' || !isFinite(n) || n < 0) return '--';
        var digits = String(Math.floor(n));
        if (/e/.test(digits)) return '--';
        return digits.replace(/\B(?=(\d{3})+(?!\d))/g, ',');
    }

    function pad2(n) {
        return n < 10 ? '0' + n : String(n);
    }

    // "59 s", "12 min 5 s", "1 h 02 min", "3 d 4 h"; 0 is "0 s".
    function formatDuration(seconds) {
        if (typeof seconds !== 'number' || !isFinite(seconds) || seconds < 0) return '--';
        var s = Math.floor(seconds);
        if (s < 60) return s + ' s';
        if (s < 3600) {
            var rest = s % 60;
            return Math.floor(s / 60) + ' min' + (rest ? ' ' + rest + ' s' : '');
        }
        if (s < 86400) return Math.floor(s / 3600) + ' h ' + pad2(Math.floor((s % 3600) / 60)) + ' min';
        return formatCount(Math.floor(s / 86400)) + ' d ' + Math.floor((s % 86400) / 3600) + ' h';
    }

    // Total ground weight: grams below 1 kg, then kg with two decimals below 10, one below 1000, whole kg above.
    // The unit is chosen on the rounded value, so 0.9996 is "1.00 kg" (not "1000 g") and 9.999 is "10.0 kg".
    function formatKg(kg) {
        if (typeof kg !== 'number' || !isFinite(kg) || kg < 0) return '--';
        var grams = Math.round(kg * 1000);
        if (grams < 1000) return grams + ' g';
        if (Math.round(kg * 100) < 1000) return kg.toFixed(2) + ' kg';
        if (Math.round(kg * 10) < 10000) return kg.toFixed(1) + ' kg';
        return formatCount(Math.round(kg)) + ' kg';
    }

    function statText(value) {
        return value === null ? '--' : formatCount(value);
    }

    // The three weight buckets of the grinder's statistics (final weight, not the profile that was used).
    var MIX_BUCKETS = [
        { key: 'single', label: 'Up to 10 g' },
        { key: 'double', label: 'Over 10 g, up to 22 g' },
        { key: 'custom', label: 'Over 22 g' }
    ];

    // Whole percentages that add up to exactly 100 (largest remainder; the first bucket wins a tie). total > 0.
    function percentages(counts, total) {
        var pcts = counts.map(function (c) { return Math.floor(c * 100 / total); });
        var left = 100 - pcts.reduce(function (a, b) { return a + b; }, 0);
        var order = counts.map(function (c, i) { return i; }).sort(function (a, b) {
            return ((counts[b] * 100) % total) - ((counts[a] * 100) % total) || a - b;
        });
        for (var k = 0; k < left; k++) pcts[order[k]] += 1;
        return pcts;
    }

    function percentText(count, pct) {
        if (count > 0 && pct === 0) return 'under 1 %';
        return pct + ' %';
    }

    function mixView(doc) {
        var counts = MIX_BUCKETS.map(function (b) { return doc[b.key]; });
        var known = counts.every(function (c) { return c !== null; });
        var total = known ? counts[0] + counts[1] + counts[2] : 0;
        if (!known || total <= 0) return { visible: false, segments: [] };
        var pcts = percentages(counts, total);
        return {
            visible: true,
            segments: MIX_BUCKETS.map(function (b, i) {
                // drawn: a bucket without grinds gets no bar segment (it would only draw a divider), but keeps its row
                return { key: b.key, label: b.label, count: counts[i], flex: counts[i], drawn: counts[i] > 0,
                    text: formatCount(counts[i]) + (counts[i] === 1 ? ' grind' : ' grinds') + ' (' + percentText(counts[i], pcts[i]) + ')' };
            })
        };
    }

    function tilesFor(doc) {
        var noError = doc.weight_mode === null || doc.weight_mode === 0 || doc.avg_abs_error_g === null;
        var noPulses = doc.total_grinds === 0 || doc.avg_pulses === null;
        return [
            { id: 'grinds', label: 'Grinds', value: statText(doc.total_grinds),
                detail: statText(doc.weight_mode) + ' by weight, ' + statText(doc.time_mode) + ' by time' },
            { id: 'coffee', label: 'Coffee ground', value: formatKg(doc.total_weight_kg), detail: 'Total weight of finished grinds' },
            { id: 'motor', label: 'Motor run time', value: doc.motor_runtime_s === null ? '--' : formatDuration(doc.motor_runtime_s),
                detail: 'Grinding, pulses and motor tests' },
            { id: 'powered', label: 'Powered on', value: doc.powered_minutes === null ? '--' : formatDuration(doc.powered_minutes * 60),
                detail: 'Total time the grinder has been switched on' },
            { id: 'error', label: 'Average error', value: noError ? '--' : '\u00b1' + (doc.avg_abs_error_g < 1000 ? doc.avg_abs_error_g.toFixed(2) : formatCount(Math.round(doc.avg_abs_error_g))) + ' g',
                detail: noError ? 'No weight-mode grinds yet' : 'Final weight against the target, weight mode' },
            { id: 'pulses', label: 'Average pulses', value: noPulses ? '--' : doc.avg_pulses.toFixed(1),
                detail: doc.total_pulses === null ? 'Correction pulses per grind' : statText(doc.total_pulses) + ' correction pulses in total' },
            { id: 'timepulses', label: 'Extra time-mode pulses', value: statText(doc.time_pulses), detail: 'Added after time-mode grinds' }
        ];
    }

    // What the Insights tab shows. kind: unavailable | loading | error | empty | ready. `message` is the sentence
    // above the tiles ('' = none); `note` says how old the numbers are or why they could not be refreshed.
    function statsView(s, conn, nowMs) {
        var online = !!conn && conn.state === 'online';
        var view = { tabVisible: s.seen && !s.unavailable, kind: 'ready', message: '', note: '', tiles: [], mix: { visible: false, segments: [] } };
        if (s.unavailable) {
            view.kind = 'unavailable';
            view.message = STATS_NOT_AVAILABLE;
            return view;
        }
        if (!s.doc) {
            if (!online && conn && conn.state !== 'connecting') {
                view.kind = 'error';
                view.message = 'The grinder is not reachable, so the statistics cannot be read. This page tries again by itself.';
            } else if (s.error) {
                view.kind = 'error';
                view.message = 'The statistics could not be read. ' + s.error + ' This page tries again by itself.';
            } else {
                view.kind = 'loading';
                view.message = 'Loading the statistics.';
            }
            return view;
        }
        var age = formatAge(nowMs - s.fetchedAt);
        if (!online) view.note = 'The grinder is not reachable. These numbers are from ' + age + ' ago.';
        else if (s.error) view.note = 'The numbers could not be refreshed (' + s.error + ') and are from ' + age + ' ago.';
        else view.note = 'Updated ' + age + ' ago.';
        if (s.doc.total_grinds === 0) {
            view.kind = 'empty';
            view.message = 'No grinds recorded yet. The totals appear after the first grind.';
            return view;
        }
        view.tiles = tilesFor(s.doc);
        view.mix = mixView(s.doc);
        return view;
    }

    // ------------------------------------------------------------------ settings form (chg016 step 3)
    // The form is driven by GET /api/schema. Groups come from the key table below; a key the table does not know
    // lands in "Other", so a firmware that adds a field never hides it. The form object is
    // {stored: values as the grinder holds them, draft: what the user has entered (raw: numbers may be text)}.

    var SETTINGS_GROUPS = [
        {
            id: 'profiles', title: 'Profiles',
            hint: 'Targets of the Start buttons. The grinder uses the weight in weight mode and the time in time mode.',
            keys: ['single_weight_g', 'single_time_s', 'double_weight_g', 'double_time_s', 'custom_weight_g', 'custom_time_s']
        },
        { id: 'display', title: 'Display', hint: '', keys: ['brightness_normal', 'brightness_screensaver'] },
        {
            id: 'grinding', title: 'Grinding', hint: '',
            keys: ['grind_mode', 'purge_mode', 'purge_amount_g', 'freshness_hours', 'auto_start', 'auto_return', 'swipe']
        },
        { id: 'device', title: 'Device', hint: '', keys: ['ble_startup', 'logging', 'device_name'] }
    ];
    var OTHER_GROUP = { id: 'other', title: 'Other', hint: '' };

    // How a key is shown; anything not listed follows its type (bool: switch, enum: segmented, string: text,
    // number: stepper).
    var WIDGET_BY_KEY = {
        brightness_normal: 'slider',
        brightness_screensaver: 'slider',
        purge_amount_g: 'slider',
        freshness_hours: 'select'
    };

    // The nine steps of the freshness setting (hours), as in the grinder menu.
    var FRESHNESS_STEPS = [0.5, 1, 2, 3, 4, 8, 12, 24, 48];

    var CHOICE_TEXT = {
        purge_mode: { keep: 'Prime (keep)', remove: 'Purge (remove)' },
        grind_mode: { weight: 'Weight', time: 'Time' }
    };

    // Shorter labels where the control itself explains the choices.
    var LABEL_OVERRIDE = { purge_mode: 'Purge mode', device_name: 'Device name' };

    var FIELD_HINT = {
        device_name: 'The grinder answers at this name followed by .local. 1 to 24 characters: lowercase a-z, 0-9 and hyphen, ' +
            'no hyphen at the start or end. Changing it means opening this page again at the new address.'
    };

    function capitalise(text) {
        var s = String(text);
        return s.charAt(0).toUpperCase() + s.slice(1);
    }

    // A limit or step for display: whole numbers without a decimal ("5", "1000"), others as the API writes them.
    function formatLimit(n) {
        if (typeof n !== 'number' || !isFinite(n)) return '';
        return Math.floor(n) === n ? String(n) : PC.formatNumber(n);
    }

    function unitSuffix(field) {
        return field && field.unit ? ' ' + field.unit : '';
    }

    // "5 to 1000 g" for a field with limits, '' otherwise.
    function rangeText(field) {
        if (!field || typeof field.min !== 'number' || typeof field.max !== 'number') return '';
        if (field.type === 'string' || field.type === 'bool' || field.type === 'enum') return '';
        return formatLimit(field.min) + ' to ' + formatLimit(field.max) + unitSuffix(field);
    }

    function freshnessLabel(hours) {
        if (hours < 1) return Math.round(hours * 60) + ' minutes';
        return hours === 1 ? '1 hour' : formatLimit(hours) + ' hours';
    }

    // The options of the freshness select; a stored value off the nine steps is added so it stays visible.
    function freshnessOptions(current) {
        var steps = FRESHNESS_STEPS.slice();
        if (typeof current === 'number' && isFinite(current) && steps.indexOf(current) < 0) {
            steps.push(current);
            steps.sort(function (a, b) { return a - b; });
        }
        return steps.map(function (h) { return { value: h, label: freshnessLabel(h) }; });
    }

    function widgetKind(field) {
        if (has(WIDGET_BY_KEY, field.key)) return WIDGET_BY_KEY[field.key];
        if (field.type === 'bool') return 'switch';
        if (field.type === 'enum') return 'segmented';
        if (field.type === 'string') return 'text';
        return 'stepper';
    }

    // Everything the DOM glue needs to build one control.
    function fieldView(field) {
        var kind = widgetKind(field);
        var choices = [];
        if (field.type === 'enum' && field.choices) {
            choices = field.choices.map(function (c) {
                var text = has(CHOICE_TEXT, field.key) && has(CHOICE_TEXT[field.key], c) ? CHOICE_TEXT[field.key][c] : capitalise(c);
                return { value: c, label: text };
            });
        }
        return {
            key: field.key,
            label: has(LABEL_OVERRIDE, field.key) ? LABEL_OVERRIDE[field.key] : capitalise(PC.fieldLabel(field.key)),
            type: field.type,
            kind: kind,
            unit: field.unit || '',
            min: typeof field.min === 'number' ? field.min : null,
            max: typeof field.max === 'number' ? field.max : null,
            step: stepFor(field),
            choices: choices,
            hint: has(FIELD_HINT, field.key) ? FIELD_HINT[field.key] : '',
            range: rangeText(field)
        };
    }

    // The sections of the form for the fields of the schema, in display order. Empty sections are left out and
    // every field appears exactly once (unknown keys in "Other").
    function settingsGroups(fields) {
        var byKey = {};
        var used = {};
        (fields || []).forEach(function (f) { byKey[f.key] = f; });
        var out = [];
        SETTINGS_GROUPS.forEach(function (g) {
            var list = [];
            g.keys.forEach(function (key) {
                if (has(byKey, key) && !has(used, key)) {
                    used[key] = true;
                    list.push(fieldView(byKey[key]));
                }
            });
            if (list.length) out.push({ id: g.id, title: g.title, hint: g.hint, fields: list });
        });
        var rest = [];
        (fields || []).forEach(function (f) {
            if (!has(used, f.key)) {
                used[f.key] = true;
                rest.push(fieldView(f));
            }
        });
        if (rest.length) out.push({ id: OTHER_GROUP.id, title: OTHER_GROUP.title, hint: OTHER_GROUP.hint, fields: rest });
        return out;
    }

    // The keys of the key table (for the static check that the table and the schema stay in step).
    function groupedKeys() {
        var keys = [];
        SETTINGS_GROUPS.forEach(function (g) { keys = keys.concat(g.keys); });
        return keys;
    }

    function currentProfileText(settings) {
        if (!settings || typeof settings.current_profile !== 'number') return '';
        var name = PROFILE_NAMES[settings.current_profile];
        return name ? 'Selected on the grinder: ' + name + '. This is shown for information; it cannot be changed here.' : '';
    }

    // --- entries

    // Text to a number: "9.5" and "9,5" (one comma, no dot) are numbers, anything else is not. Never NaN.
    function parseNumberText(text) {
        var s = String(text).replace(/^\s+|\s+$/g, '');
        if (/^[^.,]*,[^.,]*$/.test(s)) s = s.replace(',', '.');
        if (!/^-?(\d+\.?\d*|\.\d+)$/.test(s)) return undefined;
        var n = parseFloat(s);
        return isFinite(n) ? n : undefined;
    }

    // The value to send for a draft entry, in the type the API wants, or undefined for "not a usable entry"
    // (an empty or unreadable number): such a key is left out of the PATCH.
    function entryValue(field, raw) {
        if (field.type === 'bool') return typeof raw === 'boolean' ? raw : undefined;
        if (field.type === 'enum' || field.type === 'string') return typeof raw === 'string' ? raw : undefined;
        if (typeof raw === 'number') return isFinite(raw) ? raw : undefined;
        if (typeof raw === 'string') return parseNumberText(raw);
        return undefined;
    }

    // The text for a number input from a stored or stepped value.
    function draftText(field, raw) {
        if (typeof raw === 'number') return field.type === 'int' ? String(raw) : PC.formatNumber(raw);
        return typeof raw === 'string' ? raw : '';
    }

    function isNumeric(field) {
        return field.type === 'int' || field.type === 'number';
    }

    function copyScalars(values) {
        var out = {};
        Object.keys(values || {}).forEach(function (key) {
            var v = values[key];
            if (typeof v === 'boolean' || typeof v === 'number' || typeof v === 'string') out[key] = v;
        });
        return out;
    }

    function formNew(values) {
        return { stored: copyScalars(values), draft: copyScalars(values) };
    }

    function formSet(form, key, raw) {
        var draft = copyScalars(form.draft);
        draft[key] = raw;
        return { stored: form.stored, draft: draft };
    }

    function formDiscard(form) {
        return { stored: form.stored, draft: copyScalars(form.stored) };
    }

    // The body of a PATCH: only the keys whose entry differs from the stored value, in schema order. A key with
    // no usable entry is not in it. `current_profile` is not a field and can never be in it.
    function formPatch(form, fields) {
        var entered = {};
        (fields || []).forEach(function (f) {
            if (has(form.draft, f.key)) entered[f.key] = entryValue(f, form.draft[f.key]);
        });
        return PC.diffValues(form.stored, entered);
    }

    function formChangeCount(form, fields) {
        return Object.keys(formPatch(form, fields)).length;
    }

    function changeCountText(count) {
        return count === 1 ? '1 change' : count + ' changes';
    }

    // Hints for numeric entries the page cannot read (they stay out of the PATCH).
    function entryNotes(form, fields) {
        var notes = {};
        (fields || []).forEach(function (f) {
            if (isNumeric(f) && has(form.draft, f.key) && entryValue(f, form.draft[f.key]) === undefined) {
                notes[f.key] = 'Not a number. This setting stays as it is.';
            }
        });
        return notes;
    }

    // After a successful PATCH: the grinder's applied values become the stored ones. A saved key shows what the
    // grinder applied (it may have clamped it); an entry that was not part of the PATCH stays as typed.
    // Keys the user did not edit follow the reply (formRefresh rule), so no change appears that nobody made.
    function formSaved(form, appliedValues, patch, fields) {
        var next = formRefresh(form, appliedValues, fields).form;
        Object.keys(patch || {}).forEach(function (key) {
            if (has(next.stored, key)) next.draft[key] = next.stored[key];
        });
        return next;
    }

    // Fresh settings from the grinder. Fields the user has edited keep their entry; the others take the new value.
    // `notice` is non-empty when edits were kept while the grinder reports other values than before.
    function formRefresh(form, values, fields) {
        var byKey = {};
        (fields || []).forEach(function (f) { byKey[f.key] = f; });
        var fresh = copyScalars(values);
        var draft = {};
        var kept = false;
        Object.keys(fresh).forEach(function (key) {
            var raw = form.draft[key];
            var keep = false;
            var same = false; // the entry already equals the new value: keep the text as typed ('9' stays '9')
            if (has(byKey, key) && has(form.draft, key)) {
                var entry = entryValue(byKey[key], raw);
                same = entry !== undefined && entry === fresh[key];
                keep = entry === undefined || entry !== form.stored[key];
            }
            if (keep && !same) kept = true;
            draft[key] = keep || same ? raw : fresh[key];
        });
        var changed = Object.keys(fresh).filter(function (key) { return form.stored[key] !== fresh[key]; });
        var notice = '';
        if (kept && changed.length) {
            notice = 'The grinder reports new values for ' + changed.map(function (k) { return PC.fieldLabel(k); }).join(', ') +
                '. Your unsaved edits are kept; Discard loads the grinder\'s values.';
        }
        return { form: { stored: fresh, draft: draft }, notice: notice, changed: changed };
    }

    // --- steppers (no float drift: arithmetic on thousandths)

    function stepFor(field) {
        if (typeof field.step === 'number' && field.step > 0) return field.step;
        if (field.unit === 's') return 0.5;
        if (field.unit === 'g') return 0.1;
        return 1;
    }

    // The value after one press of minus (-1) or plus (+1). A blank or unreadable entry starts from `fallback`.
    // The result stays inside the limits; typing is not limited, so the grinder can still say no.
    function stepValue(raw, direction, field, fallback) {
        var current = entryValue(field, raw);
        if (current === undefined) current = typeof fallback === 'number' ? fallback : (typeof field.min === 'number' ? field.min : 0);
        var step = Math.round(stepFor(field) * 1000);
        var next = (Math.round(current * 1000) + (direction < 0 ? -step : step)) / 1000;
        if (typeof field.min === 'number' && next < field.min) next = field.min;
        if (typeof field.max === 'number' && next > field.max) next = field.max;
        return next;
    }

    // --- saving

    // Why Save cannot be used right now ('' when it can): the same connection and busy reasons as the actions, and
    // writes also need the main screen or a menu page (never an edit screen, dialog or calibration).
    function saveReason(status, conn, extra) {
        var common = commonReason(status, conn, extra);
        if (common) return common;
        if (status && (status.start_block === 'update_in_progress' || status.motor_test_block === 'update_in_progress')) {
            return REASON.update;
        }
        if (status && status.ui_state !== 'ready' && status.ui_state !== 'menu') return REASON.notReady;
        return '';
    }

    function saveBarView(form, fields, status, conn, extra) {
        var count = form ? formChangeCount(form, fields) : 0;
        var reason = count > 0 ? saveReason(status, conn, extra) : '';
        return {
            visible: count > 0,
            count: count,
            countText: changeCountText(count),
            canSave: count > 0 && reason === '',
            reason: reason
        };
    }

    // Words for the keys the grinder clamped or snapped, per key, shown beside the field.
    function adjustedNotes(adjusted, values, fields) {
        var byKey = {};
        (fields || []).forEach(function (f) { byKey[f.key] = f; });
        var notes = {};
        (adjusted || []).forEach(function (key) {
            var v = values ? values[key] : undefined;
            var f = has(byKey, key) ? byKey[key] : {};
            var whole = typeof v === 'number' && Math.floor(v) === v && (f.type === 'int' || f.unit === 'h');
            var shown = typeof v === 'number' ? (whole ? String(v) : PC.formatNumber(v)) : String(v);
            notes[key] = 'The grinder adjusted this to ' + shown + unitSuffix(f) + '.';
        });
        return notes;
    }

    // What to show for a refused PATCH: text beside fields where the page can tell which one, and a sentence for the
    // whole form. `patch` is what was sent. `reload` asks for a fresh read of the settings.
    function saveErrorView(code, patch, fields) {
        var byKey = {};
        (fields || []).forEach(function (f) { byKey[f.key] = f; });
        var keys = Object.keys(patch || {});
        var out = { fields: {}, form: PC.describeError(code) + ' Your edits are kept.', reload: false };
        // the firmware also answers malformed_json for a device name it cannot read (non-ASCII, backslash)
        if ((code === 'invalid_device_name' || code === 'malformed_json') && has(patch || {}, 'device_name')) {
            out.fields.device_name = PC.describeError('invalid_device_name');
            out.form = 'Nothing was saved. Fix the marked field and try again.';
        } else if (code === 'out_of_range') {
            keys.forEach(function (key) {
                var f = byKey[key];
                if (f && isNumeric(f) && typeof f.min === 'number' && typeof f.max === 'number' &&
                        (patch[key] < f.min || patch[key] > f.max)) {
                    out.fields[key] = 'Outside the allowed range (' + rangeText(f) + ').';
                }
            });
            if (Object.keys(out.fields).length) out.form = 'Nothing was saved. Fix the marked field and try again.';
        } else if ((code === 'wrong_type' || code === 'invalid_value') && keys.length === 1) {
            out.fields[keys[0]] = PC.describeError(code);
            out.form = 'Nothing was saved. Fix the marked field and try again.';
        } else if (code === 'ui_busy' || code === 'store_failed') {
            out.form = PC.describeError(code);
            out.reload = true;
        } else if (code === 'network' || code === 'timeout') {
            // the grinder may have stored the values before the reply was lost: read them back
            out.form = PC.describeError(code) + ' The change may or may not have been saved; the page checks. Your edits are kept.';
            out.reload = true;
        }
        return out;
    }

    // Rename: the sentence for the reply's `address`, '' when it is not a plain .local name.
    function renameNotice(address) {
        if (!/^[a-z0-9-]{1,63}\.local$/.test(String(address))) return '';
        return 'The grinder is now called ' + address + '. Open this page again at that address; the old name stops ' +
            'answering. The IP address keeps working.';
    }

    // Link target for the new address, built from how this page was opened ('' when that is not plain http).
    function renameLink(address, protocol, port) {
        if (renameNotice(address) === '' || protocol !== 'http:') return '';
        return protocol + '//' + address + (port ? ':' + port : '') + '/';
    }

    // What the settings area says while the form cannot be shown yet.
    function settingsPlaceholder(haveSchema, haveSettings, conn, schemaMissing) {
        if (haveSchema && haveSettings) return '';
        if (schemaMissing) return 'This grinder firmware does not offer the settings form yet. Update the firmware.';
        if (conn && conn.state === 'web_off') return REASON.webOff;
        if (conn && (conn.state === 'offline' || conn.state === 'reconnecting')) return REASON.offline;
        return 'Loading the settings.';
    }

    return {
        TABS: TABS,
        PROFILE_NAMES: PROFILE_NAMES,
        BACKOFF_MS: BACKOFF_MS,
        POLL_IDLE_MS: POLL_IDLE_MS,
        POLL_BUSY_MS: POLL_BUSY_MS,
        POLL_WEB_OFF_MS: POLL_WEB_OFF_MS,
        LEAD_IN_GRACE_MS: LEAD_IN_GRACE_MS,
        LEAD_IN_DROPPED_SHOW_MS: LEAD_IN_DROPPED_SHOW_MS,
        LEAD_IN_STATUS_AFTER_MS: LEAD_IN_STATUS_AFTER_MS,
        OFFLINE_AFTER_FAILURES: OFFLINE_AFTER_FAILURES,
        REASON: REASON,
        tabForViewport: tabForViewport,
        formatWeight: formatWeight,
        formatSeconds: formatSeconds,
        formatAge: formatAge,
        stateLabel: stateLabel,
        phaseLabel: phaseLabel,
        blockText: blockText,
        newConnection: newConnection,
        connectionOk: connectionOk,
        connectionFailed: connectionFailed,
        backoffDelay: backoffDelay,
        pollDelay: pollDelay,
        nextPollTask: nextPollTask,
        validSettingsDoc: validSettingsDoc,
        pollSucceeded: pollSucceeded,
        pollFailed: pollFailed,
        firstInvalidKey: firstInvalidKey,
        connectionView: connectionView,
        leadInStart: leadInStart,
        leadInTick: leadInTick,
        leadInCancel: leadInCancel,
        leadInActive: leadInActive,
        leadInView: leadInView,
        grindVisible: grindVisible,
        deriveControls: deriveControls,
        warningsView: warningsView,
        WARNING_TEXT: WARNING_TEXT,
        WARNING_CODE_MAX: WARNING_CODE_MAX,
        readoutView: readoutView,
        profileCards: profileCards,
        STATS_PERIOD_MS: STATS_PERIOD_MS,
        STATS_RETRY_MS: STATS_RETRY_MS,
        STATS_FOLLOW_UP_MS: STATS_FOLLOW_UP_MS,
        STATS_KEYS: STATS_KEYS,
        STATS_NOT_AVAILABLE: STATS_NOT_AVAILABLE,
        newStats: newStats,
        parseStats: parseStats,
        statsLoaded: statsLoaded,
        statsFailed: statsFailed,
        statsRefresh: statsRefresh,
        statsShown: statsShown,
        grindEnded: grindEnded,
        statsOnStatus: statsOnStatus,
        statsDue: statsDue,
        statsView: statsView,
        formatCount: formatCount,
        formatDuration: formatDuration,
        formatKg: formatKg,
        FRESHNESS_STEPS: FRESHNESS_STEPS,
        settingsGroups: settingsGroups,
        groupedKeys: groupedKeys,
        fieldView: fieldView,
        freshnessOptions: freshnessOptions,
        rangeText: rangeText,
        currentProfileText: currentProfileText,
        parseNumberText: parseNumberText,
        entryValue: entryValue,
        draftText: draftText,
        formNew: formNew,
        formSet: formSet,
        formDiscard: formDiscard,
        formPatch: formPatch,
        formChangeCount: formChangeCount,
        changeCountText: changeCountText,
        entryNotes: entryNotes,
        formSaved: formSaved,
        formRefresh: formRefresh,
        stepValue: stepValue,
        saveReason: saveReason,
        saveBarView: saveBarView,
        adjustedNotes: adjustedNotes,
        saveErrorView: saveErrorView,
        renameNotice: renameNotice,
        renameLink: renameLink,
        settingsPlaceholder: settingsPlaceholder
    };
}));
