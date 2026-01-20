/* --------------------------------------------------------------------------
 * MID.1 – Color Picker (CP) Module – Implementation
 * --------------------------------------------------------------------------
 * Spec v2:
 *  - Hold CP layers (8/9/10) to open an edit window.
 *  - Left encoder: CP_LAYER_DEC/INC (select slot 0..11 w/out wrap).
 *  - Right encoder on L8/L9/L10: CP_HUE_*, CP_SAT_*, CP_ANIM_* respectively.
 *  - H/S edits: render masked selection; immediate persist to EEPROM.
 *  - Animation edits: minimal step now (full-bar preview can be added later).
 *  - CP owns LEDs while window is active; on timeout, control returns to idle.
 *  - H/S/Anim edits: render masked selection; changes mark-dirty and SAVE ON EXIT.
 *  - CP_ANIM_DEC/INC: starts a 3s FULL-BAR PREVIEW (restarts on each step).
 *  - CP owns LEDs while window is active; on release or timeout, control returns to idle.
 *
 * Notes:
 *  - Value (V) follows rgblight_config.val (global brightness).
 *  - EEPROM layout is keyed and compact; bump base if you add more features.
 * -------------------------------------------------------------------------- */

#include QMK_KEYBOARD_H
#include "config.h"
#include "cp_color.h"
#include "mid1_custom_keycodes.h"
#include "eeconfig.h"
#include <avr/eeprom.h>
#include "rgblight.h"

#ifndef RGBLIGHT_ENABLE
#    error "Color Picker requires RGBLIGHT_ENABLE."
#endif

/* -- How many palette slots/layers does CP expose? ------------------------ */
#ifndef CP_NUM_LAYERS
#    define CP_NUM_LAYERS 16
#endif

/* Caps styling pseudo-slot (treated like an extra "layer" for style editing) */
#define CP_CAPS_SLOT         CP_NUM_LAYERS
#define CP_NUM_STYLE_TARGETS (CP_NUM_LAYERS + 1)

/* -- Config knobs (override in mid1_config.h if desired) ------------------ */
#ifndef CP_IDLE_TIMEOUT_MS
#    define CP_IDLE_TIMEOUT_MS 4000
#endif
#ifndef CP_TRIPLE_BLINK_MS
#    define CP_TRIPLE_BLINK_MS 450
#endif
#ifndef RGBLIGHT_SAT_STEP
#    define RGBLIGHT_SAT_STEP 12
#endif
#ifndef HSV_CP_DEFAULT
#    define HSV_CP_DEFAULT 0, 0, 255
#endif

/* -- CP preview LED mapping (slot→LED index) ------------------------------ */

#ifndef CP_PREVIEW_LEDS
#    define CP_PREVIEW_LEDS 8 /* CP_PREVIEW_LEDS */
#endif

#ifndef CP_PREVIEW_OFFSET
#    define CP_PREVIEW_OFFSET 0 /* CP_PREVIEW_OFFSET */
#endif

#ifndef CP_LEDS_REVERSED
#    define CP_LEDS_REVERSED 1 /* CP_LEDS_REVERSED */
#endif

static inline uint8_t cp_slot_to_led(uint8_t slot) {
    /* Map logical slot 0..N-1 to physical LED index, honoring wiring direction */
    return CP_PREVIEW_OFFSET + (CP_LEDS_REVERSED ? (CP_PREVIEW_LEDS - 1u - slot) : slot);
}

/* Backing store: palette entries for each style target
 *  - 0..(CP_NUM_LAYERS-1) = real layers
 *  - CP_CAPS_SLOT         = Caps Lock styling
 */
static HSV16   store[CP_NUM_STYLE_TARGETS];
static uint8_t anim_store[CP_NUM_STYLE_TARGETS]; /* per-target rgblight mode */
static bool cp_dirty = false; /* true when H/S/Anim changed since last save */

/* --- CP triangle wave helper --------------------------------------------- */

static inline uint8_t cp_breathe_delta(uint16_t period_ms, uint8_t amp) {
    uint32_t t = timer_read32() % (period_ms ? period_ms : 1);
    uint32_t half = (period_ms ? period_ms : 1) / 2;
    uint32_t up = (t <= half) ? t : ((period_ms ? period_ms : 1) - t);
    return (uint8_t)((amp * up) / (half ? half : 1));
}

/* --- CP Anim full-bar preview (non-blocking) ----------------------------- */
typedef struct {
    bool     active;
    uint8_t  layer; /* which layer we're previewing */
    uint8_t  mode; /* rgblight mode being previewed */
    uint8_t  saved_mode; /* mode to restore after preview */
    uint32_t start_ms; /* timer when preview started */
} cp_anim_preview_t;

static cp_anim_preview_t cp_prev = {0};

#ifndef CP_ANIM_PREVIEW_MS
#    define CP_ANIM_PREVIEW_MS 3000u
#endif

static inline void cp_anim_preview_begin(uint8_t layer, uint8_t mode) {
    cp_prev.layer      = layer;
    cp_prev.mode       = mode;
    cp_prev.saved_mode = rgblight_get_mode();
    cp_prev.start_ms   = timer_read32();
    cp_prev.active     = true;
    /* Set base HSV so rgblight animations use the layer's hue/sat at current V */
    HSV16 c = store[layer]; c.v = rgblight_get_val();
    rgblight_sethsv_noeeprom(c.h, c.s, c.v);
    rgblight_mode_noeeprom(mode); /* let rgblight animate the whole bar */
}

void cp_anim_preview_cancel(void) {
    if (!cp_prev.active) return;
    cp_prev.active = false;
    /* Return to STATIC so CP can draw masks again */
    rgblight_mode_noeeprom(RGBLIGHT_MODE_STATIC_LIGHT);
}

static inline void cp_anim_preview_tick(void) {
    if (!cp_prev.active) return;
    if (timer_elapsed32(cp_prev.start_ms) >= CP_ANIM_PREVIEW_MS) {
        cp_anim_preview_cancel();
    }
}

/* -- Types / storage ------------------------------------------------------ */

static const uint8_t CP_PATS[16] = {
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



// Return the LED mask for the current CP selection.
//  - 0..(CP_NUM_LAYERS-1) = real layers (use CP_PATS)
//  - CP_CAPS_SLOT         = Caps styling (OOIOOI = 0b001100)
static inline uint8_t cp_mask_for_sel(uint8_t sel) {
    if (sel < CP_NUM_LAYERS) {
        return CP_PATS[sel];
    }

    // Caps pattern (middle two LEDs): 0b00011000

    return 0b00011000;
}

static HSV16   work; /* scratch during edits */
static uint8_t sel          = 0; /* 0..11 */
static bool    active       = false;
static bool    first_render = true;
static uint32_t last_ms     = 0;

/* EEPROM key + base (bump base if other features will persist nearby) */

#ifndef CP_EE_KEY
#    define CP_EE_KEY 0xC35A
#endif

#ifndef CP_EE_BASE
#    define CP_EE_BASE (EECONFIG_USER) /* use the user block from QMK */
#endif

#define CP_STRIDE 4 /* h(2) + s(1) + v(1) */

/* -- Local helpers -------------------------------------------------------- */

static inline void mark_active(void) { last_ms = timer_read32(); }
static inline bool window_expired(void) { return timer_elapsed32(last_ms) > CP_IDLE_TIMEOUT_MS; }

static inline int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}
static inline uint8_t clamp255(int v) { return (uint8_t)clampi(v, 0, 255); }
static inline uint16_t wrap360(int v) {
    while (v < 0) v += 360;
    while (v >= 360) v -= 360;
    return (uint16_t)v;
}

/* Paint mask (lights only masked LEDs) */
static void set_mask_with_hsv(uint8_t mask, HSV16 s) {
    for (uint8_t i = 0; i < CP_PREVIEW_LEDS; i++) {
        bool on = (mask >> (CP_PREVIEW_LEDS - 1u - i)) & 1;
        uint8_t led = cp_slot_to_led(i);
        rgblight_sethsv_at(on ? s.h : 0, on ? s.s : 0, on ? s.v : 0, led);
    }
}

#if 0
/* Triple blink in current color for feedback (disabled: blocking) */
static void triple_blink(uint8_t mask, HSV16 s) {
    for (uint8_t n = 0; n < 3; n++) {
        set_mask_with_hsv(mask, s);
        wait_ms(CP_TRIPLE_BLINK_MS / 3);
        set_mask_with_hsv(mask, (HSV16){0, 0, 0});
        wait_ms(CP_TRIPLE_BLINK_MS / 3);
    }
}
#endif

/* -- EEPROM read/write ---------------------------------------------------- */

/* Persist palette: H=uint16 (2 bytes), S=uint8, V=uint8 → 4 bytes/slot */
#define CP_SLOT_BYTES 4u
static void ee_save(void) {
#ifdef EECONFIG_USER
    eeprom_update_word((void *)EECONFIG_USER, CP_EE_KEY);
    uint16_t base = (uint16_t)EECONFIG_USER + 2;
    for (uint8_t i = 0; i < CP_NUM_LAYERS; i++) {
        uint16_t off = base + (uint16_t)i * CP_SLOT_BYTES;
        eeprom_update_word((void *)(uintptr_t)(off + 0), store[i].h);
        eeprom_update_byte((void *)(uintptr_t)(off + 2), store[i].s);
        eeprom_update_byte((void *)(uintptr_t)(off + 3), store[i].v);
    }
    /* Anim block immediately after palette */
    uint16_t anim_base = base + (uint16_t)CP_NUM_LAYERS * CP_SLOT_BYTES;
    for (uint8_t i = 0; i < CP_NUM_LAYERS; i++) {
        eeprom_update_byte((void *)(uintptr_t)(anim_base + i), anim_store[i]);
    }
#else
#endif
}

static bool ee_load(void) {
#ifdef EECONFIG_USER
    if (eeprom_read_word((void *)EECONFIG_USER) != CP_EE_KEY) return false;
    uint16_t base = (uint16_t)EECONFIG_USER + 2;
    for (uint8_t i = 0; i < CP_NUM_LAYERS; i++) {
        uint16_t off = base + (uint16_t)i * CP_SLOT_BYTES;
        store[i].h = eeprom_read_word((void *)(uintptr_t)(off + 0));
        store[i].s = eeprom_read_byte((void *)(uintptr_t)(off + 2));
        /* store[i].v will be set each frame from global brightness */
    }
    /* Anim block */
    uint16_t anim_base = base + (uint16_t)CP_NUM_LAYERS * CP_SLOT_BYTES;
    for (uint8_t i = 0; i < CP_NUM_LAYERS; i++) {
        anim_store[i] = eeprom_read_byte((void *)(uintptr_t)(anim_base + i));
        if (anim_store[i] == 0) anim_store[i] = RGBLIGHT_MODE_STATIC_LIGHT; /* default */
    }
    return true;
#else
    return false;
#endif
}

/* -- Backing store -------------------------------------------------------- */

static uint8_t cp_selected_layer = 0; /* which layer the selector is pointing at */

void cp_color_select_layer(uint8_t layer) {
    if (layer < CP_NUM_LAYERS) cp_selected_layer = layer;
}

HSV16 cp_color_get_layer_hsv(uint8_t layer) {
    if (layer >= CP_NUM_LAYERS) layer = 0;
    return store[layer];
}

void cp_color_set_layer_hsv(uint8_t layer, HSV16 hsv) {
    if (layer >= CP_NUM_LAYERS) return;
    store[layer] = hsv;
    cp_dirty = true;  // persist will happen in cp_color_release()
}

uint8_t cp_color_get_layer_anim(uint8_t layer) {
    if (layer >= CP_NUM_LAYERS) layer = 0;
    return anim_store[layer] ? anim_store[layer] : RGBLIGHT_MODE_STATIC_LIGHT;
}
void cp_color_set_layer_anim(uint8_t layer, uint8_t mode, bool persist) {
    if (layer >= CP_NUM_LAYERS) return;
    anim_store[layer] = mode;
    if (persist) ee_save();
}

/* -- Caps styling accessors ----------------------------------------------- */

HSV16 cp_color_get_caps_hsv(void) {
    return store[CP_CAPS_SLOT];
}

void cp_color_set_caps_hsv(HSV16 hsv) {
    store[CP_CAPS_SLOT] = hsv;
    cp_dirty = true;        // or call your ee_save wrapper on release
}

uint8_t cp_color_get_caps_anim(void) {
    return anim_store[CP_CAPS_SLOT] ? anim_store[CP_CAPS_SLOT]
                                    : RGBLIGHT_MODE_STATIC_LIGHT;
}

void cp_color_set_caps_anim(uint8_t mode, bool persist) {
    anim_store[CP_CAPS_SLOT] = mode;
    if (persist) ee_save(); // same pattern as cp_color_set_layer_anim()
}

/* Draw the selector preview row using the saved color */
void cp_color_preview_draw(uint8_t layer, HSV16 hsv) {
    // One slot lit = the layer being edited; others off (adjust as you like)
    uint8_t slot = layer % CP_PREVIEW_LEDS;
    for (uint8_t s = 0; s < CP_PREVIEW_LEDS; s++) {
        uint8_t led = cp_slot_to_led(s);
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
        { H_OF_TUPLE(HSV_CP_DEFAULT),  S_OF_TUPLE(HSV_CP_DEFAULT),  v },
        { H_OF_TUPLE(HSV_CP_DEFAULT),  S_OF_TUPLE(HSV_CP_DEFAULT),  v },
        { H_OF_TUPLE(HSV_CP_DEFAULT),  S_OF_TUPLE(HSV_CP_DEFAULT),  v },
        { H_OF_TUPLE(HSV_CP_DEFAULT),  S_OF_TUPLE(HSV_CP_DEFAULT),  v },
    };
    for (uint8_t i = 0; i < 16; i++) store[i] = table[i];
    for (uint8_t i = 0; i < CP_NUM_LAYERS; i++) anim_store[i] = RGBLIGHT_MODE_STATIC_LIGHT;
    store[CP_CAPS_SLOT].h = store[0].h;
    store[CP_CAPS_SLOT].s = store[0].s;
    store[CP_CAPS_SLOT].v = v;
    anim_store[CP_CAPS_SLOT] = RGBLIGHT_MODE_RAINBOW_SWIRL;
    ee_save();
}

/* -- Public API ----------------------------------------------------------- */

void cp_color_init(void) {
    if (!ee_load()) {
        // Fresh EEPROM
        seed_defaults();
    } else {
        // Existing EEPROM: ensure Caps has a color and anim
        uint8_t v = rgblight_get_val();

        if (store[CP_CAPS_SLOT].s == 0 && store[CP_CAPS_SLOT].v == 0) {
            store[CP_CAPS_SLOT].h = store[0].h;
            store[CP_CAPS_SLOT].s = store[0].s;
            store[CP_CAPS_SLOT].v = v;
        }
        if (anim_store[CP_CAPS_SLOT] == 0) {
            anim_store[CP_CAPS_SLOT] = RGBLIGHT_MODE_RAINBOW_SWIRL;
        }
    }
}

bool cp_color_active(void) {
    return active && !window_expired();
}

/* Called when CP layer is released: stop immediately and persist if needed */
void cp_color_release(void) {
     if (active) {
        cp_anim_preview_cancel(); /* stop full-bar preview instantly */
        active = false;
        if (cp_dirty) { ee_save(); cp_dirty = false; }
    }
}

void cp_color_ensure_active(void) {
    if (!active) {
        sel = 0;
        first_render = false;
        active = true;
        mark_active();
        HSV16 s = store[sel];
        s.v = rgblight_get_val();
        set_mask_with_hsv(CP_PATS[sel], s);
    }
}

uint8_t cp_color_selected(void) {
    return sel;
}

void cp_color_reset_to_defaults(void) {
    seed_defaults();
    first_render = true;
    mark_active();
}

void cp_color_task(void) {
    if (!cp_color_active()) {
        active = false;
        return;
    }

    /* Full-bar animation preview overrides normal CP drawing */
    cp_anim_preview_tick();
    if (cp_prev.active) {
        /* Nothing else to do here: rgblight engine is animating the full strip */
        return;
    }

    /* Optional: gentle breathing while showing the masked layer-ID */
#ifndef CP_UI_BREATHE
#    define CP_UI_BREATHE 1
#endif

    uint8_t target = (sel < CP_NUM_LAYERS) ? sel : CP_CAPS_SLOT;

    HSV16 s = store[target];
    {
        uint8_t base_v = rgblight_get_val();
#if CP_UI_BREATHE
        uint8_t amp   = (uint8_t)((base_v >> 2) + 8);
        uint8_t delta = cp_breathe_delta(/*period*/1100, amp);
        uint16_t vv   = (uint16_t)base_v + delta;
        s.v = (vv > 255) ? 255 : (uint8_t)vv;
#else
        s.v = base_v;
#endif
    }

    set_mask_with_hsv(cp_mask_for_sel(sel), s);
}

/* -- Key processing ------------------------------------------------------- */

bool cp_color_process(uint16_t keycode, keyrecord_t *record) {
    if (!record->event.pressed) return true;

    switch (keycode) {
        case CP_LAYER_DEC:
        case CP_LAYER_INC: {
            if (!active) {
                sel = 0;
                first_render = false;
                active = true;
                mark_active();
            }
            int dir = (keycode == CP_LAYER_DEC) ? -1 : +1;

            // Now cycles 0..(CP_NUM_LAYERS-1) + CP_CAPS_SLOT (Caps)
            sel = (uint8_t)clampi((int)sel + dir, 0, CP_CAPS_SLOT);

            // Real layers 0..11 use their own index; caps uses CP_CAPS_SLOT
            uint8_t target = (sel < CP_NUM_LAYERS) ? sel : CP_CAPS_SLOT;

            HSV16 s = store[target];
            s.v = rgblight_get_val();

            set_mask_with_hsv(cp_mask_for_sel(sel), s);
            mark_active();
            return false;
        }
        case CP_HUE_DEC:
        case CP_HUE_INC: {
            if (!active) {
                sel = 0;
                first_render = true;
                active = true;
                mark_active();
            }

            // If we've scrolled past the last layer, we're editing Caps
            uint8_t target = (sel < CP_NUM_LAYERS) ? sel : CP_CAPS_SLOT;

            work = store[target];
            work.v = rgblight_get_val();

            int step = (keycode == CP_HUE_DEC) ? -(int)RGBLIGHT_HUE_STEP : (int)RGBLIGHT_HUE_STEP;
            work.h = wrap360((int)work.h + step);

            set_mask_with_hsv(cp_mask_for_sel(sel), work);

            store[target].h = work.h;
            cp_dirty = true;
            mark_active();
            return false;
        }
        case CP_SAT_DEC:
        case CP_SAT_INC: {
            if (!active) {
                sel = 0;
                first_render = true;
                active = true;
                mark_active();
            }

            uint8_t target = (sel < CP_NUM_LAYERS) ? sel : CP_CAPS_SLOT;

            work = store[target];
            work.v = rgblight_get_val();

            int sstep = (keycode == CP_SAT_DEC) ? -(int)RGBLIGHT_SAT_STEP : (int)RGBLIGHT_SAT_STEP;
            work.s = clamp255((int)work.s + sstep);

            set_mask_with_hsv(cp_mask_for_sel(sel), work);

            store[target].s = work.s;
            cp_dirty = true;
            mark_active();
            return false;
        }
        case CP_ANIM_INC:
        case CP_ANIM_DEC: {
            if (!active) {
                sel = 0;
                first_render = true;
                active = true;
                mark_active();
            }

            /* Choose next/prev mode using rgblight's stepper to keep parity with QMK effects */
            if (keycode == CP_ANIM_INC) {
                rgblight_step_noeeprom();
            } else {
                rgblight_step_reverse_noeeprom();
            }
            uint8_t mode = rgblight_get_mode();

            // Decide whether we're editing a real layer or Caps
            uint8_t target = (sel < CP_NUM_LAYERS) ? sel : CP_CAPS_SLOT;

            if (target == CP_CAPS_SLOT) {
                cp_color_set_caps_anim(mode, /*persist=*/false);
            } else {
                cp_color_set_layer_anim(target, mode, /*persist=*/false);
            }

            cp_dirty = true;

            /* Kick off 3s full-bar preview using the new mode */
            cp_anim_preview_begin(target, mode);
            mark_active();
            return false;
        }
        case CP_RESET: {
            seed_defaults();

            uint8_t target = (sel < CP_NUM_LAYERS) ? sel : CP_CAPS_SLOT;

            HSV16 s = store[target];
            s.v = rgblight_get_val();

            set_mask_with_hsv(cp_mask_for_sel(sel), s);
            mark_active();
            return false;
        }
    }
    return true;
}
