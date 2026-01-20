/* --------------------------------------------------------------------------
 * MID.1 – Color Picker (CP) Module – Header
 * --------------------------------------------------------------------------
 * Public API: call from keymap.c
 *  - cp_color_init()         : one-time init (keyboard_post_init_user)
 *  - cp_color_task()         : run while CP window is active (housekeeping gate)
 *  - cp_color_process(...)   : consume CP_* keycodes (process_record_user)
 *  - cp_color_active()       : CP owns LEDs while true
 *  - cp_color_selected()     : current selection index (0..11)
 *  - cp_color_reset_to_defaults() : reseed per-slot colors from MID.1 standards
 *
 * Style: 4-space indent, K&R braces, ~100-char wrap, thin bar section headers.
 * -------------------------------------------------------------------------- */

#pragma once

#include "quantum.h"

#ifdef RGBLIGHT_ENABLE
#    include "rgblight.h"
#endif

/* -- Public API ----------------------------------------------------------- */

void     cp_color_init(void);
void     cp_color_task(void);
bool     cp_color_process(uint16_t keycode, keyrecord_t *record);
bool     cp_color_active(void);
void     cp_color_ensure_active(void); /* activate CP & render current selection immediately */
void     cp_color_release(void);
void     cp_anim_preview_cancel(void); /* CP Anim preview (full-bar) control */

uint8_t  cp_color_selected(void);
void     cp_color_reset_to_defaults(void);

/* -- CP public color API for layers --------------------------------------- */
typedef struct { uint16_t h; uint8_t s; uint8_t v; } HSV16;

HSV16 cp_color_get_layer_hsv(uint8_t layer);
void  cp_color_set_layer_hsv(uint8_t layer, HSV16 hsv);
uint8_t cp_color_get_layer_anim(uint8_t layer);
void    cp_color_set_layer_anim(uint8_t layer, uint8_t mode, bool persist);

// Optional helpers used by the selector UI
void  cp_color_select_layer(uint8_t layer);       // remember which layer we're editing
void  cp_color_preview_draw(uint8_t layer, HSV16 hsv); // redraw the selector preview

// Caps styling accessors
HSV16   cp_color_get_caps_hsv(void);
void    cp_color_set_caps_hsv(HSV16 hsv);

uint8_t cp_color_get_caps_anim(void);
void    cp_color_set_caps_anim(uint8_t mode, bool persist);

/* -- HSV helpers (match 3-arg macros like HSV_MID1ORANGE -> h,s,v) -------- */

#ifndef H_OF
#    define H_OF(h, s, v) (h)
#endif
#ifndef S_OF
#    define S_OF(h, s, v) (s)
#endif
#ifndef V_OF
#    define V_OF(h, s, v) (v)
#endif

// Let us pass a single parenthesized tuple to H_OF/S_OF:

#define H_OF_TUPLE(T) H_OF T
#define S_OF_TUPLE(T) S_OF T
#define V_OF_TUPLE(T) V_OF T

#define HSV_LITERAL(T) (HSV_t){ H_OF T, S_OF T, V_OF T }

#ifdef SET_HS_KEEP_V
#    define SET_HS_KEEP_V_TUPLE(T) SET_HS_KEEP_V T
#endif
