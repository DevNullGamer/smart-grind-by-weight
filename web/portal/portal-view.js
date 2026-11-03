// View helpers of the web panel (pure functions, no DOM, no network), inlined by tools/portal/build_page.py and
// loaded unchanged by the node tests (tools/tests/portal-view.test.js, from step 2 on). All logic that node can
// test lives here or in portal-client.js; portal-app.js only wires the DOM.
'use strict';

(function (root, factory) {
    if (typeof module !== 'undefined' && module.exports) {
        module.exports = factory();
    } else {
        root.PortalView = factory();
    }
}(typeof self !== 'undefined' ? self : this, function () {

    // The sections of the page in tab order. Insights stays hidden until the firmware has data for it.
    var TABS = ['grind', 'settings', 'insights'];

    // A weight in grams for the big readout: one decimal, "--" while the grinder reports none.
    function formatWeight(grams) {
        if (typeof grams !== 'number' || !isFinite(grams)) return '-- g';
        return grams.toFixed(1) + ' g';
    }

    // Which tab to show for the tab the user picked. From 900 px the live pane sits beside the tabs, so the Grind tab
    // does not exist there and Settings takes its place; an unknown name also falls back to Grind or Settings.
    function tabForViewport(requested, isWide) {
        var name = TABS.indexOf(requested) >= 0 ? requested : 'grind';
        if (name === 'grind' && isWide) return 'settings';
        return name;
    }

    return {
        TABS: TABS,
        tabForViewport: tabForViewport,
        formatWeight: formatWeight
    };
}));
