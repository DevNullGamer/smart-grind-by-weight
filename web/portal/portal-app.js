// DOM glue of the web panel: reads elements, attaches events, writes text. No logic that node cannot test (that goes
// into portal-view.js). Values are written with textContent only, never as markup. Inlined by build_page.py.
// Step 1 of chg016 is the shell: tabs, the device name and a working STOP; live data follows in step 2.
(function () {
  'use strict';
  var P = PortalClient;
  var V = PortalView;
  var client = P.createClient();

  function $(id) { return document.getElementById(id); }

  function showTab(name) {
    // From 900 px the live pane is always visible beside the tabs, so the Grind tab does not exist there.
    if (name === 'grind' && window.matchMedia('(min-width: 900px)').matches) name = 'settings';
    V.TABS.forEach(function (tab) {
      $('panel-' + tab).hidden = tab !== name;
      var button = $('tab-' + tab);
      if (tab === name) button.setAttribute('aria-current', 'true');
      else button.removeAttribute('aria-current');
    });
  }

  function say(text, isError) {
    $(isError ? 'alert' : 'message').textContent = text;
    $(isError ? 'message' : 'alert').textContent = '';
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

  client.getInfo().then(function (info) {
    $('name').textContent = info.name;
    $('build').textContent = 'Firmware ' + info.version + ', build ' + info.build;
    $('readout-weight').textContent = V.formatWeight(null);
  }, function () {});
}());
