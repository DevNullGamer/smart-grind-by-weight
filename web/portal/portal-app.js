// DOM glue of the web panel: reads elements, attaches events, writes text. No logic that node cannot test (that goes
// into portal-view.js). Values are written with textContent only, never as markup. Inlined by build_page.py.
// The Grind tab is live (chg016 step 2); the Settings and Insights tabs follow in later steps.
(function () {
  'use strict';
  var P = PortalClient;
  var V = PortalView;
  var client = P.createClient();

  function $(id) { return document.getElementById(id); }
  function now() { return Date.now(); }

  // --- state of the page (everything else is derived from it by the view module)
  var conn = V.newConnection();
  var status = null;
  var settings = null;
  var leadIn = null;
  var commandPending = false;
  var needSettings = true;
  var needInfo = true;
  var polling = false;
  var pollTimer = null;
  var quickNext = false; // a request went well and settings or info are still missing: the next one follows at once

  // matchMedia may be missing in old browsers: then the narrow layout (one tab at a time) is assumed.
  var wide = typeof window.matchMedia === 'function' ? window.matchMedia('(min-width: 900px)') : null;
  var requestedTab = 'grind';

  function showTab(requested) {
    requestedTab = requested;
    var name = V.tabForViewport(requested, !!(wide && wide.matches));
    V.TABS.forEach(function (tab) {
      $('panel-' + tab).hidden = tab !== name;
      var button = $('tab-' + tab);
      if (tab === name) button.setAttribute('aria-current', 'true');
      else button.removeAttribute('aria-current');
    });
  }

  // Writes only when the text differs, so a poll does not touch the DOM (or re-announce) for an unchanged value.
  function setText(el, text) {
    if (el.textContent !== text) el.textContent = text;
  }

  function setClass(el, name, on) {
    if (on) el.classList.add(name);
    else el.classList.remove(name);
  }

  // A repeated text is announced again: the region is emptied first and filled on the next tick.
  function say(text, isError) {
    var target = $(isError ? 'alert' : 'message');
    $(isError ? 'message' : 'alert').textContent = '';
    target.textContent = '';
    setTimeout(function () { target.textContent = text; }, 0);
  }

  function sayError(err, fallback) {
    say(err && err.message ? err.message : fallback, true);
  }

  // --- rendering
  function leadInIsActive() { return V.leadInActive(leadIn); }

  function setControl(button, reasonEl, control) {
    button.disabled = !control.enabled;
    setText(reasonEl, control.enabled ? '' : control.reason);
  }

  function render() {
    var t = now();
    var controls = V.deriveControls(status, conn, { leadIn: leadInIsActive(), commandPending: commandPending });
    var view = V.connectionView(conn, t);
    var readout = V.readoutView(status, conn, t);

    // connection chip, explanation, empty state
    setText($('conn-text'), view.label);
    var icon = '#i-' + view.icon;
    if ($('conn-icon').getAttribute('href') !== icon) $('conn-icon').setAttribute('href', icon);
    var webOff = conn.state === 'web_off';
    $('weboff').hidden = !webOff;
    $('live').hidden = webOff;
    $('conn-note').hidden = webOff || view.detail === '';
    setText($('conn-detail'), view.detail);
    $('retry').hidden = !view.retry;

    // readout
    setText($('readout-state'), readout.state);
    setText($('readout-weight'), readout.weight);
    setText($('readout-phase'), readout.phase);
    setText($('readout-hint'), readout.hint);
    setText($('readout-seen'), readout.seen);
    setClass($('readout'), 'is-stale', readout.stale);
    setText($('strip-state'), readout.state);
    setText($('strip-weight'), readout.weight);

    // banner
    $('banner').hidden = !controls.banner;
    if (controls.banner) {
      setText($('banner-text'), controls.banner.text);
      setClass($('banner'), 'error', controls.banner.level === 'error');
      setClass($('banner'), 'warning', controls.banner.level === 'warning');
    }

    // STOP: never disabled; emphasised while something runs
    setClass($('stop'), 'is-busy', controls.stop.emphasis);
    setClass($('stop-pane'), 'is-busy', controls.stop.emphasis);

    // lead-in card
    var li = V.leadInView(leadIn, t);
    $('leadin').hidden = !li.visible;
    setText($('leadin-text'), li.text);

    // profile cards
    V.profileCards(settings).forEach(function (card) {
      setText($('target-' + card.index), card.target);
      $('badge-' + card.index).hidden = !card.selected;
      setControl($('start-' + card.index), $('reason-start-' + card.index), controls.start);
    });
    setControl($('tare'), $('reason-tare'), controls.tare);
    setControl($('motor-test'), $('reason-motor-test'), controls.motorTest);
  }

  function setLeadIn(next) {
    if (next && next.phase === 'dropped' && !(leadIn && leadIn.phase === 'dropped')) {
      say(V.leadInView(next, now()).text, true);
    }
    leadIn = next;
  }

  // --- polling: one request at a time (status, settings or info in turns)
  function schedule(ms) {
    if (pollTimer !== null) clearTimeout(pollTimer);
    pollTimer = null;
    if (ms === null) return;
    pollTimer = setTimeout(poll, ms);
  }

  function pollNow() {
    if (!polling) schedule(0);
  }

  function poll() {
    pollTimer = null;
    if (polling) return;
    polling = true;
    var task = V.nextPollTask({ haveStatus: status !== null, needSettings: needSettings, needInfo: needInfo, online: conn.state === 'online' });
    quickNext = false;
    var request = task === 'settings' ? client.getSettings() : task === 'info' ? client.getInfo() : client.getStatus();
    request.then(function (doc) {
      if (conn.state !== 'online') needSettings = true; // back after a gap: the grinder may have changed
      conn = V.connectionOk(conn, now());
      if (task === 'status') {
        var before = status ? status.ui_state : '';
        status = doc;
        if (before && before !== 'ready' && doc.ui_state === 'ready') needSettings = true;
        setLeadIn(V.leadInTick(leadIn, status, now()));
      } else if (task === 'settings') {
        settings = doc;
        needSettings = false;
      } else {
        setText($('name'), doc.name);
        setText($('build'), 'Firmware ' + doc.version + ', build ' + doc.build);
        needInfo = false;
      }
      quickNext = needSettings || needInfo;
    }, function (err) {
      conn = V.connectionFailed(conn, err && err.code, now());
    }).then(function () {
      polling = false;
      render();
      schedule(quickNext ? 100 : V.pollDelay(conn, status, leadInIsActive(), document.hidden));
    });
  }

  // --- commands
  // STOP always tries; a failure is reported in words (the page keeps no other state).
  function stop() {
    client.command('stop').then(function () {
      setLeadIn(V.leadInCancel());
      say('Stop sent.', false);
      render();
      pollNow();
    }, function (err) {
      sayError(err, 'Stop could not be sent.');
      if (err && err.code === 'web_interface_off') { conn = V.connectionFailed(conn, err.code, now()); render(); }
    });
  }

  // Start, Tare and Motor test: one at a time; the buttons carry the reason while a request is on the wire.
  function runCommand(cmd, profile, onAccepted) {
    commandPending = true;
    render();
    client.command(cmd, profile).then(function (doc) {
      commandPending = false;
      onAccepted(doc);
      render();
      pollNow();
    }, function (err) {
      commandPending = false;
      sayError(err, 'The command could not be sent.');
      if (err && err.code === 'web_interface_off') conn = V.connectionFailed(conn, err.code, now());
      render();
      pollNow();
    });
  }

  function startProfile(index) {
    runCommand('start', index, function (doc) {
      setLeadIn(V.leadInStart(index, doc.lead_in_ms, now()));
      say(V.leadInView(leadIn, now()).text, false);
    });
  }

  V.TABS.forEach(function (tab) {
    $('tab-' + tab).addEventListener('click', function () { showTab(tab); });
  });
  $('stop').addEventListener('click', stop);
  $('stop-pane').addEventListener('click', stop);
  [0, 1, 2].forEach(function (i) {
    $('start-' + i).addEventListener('click', function () { startProfile(i); });
  });
  $('tare').addEventListener('click', function () {
    runCommand('tare', null, function () { say('Tare started.', false); });
  });
  $('motor-test').addEventListener('click', function () {
    runCommand('motor_test', null, function () { say('Motor test running.', false); });
  });
  $('retry').addEventListener('click', pollNow);

  showTab('grind');
  // Widening or rotating across 900 px re-applies the tab so the right pane is never empty.
  if (wide) {
    if (typeof wide.addEventListener === 'function') wide.addEventListener('change', function () { showTab(requestedTab); });
    else if (typeof wide.addListener === 'function') wide.addListener(function () { showTab(requestedTab); });
  }

  // Polling pauses while the page is hidden and resumes at once when it is shown again.
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) schedule(null);
    else pollNow();
  });
  // The lead-in countdown and "last seen" move between polls.
  setInterval(function () {
    if (document.hidden) return;
    if (leadIn) setLeadIn(V.leadInTick(leadIn, status, now()));
    render();
  }, 250);

  render();
  poll();
}());
