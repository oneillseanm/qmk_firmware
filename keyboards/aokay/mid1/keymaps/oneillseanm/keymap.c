#include QMK_KEYBOARD_H

#ifdef RGBLIGHT_ENABLE
#include "rgblight.h"                     // ensure rgblight symbols are visible
extern rgblight_config_t rgblight_config; // global config (for .val)

//
// --- helpers to keep current brightness (V) when reapplying layer color ---
//
#define H_OF(h, s, v) (h)
#define S_OF(h, s, v) (s)

#define SET_HS_KEEP_V(HSV_MACRO)                                           \
    do {                                                                    \
        uint8_t __v = rgblight_config.val;                                  \
        rgblight_sethsv_noeeprom(H_OF(HSV_MACRO), S_OF(HSV_MACRO), __v);    \
    } while (0)

//
// --- Caps Lock: fast Rainbow Swirl (mode switch only; no color changes) ---
//
static bool    caps_active    = false;
static uint8_t caps_prev_mode = 0;

static void caps_effect_set(bool on) {
    if (on && !caps_active) {
        caps_active    = true;
        caps_prev_mode = rgblight_get_mode();
        rgblight_mode_noeeprom(RGBLIGHT_MODE_RAINBOW_SWIRL + 5); // fast swirl
    } else if (!on && caps_active) {
        caps_active = false;
        rgblight_mode_noeeprom(caps_prev_mode);                  // restore previous mode
    }
}

bool led_update_user(led_t led_state) {
    caps_effect_set(led_state.caps_lock);
    return true;
}

//
// --- Your layer coloring (now keeps brightness sticky) ---
//
static uint8_t last_layer = 0xFF;

layer_state_t layer_state_set_user(layer_state_t state) {
    uint8_t layer = get_highest_layer(state);
    if (layer != last_layer) {
        last_layer = layer;
        switch (layer) {
            case 0: SET_HS_KEEP_V(HSV_MID1ORANGE); break;
            case 1: SET_HS_KEEP_V(HSV_MID1BLUE);   break;
            case 2: SET_HS_KEEP_V(HSV_MID1GREEN);  break;
            case 3: SET_HS_KEEP_V(HSV_MID1PURPLE); break;
            default: SET_HS_KEEP_V(HSV_WHITE);     break;
        }
    }
    return state;
}

void keyboard_post_init_user(void) {
    // Ensure RGB is enabled and static at boot
    rgblight_enable_noeeprom();
    rgblight_mode_noeeprom(RGBLIGHT_MODE_STATIC_LIGHT);

    // Boot color, but keep whatever brightness is set
    SET_HS_KEEP_V(HSV_MID1ORANGE);

    // Apply Caps effect immediately if host starts with Caps on
    caps_effect_set(host_keyboard_led_state().caps_lock);
}

#endif  // RGBLIGHT_ENABLE
