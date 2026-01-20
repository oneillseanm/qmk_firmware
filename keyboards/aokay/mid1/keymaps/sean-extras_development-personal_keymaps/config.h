/* --------------------------------------------------------------------------
 * MID.1 – Config (oneillseanm)
 * --------------------------------------------------------------------------
 * RGB defaults, palette, and utility defines used by the keymap and modules.
 * Notes:
 *  - Values (h,s,v) are 0..359 / 0..255 / 0..255 as per QMK rgb_light.
 *  - These do not affect compilation order; they’re read by your code.
 * -------------------------------------------------------------------------- */

#pragma once

/* do not include unneeded animations */
#undef RGBLIGHT_EFFECT_SNAKE
#undef RGBLIGHT_EFFECT_CHRISTMAS
#undef RGBLIGHT_EFFECT_STATIC_GRADIENT
#undef RGBLIGHT_EFFECT_RGB_TEST
#undef RGBLIGHT_EFFECT_ALTERNATING
#undef RGBLIGHT_EFFECT_TWINKLE

/* -- RGB global defaults -------------------------------------------------- */
#define RGBLIGHT_DEFAULT_HUE    15
#define RGBLIGHT_DEFAULT_SAT    255
#define RGBLIGHT_DEFAULT_VAL    128
#define RGBLIGHT_DEFAULT_MODE   RGBLIGHT_MODE_STATIC_LIGHT
#undef RGBLIGHT_HUE_STEP
#define RGBLIGHT_HUE_STEP       5
#undef RGBLIGHT_LIMIT_VAL
#define RGBLIGHT_LIMIT_VAL      180
#define RGBLIGHT_SLEEP

/* -- MID.1 palette (HSV tuples) ------------------------------------------- */
#define HSV_MID1ORANGE  (15,  255, 255)
#define HSV_MID1BLUE    (145, 255, 255)
#define HSV_MID1GREEN   (85,  255, 255)
#define HSV_MID1PURPLE  (190, 255, 255)
#define HSV_MID1RED     (245, 255, 255)
#define HSV_MID1SAGE    (94,  122, 168)

/* Feature-specific colors */
#define HSV_POMO_BREAK  (0,   0,   255)
#define HSV_CP_DEFAULT  (0,   0,   255)

