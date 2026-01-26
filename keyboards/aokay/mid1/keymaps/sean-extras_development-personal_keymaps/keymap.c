#include QMK_KEYBOARD_H
#include "keymap_defines.h"
#include "mid1_custom_keycodes.h"
#include "config.h"
#include "led_controller.h"
#include "focus_timer.h"

void housekeeping_task_user(void) {
    focus_timer_task();
}

void matrix_scan_user(void) {
    led_controller_task();
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (!led_controller_process_keycode(keycode, record)) {
        return false;
    }
    return focus_timer_process_keycode(keycode, record);
}
