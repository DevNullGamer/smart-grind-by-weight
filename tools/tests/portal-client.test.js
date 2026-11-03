// node --test tools/tests/portal-client.test.js
// Checks the API client of the web panel page (web/portal/portal-client.js): request queue, error mapping, curl
// export, and that it never logs, stores or sends a token. Error codes come from the shared contract vectors.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const PC = require('../../web/portal/portal-client.js');

const ROOT = path.join(__dirname, '..', '..');
const vectors = JSON.parse(fs.readFileSync(path.join(__dirname, 'portal_api_vectors.json'), 'utf8'));
const clientSource = fs.readFileSync(path.join(ROOT, 'web', 'portal', 'portal-client.js'), 'utf8');

function reply(status, body) {
    const text = typeof body === 'string' ? body : JSON.stringify(body);
    return { ok: status >= 200 && status < 300, status: status, text: async () => text };
}

// A fake fetch that records requests; `handler(url, init, n)` returns a reply (or a promise of one).
function fakeFetch(handler) {
    const calls = [];
    let active = 0;
    const stats = { calls: calls, maxActive: 0 };
    const impl = async function (url, init) {
        calls.push({ url: url, init: init });
        active += 1;
        stats.maxActive = Math.max(stats.maxActive, active);
        try {
            return await handler(url, init, calls.length);
        } finally {
            active -= 1;
        }
    };
    return { fetch: impl, stats: stats };
}

const tick = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// ------------------------------------------------------------------ request shape

test('state-changing requests carry Content-Type and X-Grinder, reads do not; never an Authorization header', async () => {
    const f = fakeFetch(async () => reply(200, { ok: true }));
    const client = PC.createClient({ fetch: f.fetch });
    await client.getStatus();
    await client.getSettings();
    await client.getSchema();
    await client.getInfo();
    await client.patchSettings({ logging: true });
    await client.command('start', 1);
    const [status, settings, schema, info, patch, command] = f.stats.calls;
    assert.deepEqual([status.url, settings.url, schema.url, info.url],
        ['/api/status', '/api/settings', '/api/schema', '/api/info']);
    for (const c of [status, settings, schema, info]) {
        assert.equal(c.init.method, 'GET');
        assert.equal(c.init.headers['X-Grinder'], undefined);
        assert.equal(c.init.body, undefined);
    }
    assert.equal(patch.init.method, 'PATCH');
    assert.equal(patch.url, '/api/settings');
    assert.equal(patch.init.headers['Content-Type'], 'application/json');
    assert.equal(patch.init.headers['X-Grinder'], '1');
    assert.equal(patch.init.body, '{"logging":true}');
    assert.equal(command.init.method, 'POST');
    assert.equal(command.url, '/api/command');
    assert.equal(command.init.body, '{"cmd":"start","profile":1}');
    for (const c of f.stats.calls) {
        const names = Object.keys(c.init.headers).map((n) => n.toLowerCase());
        assert.ok(!names.includes('authorization'));
        assert.equal(c.init.credentials, 'omit');
    }
});

test('commands without a profile send only cmd', async () => {
    const f = fakeFetch(async () => reply(200, { ok: true }));
    const client = PC.createClient({ fetch: f.fetch });
    await client.command('tare');
    await client.command('stop');
    assert.equal(f.stats.calls[0].init.body, '{"cmd":"tare"}');
    assert.equal(f.stats.calls[1].init.body, '{"cmd":"stop"}');
});

test('baseUrl is prefixed', async () => {
    const f = fakeFetch(async () => reply(200, {}));
    await PC.createClient({ fetch: f.fetch, baseUrl: 'http://127.0.0.1:8080' }).getStatus();
    assert.equal(f.stats.calls[0].url, 'http://127.0.0.1:8080/api/status');
});

// ------------------------------------------------------------------ queue

test('writes and commands run one at a time, in order', async () => {
    const order = [];
    const f = fakeFetch(async (url, init, n) => {
        order.push(init.body);
        await tick(10 - n * 2);   // later requests would finish first if they ran in parallel
        return reply(200, { ok: true });
    });
    const client = PC.createClient({ fetch: f.fetch });
    const results = [
        client.patchSettings({ logging: true }),
        client.command('tare'),
        client.patchSettings({ swipe: true }),
        client.command('start', 0)
    ];
    assert.equal(client.pending(), 4);
    await Promise.all(results);
    assert.equal(f.stats.maxActive, 1);
    assert.deepEqual(order, ['{"logging":true}', '{"cmd":"tare"}', '{"swipe":true}', '{"cmd":"start","profile":0}']);
    assert.equal(client.pending(), 0);
});

test('a failed request does not block the ones queued behind it', async () => {
    const f = fakeFetch(async (url, init, n) => (n === 1 ? reply(409, { error: 'busy', reason: 'x' }) : reply(200, { ok: true })));
    const client = PC.createClient({ fetch: f.fetch });
    const first = client.command('tare');
    const second = client.patchSettings({ logging: true });
    await assert.rejects(first, (e) => e.code === 'busy' && e.status === 409);
    assert.deepEqual(await second, { ok: true });
    assert.equal(client.pending(), 0);
});

test('stop is sent at once and drops every queued request that was not sent yet', async () => {
    let release;
    const gate = new Promise((resolve) => { release = resolve; });
    const f = fakeFetch(async (url, init) => {
        if (init.body === '{"logging":true}') await gate;     // the slow PATCH
        return reply(200, { ok: true });
    });
    const client = PC.createClient({ fetch: f.fetch });
    const patch = client.patchSettings({ logging: true });
    const start = client.command('start', 0);
    const tare = client.command('tare');
    const outcome = [start, tare].map((p) => p.then(() => 'sent', (e) => e));
    await tick(5);
    assert.equal(client.pending(), 3);
    await client.command('stop');                             // answered while the PATCH is still waiting
    assert.equal(client.pending(), 1);
    const [startResult, tareResult] = await Promise.all(outcome);
    for (const r of [startResult, tareResult]) {
        assert.ok(r instanceof PC.PortalApiError);
        assert.equal(r.code, 'cancelled');
        assert.equal(r.message, PC.ERROR_TEXT.cancelled);
    }
    release();
    await patch;                                              // the request already on the wire completes
    assert.deepEqual(f.stats.calls.map((c) => c.init.body), ['{"logging":true}', '{"cmd":"stop"}']);
    assert.ok(!f.stats.calls.some((c) => c.init.body.includes('"start"')), 'start was never sent');
    assert.equal(client.pending(), 0);
    await client.command('tare');                             // the queue still works afterwards
    assert.equal(f.stats.calls.length, 3);
});

test('stop with nothing queued just sends stop', async () => {
    const f = fakeFetch(async () => reply(200, { ok: true }));
    const client = PC.createClient({ fetch: f.fetch });
    await client.command('stop');
    assert.equal(f.stats.calls.length, 1);
    assert.equal(client.pending(), 0);
});

test('getStatus shares one in-flight request', async () => {
    let release;
    const gate = new Promise((resolve) => { release = resolve; });
    const f = fakeFetch(async () => { await gate; return reply(200, { ui_state: 'ready' }); });
    const client = PC.createClient({ fetch: f.fetch });
    const a = client.getStatus();
    const b = client.getStatus();
    assert.equal(a, b);
    release();
    await a;
    await client.getStatus();
    assert.equal(f.stats.calls.length, 2);
});

// ------------------------------------------------------------------ error mapping

test('every error code of the contract has a sentence, and STOP cancelling has its own', () => {
    assert.match(PC.ERROR_TEXT.cancelled, /Cancelled by STOP/);
    for (const e of vectors.errors) {
        const text = PC.ERROR_TEXT[e.error];
        assert.ok(typeof text === 'string' && text.length > 10, e.error);
        assert.match(text, /[.]$/, e.error);
    }
});

test('a device refusal becomes a PortalApiError with code, status and words', async () => {
    for (const e of vectors.errors) {
        const f = fakeFetch(async () => reply(e.status, { error: e.error, reason: 'device text' }));
        const client = PC.createClient({ fetch: f.fetch });
        await assert.rejects(client.getSettings(), (err) => {
            assert.ok(err instanceof PC.PortalApiError);
            assert.ok(err instanceof Error);
            assert.equal(err.code, e.error);
            assert.equal(err.status, e.status);
            assert.equal(err.message, PC.ERROR_TEXT[e.error]);
            return true;
        });
    }
});

test('a refusal mentions what to do for the switch and the busy cases', () => {
    assert.match(PC.describeError('web_interface_off'), /Menu, WiFi, Web interface/);
    assert.match(PC.describeError('ui_busy'), /reload the settings/i);
    assert.match(PC.describeError('store_failed'), /reload the settings/i);
});

test('an unknown code falls back to the device reason, length limited', () => {
    assert.equal(PC.describeError('future_code', 'Something new.'), 'Something new.');
    assert.equal(PC.describeError('future_code', 'x'.repeat(500)).length, 200);
    assert.equal(PC.describeError('future_code'), PC.ERROR_TEXT.http_error);
});

test('404 on a route means the firmware does not have it yet; other failures are generic', async () => {
    let f = fakeFetch(async () => reply(404, 'Not found'));
    await assert.rejects(PC.createClient({ fetch: f.fetch }).command('tare'), (e) => e.code === 'not_supported');
    f = fakeFetch(async () => reply(500, 'oops'));
    await assert.rejects(PC.createClient({ fetch: f.fetch }).getStatus(), (e) => e.code === 'http_error' && e.status === 500);
});

test('a reply that is not a JSON object is bad_reply', async () => {
    for (const text of ['<html>', '', '"str"', '12', 'null']) {
        const f = fakeFetch(async () => reply(200, text));
        await assert.rejects(PC.createClient({ fetch: f.fetch }).getStatus(), (e) => e.code === 'bad_reply', text);
    }
});

test('a transport failure is "network" and its text never reaches the message', async () => {
    const secret = 'secret-token-0123456789abcdef';
    const f = fakeFetch(async () => { throw new TypeError('Failed to fetch http://x/?t=' + secret); });
    await assert.rejects(PC.createClient({ fetch: f.fetch }).patchSettings({ logging: true }), (e) => {
        assert.equal(e.code, 'network');
        assert.equal(e.message, PC.ERROR_TEXT.network);
        assert.ok(!JSON.stringify(e).includes(secret));
        assert.ok(!e.message.includes(secret) && !e.reason.includes(secret));
        return true;
    });
});

test('a request that does not answer in time is "timeout"', async () => {
    const f = fakeFetch((url, init) => new Promise((resolve, reject) => {
        init.signal.addEventListener('abort', () => reject(new Error('aborted')));
    }));
    await assert.rejects(PC.createClient({ fetch: f.fetch, timeoutMs: 20 }).getStatus(), (e) => e.code === 'timeout');
});

test('without any fetch the client reports network', async () => {
    const saved = globalThis.fetch;
    try {
        delete globalThis.fetch;
        await assert.rejects(PC.createClient().getStatus(), (e) => e.code === 'network');
    } finally {
        globalThis.fetch = saved;
    }
});

// ------------------------------------------------------------------ no leaks

test('the client never writes to the console, whatever happens', async () => {
    const names = ['log', 'info', 'warn', 'error', 'debug', 'trace'];
    const saved = {};
    let writes = 0;
    names.forEach((n) => { saved[n] = console[n]; console[n] = () => { writes += 1; }; });
    try {
        const replies = [reply(200, { ok: true }), reply(409, { error: 'busy', reason: 'r' }), reply(200, '<html>'), reply(404, 'x')];
        let i = 0;
        const f = fakeFetch(async () => {
            const r = replies[i % replies.length];
            i += 1;
            if (i === 5) throw new Error('boom');
            return r;
        });
        const client = PC.createClient({ fetch: f.fetch });
        for (let k = 0; k < 6; k++) {
            await client.patchSettings({ logging: true }).catch(() => {});
            await client.command('stop').catch(() => {});
            await client.getStatus().catch(() => {});
        }
    } finally {
        names.forEach((n) => { console[n] = saved[n]; });
    }
    assert.equal(writes, 0);
});

test('the module has no console, storage, cookie or token code, and the page inlines exactly this source', () => {
    const code = clientSource.split('\n').filter((l) => !l.trim().startsWith('//')).join('\n');
    for (const word of ['console', 'localStorage', 'sessionStorage', 'document.cookie', 'Authorization', 'Bearer', 'XMLHttpRequest']) {
        assert.ok(!code.includes(word), word);
    }
    const page = fs.readFileSync(path.join(ROOT, 'web', 'portal', 'index.html'), 'utf8');
    assert.ok(page.replace(/\r\n/g, '\n').includes(clientSource.replace(/\r\n/g, '\n').trimEnd()), 'index.html is out of date: run python3 tools/portal/build_page.py');
    assert.ok(!page.includes('console.'));
});

// ------------------------------------------------------------------ pure helpers

test('toCurl builds the commands of the contract and quotes the body', () => {
    assert.equal(PC.toCurl('grindbyweight.local', 'GET', '/api/status'), "curl 'http://grindbyweight.local/api/status'");
    assert.equal(PC.toCurl('192.168.1.5:80', 'PATCH', '/api/settings', '{"logging":true}'),
        'curl -H "Content-Type: application/json" -H "X-Grinder: 1" -X PATCH \'http://192.168.1.5:80/api/settings\' -d \'{"logging":true}\'');
    assert.equal(PC.toCurl('[fe80::1]', 'POST', '/api/command', '{"cmd":"it\'s"}'),
        'curl -H "Content-Type: application/json" -H "X-Grinder: 1" -X POST \'http://[fe80::1]/api/command\' -d \'{"cmd":"it\'\\\'\'s"}\'');
});

test('toCurl refuses a host that could break out of the command line', () => {
    for (const host of ['', 'a b', 'x;rm -rf /', "x'y", '$(id)', 'a`b`', 'h\nost']) {
        assert.throws(() => PC.toCurl(host, 'GET', '/api/status'), /bad host/, host);
    }
});

test('diffValues keeps only what changed', () => {
    const known = { logging: false, brightness_normal: 100, device_name: 'a', purge_mode: 'keep' };
    assert.deepEqual(PC.diffValues(known, { logging: true, brightness_normal: 100, device_name: 'a', purge_mode: 'remove' }),
        { logging: true, purge_mode: 'remove' });
    assert.deepEqual(PC.diffValues(known, { logging: false, brightness_normal: undefined }), {});
});

test('formatNumber follows the contract number rule', () => {
    assert.equal(PC.formatNumber(9), '9.0');
    assert.equal(PC.formatNumber(21.5), '21.5');
    assert.equal(PC.formatNumber(0.1234), '0.123');
    assert.equal(PC.formatNumber(NaN), '');
    assert.equal(PC.formatNumber('x'), '');
});

test('describeAdjusted names the field and the applied value', () => {
    const fields = vectors.fields;
    const text = PC.describeAdjusted(['brightness_normal', 'freshness_hours'], { brightness_normal: 15, freshness_hours: 12 }, fields);
    assert.deepEqual(text, [
        'Brightness was adjusted by the grinder to 15 %.',
        'Freshness window was adjusted by the grinder to 12.0 h.'
    ]);
    assert.deepEqual(PC.describeAdjusted([], {}, fields), []);
});

test('describeStatus turns the snapshot into words, including refusals', () => {
    const d = PC.describeStatus({
        ui_state: 'menu', grind_phase: 'idle', busy: false, weight_g: null, motor_available: true,
        start_block: 'load_cell_no_data', motor_test_block: null, uptime_s: 12, build: 5, web_interface: true
    });
    assert.equal(d.state, PC.STATE_TEXT.menu);
    assert.equal(d.weight, 'no valid reading');
    assert.equal(d.start, PC.ERROR_TEXT.load_cell_no_data);
    assert.equal(d.motorTest, 'would be accepted');
    assert.equal(d.uptime, '12 s');
    assert.equal(PC.describeStatus({ ui_state: 'ready', weight_g: 18, busy: true }).weight, '18.0 g');
});

test('describeStatus tolerates an absent or malformed warnings member', () => {
    assert.deepEqual(PC.describeStatus({ ui_state: 'ready' }).warnings, []);
    assert.deepEqual(PC.describeStatus({ ui_state: 'ready', warnings: 'motor_fault' }).warnings, []);
    assert.deepEqual(PC.describeStatus({ ui_state: 'ready', warnings: ['motor_fault', 3, null, 'x'] }).warnings, ['motor_fault', 'x']);
    assert.deepEqual(PC.describeStatus(null).warnings, []);
});

test('every field of the schema has a label', () => {
    for (const f of vectors.fields) {
        assert.ok(PC.FIELD_LABELS[f.key], f.key);
    }
    assert.equal(PC.fieldLabel('some_new_key'), 'some new key');
});

test('getStats reads GET /api/stats; 404 is not_supported, 503 ui_busy and a bad body are told apart', async () => {
    const doc = vectors.stats_cases[1].input;
    let f = fakeFetch(async () => reply(200, doc));
    assert.deepEqual(await PC.createClient({ fetch: f.fetch }).getStats(), doc);
    assert.equal(f.stats.calls[0].url, '/api/stats');
    assert.equal(f.stats.calls[0].init.method, 'GET');
    assert.equal(f.stats.calls[0].init.body, undefined);
    f = fakeFetch(async () => reply(404, 'Not found'));
    await assert.rejects(PC.createClient({ fetch: f.fetch }).getStats(), (e) => e.code === 'not_supported' && e.status === 404);
    f = fakeFetch(async () => reply(503, { error: 'ui_busy', reason: 'busy' }));
    await assert.rejects(PC.createClient({ fetch: f.fetch }).getStats(), (e) => e.code === 'ui_busy' && e.status === 503);
    f = fakeFetch(async () => reply(403, { error: 'web_interface_off', reason: 'off' }));
    await assert.rejects(PC.createClient({ fetch: f.fetch }).getStats(), (e) => e.code === 'web_interface_off');
    f = fakeFetch(async () => reply(200, '<html>'));
    await assert.rejects(PC.createClient({ fetch: f.fetch }).getStats(), (e) => e.code === 'bad_reply');
});
