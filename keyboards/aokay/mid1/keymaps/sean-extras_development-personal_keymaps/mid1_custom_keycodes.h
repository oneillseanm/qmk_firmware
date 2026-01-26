/* --------------------------------------------------------------------------
 * MID.1 – Keycodes
 * --------------------------------------------------------------------------
 * Contains all custom keycodes for:
 *  - Focus timer (FOCUS_*)
 *  - LED controller (LED_CONTROLLER_*)
 * -------------------------------------------------------------------------- */

#pragma once

#ifdef __ASSEMBLER__
#else

#ifndef SAFE_RANGE
#    include "quantum_keycodes.h"
#endif


enum custom_keycodes {
    FOCUS_TOGGLE = SAFE_RANGE,
    FOCUS_RESET,
    FOCUS_MODE_NEXT,
    FOCUS_WORK_DEC,
    FOCUS_WORK_INC,
    FOCUS_BREAK_DEC,
    FOCUS_BREAK_INC,

    LED_CONTROLLER_LAYER_DEC,
    LED_CONTROLLER_LAYER_INC,
    LED_CONTROLLER_HUE_DEC,
    LED_CONTROLLER_HUE_INC,
    LED_CONTROLLER_SAT_DEC,
    LED_CONTROLLER_SAT_INC,
    LED_CONTROLLER_ANIM_DEC,
    LED_CONTROLLER_ANIM_INC,
    LED_CONTROLLER_RESET,
    LED_CONTROLLER_BOOT_TOG,
    LED_CONTROLLER_BOOT_PLAY,
    NEW_SAFE_RANGE
};

#ifndef DYNAMIC_SAFE_RANGE
#    define DYNAMIC_SAFE_RANGE NEW_SAFE_RANGE
#endif

#endif