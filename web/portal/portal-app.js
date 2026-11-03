// DOM glue of the web panel: reads elements, attaches events, writes text. No logic that node cannot test (that goes
// into portal-view.js). Values are written with textContent only, never as markup. Inlined by build_page.py.
// Step 1 of chg016 is the shell: tabs, the device name and a working STOP; live data follows in step 2.
(function () {
  'use strict';
  var P = PortalClient;
  var V = PortalView;
  var client = P.createClient();

  function $(id) { return document.getElementById(id); }

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

  // A repeated text is announced again: the region is emptied first and filled on the next tick.
  function say(text, isError) {
    var target = $(isError ? 'alert' : 'message');
    $(isError ? 'message' : 'alert').textContent = '';
    target.textContent = '';
    setTimeout(function () { target.textContent = text; }, 0);
  }

  // STOP always tries; a failure is reported in words (the page keeps no other state).
  function stop() {
    client.command('stop').then(function () {
      say('Stop sent.', false);
    }, function (err) {
      say(err && err.message ? err.message : 'Stop could not be sent.', true);
    });
  }

  V.TABS.forEach(function (tab) {
    $('tab-' + tab).addEventListener('click', function () { showTab(tab); });
  });
  $('stop').addEventListener('click', stop);
  $('stop-pane').addEventListener('click', stop);
  showTab('grind');
  // Widening or rotating across 900 px re-applies the tab so the right pane is never empty.
  if (wide) {
    if (typeof wide.addEventListener === 'function') wide.addEventListener('change', function () { showTab(requestedTab); });
    else if (typeof wide.addListener === 'function') wide.addListener(function () { showTab(requestedTab); });
  }

  client.getInfo().then(function (info) {
    $('name').textContent = info.name;
    $('build').textContent = 'Firmware ' + info.version + ', build ' + info.build;
    $('readout-weight').textContent = V.formatWeight(null);
  }, function () {});
}());
