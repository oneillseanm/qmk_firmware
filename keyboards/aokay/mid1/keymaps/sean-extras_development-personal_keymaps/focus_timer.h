#pragma once

#ifdef FOCUS_TIMER_ENABLE

void focus_timer_task(void);
bool focus_timer_process_keycode(uint16_t keycode, keyrecord_t *record);

#else

static inline void focus_timer_task(void) {}
static inline bool focus_timer_process_keycode(uint16_t keycode, keyrecord_t *record) {
    return true;
}

#endif

#include <stdint.h>
#include <stdbool.h>
#include "quantum.h"

/* -- Focus timer public API ----------------------------------------------------------- */

void focus_timer_task(void);
bool focus_timer_process_keycode(uint16_t keycode, keyrecord_t *record);