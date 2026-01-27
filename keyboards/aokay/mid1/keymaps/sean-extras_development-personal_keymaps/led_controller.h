#pragma once
#include "quantum.h"

typedef enum {
    LED_EDIT_MODE_NONE = 0,
    LED_EDIT_MODE_HUE,
    LED_EDIT_MODE_SAT,
    LED_EDIT_MODE_VAL,
    LED_EDIT_MODE_ANIM,
} led_edit_mode_t;

void led_controller_set_edit_mode(led_edit_mode_t mode);
led_edit_mode_t led_controller_get_edit_mode(void);

bool led_controller_process_keycode(uint16_t keycode, keyrecord_t *record);
void led_controller_task(void);

/* -- LED controller core -------------------------------------------------- */

void     led_controller_task(void);
bool     led_controller_process_keycode(uint16_t keycode, keyrecord_t *record);

/* -- LED controller public API -------------------------------------------- */

void     led_controller_init(void);
bool     led_controller_active(void);
void     led_controller_ensure_active(void);
void     led_controller_release(void);
void     led_controller_anim_preview_cancel(void);
void     led_controller_set_external_lock(bool on);
bool     led_controller_visual_lock_active(void);
uint8_t  led_controller_selected(void);
void     led_controller_reset_to_defaults(void);
extern bool led_controller_boot_anim_enabled;

/* Layer color / animation */
typedef struct { uint16_t h; uint8_t s; uint8_t v; } HSV16;

HSV16 led_controller_get_layer_hsv(uint8_t layer);
void  led_controller_set_layer_hsv(uint8_t layer, HSV16 hsv);
uint8_t led_controller_get_layer_anim(uint8_t layer);
void    led_controller_set_layer_anim(uint8_t layer, uint8_t mode, bool persist);

/* Selector helpers */
void  led_controller_select_layer(uint8_t layer);
void  led_controller_preview_draw(uint8_t layer, HSV16 hsv);

/* Caps styling */
HSV16   led_controller_get_caps_hsv(void);
void    led_controller_set_caps_hsv(HSV16 hsv);
uint8_t led_controller_get_caps_anim(void);
void    led_controller_set_caps_anim(uint8_t mode, bool persist);

/* -- LED controller preview / startup LEDs -------------------------------- */

/* Number of LEDs used for preview + startup animation */
#ifndef LED_CONTROLLER_PREVIEW_LEDS
#    define LED_CONTROLLER_PREVIEW_LEDS 8
#endif

/* 1 if preview LEDs are wired right-to-left */
#ifndef LED_CONTROLLER_LEDS_REVERSED
#    define LED_CONTROLLER_LEDS_REVERSED 1
#endif

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

#ifndef SET_HS_KEEP_V3
#  define SET_HS_KEEP_V3(h, s, v)                          \
    do {                                                   \
        uint8_t __v = rgblight_get_val();                  \
        rgblight_sethsv_noeeprom((h), (s), __v);           \
    } while (0)
#endif

#ifndef SET_HS_KEEP_V_TUPLE
#  define SET_HS_KEEP_V_TUPLE(T)  SET_HS_KEEP_V3 T
#endif

#define H_OF_TUPLE(T) H_OF T
#define S_OF_TUPLE(T) S_OF T
#define V_OF_TUPLE(T) V_OF T

#define HSV_LITERAL(T) (HSV_t){ H_OF T, S_OF T, V_OF T }

#ifdef SET_HS_KEEP_V
#    define SET_HS_KEEP_V_TUPLE(T) SET_HS_KEEP_V T
#endif

#ifndef LED_ALL_MASK
#    define LED_ALL_MASK ((uint8_t)0xFF)
#endif