#pragma once

#include "quantum.h"  // for uint16_t, keyrecord_t

#if FOCUS_TIMER_ENABLE

void focus_timer_task(void);
bool focus_timer_process_keycode(uint16_t keycode, keyrecord_t *record);

#else

static inline void focus_timer_task(void) {}
static inline bool focus_timer_process_keycode(uint16_t keycode, keyrecord_t *record) {
    (void)keycode;
    (void)record;
    return true;
}

#endif