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

    // The sections of the page in tab order. Insights stays hidden until the firmware has data for it.
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
        if (flags.needSettings) return 'settings';
        if (flags.needInfo) return 'info';
        return 'status';
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

    // Persistent banner for update / motor problems, or null.
    function bannerFor(status, conn) {
        if (!status || !isOnline(conn)) return null;
        var blocks = [status.start_block, status.motor_test_block];
        if (status.ui_state === 'ota_update' || blocks.indexOf('update_in_progress') >= 0) {
            return { level: 'warning', text: 'Firmware update running. Everything except STOP is disabled until it is finished.' };
        }
        if (status.ui_state === 'ota_update_failed') {
            return { level: 'error', text: 'The firmware update failed. See the grinder screen.' };
        }
        if (blocks.indexOf('motor_fault') >= 0) {
            return { level: 'error', text: 'Motor fault. The motor output has a fault that stays until the grinder is restarted. Restart the grinder.' };
        }
        if (blocks.indexOf('motor_disabled') >= 0 || status.motor_available === false) {
            return { level: 'error', text: 'Motor disabled. ' + BLOCK_TEXT.motor_disabled };
        }
        return null;
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
        connectionView: connectionView,
        leadInStart: leadInStart,
        leadInTick: leadInTick,
        leadInCancel: leadInCancel,
        leadInActive: leadInActive,
        leadInView: leadInView,
        grindVisible: grindVisible,
        deriveControls: deriveControls,
        readoutView: readoutView,
        profileCards: profileCards
    };
}));
