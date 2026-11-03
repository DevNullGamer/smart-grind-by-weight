// API client of the web panel (docs/PORTAL-API.md), shared by the reference page (inlined at build by
// tools/portal/build_page.py) and the node tests (tools/tests/portal-client.test.js).
//
// There is no token and no stored state: the panel is open on the local network and the on-screen Web interface
// switch is the only gate (contract section 1). This module never touches storage, never logs and never puts a
// request body or a transport error text into a message; every refusal becomes a PortalApiError whose message is
// a sentence in words.
'use strict';

(function (root, factory) {
    if (typeof module !== 'undefined' && module.exports) {
        module.exports = factory();
    } else {
        root.PortalClient = factory();
    }
}(typeof self !== 'undefined' ? self : this, function () {

    var REQUEST_TIMEOUT_MS = 5000;

    // One sentence per error code of docs/PORTAL-API.md section 8 (plus the client-side ones at the end).
    var ERROR_TEXT = {
        empty_body: 'The request was empty.',
        body_too_large: 'The request was too large for the grinder.',
        malformed_json: 'The grinder could not read the request. Reload the page and try again.',
        nested_value: 'The request contained a nested value, which the grinder does not accept.',
        duplicate_key: 'The request named the same setting twice.',
        unknown_key: 'The grinder does not know one of the settings in the request.',
        unexpected_field: 'The request contained a field that is not allowed for this command.',
        missing_field: 'The request is missing a required field.',
        empty_patch: 'Nothing to change: no setting was different.',
        wrong_type: 'One of the values has the wrong type (for example text instead of a number).',
        invalid_value: 'One of the values is not acceptable to the grinder.',
        out_of_range: 'One of the values is outside the range the grinder allows.',
        unknown_command: 'The grinder does not know this command.',
        invalid_device_name: 'The device name must be 1 to 24 characters of lowercase a-z, 0-9 and hyphen, and must not start or end with a hyphen.',
        web_interface_off: 'The web interface is switched off on the grinder. Turn it on at the grinder: Menu, WiFi, Web interface.',
        bad_host: 'The grinder refused the address this page was opened with. Open it at its .local name or its IP address.',
        bad_origin: 'The grinder refused the request because it did not come from its own page.',
        missing_custom_header: 'The grinder refused the request because the X-Grinder: 1 header is missing.',
        unsupported_media_type: 'The grinder refused the request because it was not sent as JSON.',
        command_in_flight: 'The grinder is still busy with the previous request. Try again in a moment.',
        busy: 'The grinder is busy (a grind, autotune or motor run is active). Stop it first or wait until it is done.',
        not_ready: 'The grinder screen is not in a state that allows this. Go back to the main screen on the grinder.',
        update_in_progress: 'A firmware update is running, so the grinder cannot do this now.',
        motor_disabled: 'The motor is disabled because the grinder has not confirmed which board it is. Check Menu, Hardware board on the grinder.',
        motor_fault: 'The motor output has a fault that stays until the grinder is restarted.',
        load_cell_not_connected: 'The load cell is not connected.',
        load_cell_no_data: 'The load cell is not delivering data.',
        load_cell_rate_invalid: 'The load cell sample rate is invalid.',
        ui_busy: 'The grinder screen did not answer in time. Wait a moment and reload the settings to see what is stored.',
        store_failed: 'The grinder could not save a value. Some earlier values may have been saved; reload the settings to see what is stored.',
        // client side
        network: 'The grinder did not answer. Check that it is on and on the same WiFi.',
        timeout: 'The grinder took too long to answer. It may be busy; try again.',
        bad_reply: 'The grinder sent a reply this page does not understand.',
        not_supported: 'This grinder firmware does not have this feature yet.',
        cancelled: 'Cancelled by STOP.',
        http_error: 'The grinder answered with an error.'
    };

    var STATE_TEXT = {
        ready: 'Ready',
        grinding: 'Grinding',
        grind_complete: 'Grind complete (waiting on the grinder screen)',
        grind_timeout: 'Grind timed out (waiting on the grinder screen)',
        edit: 'Editing a value on the grinder screen',
        menu: 'Menu open on the grinder screen',
        calibration: 'Calibrating',
        confirm: 'Waiting for a confirmation on the grinder screen',
        purge_confirm: 'Waiting on the grinder screen (purge)',
        autotuning: 'Autotuning',
        ota_update: 'Firmware update running',
        ota_update_failed: 'Firmware update failed'
    };

    var FIELD_LABELS = {
        ble_startup: 'Bluetooth on at startup',
        logging: 'Logging',
        swipe: 'Swipe to change mode',
        auto_start: 'Start on cup',
        auto_return: 'Return on removal',
        brightness_normal: 'Brightness',
        brightness_screensaver: 'Brightness (screensaver)',
        purge_mode: 'Purge mode (keep = prime, remove = purge)',
        purge_amount_g: 'Purge amount',
        freshness_hours: 'Freshness window',
        grind_mode: 'Grind mode',
        single_weight_g: 'Single, weight',
        single_time_s: 'Single, time',
        double_weight_g: 'Double, weight',
        double_time_s: 'Double, time',
        custom_weight_g: 'Custom, weight',
        custom_time_s: 'Custom, time',
        device_name: 'Device name (mDNS address)'
    };

    // ------------------------------------------------------------------ errors

    function PortalApiError(code, status, reason) {
        this.name = 'PortalApiError';
        this.code = code;
        this.status = status || 0;
        this.reason = reason || '';
        this.message = describeError(code, this.reason);
    }
    PortalApiError.prototype = Object.create(Error.prototype);
    PortalApiError.prototype.constructor = PortalApiError;

    // Words for an error code. An unknown code falls back to the device's own reason text (length-limited).
    function describeError(code, reason) {
        if (Object.prototype.hasOwnProperty.call(ERROR_TEXT, code)) return ERROR_TEXT[code];
        if (typeof reason === 'string' && reason) return reason.slice(0, 200);
        return ERROR_TEXT.http_error;
    }

    // ------------------------------------------------------------------ pure helpers

    function formatNumber(n) {
        if (typeof n !== 'number' || !isFinite(n)) return '';
        var s = n.toFixed(3).replace(/0+$/, '');
        if (s.charAt(s.length - 1) === '.') s += '0';
        return s;
    }

    function fieldLabel(key) {
        if (Object.prototype.hasOwnProperty.call(FIELD_LABELS, key)) return FIELD_LABELS[key];
        return String(key).replace(/_/g, ' ');
    }

    // The members of `entered` that differ from `known`: the body of a PATCH. Missing or undefined entries are
    // "not touched".
    function diffValues(known, entered) {
        var patch = {};
        Object.keys(entered).forEach(function (key) {
            if (entered[key] === undefined) return;
            if (known[key] !== entered[key]) patch[key] = entered[key];
        });
        return patch;
    }

    // Sentences for the keys the grinder clamped or snapped ("adjusted" of a PATCH reply).
    function describeAdjusted(adjusted, values, schemaFields) {
        var units = {};
        (schemaFields || []).forEach(function (f) { units[f.key] = f.unit || ''; });
        return (adjusted || []).map(function (key) {
            var value = values[key];
            var shown = typeof value === 'number' ? formatNumber(value) : String(value);
            if (typeof value === 'number' && Math.floor(value) === value && units[key] === '%') shown = String(value);
            var unit = units[key] ? ' ' + units[key] : '';
            return fieldLabel(key) + ' was adjusted by the grinder to ' + shown + unit + '.';
        });
    }

    // A readable summary of /api/status for the page.
    function describeStatus(status) {
        var s = status || {};
        var state = Object.prototype.hasOwnProperty.call(STATE_TEXT, s.ui_state) ? STATE_TEXT[s.ui_state] : String(s.ui_state);
        return {
            state: state,
            phase: String(s.grind_phase),
            weight: typeof s.weight_g === 'number' ? formatNumber(s.weight_g) + ' g' : 'no valid reading',
            busy: s.busy ? 'yes' : 'no',
            motor: s.motor_available ? 'available' : 'not available',
            start: s.start_block ? describeError(s.start_block) : 'would be accepted',
            motorTest: s.motor_test_block ? describeError(s.motor_test_block) : 'would be accepted',
            uptime: typeof s.uptime_s === 'number' ? s.uptime_s + ' s' : '',
            build: s.build === null || s.build === undefined ? '' : String(s.build),
            // a missing member (an older firmware) is no warnings
            warnings: Array.isArray(s.warnings) ? s.warnings.filter(function (c) { return typeof c === 'string'; }) : []
        };
    }

    function shellQuote(text) {
        return "'" + String(text).replace(/'/g, "'\\''") + "'";
    }

    // curl command line for a request, as in docs/PORTAL-API.md section 5. `host` is what the page was opened
    // with (location.host). Throws on a host that could break out of the command line.
    function toCurl(host, method, path, body) {
        if (!/^[A-Za-z0-9.\-:\[\]]+$/.test(host || '')) throw new Error('bad host');
        var url = 'http://' + host + path;
        if (method === 'GET') return 'curl ' + shellQuote(url);
        return 'curl -H "Content-Type: application/json" -H "X-Grinder: 1" -X ' + method + ' ' + shellQuote(url) +
            ' -d ' + shellQuote(body);
    }

    // ------------------------------------------------------------------ client

    // options: fetch (default global fetch), baseUrl (default same origin), timeoutMs.
    function createClient(options) {
        var opts = options || {};
        var fetchImpl = opts.fetch || (typeof fetch !== 'undefined' ? fetch.bind(root_()) : null);
        var baseUrl = opts.baseUrl || '';
        var timeoutMs = opts.timeoutMs || REQUEST_TIMEOUT_MS;
        var queue = [];       // tasks not sent yet: {task, resolve, reject}
        var running = false;  // one task is on the wire
        var statusInFlight = null;

        function root_() {
            return typeof self !== 'undefined' ? self : globalThis;
        }

        function request(method, path, bodyObject) {
            if (!fetchImpl) return Promise.reject(new PortalApiError('network', 0));
            var headers = { Accept: 'application/json' };
            var init = { method: method, headers: headers, cache: 'no-store', credentials: 'omit' };
            if (bodyObject !== undefined) {
                headers['Content-Type'] = 'application/json';
                headers['X-Grinder'] = '1';
                init.body = JSON.stringify(bodyObject);
            }
            var controller = typeof AbortController !== 'undefined' ? new AbortController() : null;
            var timedOut = false;
            var timer = null;
            if (controller) {
                init.signal = controller.signal;
                timer = setTimeout(function () { timedOut = true; controller.abort(); }, timeoutMs);
            }
            function done() { if (timer !== null) clearTimeout(timer); }

            return Promise.resolve().then(function () {
                return fetchImpl(baseUrl + path, init);
            }).then(function (res) {
                return res.text().then(function (text) { return { res: res, text: text }; });
            }).then(function (got) {
                done();
                var json = null;
                try { json = JSON.parse(got.text); } catch (e) { json = null; }
                if (!got.res.ok) {
                    if (json && typeof json.error === 'string') {
                        throw new PortalApiError(json.error, got.res.status, json.reason);
                    }
                    if (got.res.status === 404) throw new PortalApiError('not_supported', 404);
                    throw new PortalApiError('http_error', got.res.status);
                }
                if (json === null || typeof json !== 'object') throw new PortalApiError('bad_reply', got.res.status);
                return json;
            }, function () {
                // The transport error text is dropped on purpose: only the words below reach the page.
                done();
                throw new PortalApiError(timedOut ? 'timeout' : 'network', 0);
            });
        }

        // Writes and commands go one at a time, in order (contract section 6: one request in flight). A failed
        // request does not stop the ones queued behind it.
        function pump() {
            if (running || queue.length === 0) return;
            running = true;
            var item = queue.shift();
            // `running` is cleared before the caller hears back, so pending() is right in its continuation
            Promise.resolve().then(item.task).then(function (result) {
                running = false;
                item.resolve(result);
                pump();
            }, function (error) {
                running = false;
                item.reject(error);
                pump();
            });
        }

        function enqueue(task) {
            return new Promise(function (resolve, reject) {
                queue.push({ task: task, resolve: resolve, reject: reject });
                pump();
            });
        }

        // STOP drops everything that was not sent yet, so a queued start cannot run after it.
        function cancelQueued() {
            queue.splice(0).forEach(function (item) { item.reject(new PortalApiError('cancelled', 0)); });
        }

        return {
            getInfo: function () { return request('GET', '/api/info'); },
            getSchema: function () { return request('GET', '/api/schema'); },
            getSettings: function () { return request('GET', '/api/settings'); },
            // Polling-friendly: a call while one is still waiting returns the same promise.
            getStatus: function () {
                if (statusInFlight) return statusInFlight;
                var p = request('GET', '/api/status');
                statusInFlight = p;
                var clear = function () { statusInFlight = null; };
                p.then(clear, clear);
                return p;
            },
            patchSettings: function (patch) {
                return enqueue(function () { return request('PATCH', '/api/settings', patch); });
            },
            // `stop` is always accepted by the grinder: it is never queued, and it cancels the queued requests.
            command: function (cmd, profile) {
                var body = { cmd: cmd };
                if (profile !== undefined && profile !== null) body.profile = profile;
                if (cmd === 'stop') {
                    cancelQueued();
                    return request('POST', '/api/command', body);
                }
                return enqueue(function () { return request('POST', '/api/command', body); });
            },
            pending: function () { return queue.length + (running ? 1 : 0); }
        };
    }

    return {
        createClient: createClient,
        PortalApiError: PortalApiError,
        describeError: describeError,
        describeStatus: describeStatus,
        describeAdjusted: describeAdjusted,
        diffValues: diffValues,
        formatNumber: formatNumber,
        fieldLabel: fieldLabel,
        toCurl: toCurl,
        ERROR_TEXT: ERROR_TEXT,
        STATE_TEXT: STATE_TEXT,
        FIELD_LABELS: FIELD_LABELS
    };
}));
