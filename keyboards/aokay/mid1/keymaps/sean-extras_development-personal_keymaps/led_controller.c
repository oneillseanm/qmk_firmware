/*
 * LED Controller
 *
 * This module implements a mode-driven LED editing system for MID.1.
 *
 * Key concepts:
 * - Editing behavior is controlled by explicit edit modes (not layers).
 * - Selection refers to a style target (keyboard layers + Caps slot).
 * - Encoders and keys emit generic commands; behavior depends on edit mode.
 * - This module has no dependencies on keymap layers or encoder mapping.
 *
 * LED ownership rules:
 * - Startup animation owns LEDs while active.
 * - LED controller owns LEDs while edit mode != NONE.
 * - rgblight owns LEDs otherwise.
 */

#include QMK_KEYBOARD_H
#include "mid1_custom_keycodes.h"
#include "led_controller.h"
#include "eeconfig.h"

#ifdef RGBLIGHT_ENABLE
#    include "rgblight.h"
#    include <avr/eeprom.h>
#endif

#define LED_CONTROLLER_EEPROM_MAGIC 0xC1

/* Configuration */

/* Structural requirements */
#ifndef RGBLIGHT_ENABLE
#    error "LED controller requires RGBLIGHT_ENABLE."
#endif

/* Style targets: keyboard layers plus a Caps slot. */
#ifndef LED_CONTROLLER_NUM_LAYERS
#    define LED_CONTROLLER_NUM_LAYERS                    16
#endif

/* Caps styling slot (treated as a style target). */
#define LED_CONTROLLER_CAPS_SLOT         LED_CONTROLLER_NUM_LAYERS
#define LED_CONTROLLER_NUM_STYLE_TARGETS (LED_CONTROLLER_NUM_LAYERS + 1)

/* Behavior / policy */
#define LED_CONTROLLER_IDLE_TIMEOUT_MS                 4000
#define LED_CONTROLLER_TRIPLE_BLINK_MS                  450
#define LED_CONTROLLER_SAT_STEP                          12

/* Layout */
#define LED_CONTROLLER_PREVIEW_LEDS                       8
#define LED_CONTROLLER_PREVIEW_OFFSET                     0
#define LED_CONTROLLER_LEDS_REVERSED                      1

/* Preview-only dimming for layers with inherited style */
#define LED_CONTROLLER_INHERITED_DIM_SCALE              70  /* 0–255 */

/* Base layer (layer 0) defaults */
#define LED_CONTROLLER_BASE_H                            15
#define LED_CONTROLLER_BASE_S                           255
#define LED_CONTROLLER_BASE_V                           255
#define LED_CONTROLLER_BASE_ANIM RGBLIGHT_MODE_STATIC_LIGHT

/* Reset confirmation blink */
#define LED_CONTROLLER_RESET_BLINK_COUNT                  2
#define LED_CONTROLLER_RESET_BLINK_MS                   120

#define EE_BASE_H_SHIFT    0
#define EE_BASE_S_SHIFT    9
#define EE_BASE_ANIM_SHIFT 17

#define EE_BASE_H_MASK     0x1FFu
#define EE_BASE_S_MASK     0xFFu
#define EE_BASE_ANIM_MASK  0x7Fu

static led_edit_mode_t current_edit_mode = LED_EDIT_MODE_NONE;

static inline uint8_t led_controller_slot_to_led(uint8_t slot);

void led_controller_set_edit_mode(led_edit_mode_t mode) {
    current_edit_mode = mode;
}

led_edit_mode_t led_controller_get_edit_mode(void) {
    return current_edit_mode;
}

static void boot_anim_load_config(void);

static void led_controller_anim_preview_end(void);

static void led_controller_anim_preview_end(void) {
    led_controller_anim_preview_cancel();
}

#ifndef FOCUS_TIMER_ENABLE
#    define FOCUS_TIMER_ENABLE 0
#endif

#if !FOCUS_TIMER_ENABLE

void focus_timer_task(void) {
}

bool focus_timer_process_keycode(uint16_t keycode, keyrecord_t *record) {
    return true;
}

#endif

#define EE_UNINIT_BYTE 0xFF

#ifdef EECONFIG_USERSPACE
#    define EE_LED_CONTROLLER_BOOT_ANIM_ENABLED (EECONFIG_USERSPACE)
#endif

bool led_controller_process(uint16_t keycode, keyrecord_t *record);

bool     led_controller_boot_anim_enabled = true;
static bool     startup_anim_active       = false;
static bool     startup_anim_was_active   = false;
static uint32_t startup_anim_started_at   = 0;

static bool caps_active    = false;
static bool caps_suspended = false;

static bool style_set[LED_CONTROLLER_NUM_STYLE_TARGETS];

static HSV16 store[LED_CONTROLLER_NUM_STYLE_TARGETS];

static void boot_anim_load_config(void) {
#ifdef EE_LED_CONTROLLER_BOOT_ANIM_ENABLED
    uint8_t raw = eeprom_read_byte((void *)EE_LED_CONTROLLER_BOOT_ANIM_ENABLED);

    if (raw == EE_UNINIT_BYTE) {
        led_controller_boot_anim_enabled = true;
        eeprom_update_byte((void *)EE_LED_CONTROLLER_BOOT_ANIM_ENABLED, 1);
    } else {
        led_controller_boot_anim_enabled = (raw != 0);
    }
#else
    led_controller_boot_anim_enabled = true;
#endif
}

static bool external_visual_lock = false;

void led_controller_set_external_lock(bool on) {
    external_visual_lock = on;
}

bool led_controller_visual_lock_active(void) {
    return external_visual_lock || led_controller_active();
}

static void caps_effect_set(bool on) {
    if (led_controller_visual_lock_active()) {

        if (on) {
            caps_suspended = true;
        } else {
            caps_suspended = false;
        }
        return;
    }

    if (on && !caps_active) {
        caps_active = true;

        HSV16   hsv  = led_controller_get_caps_hsv();
        uint8_t val  = rgblight_get_val();
        uint8_t mode = led_controller_get_caps_anim();

        rgblight_enable_noeeprom();
        rgblight_mode_noeeprom(mode);
        rgblight_sethsv_noeeprom(hsv.h, hsv.s, val);

    } else if (!on && caps_active) {
        caps_active = false;

        uint8_t layer = get_highest_layer(layer_state);
        HSV16   base  = led_controller_get_layer_hsv(layer);
        uint8_t val   = rgblight_get_val();
        uint8_t mode  = led_controller_get_layer_anim(layer);

        rgblight_enable_noeeprom();
        rgblight_mode_noeeprom(mode);
        rgblight_sethsv_noeeprom(base.h, base.s, val);
    }
}

layer_state_t layer_state_set_user(layer_state_t state) {
    uint8_t layer = get_highest_layer(state);
    static uint8_t last_layer = LED_ALL_MASK;

    if (startup_anim_active || caps_active) {
        last_layer = layer;
        return state;
    }

    if (layer != last_layer) {
        last_layer = layer;

        HSV16  c    = led_controller_get_layer_hsv(layer);
        uint8_t v   = rgblight_get_val();
        uint8_t mode= led_controller_get_layer_anim(layer);
        rgblight_mode_noeeprom(mode);
        rgblight_sethsv_noeeprom(c.h, c.s, v);
    }
    return state;
}

bool led_update_user(led_t state) {
    caps_effect_set(state.caps_lock);
    return true;
}

/* Encoders emit selection/parameter commands while an edit mode is active. */
bool encoder_update_user(uint8_t index, bool clockwise) {

    if (led_controller_get_edit_mode() != LED_EDIT_MODE_NONE) {
        bool left = (index == 0);

        if (left) {
            tap_code(clockwise
                     ? LED_CONTROLLER_LAYER_INC
                     : LED_CONTROLLER_LAYER_DEC);
        } else {
            tap_code(clockwise
                     ? LED_CONTROLLER_PARAM_INC
                     : LED_CONTROLLER_PARAM_DEC);
        }
        return false;
    }

    return true;
}

#ifndef LED_CONTROLLER_STARTUP_LED_COUNT
#    define LED_CONTROLLER_STARTUP_LED_COUNT LED_CONTROLLER_PREVIEW_LEDS
#endif

#ifndef LED_CONTROLLER_STARTUP_LED_FIRST
#    define LED_CONTROLLER_STARTUP_LED_FIRST 0
#endif

#define STARTUP_ROLL_MS          400u
#define STARTUP_ROLL_COUNT       8u
#define STARTUP_STEPS_PER_ROLL   LED_CONTROLLER_STARTUP_LED_COUNT
#define STARTUP_TOTAL_STEPS      (STARTUP_ROLL_COUNT * STARTUP_STEPS_PER_ROLL)

static void startup_anim_begin(void) {
    startup_anim_active     = true;
    startup_anim_started_at = timer_read32();
}

static void startup_anim_tick(void) {
    const uint32_t total_ms = STARTUP_ROLL_MS * STARTUP_ROLL_COUNT;
    uint32_t       t        = timer_elapsed32(startup_anim_started_at);

    if (t >= total_ms) {
        startup_anim_active = false;
        return;
    }

    uint32_t step_ms = STARTUP_ROLL_MS / STARTUP_STEPS_PER_ROLL;
    if (step_ms == 0) step_ms = 1;

    uint32_t step = t / step_ms;
    if (step >= STARTUP_TOTAL_STEPS) step = STARTUP_TOTAL_STEPS - 1;

    uint8_t roll    = (uint8_t)(step / STARTUP_STEPS_PER_ROLL);  /* 0..7 */
    uint8_t pos     = (uint8_t)(step % STARTUP_STEPS_PER_ROLL);  /* 0..5 */
    uint8_t pos_rev = (uint8_t)(STARTUP_STEPS_PER_ROLL - 1u - pos);  /* reverse direction */

    uint8_t target = rgblight_get_val();
    HSV16 hsv = store[0];
    hsv.v = 0;

    if (roll < (STARTUP_ROLL_COUNT - 1u)) {
        uint16_t base_scaled = (uint16_t)target * (uint16_t)roll / (STARTUP_ROLL_COUNT); /* 0.. ~7/8 */
        uint16_t hi_scaled   = base_scaled + (uint16_t)target / 3u;
        if (hi_scaled > target) hi_scaled = target;

        uint8_t baseline_v  = (uint8_t)base_scaled;
        uint8_t highlight_v = (uint8_t)hi_scaled;

        for (uint8_t i = 0; i < LED_CONTROLLER_STARTUP_LED_COUNT; i++) {
            uint8_t bit_index = LED_CONTROLLER_LEDS_REVERSED ? i : (LED_CONTROLLER_STARTUP_LED_COUNT - 1 - i);
            uint8_t led_index = LED_CONTROLLER_STARTUP_LED_FIRST + i;
            uint8_t v         = (bit_index == pos_rev) ? highlight_v : baseline_v;

            hsv.v = v;
            rgblight_sethsv_at(hsv.h, hsv.s, hsv.v, led_index);
        }
    } else {
        uint8_t baseline_v  = (uint8_t)((uint16_t)target * 3u / 4u);
        uint8_t highlight_v = target;

        for (uint8_t i = 0; i < LED_CONTROLLER_STARTUP_LED_COUNT; i++) {
            uint8_t bit_index = LED_CONTROLLER_LEDS_REVERSED ? i : (LED_CONTROLLER_STARTUP_LED_COUNT - 1 - i);
            uint8_t led_index = LED_CONTROLLER_STARTUP_LED_FIRST + i;

            uint8_t v = (bit_index >= pos_rev) ? highlight_v : baseline_v;

            hsv.v = v;
            rgblight_sethsv_at(hsv.h, hsv.s, hsv.v, led_index);
        }
    }
}

void keyboard_post_init_user(void) {
    rgblight_enable_noeeprom();
    rgblight_mode_noeeprom(RGBLIGHT_MODE_STATIC_LIGHT);
    rgblight_sethsv_noeeprom HSV_MID1ORANGE;
    boot_anim_load_config();
    led_controller_init();
    caps_effect_set(host_keyboard_led_state().caps_lock);
    if (led_controller_boot_anim_enabled) {
        startup_anim_begin();
    }
}

static void led_controller_set_all_preview_leds(HSV16 hsv) {
    for (uint8_t i = 0; i < LED_CONTROLLER_PREVIEW_LEDS; i++) {
        uint8_t led = led_controller_slot_to_led(i);
        rgblight_sethsv_at(hsv.h, hsv.s, hsv.v, led);
    }
}

bool led_controller_process_keycode(uint16_t keycode, keyrecord_t *record) {

    switch (keycode) {
        case LED_EDIT_HUE:
            if (record->event.pressed) {
                led_controller_set_edit_mode(LED_EDIT_MODE_HUE);
            } else {
                led_controller_set_edit_mode(LED_EDIT_MODE_NONE);
            }
            return false;

        case LED_EDIT_SAT:
            if (record->event.pressed) {
                led_controller_set_edit_mode(LED_EDIT_MODE_SAT);
            } else {
                led_controller_set_edit_mode(LED_EDIT_MODE_NONE);
            }
            return false;

        case LED_EDIT_VAL:
            if (record->event.pressed) {
                led_controller_set_edit_mode(LED_EDIT_MODE_VAL);
            } else {
                led_controller_set_edit_mode(LED_EDIT_MODE_NONE);
            }
            return false;

        case LED_EDIT_ANIM:
            if (record->event.pressed) {
                led_controller_set_edit_mode(LED_EDIT_MODE_ANIM);
            } else {
                led_controller_set_edit_mode(LED_EDIT_MODE_NONE);
            }
            return false;
    }

    if (!led_controller_process(keycode, record)) return false;

    if (!record->event.pressed) return true;

    switch (keycode) {

        case LED_CONTROLLER_BOOT_TOG: {
            led_controller_boot_anim_enabled = !led_controller_boot_anim_enabled;

        #ifdef EE_LED_CONTROLLER_BOOT_ANIM_ENABLED
            eeprom_update_byte(
                (void *)EE_LED_CONTROLLER_BOOT_ANIM_ENABLED,
                led_controller_boot_anim_enabled ? 1 : 0
            );
        #endif

            HSV16 hsv = store[0];
            hsv.v = rgblight_get_val();

            if (!led_controller_boot_anim_enabled) {
                hsv.v = 0;
            }

            led_controller_set_all_preview_leds(hsv);

            return false;
        }

        case LED_CONTROLLER_BOOT_PLAY:
            /* Play the startup animation on demand */
            if (!startup_anim_active) {
                startup_anim_begin();
            }
            return false;
    }

    return true;
}

static bool style_set[LED_CONTROLLER_NUM_STYLE_TARGETS];

static inline uint8_t led_controller_slot_to_led(uint8_t slot) {
    return LED_CONTROLLER_PREVIEW_OFFSET + (LED_CONTROLLER_LEDS_REVERSED ? (LED_CONTROLLER_PREVIEW_LEDS - 1u - slot) : slot);
}

static uint8_t anim_store[LED_CONTROLLER_NUM_STYLE_TARGETS];
static bool led_controller_dirty = false;

static inline HSV16 led_controller_resolve_hsv(uint8_t target, bool *inherited) {
    if (target != 0 && !style_set[target]) {
        if (inherited) *inherited = true;
        return store[0];
    }

    if (inherited) *inherited = false;
    return store[target];
}

static inline uint8_t led_controller_breathe_delta(uint16_t period_ms, uint8_t amp) {
    uint32_t t = timer_read32() % (period_ms ? period_ms : 1);
    uint32_t half = (period_ms ? period_ms : 1) / 2;
    uint32_t up = (t <= half) ? t : ((period_ms ? period_ms : 1) - t);
    return (uint8_t)((amp * up) / (half ? half : 1));
}

typedef struct {
    bool     active;
    uint8_t  layer;
    uint8_t  mode;
    uint8_t  saved_mode;
    uint32_t start_ms;
} led_controller_anim_preview_t;

static led_controller_anim_preview_t led_controller_prev = {0};

#ifndef LED_CONTROLLER_ANIM_PREVIEW_MS
#    define LED_CONTROLLER_ANIM_PREVIEW_MS 3000u
#endif

static inline void led_controller_anim_preview_begin(uint8_t layer, uint8_t mode) {
    led_controller_prev.layer      = layer;
    led_controller_prev.mode       = mode;
    led_controller_prev.saved_mode = rgblight_get_mode();
    led_controller_prev.start_ms   = timer_read32();
    led_controller_prev.active     = true;

    /* Temporarily hand control to rgblight engine. */
    rgblight_set_effect_range(0, RGBLIGHT_LED_COUNT);

    /* Set base HSV so animations inherit hue/sat. */
    HSV16 c = led_controller_resolve_hsv(layer, NULL);
    c.v = rgblight_get_val();

    rgblight_sethsv_noeeprom(c.h, c.s, c.v);
    rgblight_mode_noeeprom(mode);
}

void led_controller_anim_preview_cancel(void) {
    if (!led_controller_prev.active) return;

    led_controller_prev.active = false;

    /* Return to static so the engine stops animating. */
    rgblight_mode_noeeprom(RGBLIGHT_MODE_STATIC_LIGHT);

    /* Restrict engine output so the controller can draw masks. */
    rgblight_set_effect_range(0, 0);
}

static inline void led_controller_anim_preview_tick(void) {
    if (!led_controller_prev.active) return;
    if (timer_elapsed32(led_controller_prev.start_ms) >= LED_CONTROLLER_ANIM_PREVIEW_MS) {
        led_controller_anim_preview_cancel();
    }
}

/* Preview LED masks for layer slots. */
static const uint8_t LED_CONTROLLER_PATS[16] = {
    0b10000001, /* 0 */
    0b10000000, /* 1 */
    0b11000000, /* 2 */
    0b11100000, /* 3 */
    0b11110000, /* 4 */
    0b11111000, /* 5 */
    0b11111100, /* 6 */
    0b11111110, /* 7 */
    0b11111111, /* 8 */
    0b01111111, /* 9 */
    0b00111111, /* 10 */
    0b00011111, /* 11 */
    0b00001111, /* 12 */
    0b00000111, /* 13 */
    0b00000011, /* 14 */
    0b00000001  /* 15 */
};

/* Return the preview LED mask for a style target selection. */
static inline uint8_t led_controller_mask_for_sel(uint8_t sel) {
    if (sel < LED_CONTROLLER_NUM_LAYERS) {
        return LED_CONTROLLER_PATS[sel];
    }
    return 0b00011000;
}

static HSV16   work;
static uint8_t sel      = 0; /* style target selection */
static bool    active   = false;
static uint32_t last_ms = 0;

#ifndef LED_CONTROLLER_EE_KEY
#    define LED_CONTROLLER_EE_KEY 0xC35A
#endif

#ifndef LED_CONTROLLER_EE_BASE
#    define LED_CONTROLLER_EE_BASE (EECONFIG_USER)
#endif

#define LED_CONTROLLER_STRIDE 4 /* h(2) + s(1) + v(1) */

static inline void mark_active(void) { last_ms = timer_read32(); }
static inline bool window_expired(void) { return timer_elapsed32(last_ms) > LED_CONTROLLER_IDLE_TIMEOUT_MS; }

static inline int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}
static inline uint8_t clamp255(int v) { return (uint8_t)clampi(v, 0, 255); }
static inline uint16_t wrap360(int v) {
    while (v < 0) v += 360;
    while (v >= 360) v -= 360;
    return (uint16_t)v;
}

static void set_mask_with_hsv(uint8_t mask, HSV16 s) {
    for (uint8_t i = 0; i < LED_CONTROLLER_PREVIEW_LEDS; i++) {
        bool on = (mask >> (LED_CONTROLLER_PREVIEW_LEDS - 1u - i)) & 1;
        uint8_t led = led_controller_slot_to_led(i);
        rgblight_sethsv_at(on ? s.h : 0, on ? s.s : 0, on ? s.v : 0, led);
    }
}

#define LED_CONTROLLER_SLOT_BYTES 4u
static void ee_save(void) {
    uint32_t v = 0;

    v |= ((uint32_t)(store[0].h & EE_BASE_H_MASK)) << EE_BASE_H_SHIFT;
    v |= ((uint32_t)(store[0].s & EE_BASE_S_MASK)) << EE_BASE_S_SHIFT;
    v |= ((uint32_t)(anim_store[0] & EE_BASE_ANIM_MASK)) << EE_BASE_ANIM_SHIFT;

    eeconfig_update_user(v);
}

static bool ee_load(void) {
    uint32_t v = eeconfig_read_user();
    if (v == 0) return false;

    store[0].h = (v >> EE_BASE_H_SHIFT) & EE_BASE_H_MASK;
    store[0].s = (v >> EE_BASE_S_SHIFT) & EE_BASE_S_MASK;
    store[0].v = LED_CONTROLLER_BASE_V;

    anim_store[0] = (v >> EE_BASE_ANIM_SHIFT) & EE_BASE_ANIM_MASK;
    if (anim_store[0] == 0)
        anim_store[0] = LED_CONTROLLER_BASE_ANIM;

    style_set[0] = true;
    for (uint8_t i = 1; i < LED_CONTROLLER_NUM_STYLE_TARGETS; i++) {
        style_set[i] = false;
    }

    return true;
}

HSV16 led_controller_get_layer_hsv(uint8_t layer) {
    if (layer >= LED_CONTROLLER_NUM_LAYERS) layer = 0;
    return led_controller_resolve_hsv(layer, NULL);
}

void led_controller_set_layer_hsv(uint8_t layer, HSV16 hsv) {
    if (layer >= LED_CONTROLLER_NUM_LAYERS) return;
    store[layer] = hsv;
    led_controller_dirty = true;
}

uint8_t led_controller_get_layer_anim(uint8_t layer) {
    if (layer >= LED_CONTROLLER_NUM_LAYERS) layer = 0;
    return anim_store[layer] ? anim_store[layer] : RGBLIGHT_MODE_STATIC_LIGHT;
}
void led_controller_set_layer_anim(uint8_t layer, uint8_t mode, bool persist) {
    if (layer >= LED_CONTROLLER_NUM_LAYERS) return;
    anim_store[layer] = mode;
    if (persist) ee_save();
}

HSV16 led_controller_get_caps_hsv(void) {
    return store[LED_CONTROLLER_CAPS_SLOT];
}

void led_controller_set_caps_hsv(HSV16 hsv) {
    store[LED_CONTROLLER_CAPS_SLOT] = hsv;
    led_controller_dirty = true;
}

uint8_t led_controller_get_caps_anim(void) {
    return anim_store[LED_CONTROLLER_CAPS_SLOT] ? anim_store[LED_CONTROLLER_CAPS_SLOT]
                                    : RGBLIGHT_MODE_STATIC_LIGHT;
}

void led_controller_set_caps_anim(uint8_t mode, bool persist) {
    anim_store[LED_CONTROLLER_CAPS_SLOT] = mode;
    if (persist) ee_save();
}

void led_controller_preview_draw(uint8_t layer, HSV16 hsv) {
    uint8_t slot = layer % LED_CONTROLLER_PREVIEW_LEDS;
    for (uint8_t s = 0; s < LED_CONTROLLER_PREVIEW_LEDS; s++) {
        uint8_t led = led_controller_slot_to_led(s);
        if (s == slot) rgblight_sethsv_at(hsv.h, hsv.s, hsv.v, led);
        else           rgblight_sethsv_at(0, 0, 0, led);
    }
}

static void seed_defaults(void) {
    /* Clear everything to the "unstyled" marker (s==0 && v==0). */
    for (uint8_t i = 0; i < LED_CONTROLLER_NUM_STYLE_TARGETS; i++) {
        store[i].h = 0;
        store[i].s = 0;
        store[i].v = 0;
        anim_store[i] = RGBLIGHT_MODE_STATIC_LIGHT;
        style_set[i] = false;
    }

    /* Base layer is always styled and defines the global default. */
    store[0].h = LED_CONTROLLER_BASE_H;
    store[0].s = LED_CONTROLLER_BASE_S;
    store[0].v = LED_CONTROLLER_BASE_V;
    anim_store[0] = LED_CONTROLLER_BASE_ANIM;
    style_set[0] = true;

    /* Caps slot: defaults to base color + a visible animation. */
    store[LED_CONTROLLER_CAPS_SLOT] = store[0];
    anim_store[LED_CONTROLLER_CAPS_SLOT] = RGBLIGHT_MODE_RAINBOW_SWIRL;
    style_set[LED_CONTROLLER_CAPS_SLOT] = true;

    ee_save();
}

void led_controller_init(void) {
    /* Load persisted state; if absent, seed defaults (and persist them). */
    if (!ee_load()) {
        seed_defaults();
        /* ee_load() is now guaranteed to succeed after seeding. */
        (void)ee_load();
    }

    /* Base layer is always styled (and may have been user-edited). */
    style_set[0] = true;

    /* If Caps slot is unstyled/uninitialized, mirror base and set a default anim. */
    if (store[LED_CONTROLLER_CAPS_SLOT].s == 0 && store[LED_CONTROLLER_CAPS_SLOT].v == 0) {
        store[LED_CONTROLLER_CAPS_SLOT] = store[0];
        store[LED_CONTROLLER_CAPS_SLOT].v = rgblight_get_val();
        style_set[LED_CONTROLLER_CAPS_SLOT] = true;
    }
    if (anim_store[LED_CONTROLLER_CAPS_SLOT] == 0) {
        anim_store[LED_CONTROLLER_CAPS_SLOT] = RGBLIGHT_MODE_RAINBOW_SWIRL;
    }
}

bool led_controller_active(void) {
    return active && !window_expired();
}

void led_controller_release(void) {
    if (active) {
        led_controller_anim_preview_cancel();
        active = false;

        rgblight_set_effect_range(0, RGBLIGHT_LED_COUNT);

        if (led_controller_dirty) {
            ee_save();
            led_controller_dirty = false;
        }
    }
}

void led_controller_ensure_active(void) {
    if (!active) {
        sel = 0;
        active = true;
        mark_active();

        rgblight_set_effect_range(0, 0);

        uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS) ? sel : LED_CONTROLLER_CAPS_SLOT;
        HSV16 s = led_controller_resolve_hsv(target, NULL);
        s.v = rgblight_get_val();
        set_mask_with_hsv(led_controller_mask_for_sel(sel), s);
    }
}

uint8_t led_controller_selected(void) {
    return sel;
}

void led_controller_reset_to_defaults(void) {
    seed_defaults();
    mark_active();
}

void led_controller_render_task(void) {
    led_controller_anim_preview_tick();
    if (led_controller_prev.active) {
        return;
    }

    /* Draw masked style-target preview. */
    uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS)
                       ? sel
                       : LED_CONTROLLER_CAPS_SLOT;

    uint8_t base_v = rgblight_get_val();

    bool inherited = false;
    HSV16 s = led_controller_resolve_hsv(target, &inherited);
    s.v = base_v;

#if LED_CONTROLLER_UI_BREATHE
    uint8_t amp   = (uint8_t)((base_v >> 2) + 8);
    uint8_t delta = led_controller_breathe_delta(1100, amp);
    uint16_t vv   = (uint16_t)s.v + delta;
    s.v = (vv > 255) ? 255 : (uint8_t)vv;
#endif

    set_mask_with_hsv(led_controller_mask_for_sel(sel), s);
}

bool led_controller_process(uint16_t keycode, keyrecord_t *record) {
    if (!record->event.pressed) return true;

    switch (keycode) {

        case LED_CONTROLLER_PARAM_INC:
        case LED_CONTROLLER_PARAM_DEC: {
            if (!active) {
                sel = 0;
                active = true;
                mark_active();
            }

            bool inc = (keycode == LED_CONTROLLER_PARAM_INC);

            switch (led_controller_get_edit_mode()) {

                case LED_EDIT_MODE_HUE: {
                    uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS)
                                    ? sel
                                    : LED_CONTROLLER_CAPS_SLOT;

                    bool inherited = false;
                    work = led_controller_resolve_hsv(target, &inherited);
                    work.v = rgblight_get_val();
                    
                    int step = inc ? RGBLIGHT_HUE_STEP : -RGBLIGHT_HUE_STEP;
                    work.h = (uint8_t)((uint8_t)work.h + step);
                    
                    if (!style_set[target]) {
                        store[target] = work;
                        style_set[target] = true;
                    } else {
                        store[target].h = work.h;
                    }
                    led_controller_dirty = true;
                    ee_save();
                    led_controller_dirty = false;

                    set_mask_with_hsv(led_controller_mask_for_sel(sel), work);
                    break;
                }

                case LED_EDIT_MODE_SAT: {
                    uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS)
                                    ? sel
                                    : LED_CONTROLLER_CAPS_SLOT;

                    bool inherited = false;
                    work = led_controller_resolve_hsv(target, &inherited);
                    work.v = rgblight_get_val();
                    
                    int step = inc ? RGBLIGHT_SAT_STEP : -RGBLIGHT_SAT_STEP;
                    work.s = clamp255((int)work.s + step);
                    
                    if (!style_set[target]) {
                        store[target] = work;
                        style_set[target] = true;
                    } else {
                        store[target].s = work.s;
                    }
                    led_controller_dirty = true;
                    ee_save();
                    led_controller_dirty = false;

                    set_mask_with_hsv(led_controller_mask_for_sel(sel), work);
                    break;
                }

                case LED_EDIT_MODE_ANIM: {
                    if (inc) rgblight_step_noeeprom();
                    else     rgblight_step_reverse_noeeprom();

                    uint8_t mode = rgblight_get_mode();
                    uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS)
                                    ? sel
                                    : LED_CONTROLLER_CAPS_SLOT;

                    if (target == LED_CONTROLLER_CAPS_SLOT)
                        led_controller_set_caps_anim(mode, false);
                    else
                        led_controller_set_layer_anim(target, mode, false);

                    led_controller_anim_preview_begin(target, mode);

                    if (!style_set[target]) {
                        store[target] = store[0];
                        style_set[target] = true;
                    }
                    led_controller_dirty = true;
                    ee_save();
                    led_controller_dirty = false;
                    break;
                }

                default:
                    return false;
            }

            mark_active();
            return false;
        }

        case LED_CONTROLLER_LAYER_DEC:
        case LED_CONTROLLER_LAYER_INC: {
            if (!active) {
                sel = 0;
                active = true;
                mark_active();
            }
            int dir = (keycode == LED_CONTROLLER_LAYER_DEC) ? -1 : +1;

            sel = (uint8_t)clampi((int)sel + dir, 0, LED_CONTROLLER_CAPS_SLOT);

            uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS) ? sel : LED_CONTROLLER_CAPS_SLOT;

            HSV16 s = led_controller_resolve_hsv(target, NULL);
            s.v = rgblight_get_val();

            set_mask_with_hsv(led_controller_mask_for_sel(sel), s);
            mark_active();
            return false;
        }

        case LED_CONTROLLER_RESET: {
            seed_defaults();

            for (uint8_t i = 1; i < LED_CONTROLLER_NUM_STYLE_TARGETS; i++) {
                style_set[i] = false;
            }
            style_set[0] = true;


            sel = 0;
            active = false;
            led_controller_anim_preview_end();
            led_controller_dirty = false;

            /* Blink confirmation */
            HSV16 base = store[0];
            base.v = rgblight_get_val();

            for (uint8_t i = 0; i < LED_CONTROLLER_RESET_BLINK_COUNT; i++) {
                HSV16 off = base;
                off.v = 0;
                set_mask_with_hsv(led_controller_mask_for_sel(0), off);
                wait_ms(LED_CONTROLLER_RESET_BLINK_MS);

                set_mask_with_hsv(led_controller_mask_for_sel(0), base);
                wait_ms(LED_CONTROLLER_RESET_BLINK_MS);
            }

            /* Return LED ownership to rgblight */
            rgblight_set_effect_range(0, RGBLIGHT_LED_COUNT);

            /* Restore base layer */
            rgblight_mode_noeeprom(anim_store[0]);
            rgblight_sethsv_noeeprom(store[0].h, store[0].s, rgblight_get_val());

            return false;
        }
    }

    return true;
}

void led_controller_task(void) {
    static bool was_led_controller = false;

    if (startup_anim_active) {
        startup_anim_tick();
        startup_anim_was_active = true;
        return;
    }

    /* Startup animation ended: restore base layer style. */
    if (startup_anim_was_active) {
        startup_anim_was_active = false;

    uint8_t layer = get_highest_layer(layer_state);

        /* Global RGB is allowed here because controller is NOT active */
        HSV16 c      = led_controller_resolve_hsv(layer, NULL);
        uint8_t v    = rgblight_get_val();
        uint8_t mode = led_controller_get_layer_anim(layer);

        rgblight_mode_noeeprom(mode);
        rgblight_sethsv_noeeprom(c.h, c.s, v);
    }

bool on_led_controller =
    (led_controller_get_edit_mode() != LED_EDIT_MODE_NONE);

    if (on_led_controller) {
        /* Edit mode active: ensure controller is active. */
        led_controller_ensure_active();
    } else if (was_led_controller) {
        led_controller_release();

        /* Edit mode ended: restore base layer style if Caps is not active. */
        if (!caps_active) {
            uint8_t layer = get_highest_layer(layer_state);
            HSV16   c     = led_controller_get_layer_hsv(layer);
            uint8_t v     = rgblight_get_val();
            uint8_t mode  = led_controller_get_layer_anim(layer);

            rgblight_enable_noeeprom();
            rgblight_mode_noeeprom(mode);
            rgblight_sethsv_noeeprom(c.h, c.s, v);
        }
    }

    was_led_controller = on_led_controller;

    if (led_controller_active()) {
        led_controller_render_task();
    }

}