/* --------------------------------------------------------------------------
 * MID.1 – Keycodes (oneillseanm)
 * --------------------------------------------------------------------------
 * Contains all custom keycodes for:
 *  - Pomodoro timer (POMO_*)
 *  - Color Picker (CP_*)
 * Style: 4-space indent, K&R braces, ~100-char wrap, thin bar headers.
 * -------------------------------------------------------------------------- */

#pragma once

#ifdef __ASSEMBLER__
/* Assembly units should see nothing from this header */
#else

#ifndef SAFE_RANGE
#    include "quantum_keycodes.h"   // provides SAFE_RANGE for the enum
#endif


/* -- Custom keycodes (QMK will allocate from SAFE_RANGE) ------------------ */
enum custom_keycodes {
    /* Pomodoro timer keycodes --------------------------------------------- */
    POMO_TOGGLE = SAFE_RANGE,
    POMO_RESET,
    POMO_MODE_NEXT,
    POMO_WORK_DEC,
    POMO_WORK_INC,
    POMO_BREAK_DEC,
    POMO_BREAK_INC,

    /* Color Picker keycodes ----------------------------------------------- */
    CP_LAYER_DEC,
    CP_LAYER_INC,
    CP_HUE_DEC,
    CP_HUE_INC,
    CP_SAT_DEC,
    CP_SAT_INC,
    CP_ANIM_DEC,
    CP_ANIM_INC,
    CP_RESET,
    CP_BOOT_TOG,
    CP_BOOT_PLAY,
    NEW_SAFE_RANGE
};

// (Optional) If other modules depend on the next free range:
#ifndef DYNAMIC_SAFE_RANGE
#    define DYNAMIC_SAFE_RANGE NEW_SAFE_RANGE
#endif

#endif /* __ASSEMBLER__ */
