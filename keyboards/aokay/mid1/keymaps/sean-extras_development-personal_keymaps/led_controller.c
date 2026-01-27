/* -------------------------------------------------------------------------
 * MID.1 LED controller
 * -------------------------------------------------------------------------
 *  Overview
 *  - Hold LED_EDIT_* keycodes to enter edit modes.
 *  - Left encoder selects style target (layer / caps).
 *  - Right encoder edits hue / saturation / animation based on edit mode.
 *  - H/S edits: render masked selection; immediate persist to EEPROM.
 *  - LED controller owns LEDs while window is active; on timeout, control returns to idle.
 *  - H/S/Anim edits: render masked selection; changes mark-dirty and SAVE ON EXIT.
 *  - LED_CONTROLLER_ANIM_DEC/INC: starts a 3s FULL-BAR PREVIEW (restarts on each step).
 *  - LED controller owns LEDs while window is active; on release or timeout, control returns to idle.
 *  - Value (V) follows rgblight_config.val (global brightness).
 *
 *  LED ownership rules:
 *  - Startup animation owns LEDs while active
 *  - LED controller owns LEDs while active
 *  - Global rgblight setters are only allowed on enter/exit
 * ------------------------------------------------------------------------- */


#include QMK_KEYBOARD_H
#include "mid1_custom_keycodes.h"
#include "led_controller.h"
#include "eeconfig.h"

#ifdef RGBLIGHT_ENABLE
#    include "rgblight.h"
#    include <avr/eeprom.h>
#endif

static led_edit_mode_t current_edit_mode = LED_EDIT_MODE_NONE;

static inline uint8_t led_controller_slot_to_led(uint8_t slot);

void led_controller_set_edit_mode(led_edit_mode_t mode) {
    current_edit_mode = mode;
}

led_edit_mode_t led_controller_get_edit_mode(void) {
    return current_edit_mode;
}

static void boot_anim_load_config(void);

#ifndef FOCUS_TIMER_ENABLE
#    define FOCUS_TIMER_ENABLE 0
#endif

#if !FOCUS_TIMER_ENABLE

void focus_timer_task(void) {
    /* Focus timer disabled */
}

bool focus_timer_process_keycode(uint16_t keycode, keyrecord_t *record) {
    /* Focus timer disabled */
    return true;
}

#endif
/* --- Local constants --------------------------------------------------- */
#define EE_UNINIT_BYTE 0xFF

#ifdef EECONFIG_USERSPACE
#    define EE_LED_CONTROLLER_BOOT_ANIM_ENABLED (EECONFIG_USERSPACE)
#endif

bool led_controller_process(uint16_t keycode, keyrecord_t *record);

/* -- Boot animation ----------------------------------------------------- */
bool     led_controller_boot_anim_enabled = true;
static bool     startup_anim_active        = false;
static bool     startup_anim_was_active    = false;
static uint32_t startup_anim_started_at    = 0;

/* ----------------------------------------------------------------------- */

static bool caps_active = false;
static bool caps_suspended = false;

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

/* -- Layer coloring (keeps brightness sticky) ----------------------------- */

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

/* -- CapsLock LED hook: triggers rainbow swirl when Caps toggles ---------- */

bool led_update_user(led_t state) {
    caps_effect_set(state.caps_lock);
    return true;
}

/* -- Encoders: while in LED controller modes ------------------------------ */
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

/* -- Startup animation config --------------------------------------------- */
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

/* -- Startup animation ---------------------------------------------------- */
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
    HSV16 hsv = (HSV16){ H_OF_TUPLE(HSV_MID1ORANGE), S_OF_TUPLE(HSV_MID1ORANGE), 0 };

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

/* -- Init ----------------------------------------------------------------- */

void keyboard_post_init_user(void) {
    rgblight_enable_noeeprom();
    rgblight_mode_noeeprom(RGBLIGHT_MODE_STATIC_LIGHT);

    /* Set initial color ONCE (tuple-style macro) */
    rgblight_sethsv_noeeprom HSV_MID1ORANGE;

    /* Load boot animation toggle from EEPROM */
    boot_anim_load_config();

    /* LED controller init */
    led_controller_init();

    /* Caps init in case Caps was on at boot */
    caps_effect_set(host_keyboard_led_state().caps_lock);

    /* One-time startup animation if enabled */
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

            HSV16 hsv16 = (HSV16){
                H_OF_TUPLE(HSV_MID1ORANGE),
                S_OF_TUPLE(HSV_MID1ORANGE),
                rgblight_get_val()
            };

            if (!led_controller_boot_anim_enabled) {
                hsv16.v = 0;
            }

            led_controller_set_all_preview_leds(hsv16);
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

#ifndef RGBLIGHT_ENABLE
#    error "LED controller requires RGBLIGHT_ENABLE."
#endif

/* -- How many palette slots/layers does LED controller expose? ------------------------ */
#ifndef LED_CONTROLLER_NUM_LAYERS
#    define LED_CONTROLLER_NUM_LAYERS 16
#endif

/* Caps styling pseudo-slot (treated like an extra "layer" for style editing) */
#define LED_CONTROLLER_CAPS_SLOT         LED_CONTROLLER_NUM_LAYERS
#define LED_CONTROLLER_NUM_STYLE_TARGETS (LED_CONTROLLER_NUM_LAYERS + 1)

/* -- Config knobs (override in mid1_config.h if desired) ------------------ */
#ifndef LED_CONTROLLER_IDLE_TIMEOUT_MS
#    define LED_CONTROLLER_IDLE_TIMEOUT_MS 4000
#endif
#ifndef LED_CONTROLLER_TRIPLE_BLINK_MS
#    define LED_CONTROLLER_TRIPLE_BLINK_MS 450
#endif
#ifndef RGBLIGHT_SAT_STEP
#    define RGBLIGHT_SAT_STEP 12
#endif
#ifndef HSV_LED_CONTROLLER_DEFAULT
#    define HSV_LED_CONTROLLER_DEFAULT 0, 0, 255
#endif

/* -- LED controller preview LED mapping (slot→LED index) ------------------------------ */

#ifndef LED_CONTROLLER_PREVIEW_LEDS
#    define LED_CONTROLLER_PREVIEW_LEDS 8 /* LED_CONTROLLER_PREVIEW_LEDS */
#endif

#ifndef LED_CONTROLLER_PREVIEW_OFFSET
#    define LED_CONTROLLER_PREVIEW_OFFSET 0 /* LED_CONTROLLER_PREVIEW_OFFSET */
#endif

#ifndef LED_CONTROLLER_LEDS_REVERSED
#    define LED_CONTROLLER_LEDS_REVERSED 1 /* LED_CONTROLLER_LEDS_REVERSED */
#endif

static inline uint8_t led_controller_slot_to_led(uint8_t slot) {
    return LED_CONTROLLER_PREVIEW_OFFSET + (LED_CONTROLLER_LEDS_REVERSED ? (LED_CONTROLLER_PREVIEW_LEDS - 1u - slot) : slot);
}

static HSV16   store[LED_CONTROLLER_NUM_STYLE_TARGETS];
static uint8_t anim_store[LED_CONTROLLER_NUM_STYLE_TARGETS]; /* per-target rgblight mode */
static bool led_controller_dirty = false; /* true when H/S/Anim changed since last save */

/* --- LED controller triangle wave helper --------------------------------------------- */

static inline uint8_t led_controller_breathe_delta(uint16_t period_ms, uint8_t amp) {
    uint32_t t = timer_read32() % (period_ms ? period_ms : 1);
    uint32_t half = (period_ms ? period_ms : 1) / 2;
    uint32_t up = (t <= half) ? t : ((period_ms ? period_ms : 1) - t);
    return (uint8_t)((amp * up) / (half ? half : 1));
}

/* --- LED controller Anim full-bar preview (non-blocking) ----------------------------- */
typedef struct {
    bool     active;
    uint8_t  layer; /* which layer we're previewing */
    uint8_t  mode; /* rgblight mode being previewed */
    uint8_t  saved_mode; /* mode to restore after preview */
    uint32_t start_ms; /* timer when preview started */
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

    // Give engine full control temporarily
    rgblight_set_effect_range(0, RGBLIGHT_LED_COUNT);

    // Set base HSV so animations inherit hue/sat
    HSV16 c = store[layer];
    c.v = rgblight_get_val();

    rgblight_sethsv_noeeprom(c.h, c.s, c.v);
    rgblight_mode_noeeprom(mode);
}

void led_controller_anim_preview_cancel(void) {
    if (!led_controller_prev.active) return;

    led_controller_prev.active = false;

    // Return to static so engine stops animating
    rgblight_mode_noeeprom(RGBLIGHT_MODE_STATIC_LIGHT);

    // Lock engine again so LED controller can draw masks
    rgblight_set_effect_range(0, 0);
}

/* IMPORTANT: do not force STATIC while animation preview is active */
static inline void led_controller_anim_preview_tick(void) {
    if (!led_controller_prev.active) return;
    if (timer_elapsed32(led_controller_prev.start_ms) >= LED_CONTROLLER_ANIM_PREVIEW_MS) {
        led_controller_anim_preview_cancel();
    }
}

/* -- Types / storage ------------------------------------------------------ */

static const uint8_t LED_CONTROLLER_PATS[16] = {
    0b10000001, /*  0: layer 0  IOOOOOOI  (special) */
    0b10000000, /*  1: layer 1  IOOOOOOO */
    0b11000000, /*  2: layer 2  IIOOOOOO */
    0b11100000, /*  3: layer 3  IIIOOOOO */
    0b11110000, /*  4: layer 4  IIIIOOOO */
    0b11111000, /*  5: layer 5  IIIIIOOO */
    0b11111100, /*  6: layer 6  IIIIIIOO */
    0b11111110, /*  7: layer 7  IIIIIIIO */
    0b11111111, /*  8: layer 8  IIIIIIII */
    0b01111111, /*  9: layer 9  OIIIIIII */
    0b00111111, /* 10: layer 10 OOIIIIII */
    0b00011111, /* 11: layer 11 OOOIIIII */
    0b00001111, /* 12: layer 12 OOOOIIII */
    0b00000111, /* 13: layer 13 OOOOOIII */
    0b00000011, /* 14: layer 14 OOOOOOII */
    0b00000001  /* 15: layer 15 OOOOOOOI */
};



// Return the LED mask for the current LED controller selection.
//  - 0..(LED_CONTROLLER_NUM_LAYERS-1) = real layers (use LED_CONTROLLER_PATS)
//  - LED_CONTROLLER_CAPS_SLOT         = Caps styling (OOIOOI = 0b001100)
static inline uint8_t led_controller_mask_for_sel(uint8_t sel) {
    if (sel < LED_CONTROLLER_NUM_LAYERS) {
        return LED_CONTROLLER_PATS[sel];
    }

    // Caps pattern (middle two LEDs): 0b00011000

    return 0b00011000;
}

static HSV16   work;
static uint8_t sel          = 0; /* style slot selection */
static bool    active       = false;
static bool    first_render = true;
static uint32_t last_ms     = 0;

#ifndef LED_CONTROLLER_EE_KEY
#    define LED_CONTROLLER_EE_KEY 0xC35A
#endif

#ifndef LED_CONTROLLER_EE_BASE
#    define LED_CONTROLLER_EE_BASE (EECONFIG_USER)
#endif

#define LED_CONTROLLER_STRIDE 4 /* h(2) + s(1) + v(1) */

/* -- Local helpers -------------------------------------------------------- */

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

#if 0
/* Triple blink in current color for feedback (disabled: blocking) */
static void triple_blink(uint8_t mask, HSV16 s) {
    for (uint8_t n = 0; n < 3; n++) {
        set_mask_with_hsv(mask, s);
        wait_ms(LED_CONTROLLER_TRIPLE_BLINK_MS / 3);
        set_mask_with_hsv(mask, (HSV16){0, 0, 0});
        wait_ms(LED_CONTROLLER_TRIPLE_BLINK_MS / 3);
    }
}
#endif

/* -- EEPROM read/write ---------------------------------------------------- */

#define LED_CONTROLLER_SLOT_BYTES 4u
static void ee_save(void) {
#ifdef EECONFIG_USER
    eeprom_update_word((void *)EECONFIG_USER, LED_CONTROLLER_EE_KEY);
    uint16_t base = (uint16_t)EECONFIG_USER + 2;
    for (uint8_t i = 0; i < LED_CONTROLLER_NUM_LAYERS; i++) {
        uint16_t off = base + (uint16_t)i * LED_CONTROLLER_SLOT_BYTES;
        eeprom_update_word((void *)(uintptr_t)(off + 0), store[i].h);
        eeprom_update_byte((void *)(uintptr_t)(off + 2), store[i].s);
        eeprom_update_byte((void *)(uintptr_t)(off + 3), store[i].v);
    }
    uint16_t anim_base = base + (uint16_t)LED_CONTROLLER_NUM_LAYERS * LED_CONTROLLER_SLOT_BYTES;
    for (uint8_t i = 0; i < LED_CONTROLLER_NUM_LAYERS; i++) {
        eeprom_update_byte((void *)(uintptr_t)(anim_base + i), anim_store[i]);
    }
#else
#endif
}

static bool ee_load(void) {
#ifdef EECONFIG_USER
    if (eeprom_read_word((void *)EECONFIG_USER) != LED_CONTROLLER_EE_KEY) return false;
    uint16_t base = (uint16_t)EECONFIG_USER + 2;
    for (uint8_t i = 0; i < LED_CONTROLLER_NUM_LAYERS; i++) {
        uint16_t off = base + (uint16_t)i * LED_CONTROLLER_SLOT_BYTES;
        store[i].h = eeprom_read_word((void *)(uintptr_t)(off + 0));
        store[i].s = eeprom_read_byte((void *)(uintptr_t)(off + 2));
    }
    uint16_t anim_base = base + (uint16_t)LED_CONTROLLER_NUM_LAYERS * LED_CONTROLLER_SLOT_BYTES;
    for (uint8_t i = 0; i < LED_CONTROLLER_NUM_LAYERS; i++) {
        anim_store[i] = eeprom_read_byte((void *)(uintptr_t)(anim_base + i));
        if (anim_store[i] == 0) anim_store[i] = RGBLIGHT_MODE_STATIC_LIGHT;
    }
    return true;
#else
    return false;
#endif
}

/* -- Backing store -------------------------------------------------------- */

static uint8_t led_controller_selected_layer = 0;

void led_controller_select_layer(uint8_t layer) {
    if (layer < LED_CONTROLLER_NUM_LAYERS) led_controller_selected_layer = layer;
}

HSV16 led_controller_get_layer_hsv(uint8_t layer) {
    if (layer >= LED_CONTROLLER_NUM_LAYERS) layer = 0;
    return store[layer];
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

/* -- Caps styling accessors ----------------------------------------------- */

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

/* -- Defaults (seed from MID.1 palette) ---------------------------------- */

#ifndef HSV_MID1ORANGE
#    define HSV_MID1ORANGE  15, 255, 255
#endif
#ifndef HSV_MID1BLUE
#    define HSV_MID1BLUE   180, 255, 255
#endif
#ifndef HSV_MID1GREEN
#    define HSV_MID1GREEN   85, 255, 255
#endif
#ifndef HSV_MID1PURPLE
#    define HSV_MID1PURPLE 200, 255, 255
#endif
#ifndef HSV_MID1RED
#    define HSV_MID1RED      0, 255, 255
#endif
#ifndef HSV_MID1SAGE
#    define HSV_MID1SAGE   110, 120, 255
#endif

static void seed_defaults(void) {
    const uint8_t v = rgblight_get_val();
    const HSV16 table[16] = {
        { H_OF_TUPLE(HSV_MID1ORANGE),  S_OF_TUPLE(HSV_MID1ORANGE),  v },
        { H_OF_TUPLE(HSV_MID1BLUE),    S_OF_TUPLE(HSV_MID1BLUE),    v },
        { H_OF_TUPLE(HSV_MID1GREEN),   S_OF_TUPLE(HSV_MID1GREEN),   v },
        { H_OF_TUPLE(HSV_MID1PURPLE),  S_OF_TUPLE(HSV_MID1PURPLE),  v },
        { H_OF_TUPLE(HSV_MID1RED),     S_OF_TUPLE(HSV_MID1RED),     v },
        { H_OF_TUPLE(HSV_MID1RED),     S_OF_TUPLE(HSV_MID1RED),     v },
        { H_OF_TUPLE(HSV_MID1SAGE),    S_OF_TUPLE(HSV_MID1SAGE),    v },
        { 0,                           0,                           v }, /* white */
        { H_OF_TUPLE(HSV_LED_CONTROLLER_DEFAULT),  S_OF_TUPLE(HSV_LED_CONTROLLER_DEFAULT),  v },
        { H_OF_TUPLE(HSV_LED_CONTROLLER_DEFAULT),  S_OF_TUPLE(HSV_LED_CONTROLLER_DEFAULT),  v },
        { H_OF_TUPLE(HSV_LED_CONTROLLER_DEFAULT),  S_OF_TUPLE(HSV_LED_CONTROLLER_DEFAULT),  v },
        { H_OF_TUPLE(HSV_LED_CONTROLLER_DEFAULT),  S_OF_TUPLE(HSV_LED_CONTROLLER_DEFAULT),  v },
    };
    for (uint8_t i = 0; i < 16; i++) store[i] = table[i];
    for (uint8_t i = 0; i < LED_CONTROLLER_NUM_LAYERS; i++) anim_store[i] = RGBLIGHT_MODE_STATIC_LIGHT;
    store[LED_CONTROLLER_CAPS_SLOT].h = store[0].h;
    store[LED_CONTROLLER_CAPS_SLOT].s = store[0].s;
    store[LED_CONTROLLER_CAPS_SLOT].v = v;
    anim_store[LED_CONTROLLER_CAPS_SLOT] = RGBLIGHT_MODE_RAINBOW_SWIRL;
    ee_save();
}

/* -- Public API ----------------------------------------------------------- */

void led_controller_init(void) {
    if (!ee_load()) {
        seed_defaults();
    } else {
        uint8_t v = rgblight_get_val();

        if (store[LED_CONTROLLER_CAPS_SLOT].s == 0 && store[LED_CONTROLLER_CAPS_SLOT].v == 0) {
            store[LED_CONTROLLER_CAPS_SLOT].h = store[0].h;
            store[LED_CONTROLLER_CAPS_SLOT].s = store[0].s;
            store[LED_CONTROLLER_CAPS_SLOT].v = v;
        }
        if (anim_store[LED_CONTROLLER_CAPS_SLOT] == 0) {
            anim_store[LED_CONTROLLER_CAPS_SLOT] = RGBLIGHT_MODE_RAINBOW_SWIRL;
        }
    }
}

bool led_controller_active(void) {
    return active && !window_expired();
}

void led_controller_release(void) {
    if (active) {
        led_controller_anim_preview_cancel();
        active = false;

        rgblight_set_effect_range(0, RGBLIGHT_LED_COUNT); // engine back in control

        if (led_controller_dirty) {
            ee_save();
            led_controller_dirty = false;
        }
    }
}

void led_controller_ensure_active(void) {
    if (!active) {
        sel = 0;
        first_render = true;
        active = true;
        mark_active();

        rgblight_set_effect_range(0, 0);

        HSV16 s = store[sel];
        s.v = rgblight_get_val();
        set_mask_with_hsv(LED_CONTROLLER_PATS[sel], s);
    }
}

uint8_t led_controller_selected(void) {
    return sel;
}

void led_controller_reset_to_defaults(void) {
    seed_defaults();
    first_render = true;
    mark_active();
}

void led_controller_render_task(void) {
    /* Full-bar animation preview overrides normal LED controller drawing */
    led_controller_anim_preview_tick();
    if (led_controller_prev.active) {
        /* rgblight engine owns LEDs during preview */
        return;
    }

    /* Draw masked layer / caps preview */
    uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS)
                       ? sel
                       : LED_CONTROLLER_CAPS_SLOT;

    HSV16 s = store[target];

    /* V always follows global brightness (with optional breathe) */
    uint8_t base_v = rgblight_get_val();
#if LED_CONTROLLER_UI_BREATHE
    uint8_t amp   = (uint8_t)((base_v >> 2) + 8);
    uint8_t delta = led_controller_breathe_delta(1100, amp);
    uint16_t vv   = (uint16_t)base_v + delta;
    s.v = (vv > 255) ? 255 : (uint8_t)vv;
#else
    s.v = base_v;
#endif

    set_mask_with_hsv(led_controller_mask_for_sel(sel), s);
}

/* -- Key processing ------------------------------------------------------- */

bool led_controller_process(uint16_t keycode, keyrecord_t *record) {
    if (!record->event.pressed) return true;

    switch (keycode) {

        case LED_CONTROLLER_PARAM_INC:
        case LED_CONTROLLER_PARAM_DEC: {
            if (!active) {
                sel = 0;
                first_render = true;
                active = true;
                mark_active();
            }

            bool inc = (keycode == LED_CONTROLLER_PARAM_INC);

            switch (led_controller_get_edit_mode()) {

                case LED_EDIT_MODE_HUE: {
                    uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS)
                                    ? sel
                                    : LED_CONTROLLER_CAPS_SLOT;

                    work = store[target];
                    work.v = rgblight_get_val();

                    int step = inc ? RGBLIGHT_HUE_STEP : -RGBLIGHT_HUE_STEP;
                    work.h = wrap360((int)work.h + step);

                    store[target].h = work.h;
                    led_controller_dirty = true;

                    set_mask_with_hsv(led_controller_mask_for_sel(sel), work);
                    break;
                }

                case LED_EDIT_MODE_SAT: {
                    uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS)
                                    ? sel
                                    : LED_CONTROLLER_CAPS_SLOT;

                    work = store[target];
                    work.v = rgblight_get_val();

                    int step = inc ? RGBLIGHT_SAT_STEP : -RGBLIGHT_SAT_STEP;
                    work.s = clamp255((int)work.s + step);

                    store[target].s = work.s;
                    led_controller_dirty = true;

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

                    led_controller_dirty = true;
                    led_controller_anim_preview_begin(target, mode);
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
                first_render = false;
                active = true;
                mark_active();
            }
            int dir = (keycode == LED_CONTROLLER_LAYER_DEC) ? -1 : +1;

            // Now cycles 0..(LED_CONTROLLER_NUM_LAYERS-1) + LED_CONTROLLER_CAPS_SLOT (Caps)
            sel = (uint8_t)clampi((int)sel + dir, 0, LED_CONTROLLER_CAPS_SLOT);

            // Real layers 0..11 use their own index; caps uses LED_CONTROLLER_CAPS_SLOT
            uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS) ? sel : LED_CONTROLLER_CAPS_SLOT;

            HSV16 s = store[target];
            s.v = rgblight_get_val();

            set_mask_with_hsv(led_controller_mask_for_sel(sel), s);
            mark_active();
            return false;
        }
        case LED_CONTROLLER_HUE_DEC:
        case LED_CONTROLLER_HUE_INC: {
            if (!active) {
                sel = 0;
                first_render = true;
                active = true;
                mark_active();
            }

            // If we've scrolled past the last layer, we're editing Caps
            uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS) ? sel : LED_CONTROLLER_CAPS_SLOT;

            work = store[target];
            work.v = rgblight_get_val();

            int step = (keycode == LED_CONTROLLER_HUE_DEC) ? -(int)RGBLIGHT_HUE_STEP : (int)RGBLIGHT_HUE_STEP;
            work.h = wrap360((int)work.h + step);

            set_mask_with_hsv(led_controller_mask_for_sel(sel), work);

            store[target].h = work.h;
            led_controller_dirty = true;
            mark_active();
            return false;
        }
        case LED_CONTROLLER_SAT_DEC:
        case LED_CONTROLLER_SAT_INC: {
            if (!active) {
                sel = 0;
                first_render = true;
                active = true;
                mark_active();
            }

            uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS) ? sel : LED_CONTROLLER_CAPS_SLOT;

            work = store[target];
            work.v = rgblight_get_val();

            int sstep = (keycode == LED_CONTROLLER_SAT_DEC) ? -(int)RGBLIGHT_SAT_STEP : (int)RGBLIGHT_SAT_STEP;
            work.s = clamp255((int)work.s + sstep);

            set_mask_with_hsv(led_controller_mask_for_sel(sel), work);

            store[target].s = work.s;
            led_controller_dirty = true;
            mark_active();
            return false;
        }
        case LED_CONTROLLER_ANIM_INC:
        case LED_CONTROLLER_ANIM_DEC: {
            if (!active) {
                sel = 0;
                first_render = true;
                active = true;
                mark_active();
            }

            if (keycode == LED_CONTROLLER_ANIM_INC) {
                rgblight_step_noeeprom();
            } else {
                rgblight_step_reverse_noeeprom();
            }
            uint8_t mode = rgblight_get_mode();

            uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS) ? sel : LED_CONTROLLER_CAPS_SLOT;

            if (target == LED_CONTROLLER_CAPS_SLOT) {
                led_controller_set_caps_anim(mode, /*persist=*/false);
            } else {
                led_controller_set_layer_anim(target, mode, /*persist=*/false);
            }

            led_controller_dirty = true;

            /* Kick off 3s full-bar preview using the new mode */
            led_controller_anim_preview_begin(target, mode);
            mark_active();
            return false;
        }
        case LED_CONTROLLER_RESET: {
            seed_defaults();

            uint8_t target = (sel < LED_CONTROLLER_NUM_LAYERS) ? sel : LED_CONTROLLER_CAPS_SLOT;

            HSV16 s = store[target];
            s.v = rgblight_get_val();

            set_mask_with_hsv(led_controller_mask_for_sel(sel), s);
            mark_active();
            return false;
        }
    }
    return true;
}

void led_controller_task(void) {
    static bool was_led_controller = false;

    /* ------------------------------------------------------------
     * Startup animation owns LEDs exclusively
     * ------------------------------------------------------------ */
    if (startup_anim_active) {
        startup_anim_tick();
        startup_anim_was_active = true;
        return;
    }

    /* Startup animation just ended: restore base layer style ONCE */
    if (startup_anim_was_active) {
        startup_anim_was_active = false;

    uint8_t layer = get_highest_layer(layer_state);

        /* Global RGB is allowed here because controller is NOT active */
        HSV16  c     = led_controller_get_layer_hsv(layer);
        uint8_t v    = rgblight_get_val();
        uint8_t mode = led_controller_get_layer_anim(layer);

        rgblight_mode_noeeprom(mode);
        rgblight_sethsv_noeeprom(c.h, c.s, v);
    }

bool on_led_controller =
    (led_controller_get_edit_mode() != LED_EDIT_MODE_NONE);

    if (on_led_controller) {
        /* Enter / stay in controller mode */
        led_controller_ensure_active();
    } else if (was_led_controller) {
        led_controller_release();

        /* Restore base layer RGB ONCE (only if Caps not active) */
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

    /* ------------------------------------------------------------
     * Controller owns LEDs while active
     * ------------------------------------------------------------ */
    if (led_controller_active()) {
        led_controller_render_task();
    }

}