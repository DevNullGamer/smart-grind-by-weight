// DOM glue of the web panel: reads elements, attaches events, writes text. No logic that node cannot test (that goes
// into portal-view.js). Values are written with textContent only, never as markup. Inlined by build_page.py.
// The Grind, Settings and Insights tabs are live (chg016 steps 2, 3, 9 and 12; the Insights tab shows once the
// firmware has sent its statistics, and its recent-grinds list only where the firmware has /api/history). The Device
// tab (chg021 V3) shows the name, address and firmware the page already knows.
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
  var settingsGen = 0; // bumped when a save finishes: settings replies requested earlier are stale and dropped
  var needInfo = true;
  var needSchema = true;
  var schema = null; // the field list of GET /api/schema
  var schemaMissing = false; // the firmware has no /api/schema
  var form = null; // the settings form state (stored values and the user's entries), see portal-view.js
  var currentProfileEl = null; // the "selected on the grinder" line of the Profiles group
  var fieldControls = {}; // key -> {write, msg, inputs, focusEl} of the built settings controls
  var fieldErrors = {}; // key -> text of a refusal beside the field
  var fieldNotes = {}; // key -> text of an "adjusted by the grinder" note
  var formError = ''; // refusal text for the whole form
  var settingsNotice = ''; // "the grinder changed values while you were editing"
  var quietRefresh = false; // the next settings reload follows a failed save: no "changed" notice
  var stats = V.newStats(); // lifetime statistics (GET /api/stats), see portal-view.js
  var history = V.newStats(); // recent grinds (GET /api/history): the same state shape, see V.historyLoaded
  var polling = false;
  var pollTimer = null;
  var statusAt = null; // when the latest status reply was received (the lead-in must not judge a stale status)
  var forceStatus = false; // the last settings or info request failed: status goes next
  var quickNext = false; // a request went well and settings or info are still missing: the next one follows at once

  var requestedTab = 'grind';

  function showTab(requested) {
    requestedTab = requested;
    var name = V.tabForViewport(requested);
    V.TABS.forEach(function (tab) {
      setHidden($('panel-' + tab), tab !== name);
      var button = $('tab-' + tab);
      if (tab === name) button.setAttribute('aria-current', 'true');
      else button.removeAttribute('aria-current');
    });
    setSavebarHeight(); // the save bar only shows on the Settings tab: its height follows the tab
    if (name === 'insights') {
      stats = V.statsShown(stats, now()); // opening the tab fetches the numbers
      history = V.statsShown(history, now());
      pollNow();
    }
  }

  // Writes only when the text differs, so a poll does not touch the DOM (or re-announce) for an unchanged value.
  function setText(el, text) {
    if (el.textContent !== text) el.textContent = text;
  }

  // Touches the class list only when the class really changes, so a poll leaves the element alone.
  function setClass(el, name, on) {
    if (el.classList.contains(name) !== !!on) el.classList.toggle(name, !!on);
  }

  // Same for the hidden attribute: written only when it changes, so an idle poll causes no attribute mutation.
  function setHidden(el, hidden) {
    hidden = !!hidden;
    if (el.hidden !== hidden) el.hidden = hidden;
  }

  function setDisabled(el, disabled) {
    disabled = !!disabled;
    if (el.disabled !== disabled) el.disabled = disabled;
  }

  // A repeated text is announced again: the region is emptied first and filled on the next tick.
  // Both regions are emptied at once; only the wanted one is filled, in one place.
  var sayTimer = null;
  function say(text, isError) {
    $('alert').textContent = '';
    $('message').textContent = '';
    if (sayTimer !== null) clearTimeout(sayTimer);
    sayTimer = setTimeout(function () {
      sayTimer = null;
      $(isError ? 'alert' : 'message').textContent = text;
    }, 0);
  }

  function sayError(err, fallback) {
    say(err && err.message ? err.message : fallback, true);
  }

  // --- rendering
  function leadInIsActive() { return V.leadInActive(leadIn); }

  function setControl(button, reasonEl, control) {
    setDisabled(button, !control.enabled);
    setText(reasonEl, control.enabled ? '' : control.reason);
  }

  // The colour of the state dot (see V.readoutView): ready and busy have a class, neutral has none, stale is is-stale.
  function setTone(el, tone) {
    setClass(el, 'is-ready', tone === 'ready');
    setClass(el, 'is-busy', tone === 'busy');
  }

  var ringValue = ''; // the last value written to the countdown ring

  // The warnings card is rebuilt only when its rows change, so a poll does not re-announce an unchanged list.
  var warningsKey = '';
  function renderWarnings() {
    var view = V.warningsView(status, conn);
    setHidden($('warnings'), !view.visible);
    var key = JSON.stringify(view.rows);
    if (key === warningsKey) return;
    warningsKey = key;
    var list = $('warnings-list');
    while (list.firstChild) list.removeChild(list.firstChild);
    view.rows.forEach(function (row) {
      // cloned from the <template> in the page: the icons are the page's own inline sprite symbols
      var item = $('warning-row').content.firstElementChild.cloneNode(true);
      item.classList.add(row.level);
      item.querySelector('.warn-tag').textContent = row.label;
      item.querySelector('.warn-name').textContent = row.title;
      item.querySelector('.warn-action').textContent = row.action;
      list.appendChild(item);
    });
  }

  // The tiles are rebuilt only when their content changes. Values go in with textContent only.
  var insightsKey = '';
  function renderInsights() {
    var view = V.statsView(stats, conn, now());
    var tab = $('tab-insights');
    if (tab.hidden === view.tabVisible) {
      setHidden(tab, !view.tabVisible);
      if (!view.tabVisible && requestedTab === 'insights') showTab('grind'); // the data went away: do not stay on an empty tab
    }
    setText($('insights-message'), view.message);
    setText($('insights-note'), view.note);
    setHidden($('tiles'), view.tiles.length === 0);
    setHidden($('tiles-skel'), view.kind !== 'loading');
    setClass($('tiles-skel'), 'is-loading', view.kind === 'loading');
    setHidden($('mix'), !view.mix.visible);
    var key = JSON.stringify([view.tiles, view.mix]);
    if (key === insightsKey) return;
    insightsKey = key;
    var tiles = $('tiles');
    while (tiles.firstChild) tiles.removeChild(tiles.firstChild);
    view.tiles.forEach(function (tile) {
      var item = $('tile-row').content.firstElementChild.cloneNode(true);
      item.querySelector('use').setAttribute('href', '#i-' + tile.icon);
      item.querySelector('.tile-label').textContent = tile.label;
      // a line may break after a comma (never inside a group of digits): a <wbr> after each comma, so copied text stays clean
      var value = item.querySelector('.stat-value');
      tile.value.split(',').forEach(function (part, n, parts) {
        if (n > 0) value.appendChild(el('wbr'));
        value.appendChild(document.createTextNode(n < parts.length - 1 ? part + ',' : part));
      });
      item.querySelector('.stat-detail').textContent = tile.detail;
      tiles.appendChild(item);
    });
    var bar = $('mix-bar');
    var list = $('mix-list');
    while (bar.firstChild) bar.removeChild(bar.firstChild);
    while (list.firstChild) list.removeChild(list.firstChild);
    view.mix.segments.forEach(function (segment, i) {
      if (segment.drawn) {
        var seg = el('span', 'seg s' + i);
        seg.style.flexGrow = String(segment.flex);
        bar.appendChild(seg);
      }
      var row = el('div', 'mix-row');
      var name = el('dt');
      name.appendChild(el('span', 'swatch s' + i));
      name.appendChild(el('span', '', segment.label));
      row.appendChild(name);
      row.appendChild(el('dd', '', segment.text));
      list.appendChild(row);
    });
  }

  // The recent-grinds card is rebuilt only when its content changes (an age text changes once a minute).
  var historyKey = '';
  function renderHistory() {
    var view = V.historyView(history, conn, now());
    setHidden($('history'), !view.visible);
    setText($('history-message'), view.message);
    setText($('history-note'), view.note);
    setText($('history-notice'), view.notice);
    setHidden($('history-notice'), view.notice === '');
    setHidden($('history-skel'), view.kind !== 'loading');
    setClass($('history-skel'), 'is-loading', view.kind === 'loading');
    setHidden($('history-chart'), !view.chart.visible);
    var key = JSON.stringify([view.rows, view.chart]);
    if (key === historyKey) return;
    historyKey = key;
    setText($('history-caption'), view.chart.caption);
    var bars = $('history-bars');
    var list = $('history-list');
    while (bars.firstChild) bars.removeChild(bars.firstChild);
    while (list.firstChild) list.removeChild(list.firstChild);
    // the chart is plain divs with a height in percent (the list carries the same numbers in words)
    view.chart.bars.forEach(function (bar) {
      var col = el('div', 'h-col ' + bar.dir);
      var up = el('div', 'h-up');
      var down = el('div', 'h-down');
      if (bar.dir === 'over' || bar.dir === 'under') {
        var fill = el('div', 'h-bar ' + bar.dir);
        fill.style.height = bar.pct + '%';
        (bar.dir === 'over' ? up : down).appendChild(fill);
      }
      col.appendChild(up);
      col.appendChild(el('div', 'h-axis'));
      col.appendChild(down);
      bars.appendChild(col);
    });
    view.rows.forEach(function (row) {
      var item = $('history-row').content.firstElementChild.cloneNode(true);
      item.querySelector('.h-name').textContent = row.name;
      item.querySelector('.h-age').textContent = row.age;
      item.querySelector('.h-final').textContent = row.final;
      item.querySelector('.h-target').textContent = row.target;
      item.querySelector('.h-error').textContent = row.error;
      item.querySelector('.h-detail').textContent = row.detail;
      item.querySelector('.h-motor-text').textContent = row.motor;
      setHidden(item.querySelector('.h-motor'), row.motor === '');
      item.querySelector('.h-result').textContent = row.badge.label;
      item.querySelector('.h-badge').classList.add(row.badge.level);
      list.appendChild(item);
    });
  }

  function render() {
    var t = now();
    var controls = V.deriveControls(status, conn, { leadIn: leadInIsActive(), commandPending: commandPending });
    var view = V.connectionView(conn, t);
    var readout = V.readoutView(status, conn, t, { leadIn: leadInIsActive() });

    // connection chip, explanation, empty state
    setText($('conn-text'), view.label);
    var icon = '#i-' + view.icon;
    if ($('conn-icon').getAttribute('href') !== icon) $('conn-icon').setAttribute('href', icon);
    var webOff = conn.state === 'web_off';
    setHidden($('weboff'), !webOff);
    setHidden($('live'), webOff);
    setHidden($('conn-note'), webOff || view.detail === '');
    setText($('conn-detail'), view.detail);
    setHidden($('retry'), !view.retry);

    // readout (Grind section) and status bar: the bar has the short state words, the readout the full ones
    setText($('readout-state'), readout.short);
    setText($('readout-weight'), readout.value);
    setText($('readout-phase'), readout.phase);
    setText($('readout-detail'), readout.detail);
    setText($('readout-hint'), readout.hint);
    setText($('readout-seen'), readout.seen);
    setClass($('readout'), 'is-stale', readout.stale);
    setTone($('readout'), readout.tone);
    setText($('strip-state'), readout.short);
    if ($('strip-state').title !== readout.state) $('strip-state').title = readout.state; // the full words, for a pointer
    setText($('strip-weight'), readout.weight);
    setClass($('strip'), 'is-stale', readout.stale);
    setClass($('strip'), 'is-reconnecting', conn.state === 'reconnecting');
    setClass($('readout'), 'is-reconnecting', conn.state === 'reconnecting');
    setTone($('strip'), readout.tone);

    // banner
    setHidden($('banner'), !controls.banner);
    if (controls.banner) {
      setText($('banner-text'), controls.banner.text);
      setClass($('banner'), 'error', controls.banner.level === 'error');
      setClass($('banner'), 'warning', controls.banner.level === 'warning');
    }

    // STOP: never disabled; emphasised while something runs
    setClass($('stop'), 'is-busy', controls.stop.emphasis);

    // lead-in block: the ring is drawn by the stylesheet from one custom property
    var li = V.leadInView(leadIn, t);
    setHidden($('leadin'), !li.visible);
    setText($('leadin-text'), li.text);
    setText($('ring-count'), li.count);
    setClass($('leadin'), 'is-dropped', li.dropped);
    var ring = li.fraction.toFixed(3);
    if (ring !== ringValue) {
      ringValue = ring;
      $('ring').style.setProperty('--ring', ring);
    }

    // profile cards: the three Start buttons share one reason, shown once under the cards
    V.profileCards(settings).forEach(function (card) {
      setText($('target-' + card.index), card.target);
      setHidden($('badge-' + card.index), !card.selected);
      setClass($('profile-' + card.index), 'is-selected', card.selected);
      setControl($('start-' + card.index), $('reason-start-' + card.index), controls.start);
    });
    setText($('reason-start'), controls.start.enabled ? '' : controls.start.reason);
    setControl($('tare'), $('reason-tare'), controls.tare);
    setControl($('motor-test'), $('reason-motor-test'), controls.motorTest);
    renderWarnings();
    renderSettings();
    renderInsights();
    renderHistory();
  }

  // --- settings form
  function el(tag, className, text) {
    var e = document.createElement(tag);
    if (className) e.className = className;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  function setValue(input, value) {
    if (input.value !== value) input.value = value;
  }

  function onEdit(key, raw) {
    form = V.formSet(form, key, raw);
    delete fieldErrors[key];
    delete fieldNotes[key];
    formError = '';
    renderSettings();
  }

  function numberText(view, raw) { return V.draftText(view, raw); }

  // Builds one control; returns its wrapper. The registry entry knows how to show a draft value.
  function buildField(view) {
    var key = view.key;
    var id = 'f-' + key;
    var msg = el('p', 'field-msg');
    msg.id = 'msg-' + key;
    var describedBy = msg.id;
    var hintText = view.hint || (view.kind === 'stepper' && view.range ? 'Allowed: ' + view.range : '');
    var hint = null;
    if (hintText) {
      hint = el('p', 'hint', hintText);
      hint.id = 'hint-' + key;
      describedBy = hint.id + ' ' + msg.id;
    }
    var reg = { msg: msg, inputs: [], focusEl: null, write: null };
    var wrap;
    var input;

    if (view.kind === 'switch') {
      wrap = el('div', 'field field-switch');
      var row = el('label', 'switch-row');
      row.setAttribute('for', id);
      row.appendChild(el('span', 'lbl', view.label));
      input = el('input', 'switch');
      input.type = 'checkbox';
      input.id = id;
      input.setAttribute('role', 'switch');
      row.appendChild(input);
      wrap.appendChild(row);
      input.addEventListener('change', function () { onEdit(key, input.checked); });
      reg.inputs = [input];
      reg.focusEl = input;
      reg.write = function (raw) { input.checked = raw === true; };
    } else if (view.kind === 'segmented') {
      wrap = el('fieldset', 'field field-seg');
      wrap.setAttribute('role', 'radiogroup');
      wrap.appendChild(el('legend', '', view.label));
      var opts = el('div', 'seg-opts');
      view.choices.forEach(function (choice, i) {
        var label = el('label');
        var radio = el('input');
        radio.type = 'radio';
        radio.name = id;
        radio.value = choice.value;
        radio.id = id + '-' + i;
        radio.addEventListener('change', function () { if (radio.checked) onEdit(key, choice.value); });
        label.appendChild(radio);
        label.appendChild(el('span', '', choice.label));
        opts.appendChild(label);
        reg.inputs.push(radio);
      });
      wrap.appendChild(opts);
      reg.focusEl = reg.inputs[0] || null;
      reg.write = function (raw) {
        reg.inputs.forEach(function (radio) { radio.checked = radio.value === raw; });
      };
    } else if (view.kind === 'slider') {
      wrap = el('div', 'field');
      var head = el('div', 'slider-head');
      var label = el('label', '', view.label);
      label.setAttribute('for', id);
      var out = el('output');
      out.setAttribute('for', id);
      head.appendChild(label);
      head.appendChild(out);
      wrap.appendChild(head);
      input = el('input');
      input.type = 'range';
      input.id = id;
      if (view.min !== null) input.min = String(view.min);
      if (view.max !== null) input.max = String(view.max);
      input.step = String(view.step);
      wrap.appendChild(input);
      var showOut = function () { setText(out, numberText(view, V.entryValue(view, input.value)) + (view.unit ? ' ' + view.unit : '')); };
      input.addEventListener('input', function () { showOut(); onEdit(key, input.value); });
      reg.inputs = [input];
      reg.focusEl = input;
      reg.write = function (raw) { setValue(input, numberText(view, raw)); showOut(); };
    } else if (view.kind === 'select') {
      wrap = el('div', 'field');
      var selLabel = el('label', '', view.label);
      selLabel.setAttribute('for', id);
      wrap.appendChild(selLabel);
      input = el('select');
      input.id = id;
      wrap.appendChild(input);
      input.addEventListener('change', function () { onEdit(key, input.value); });
      reg.inputs = [input];
      reg.focusEl = input;
      reg.write = function (raw) {
        var entry = V.entryValue(view, raw);
        var wanted = entry === undefined ? '' : String(entry);
        var have = false;
        for (var i = 0; i < input.options.length; i++) if (input.options[i].value === wanted) have = true;
        if (!have) {
          while (input.firstChild) input.removeChild(input.firstChild);
          V.selectOptions(key, entry).forEach(function (o) {
            var option = el('option', '', o.label);
            option.value = String(o.value);
            input.appendChild(option);
          });
        }
        setValue(input, wanted);
      };
    } else if (view.kind === 'text') {
      wrap = el('div', 'field');
      var textLabel = el('label', '', view.label);
      textLabel.setAttribute('for', id);
      wrap.appendChild(textLabel);
      var nameRow = el('div', 'name-row');
      input = el('input');
      input.type = 'text';
      input.id = id;
      input.setAttribute('autocapitalize', 'none');
      input.setAttribute('autocomplete', 'off');
      input.setAttribute('autocorrect', 'off');
      input.spellcheck = false;
      if (view.max !== null) input.maxLength = view.max;
      nameRow.appendChild(input);
      if (key === 'device_name') nameRow.appendChild(el('span', 'unit', '.local'));
      wrap.appendChild(nameRow);
      input.addEventListener('input', function () { onEdit(key, input.value); });
      reg.inputs = [input];
      reg.focusEl = input;
      reg.write = function (raw) { setValue(input, typeof raw === 'string' ? raw : ''); };
    } else {
      // stepper: minus, number, plus. The step is a convenience; typing any value is allowed.
      wrap = el('div', 'field field-stepper');
      var numLabel = el('label', '', view.label);
      numLabel.setAttribute('for', id);
      wrap.appendChild(numLabel);
      var stepper = el('div', 'stepper');
      var minus = el('button', 'btn', '−');
      minus.type = 'button';
      minus.setAttribute('aria-label', 'Decrease ' + view.label);
      var plus = el('button', 'btn', '+');
      plus.type = 'button';
      plus.setAttribute('aria-label', 'Increase ' + view.label);
      input = el('input');
      input.type = 'number';
      input.id = id;
      input.step = 'any';
      input.setAttribute('inputmode', 'decimal');
      stepper.appendChild(minus);
      stepper.appendChild(input);
      if (view.unit) stepper.appendChild(el('span', 'unit', view.unit));
      stepper.appendChild(plus);
      wrap.appendChild(stepper);
      var press = function (direction) {
        var next = V.stepValue(form.draft[key], direction, view, form.stored[key]);
        onEdit(key, next);
        setValue(input, numberText(view, next));
      };
      minus.addEventListener('click', function () { press(-1); });
      plus.addEventListener('click', function () { press(1); });
      input.addEventListener('input', function () { onEdit(key, input.value); });
      reg.inputs = [input];
      reg.focusEl = input;
      reg.write = function (raw) { setValue(input, numberText(view, raw)); };
    }

    reg.inputs.forEach(function (i) { i.setAttribute('aria-describedby', describedBy); });
    if (hint) wrap.appendChild(hint);
    wrap.appendChild(msg);
    fieldControls[key] = reg;
    return wrap;
  }

  function buildForm() {
    var host = $('settings-form');
    while (host.firstChild) host.removeChild(host.firstChild);
    fieldControls = {};
    currentProfileEl = null;
    V.settingsGroups(schema).forEach(function (group) {
      var card = el('section', 'card group');
      var headingId = 'group-' + group.id;
      var h = el('h2', '', group.title);
      h.id = headingId;
      card.setAttribute('aria-labelledby', headingId);
      // the header icon is cloned from the <template> in the page (its own sprite symbol, named by the view module)
      var head = $('group-head').content.firstElementChild.cloneNode(true);
      head.querySelector('use').setAttribute('href', '#i-' + group.icon);
      head.appendChild(h);
      card.appendChild(head);
      if (group.hint) card.appendChild(el('p', 'hint', group.hint));
      if (group.id === 'profiles') {
        currentProfileEl = el('p', 'hint');
        card.appendChild(currentProfileEl);
      }
      var fields = el('div', 'fields');
      group.fields.forEach(function (view) { fields.appendChild(buildField(view)); });
      card.appendChild(fields);
      host.appendChild(card);
    });
  }

  function writeFields() {
    Object.keys(fieldControls).forEach(function (key) {
      if (form && Object.prototype.hasOwnProperty.call(form.draft, key)) fieldControls[key].write(form.draft[key]);
    });
  }

  // The form exists once the schema and the settings are both known.
  function ensureForm() {
    if (form || !schema || !settings) return;
    form = V.formNew(settings.values);
    buildForm();
    writeFields();
  }

  // A settings reply: the first one builds the form, later ones never overwrite the user's entries.
  function onSettings() {
    if (!form) { ensureForm(); return; }
    var result = V.formRefresh(form, settings.values, schema);
    form = result.form;
    if (result.notice && !quietRefresh) settingsNotice = result.notice;
    quietRefresh = false;
    writeFields();
  }

  // The save bar covers the bottom of the page: tell the CSS how tall it is so a focused field scrolls above it.
  var savebarHeight = '0px';
  function setSavebarHeight() {
    var bar = $('savebar');
    var h = bar.hidden ? '0px' : bar.offsetHeight + 'px';
    if (h !== savebarHeight) {
      savebarHeight = h;
      document.documentElement.style.setProperty('--savebar-h', h);
    }
  }

  function renderSettings() {
    var placeholder = V.settingsPlaceholder(!!schema, !!settings, conn, schemaMissing);
    setHidden($('settings-status'), placeholder === '');
    setText($('settings-status-text'), placeholder);
    setHidden($('settings-form'), !form);
    if (!form) { setHidden($('savebar'), true); setHidden($('settings-notice'), true); setSavebarHeight(); return; }

    var bar = V.saveBarView(form, schema, status, conn, { leadIn: leadInIsActive(), commandPending: commandPending });
    if (!bar.visible) { settingsNotice = ''; formError = ''; }
    setHidden($('savebar'), !bar.visible);
    setText($('save-count'), bar.countText);
    setText($('save-reason'), bar.reason);
    setText($('save-error'), formError);
    setDisabled($('save'), !bar.canSave);
    setHidden($('settings-notice'), settingsNotice === '');
    setText($('settings-notice-text'), settingsNotice);
    if (currentProfileEl) setText(currentProfileEl, V.currentProfileText(settings));
    setSavebarHeight();

    var notes = V.entryNotes(form, schema);
    Object.keys(fieldControls).forEach(function (key) {
      var reg = fieldControls[key];
      var error = fieldErrors[key] || notes[key] || '';
      var text = error || fieldNotes[key] || '';
      setText(reg.msg, text);
      setClass(reg.msg, 'error', error !== '');
      setClass(reg.msg, 'adjusted', error === '' && text !== '');
      reg.inputs.forEach(function (i) {
        if (error) i.setAttribute('aria-invalid', 'true');
        else i.removeAttribute('aria-invalid');
        setClass(i, 'is-invalid', error !== '' && i.type !== 'radio');
      });
    });
  }

  function showRename(address) {
    var text = V.renameNotice(address);
    setHidden($('rename'), text === '');
    setText($('rename-text'), text);
    var href = V.renameLink(address, window.location.protocol, window.location.port);
    var link = $('rename-link');
    setHidden(link, href === '');
    if (href) {
      setText(link, address);
      link.setAttribute('href', href);
    }
  }

  function saveSettings() {
    if (!form) return;
    var patch = V.formPatch(form, schema);
    var count = Object.keys(patch).length;
    var bar = V.saveBarView(form, schema, status, conn, { leadIn: leadInIsActive(), commandPending: commandPending });
    if (!bar.canSave) return;
    commandPending = true;
    formError = '';
    render();
    client.patchSettings(patch).then(function (doc) {
      commandPending = false;
      settingsGen++;
      if (!V.validSettingsDoc(doc)) {
        // saved, but the reply cannot be read: read the settings back instead of guessing
        needSettings = true;
        quietRefresh = true;
        say('The grinder answered in a way this page cannot read. The page checks the settings.', true);
        render();
        pollNow();
        return;
      }
      form = V.formSaved(form, doc.values, patch, schema);
      settings = { values: doc.values, current_profile: doc.current_profile };
      fieldErrors = {};
      fieldNotes = V.adjustedNotes(doc.adjusted, doc.values, schema);
      settingsNotice = '';
      writeFields();
      if (doc.address) showRename(doc.address);
      var adjusted = doc.adjusted && doc.adjusted.length ? ' The grinder adjusted some values.' : '';
      say('Saved ' + V.changeCountText(count) + '.' + adjusted, false);
      render();
    }, function (err) {
      commandPending = false;
      settingsGen++;
      var code = err && err.code;
      var shown = V.saveErrorView(code, patch, schema);
      fieldErrors = shown.fields;
      formError = shown.form;
      say(err && err.message ? err.message : shown.form, true);
      if (code === 'web_interface_off') conn = V.connectionFailed(conn, code, now());
      if (shown.reload) { needSettings = true; quietRefresh = true; }
      render();
      var first = V.firstInvalidKey(fieldErrors, schema);
      if (first && fieldControls[first] && fieldControls[first].focusEl) fieldControls[first].focusEl.focus();
      pollNow();
    });
  }

  function discardSettings() {
    if (!form) return;
    form = V.formDiscard(form);
    fieldErrors = {};
    fieldNotes = {};
    formError = '';
    settingsNotice = '';
    writeFields();
    render();
    say('Changes discarded.', false);
  }

  function setLeadIn(next) {
    var wasDropped = !!leadIn && leadIn.phase === 'dropped';
    if (next && next.phase === 'dropped' && !wasDropped) {
      say(V.leadInView(next, now()).text, true);
    } else if (wasDropped && !(next && next.phase === 'dropped')) {
      // The notice is over (a grind appeared or it timed out): do not leave it standing in the alert region.
      var droppedText = V.leadInView(leadIn, now()).text;
      if ($('alert').textContent === droppedText) $('alert').textContent = '';
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
    var statsDue = V.statsDue(stats, {
      online: conn.state === 'online', haveStatus: status !== null, hidden: document.hidden,
      tabShown: !$('panel-insights').hidden, commandBusy: commandPending || client.pending() > 0, nowMs: now()
    });
    var historyDue = V.historyDue(history, {
      online: conn.state === 'online', haveStatus: status !== null, hidden: document.hidden,
      tabShown: !$('panel-insights').hidden, commandBusy: commandPending || client.pending() > 0, nowMs: now()
    });
    var task = V.nextPollTask({
      haveStatus: status !== null, needSchema: needSchema, needSettings: needSettings, needInfo: needInfo,
      online: conn.state === 'online', forceStatus: forceStatus, statsDue: statsDue, historyDue: historyDue
    });
    forceStatus = false;
    quickNext = false;
    var request = task === 'schema' ? client.getSchema() : task === 'settings' ? client.getSettings() :
      task === 'info' ? client.getInfo() : task === 'stats' ? client.getStats() :
      task === 'history' ? client.getHistory() : client.getStatus();
    // Runs after success and after failure, even if the handler or render() throws: polling must go on.
    function finalize() {
      polling = false;
      try {
        render();
      } finally {
        schedule(document.hidden ? null : quickNext ? 100 : V.pollDelay(conn, status, leadInIsActive(), document.hidden));
      }
    }
    var gen = settingsGen;
    request.then(function (doc) {
      var wasOnline = conn.state === 'online';
      var before = status ? status.ui_state : '';
      conn = V.connectionOk(conn, now());
      // A settings reply that started before a save finished is older than the saved values: drop it.
      if (task === 'settings' && gen !== settingsGen) {
        quickNext = needSchema || needSettings || needInfo;
        return;
      }
      var next = V.pollSucceeded(task, { needSchema: needSchema, needSettings: needSettings, needInfo: needInfo }, doc, wasOnline, before);
      needSchema = next.needSchema;
      needSettings = next.needSettings;
      needInfo = next.needInfo;
      forceStatus = next.forceStatus;
      if (task === 'status') {
        status = doc;
        statusAt = now();
        stats = V.statsOnStatus(stats, before, status.ui_state, statusAt);
        history = V.statsOnStatus(history, before, status.ui_state, statusAt);
        setLeadIn(V.leadInTick(leadIn, status, statusAt, statusAt));
      } else if (task === 'schema') {
        schema = doc && Array.isArray(doc.fields) ? doc.fields : [];
        ensureForm();
      } else if (task === 'settings') {
        if (V.validSettingsDoc(doc)) {
          settings = doc;
          onSettings();
        }
      } else if (task === 'stats') {
        stats = V.statsLoaded(stats, doc, now());
      } else if (task === 'history') {
        history = V.historyLoaded(history, doc, now());
      } else {
        var firmware = 'Firmware ' + doc.version + ', build ' + doc.build;
        setText($('name'), doc.name);
        setText($('side-name'), doc.name);
        setText($('build'), firmware);
        setText($('about-name'), doc.name);
        setText($('about-firmware'), doc.version + ', build ' + doc.build);
      }
      quickNext = next.quick;
    }, function (err) {
      var failed = V.pollFailed(task, err && err.code);
      if (task === 'stats') stats = V.statsFailed(stats, err && err.code, now());
      if (task === 'history') history = V.statsFailed(history, err && err.code, now());
      if (failed.schemaMissing) {
        // an older firmware without /api/schema: reachable, but there is no settings form
        schemaMissing = true;
        needSchema = false;
      }
      if (failed.connectionLost) conn = V.connectionFailed(conn, err && err.code, now());
      else conn = V.connectionOk(conn, now());
      if (failed.forceStatus) forceStatus = true; // do not let a failing settings or info request starve status
    }).then(finalize, finalize);
  }

  // --- commands
  // STOP always tries; a failure is reported in words (the page keeps no other state).
  function stop() {
    setClass($('stop'), 'is-sending', true); // the ring shows while the request is on the wire; STOP stays enabled
    client.command('stop').then(function () {
      setClass($('stop'), 'is-sending', false);
      setLeadIn(V.leadInCancel());
      say('Stop sent.', false);
      render();
      pollNow();
    }, function (err) {
      setClass($('stop'), 'is-sending', false);
      if (err && (err.code === 'network' || err.code === 'timeout')) {
        say('STOP was not confirmed by the grinder. Press STOP again and check the grinder. ' + (err.message || ''), true);
      } else {
        sayError(err, 'Stop could not be sent.');
      }
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
  $('save').addEventListener('click', saveSettings);
  $('discard').addEventListener('click', discardSettings);

  setText($('about-address'), window.location.host);
  showTab('grind');
  // The save bar wraps differently at another width: keep the scroll padding in step.
  window.addEventListener('resize', setSavebarHeight);

  // Polling pauses while the page is hidden and resumes at once when it is shown again.
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) schedule(null);
    else pollNow();
  });
  // The lead-in countdown and "last seen" move between polls.
  setInterval(function () {
    if (document.hidden) return;
    if (leadIn) setLeadIn(V.leadInTick(leadIn, status, now(), statusAt));
    render();
  }, 250);

  render();
  poll();
}());
