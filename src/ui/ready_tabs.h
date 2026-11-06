#pragma once
#include <stdint.h>
#include "../config/user.h"

// Page layout of the READY tabview (chg020, docs/DESIGN-NOTES.md "GaggiMate link (chg020)"). Pure: no LVGL.
//
// Pages are Single, Double, Custom, then the PROFILE page (the GaggiMate Profile Dose) only once a dose has been set,
// then MENU last. A page index below the menu index is the profile index, so the PROFILE page is tab 3 and MENU moves
// from tab 3 to tab 4 when it appears. Ask for the menu index with these names; never use a number.

constexpr int READY_MENU_TAB_INDEX = USER_PROFILE_COUNT;                  // MENU page while the PROFILE page exists
constexpr int READY_MENU_TAB_INDEX_NO_DOSE = USER_BASE_PROFILE_COUNT;     // MENU page without it

// A screen shown by the rebuild must not be touched: the READY screen is rebuilt only after this long without input
constexpr uint32_t READY_REBUILD_IDLE_MS = 400;

constexpr int ready_menu_tab_index(bool dose_page) {
    return dose_page ? READY_MENU_TAB_INDEX : READY_MENU_TAB_INDEX_NO_DOSE;
}

// The page to show after the PROFILE page appears or disappears: MENU stays MENU (its index changes), a profile page
// keeps its index, and a profile page that no longer exists falls back to fallback_profile (itself clamped to the
// remaining profile pages).
constexpr int ready_remap_tab(int tab, bool old_dose_page, bool new_dose_page, int fallback_profile) {
    const int new_menu = ready_menu_tab_index(new_dose_page);
    if (tab == ready_menu_tab_index(old_dose_page)) return new_menu;
    if (tab >= 0 && tab < new_menu) return tab;
    return (fallback_profile >= 0 && fallback_profile < new_menu) ? fallback_profile : 0;
}
