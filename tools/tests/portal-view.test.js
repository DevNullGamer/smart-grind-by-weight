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
