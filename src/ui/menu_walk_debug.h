#pragma once
#include "../config/constants.h"

#if DEBUG_MENU_WALK

class UIManager;
class StateMachine;

// Bench harness (...-menu-walk env only): ~8 s after boot an lv_timer walks every menu page, drives every
// setting widget through its real LVGL event and logs "[MENUWALK]" lines. Call once at the end of UIManager::init.
void menu_walk_start(UIManager* ui, StateMachine* state_machine);

#endif
