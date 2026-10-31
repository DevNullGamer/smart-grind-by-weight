#include "menu_walk_debug.h"

#if DEBUG_MENU_WALK

// Test tooling, built only by the ...-menu-walk env (see docs/DESIGN-NOTES.md, chg006 item 23).
//
// One lv_timer on the UI task performs ONE action per tick (so layout settles in between):
//   1. switches to the MENU state and opens every page the walk covers (Scale and WiFi are skipped: Scale
//      tares, WiFi opens the BLE provisioning window) by sending LV_EVENT_CLICKED to the real root item;
//   2. logs a page fingerprint (object count + FNV-1a over hidden/checked/disabled flags, label text without
//      digits, slider values, button colours and geometry of visible non-label objects);
//   3. drives each setting widget through its real event path and checks NVS with a raw Preferences read that
//      uses hard-coded legacy namespace / key / type (deliberately independent of any settings facade);
//   4. restores every NVS value exactly (removing keys that did not exist), returns to the original UI state
//      and logs "[MENUWALK] DONE fail=<n>".
// BLE enable, WiFi and the board override are never touched; no grind or motor action is started.

#include <Arduino.h>
#include <Preferences.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include "../hardware/hardware_manager.h"
#include "../system/state_machine.h"
#include "components/blocking_overlay.h"
#include "ui_helpers.h"
#include "ui_manager.h"

// lv_menu is deprecated since LVGL 9.6 (same file-level suppression as the menu screen files)
LV_DEPRECATIONS_IGNORE_BEGIN

namespace {

constexpr uint32_t kStartAfterMs = 8000;
constexpr uint32_t kTickMs = 300;

//------------------------------------------------------------------------------
// NVS oracle: raw Preferences access with literal namespaces, keys and types
//------------------------------------------------------------------------------

enum class Kind { BOOL, INT, FLOAT };

struct NvsKey {
    const char* name;
    const char* ns;
    const char* key;
    Kind kind;
    float def;
};

enum KeyId {
    K_BLE_STARTUP, K_LOGGING, K_SWIPE, K_AUTO_START, K_AUTO_RETURN,
    K_BRIGHT_NORMAL, K_BRIGHT_SAVER, K_GRIND_MODE, K_PURGE_MODE, K_PURGE_AMOUNT, K_FRESHNESS,
    K_COUNT
};

const NvsKey kKeys[K_COUNT] = {
    {"ble_startup", "bluetooth", "startup", Kind::BOOL, 1.0f},
    {"logging", "logging", "enabled", Kind::BOOL, 0.0f},
    {"swipe", "swipe", "enabled", Kind::BOOL, 0.0f},
    {"auto_start", "autogrind", "auto_start", Kind::BOOL, 0.0f},
    {"auto_return", "autogrind", "auto_return", Kind::BOOL, 0.0f},
    {"brightness_normal", "brightness", "normal", Kind::FLOAT, 1.0f},
    {"brightness_screensaver", "brightness", "screensaver", Kind::FLOAT, 0.35f},
    {"grind_mode", "grinder", "grind_mode", Kind::INT, 0.0f},
    {"purge_mode", "grinder", "grinder_mode", Kind::INT, 1.0f},
    {"purge_amount_g", "grinder", "grinder_amt_g", Kind::FLOAT, 1.0f},
    {"freshness_hours", "grinder", "freshness_hrs", Kind::FLOAT, 8.0f},
};

struct RawVal {
    bool present = false;
    int type = -1;     // PreferenceType, -1 = absent
    float value = 0;   // bool / int / float as float
};

PreferenceType expected_type(Kind kind) {
    switch (kind) {
        case Kind::BOOL: return PT_U8;
        case Kind::INT: return PT_I32;
        default: return PT_BLOB;  // Preferences::putFloat stores 4 bytes as a blob
    }
}

RawVal raw_read(KeyId id) {
    const NvsKey& k = kKeys[id];
    RawVal r;
    r.value = k.def;
    Preferences p;
    if (!p.begin(k.ns, true)) return r;  // namespace does not exist yet
    r.present = p.isKey(k.key);
    if (r.present) {
        r.type = static_cast<int>(p.getType(k.key));
        switch (k.kind) {
            case Kind::BOOL: r.value = p.getBool(k.key, k.def != 0.0f) ? 1.0f : 0.0f; break;
            case Kind::INT: r.value = static_cast<float>(p.getInt(k.key, static_cast<int>(k.def))); break;
            case Kind::FLOAT: r.value = p.getFloat(k.key, k.def); break;
        }
    }
    p.end();
    return r;
}

// Puts the exact original back (or removes the key if it did not exist before the walk)
void raw_restore(KeyId id, const RawVal& orig) {
    const NvsKey& k = kKeys[id];
    Preferences p;
    if (!p.begin(k.ns, false)) return;
    if (!orig.present) {
        p.remove(k.key);
    } else {
        switch (k.kind) {
            case Kind::BOOL: p.putBool(k.key, orig.value != 0.0f); break;
            case Kind::INT: p.putInt(k.key, static_cast<int>(orig.value)); break;
            case Kind::FLOAT: p.putFloat(k.key, orig.value); break;
        }
    }
    p.end();
}

bool same_raw(const RawVal& a, const RawVal& b) {
    return a.present == b.present && (!a.present || (a.type == b.type && a.value == b.value));
}

//------------------------------------------------------------------------------
// Page fingerprint
//------------------------------------------------------------------------------

struct Fingerprint {
    uint32_t hash = 2166136261u;
    uint32_t geo = 2166136261u;
    int objs = 0;

    static void mix(uint32_t& h, const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; i++) {
            h ^= p[i];
            h *= 16777619u;
        }
    }
    static void mix_i32(uint32_t& h, int32_t v) { mix(h, &v, sizeof(v)); }
};

// Label text with all digits removed (and a minus sign directly in front of a digit)
void mix_text(uint32_t& h, const char* s) {
    for (; s && *s; ++s) {
        const char c = *s;
        if (c >= '0' && c <= '9') continue;
        if ((c == '-' || c == '+') && s[1] >= '0' && s[1] <= '9') continue;
        Fingerprint::mix(h, &c, 1);
    }
    const uint8_t end = 0xFF;
    Fingerprint::mix(h, &end, 1);
}

void fingerprint_obj(lv_obj_t* o, Fingerprint& f, bool hidden_above) {
    f.objs++;
    const bool hidden = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
    uint8_t tag = 0;
    uint8_t flags = static_cast<uint8_t>((hidden ? 1 : 0) | (lv_obj_has_state(o, LV_STATE_CHECKED) ? 2 : 0) |
                                         (lv_obj_has_state(o, LV_STATE_DISABLED) ? 4 : 0));
    const bool is_label = lv_obj_has_class(o, &lv_label_class);
    if (is_label) tag = 1;
    else if (lv_obj_has_class(o, &lv_switch_class)) tag = 2;
    else if (lv_obj_has_class(o, &lv_slider_class)) tag = 3;
    else if (lv_obj_has_class(o, &lv_button_class)) tag = 4;
    Fingerprint::mix(f.hash, &tag, 1);
    Fingerprint::mix(f.hash, &flags, 1);

    if (is_label) {
        mix_text(f.hash, lv_label_get_text(o));
    } else {
        if (tag == 3) Fingerprint::mix_i32(f.hash, lv_slider_get_value(o));
        if (tag == 4) Fingerprint::mix_i32(f.hash, static_cast<int32_t>(lv_color_to_u32(lv_obj_get_style_bg_color(o, LV_PART_MAIN))));
        // Geometry of visible non-label objects (label widths follow live text)
        if (!hidden && !hidden_above) {
            Fingerprint::mix_i32(f.geo, lv_obj_get_x(o));
            Fingerprint::mix_i32(f.geo, lv_obj_get_y(o));
            Fingerprint::mix_i32(f.geo, lv_obj_get_width(o));
            Fingerprint::mix_i32(f.geo, lv_obj_get_height(o));
        }
    }

    const uint32_t n = lv_obj_get_child_count(o);
    Fingerprint::mix_i32(f.hash, static_cast<int32_t>(n));
    for (uint32_t i = 0; i < n; i++) {
        fingerprint_obj(lv_obj_get_child(o, i), f, hidden_above || hidden);
    }
}

//------------------------------------------------------------------------------
// Walk program
//------------------------------------------------------------------------------

enum class Op {
    ENTER_MENU, FP_ROOT, OPEN, FP, FP_POST, BACK,
    TOGGLE, SLIDER_BRIGHTNESS, SLIDER_PURGE_AMOUNT, SLIDER_FRESHNESS, RADIO_GRIND_MODE, RADIO_PURGE_MODE,
    FINISH
};

struct Step {
    Op op;
    int arg;
};

struct PageDef {
    const char* item;   // label of the root list item
    const char* name;   // page name used in the log
};

const PageDef kPages[] = {
    {"System Info", "Info"},
    {"Bluetooth", "Bluetooth"},
    {"Display", "Display"},
    {"Grind Settings", "Grind Settings"},
    {"Hardware board", "Hardware board"},
    {"Logs & Data", "Logs & Data"},
    {"Lifetime Stats", "Lifetime Stats"},
    {"Diagnostics", "Diagnostics"},
};
constexpr int kPageCount = sizeof(kPages) / sizeof(kPages[0]);

const float kFreshnessStepsH[9] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 8.0f, 12.0f, 24.0f, 48.0f};

struct Walk {
    UIManager* ui = nullptr;
    StateMachine* sm = nullptr;
    lv_timer_t* timer = nullptr;
    std::vector<Step> program;
    size_t pc = 0;
    int fail = 0;
    bool waiting_logged = false;
    UIState original_state = UIState::READY;
    lv_obj_t* root_page = nullptr;
    RawVal original[K_COUNT];
};

Walk g_walk;

MenuScreen& menu_screen() { return g_walk.ui->menu_screen; }
lv_obj_t* menu_obj() { return menu_screen().get_tabview(); }

void check(bool ok, const char* what) {
    if (!ok) {
        g_walk.fail++;
        Serial.printf("[MENUWALK] FAIL %s\n", what);
    }
}

const char* verdict(bool ok) {
    if (!ok) g_walk.fail++;
    return ok ? "OK" : "FAIL";
}

lv_obj_t* find_root_item(const char* text) {
    lv_obj_t* root = g_walk.root_page;
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* item = lv_obj_get_child(root, i);
        if (lv_obj_get_child_count(item) == 0) continue;
        lv_obj_t* first = lv_obj_get_child(item, 0);
        if (lv_obj_has_class(first, &lv_label_class) && strcmp(lv_label_get_text(first), text) == 0) return item;
    }
    return nullptr;
}

const char* header_title() {
    lv_obj_t* header = lv_menu_get_main_header(menu_obj());
    lv_obj_t* title = header ? lv_obj_get_child(header, 1) : nullptr;
    return (title && lv_obj_has_class(title, &lv_label_class)) ? lv_label_get_text(title) : "?";
}

void log_fingerprint(const char* name, const char* suffix) {
    lv_obj_t* cur = lv_menu_get_cur_main_page(menu_obj());
    lv_obj_update_layout(lv_screen_active());
    Fingerprint f;
    fingerprint_obj(cur, f, false);
    Serial.printf("[MENUWALK] page=%s%s objs=%d hash=0x%08lx geo=0x%08lx title=\"%s\"\n", name, suffix, f.objs,
                  static_cast<unsigned long>(f.hash), static_cast<unsigned long>(f.geo), header_title());
}

// Logs "<setting> <what> raw=<v> ty=<t> [label=..] OK|FAIL" for one driven value
void log_check(KeyId id, const char* what, float expected, const char* label) {
    const RawVal r = raw_read(id);
    const bool ok = r.present && r.type == expected_type(kKeys[id].kind) && r.value == expected;
    const char* result = verdict(ok);
    if (label) {
        Serial.printf("[MENUWALK] %s %s raw=%g ty=%d label=\"%s\" %s\n", kKeys[id].name, what, static_cast<double>(r.value),
                      r.type, label, result);
    } else {
        Serial.printf("[MENUWALK] %s %s raw=%g ty=%d %s\n", kKeys[id].name, what, static_cast<double>(r.value), r.type,
                      result);
    }
}

void log_before(KeyId id) {
    const RawVal& o = g_walk.original[id];
    Serial.printf("[MENUWALK] %s before=%g present=%d ty=%d\n", kKeys[id].name, static_cast<double>(o.value),
                  o.present ? 1 : 0, o.type);
}

//------------------------------------------------------------------------------
// Setting drivers
//------------------------------------------------------------------------------

lv_obj_t* toggle_widget(int id) {
    MenuScreen& m = menu_screen();
    switch (id) {
        case K_BLE_STARTUP: return m.get_setting_toggle(ToggleSetting::BLE_STARTUP);
        case K_LOGGING: return m.get_setting_toggle(ToggleSetting::LOGGING);
        case K_SWIPE: return m.get_setting_toggle(ToggleSetting::SWIPE);
        case K_AUTO_START: return m.get_setting_toggle(ToggleSetting::AUTO_START);
        default: return m.get_setting_toggle(ToggleSetting::AUTO_RETURN);
    }
}

void set_toggle(lv_obj_t* sw, bool on) {
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else lv_obj_remove_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
}

// Flips the toggle and puts it back inside this one call (auto_start / auto_return are never enabled
// while update_auto_actions() could run: both happen on the UI task, outside this timer callback)
void drive_toggle(KeyId id) {
    lv_obj_t* sw = toggle_widget(id);
    if (!sw) { check(false, kKeys[id].name); return; }
    log_before(id);
    const bool ui_state = lv_obj_has_state(sw, LV_STATE_CHECKED);
    set_toggle(sw, !ui_state);
    log_check(id, ui_state ? "new=0" : "new=1", ui_state ? 0.0f : 1.0f, nullptr);
    set_toggle(sw, ui_state);
    log_check(id, ui_state ? "restored=1" : "restored=0", ui_state ? 1.0f : 0.0f, nullptr);
}

lv_obj_t* slider_label(lv_obj_t* slider) { return lv_obj_get_child(lv_obj_get_parent(slider), 0); }

void send_slider_events(lv_obj_t* slider, int value) {
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
}

float expect_brightness(int percent) {
    if (percent < 15) percent = 15;
    return percent / 100.0f;
}
float expect_purge(int units) { return units / 10.0f; }
float expect_freshness(int index) { return kFreshnessStepsH[index < 0 ? 0 : (index > 8 ? 8 : index)]; }

void drive_slider(KeyId id, lv_obj_t* slider, const int* values, int count, float (*expect)(int)) {
    if (!slider) { check(false, kKeys[id].name); return; }
    log_before(id);
    const int orig = lv_slider_get_value(slider);
    char what[32];
    for (int i = 0; i < count; i++) {
        send_slider_events(slider, values[i]);
        snprintf(what, sizeof(what), "set=%d", values[i]);
        log_check(id, what, expect(values[i]), lv_label_get_text(slider_label(slider)));
    }
    send_slider_events(slider, orig);
    snprintf(what, sizeof(what), "restored=%d", orig);
    log_check(id, what, expect(orig), lv_label_get_text(slider_label(slider)));
}

void click_radio(lv_obj_t* group, int index) {
    lv_obj_send_event(lv_obj_get_child(group, index), LV_EVENT_CLICKED, nullptr);
}

void drive_radio(KeyId id, lv_obj_t* group, int count, float (*expect)(int)) {
    if (!group) { check(false, kKeys[id].name); return; }
    log_before(id);
    const int sel = radio_button_group_get_selection(group);
    if (sel < 0) { check(false, "radio without selection"); return; }
    const int other = (sel + 1) % count;
    char what[32];
    click_radio(group, other);
    snprintf(what, sizeof(what), "click=%d", other);
    log_check(id, what, expect(other), nullptr);
    click_radio(group, sel);
    snprintf(what, sizeof(what), "restored=%d", sel);
    log_check(id, what, expect(sel), nullptr);
}

float expect_grind_mode(int index) { return index == 0 ? 0.0f : 1.0f; }  // GrindMode::WEIGHT = 0, TIME = 1
float expect_index(int index) { return static_cast<float>(index); }

//------------------------------------------------------------------------------
// Steps
//------------------------------------------------------------------------------

const char* state_name(UIState s) {
    switch (s) {
        case UIState::READY: return "READY";
        case UIState::CALIBRATION: return "CALIBRATION";
        default: return "OTHER";
    }
}

void take_snapshot() {
    for (int i = 0; i < K_COUNT; i++) g_walk.original[i] = raw_read(static_cast<KeyId>(i));
}

void finish() {
    // Exact restore of the NVS originals, then verify against the snapshot
    for (int i = 0; i < K_COUNT; i++) raw_restore(static_cast<KeyId>(i), g_walk.original[i]);
    for (int i = 0; i < K_COUNT; i++) {
        const RawVal now = raw_read(static_cast<KeyId>(i));
        const bool ok = same_raw(now, g_walk.original[i]);
        Serial.printf("[MENUWALK] final %s present=%d raw=%g ty=%d %s\n", kKeys[i].name, now.present ? 1 : 0,
                      static_cast<double>(now.value), now.type, verdict(ok));
    }
    // Live state: auto-action flags and display brightness follow NVS again
    g_walk.ui->refresh_auto_action_settings();
    float normal = g_walk.original[K_BRIGHT_NORMAL].value;
    if (normal < 0.15f) normal = 0.15f;
    if (auto* hw = g_walk.ui->get_hardware_manager()) {
        if (hw->get_display()) hw->get_display()->set_brightness(normal);
    }
    g_walk.ui->switch_to_state(g_walk.original_state);
    Serial.printf("[MENUWALK] DONE fail=%d\n", g_walk.fail);
}

void run_step(const Step& s) {
    switch (s.op) {
        case Op::ENTER_MENU:
            g_walk.ui->switch_to_state(UIState::MENU);
            g_walk.root_page = lv_menu_get_cur_main_page(menu_obj());
            check(g_walk.root_page != nullptr, "root page");
            break;
        case Op::FP_ROOT:
            check(lv_menu_get_cur_main_page(menu_obj()) == g_walk.root_page, "at root");
            log_fingerprint("Menu", "");
            break;
        case Op::OPEN: {
            check(lv_menu_get_cur_main_page(menu_obj()) == g_walk.root_page, "at root before open");
            lv_obj_t* item = find_root_item(kPages[s.arg].item);
            check(item != nullptr, kPages[s.arg].item);
            if (item) lv_obj_send_event(item, LV_EVENT_CLICKED, nullptr);
            break;
        }
        case Op::FP:
            check(lv_menu_get_cur_main_page(menu_obj()) != g_walk.root_page, "page opened");
            log_fingerprint(kPages[s.arg].name, "");
            break;
        case Op::FP_POST:
            log_fingerprint(kPages[s.arg].name, ".post");
            break;
        case Op::BACK:
            lv_obj_send_event(lv_menu_get_main_header_back_button(menu_obj()), LV_EVENT_CLICKED, nullptr);
            break;
        case Op::TOGGLE:
            drive_toggle(static_cast<KeyId>(s.arg));
            break;
        case Op::SLIDER_BRIGHTNESS: {
            static const int values[] = {5, 60, 100};  // 5 must clamp to 15 % and store 0.15
            const bool normal = (s.arg == K_BRIGHT_NORMAL);
            drive_slider(static_cast<KeyId>(s.arg),
                         normal ? menu_screen().get_brightness_normal_slider() : menu_screen().get_brightness_screensaver_slider(),
                         values, 3, expect_brightness);
            break;
        }
        case Op::SLIDER_PURGE_AMOUNT: {
            static const int values[] = {1, 13, 25};  // 0.1 g, 1.3 g, 2.5 g
            drive_slider(K_PURGE_AMOUNT, menu_screen().get_grinder_purge_amount_slider(), values, 3, expect_purge);
            break;
        }
        case Op::SLIDER_FRESHNESS: {
            static const int values[] = {0, 3, 8};  // 0.5 h, 3 h, 48 h
            drive_slider(K_FRESHNESS, menu_screen().get_grind_freshness_hours_slider(), values, 3, expect_freshness);
            break;
        }
        case Op::RADIO_GRIND_MODE:
            drive_radio(K_GRIND_MODE, menu_screen().get_grind_mode_radio_group(), 2, expect_grind_mode);
            break;
        case Op::RADIO_PURGE_MODE:
            drive_radio(K_PURGE_MODE, menu_screen().get_grinder_purge_mode_radio_group(), 2, expect_index);
            break;
        case Op::FINISH:
            finish();
            break;
    }
}

void build_program() {
    auto& p = g_walk.program;
    p.push_back({Op::ENTER_MENU, 0});
    p.push_back({Op::FP_ROOT, 0});
    for (int page = 0; page < kPageCount; page++) {
        p.push_back({Op::OPEN, page});
        p.push_back({Op::FP, page});
        bool has_settings = true;
        const char* name = kPages[page].name;
        if (strcmp(name, "Bluetooth") == 0) {
            p.push_back({Op::TOGGLE, K_BLE_STARTUP});
        } else if (strcmp(name, "Display") == 0) {
            p.push_back({Op::SLIDER_BRIGHTNESS, K_BRIGHT_NORMAL});
            p.push_back({Op::SLIDER_BRIGHTNESS, K_BRIGHT_SAVER});
        } else if (strcmp(name, "Grind Settings") == 0) {
            p.push_back({Op::TOGGLE, K_SWIPE});
            p.push_back({Op::TOGGLE, K_AUTO_START});
            p.push_back({Op::TOGGLE, K_AUTO_RETURN});
            p.push_back({Op::RADIO_GRIND_MODE, 0});
            p.push_back({Op::RADIO_PURGE_MODE, 0});
            p.push_back({Op::SLIDER_PURGE_AMOUNT, 0});
            p.push_back({Op::SLIDER_FRESHNESS, 0});
        } else if (strcmp(name, "Logs & Data") == 0) {
            p.push_back({Op::TOGGLE, K_LOGGING});
        } else {
            has_settings = false;
        }
        if (has_settings) p.push_back({Op::FP_POST, page});
        p.push_back({Op::BACK, page});
    }
    p.push_back({Op::FINISH, 0});
}

void walk_tick(lv_timer_t*) {
    Walk& w = g_walk;
    if (millis() < kStartAfterMs) return;
    if (BlockingOperationOverlay::getInstance().is_operation_active()) return;  // e.g. statistics loading

    if (w.pc == 0) {
        const UIState st = w.sm->get_current_state();
        if (st != UIState::READY && st != UIState::CALIBRATION) {
            if (!w.waiting_logged) {
                Serial.printf("[MENUWALK] WAIT state is not READY/CALIBRATION\n");
                w.waiting_logged = true;
            }
            return;
        }
        w.original_state = st;
        take_snapshot();
        Serial.printf("[MENUWALK] START state=%s\n", state_name(st));
    }

    run_step(w.program[w.pc]);
    w.pc++;
    if (w.pc >= w.program.size()) {
        lv_timer_delete(w.timer);
        w.timer = nullptr;
    }
}

}  // namespace

void menu_walk_start(UIManager* ui, StateMachine* state_machine) {
    if (!ui || !state_machine || g_walk.timer) return;
    g_walk.ui = ui;
    g_walk.sm = state_machine;
    build_program();
    g_walk.timer = lv_timer_create(walk_tick, kTickMs, nullptr);
    Serial.printf("[MENUWALK] armed, %u steps\n", static_cast<unsigned>(g_walk.program.size()));
}

LV_DEPRECATIONS_IGNORE_END

#endif  // DEBUG_MENU_WALK
