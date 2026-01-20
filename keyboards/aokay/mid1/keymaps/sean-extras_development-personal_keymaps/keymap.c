/* -------------------------------------------------------------------------
 * MID.1 – Keymap (oneillseanm)
 * -------------------------------------------------------------------------
 * Subsystems
 *  - Pomodoro timer (hold-to-edit layers, 6-LED bar painter)
 *  - Color Picker (cp_color module; H/S/Anim with per-slot persistence)
 *  - CapsLock rainbow swirl (mode-only; respects visual-lock ownership)
 *  - Layer-state boot colors
 *
 * Style: 4-space indent, K&R braces, ~100-char wrap, thin bar headers.
 * ------------------------------------------------------------------------- */

#include QMK_KEYBOARD_H
#include "keymap_defines.h"
#include "mid1_custom_keycodes.h"
#include "config.h"
#include "cp_color.h"

/* -------------------------------------------------------------------------
 * Pomodoro (drop-in)
 * ------------------------------------------------------------------------- */

/* -- Config (tweak if needed) --------------------------------------------- */
#define CP_NUM_STYLE_TARGETS (CP_NUM_LAYERS + 1)  // +1 for caps
#define EE_UNINIT_BYTE 0xFF
#define LED_ALL_MASK ((uint8_t)((1u << POMO_LED_COUNT) - 1u))
#define POMO_LED_FIRST         0     /* index of the first of the 8 LEDs used by the Pomodoro bar */
#define POMO_LED_COUNT         8     /* physical LED count */
#define POMO_DEFAULT_WORK_MIN  20    /* default work minutes at power-on */
#define POMO_DEFAULT_BREAK_MIN 10    /* default break minutes at power-on */
#define POMO_PREVIEW_MS        2000  /* preview-window duration when adjusting with timer OFF */
#define POMO_RAINBOW_MS        3000  /* duration of rainbow wave on period completion */
#define POMO_BLINK_LOW_RATIO   40    /* percent of current brightness for the low phase */
#define POMO_LEDS_REVERSED     1     /* set 1 if your bar is wired right->left */
#define POMO_PULSE_LOW_PCT     35    /* low phase brightness as % of current V */
#define POMO_PULSE_HIGH_PCT    100   /* high phase brightness as % of current V */
#define POMO_SWEEP_STEP_MS     90    /* each sweep step duration (tweak to taste) */
#define POMO_DOUBLE_BLINK_MS   450   /* preview double-pulse total duration */
#define POMO_TIP_PERIOD_MS     1200  /* gentle, slower than sub-5 pulses */
#define POMO_TIP_LOW_PCT       8     /* avoid full black to prevent flicker */
#define POMO_TIP_HIGH_PCT      90
#define WORK_EDIT_LAYER        6     /* match your keymap.json */
#define BREAK_EDIT_LAYER       7
#ifndef POMO_IDLE_PULSE
#   define POMO_IDLE_PULSE     0     /* 1=enable idle breathing, 0=keep solid */
#endif


static uint8_t saved_mode __attribute__((unused))     = 0;
static bool    mode_suspended __attribute__((unused)) = false;

static bool timer_paused  = false;
static bool was_cfg_layer = false;   /* edge detect for hold/release */

static bool cp_boot_anim_enabled = true;
static bool startup_anim_was_active = false;

#ifdef EECONFIG_USERSPACE
#    define EE_CP_BOOT_ANIM_ENABLED (EECONFIG_USERSPACE)  /* first byte of userspace */
#endif

/* -- Timer-config layer hooks (match your keymap.json layer numbers) ------ */
static inline bool work_config_layer_on(void)  { return layer_state_is(WORK_EDIT_LAYER);  }
static inline bool break_config_layer_on(void) { return layer_state_is(BREAK_EDIT_LAYER); }

/* -- Keycodes (provided in mid1_config.h), listed here for clarity -------- */
/* POMO_TOGGLE, POMO_WORK_DEC, POMO_WORK_INC, POMO_BREAK_DEC, POMO_BREAK_INC */

/* -- Colors --------------------------------------------------------------- */
typedef struct { uint16_t h; uint8_t s; uint8_t v; } HSV_t;

static inline HSV_t work_color_from_layer(void) {
    HSV_t hsv = { .h = rgblight_get_hue(), .s = rgblight_get_sat(), .v = rgblight_get_val() };
    return hsv;
}
static inline HSV_t break_color_white(void) {
    HSV_t hsv = HSV_LITERAL(HSV_POMO_BREAK);
    hsv.v = rgblight_get_val();
    return hsv;
}

/* -- State machine -------------------------------------------------------- */
typedef enum {
    PSTATE_OFF = 0,
    PSTATE_WORK,
    PSTATE_BREAK,
    PSTATE_RAINBOW  /* transitional animation (0->break) or (break->off→work toggle) */
} pomo_state_t;

static pomo_state_t pstate = PSTATE_OFF;

/* Settings (5-minute steps, stored as minutes) */
static uint8_t work_setting_min  = POMO_DEFAULT_WORK_MIN;
static uint8_t break_setting_min = POMO_DEFAULT_BREAK_MIN;

/* Runtime */
static uint32_t period_ms_total = 0;           /* ms for current period (work or break) */
static uint32_t period_ms_left  = 0;           /* ms remaining */

/* Animation timers */
static uint32_t anim_started_at = 0;
static bool     anim_gate       = false;       /* for blink toggles etc. */

/* Preview while OFF (first press shows, second press within window changes) */
typedef enum { PV_NONE = 0, PV_WORK, PV_BREAK } preview_kind_t;
static preview_kind_t preview_kind    = PV_NONE;
static uint32_t       preview_until   = 0;
static bool           preview_blinked = false; /* blocks repeat double-blink during window */

/* -- LED pattern map (8 LEDs) -------------------------------------------- */
/* Index 0..15 covering these ranges (minutes remaining):
 * 0:  1..0   -> IOOOOOOI
 * 1:  5..0   -> IOOOOOOO
 * 2: 10..5   -> IIOOOOOI
 * 3: 15..10  -> IIIOOOOI
 * 4: 20..15  -> IIIIOOOI
 * 5: 25..20  -> IIIIIOOI
 * 6: 30..25  -> IIIIIIOI
 * 7: 35..30  -> IIIIIIIO
 * 8: 40..35  -> IIIIIIII
 * 9: 45..40  -> OIIIIIII
 * 10:50..45  -> OOIIIIII
 * 11:55..50  -> OOOIIIII
 * 12:60..55  -> OOOOIIII
 * 13:65..60  -> OOOOOIII
 * 14:70..65  -> OOOOOOII
 * 15:75..70  -> OOOOOOOI
 *
 * Encoded as bits b7..b0 (LED index 0 is leftmost shown first).
 */
static const uint8_t PATS[16] = {
    0b10000001, /* 0:  1 min  -> IOOOOOOI  (special) */
    0b10000000, /* 1:  5 min  -> IOOOOOOO */
    0b11000000, /* 2: 10 min  -> IIOOOOOI */
    0b11100000, /* 3: 15 min  -> IIIOOOOI */
    0b11110000, /* 4: 20 min  -> IIIIOOOI */
    0b11111000, /* 5: 25 min  -> IIIIIOOI */
    0b11111100, /* 6: 30 min  -> IIIIIIOI */
    0b11111110, /* 7: 35 min  -> IIIIIIIO */
    0b11111111, /* 8: 40 min  -> IIIIIIII */
    0b01111111, /* 9: 45 min  -> OIIIIIII */
    0b00111111, /* 10:50 min  -> OOIIIIII */
    0b00011111, /* 11:55 min  -> OOOIIIII */
    0b00001111, /* 12:60 min  -> OOOOIIII */
    0b00000111, /* 13:65 min  -> OOOOOIII */
    0b00000011, /* 14:70 min  -> OOOOOOII */
    0b00000001  /* 15:75 min  -> OOOOOOOI */
};


/* -- Minute helpers ------------------------------------------------------- */
static uint8_t clamp_1_to_75(uint8_t m) {
    if (m < 1)  return 1;
    if (m > 75) return 75;
    return m;
}
static uint8_t minus5_or_to1(uint8_t m) {
    return (m > 5) ? (uint8_t)(m - 5) : 1;
}
static uint8_t plus5_from1(uint8_t m) {
    if (m == 1) return 5;
    uint8_t nxt = (uint8_t)(m + 5);
    return nxt > 75 ? 75 : nxt;
}

/* -- Start animation (sweep) ---------------------------------------------- */
static bool     in_start_sweep     = false;
static uint8_t  sweep_current      = 1;     /* starts from IOOOOO (index 1) */
static uint8_t  sweep_target       = 1;     /* where to stop (pattern index) */
static uint32_t sweep_started_at   = 0;
static uint32_t sweep_last_step    = 0;
static bool     sweep_wrapped      = false; /* have we wrapped 11->1 at least once? */
static bool     sweep_completed    = false; /* has the start sweep already run this period? */

static void boot_anim_load_config(void) {
#ifdef EE_CP_BOOT_ANIM_ENABLED
    /* Read raw byte from EEPROM.
     *  - LED_ALL_MASK means 'uninitialized' (fresh EEPROM).
     *  - 0 means disabled.
     *  - non-zero means enabled.
     */
    uint8_t raw = eeprom_read_byte((void *)EE_CP_BOOT_ANIM_ENABLED);

    if (raw == EE_UNINIT_BYTE) {
        /* First time: default ON and write it back so it's no longer LED_ALL_MASK */
        cp_boot_anim_enabled = true;
        eeprom_update_byte((void *)EE_CP_BOOT_ANIM_ENABLED, 1);
    } else {
        cp_boot_anim_enabled = (raw != 0);
    }
#else
    /* If for some reason userspace isn't available, just default to ON in RAM */
    cp_boot_anim_enabled = true;
#endif
}

/* Given current remaining, compute next lower/upper 5-min boundary (for DEC/INC while running) */
static uint32_t step_down_ms(uint32_t ms_left) {
    uint32_t m   = ms_left / 60000u; /* floor minutes */
    uint32_t rem = ms_left % 60000u;
    if (m <= 5) return 5 * 60000u;
    if (rem == 0) {
        /* exact boundary: go down one bucket */
        return (uint32_t)(m - 5) * 60000u;
    } else {
        /* snap to lower bucket */
        uint8_t down = (uint8_t)(m / 5) * 5;
        return (uint32_t)down * 60000u;
    }
}
static uint32_t step_up_ms(uint32_t ms_left) {
    uint32_t m   = ms_left / 60000u;  /* floor minutes */
    uint32_t rem = ms_left % 60000u;
    if (m >= 75) return 75 * 60000u;
    if (rem == 0) {
        /* exact boundary: go up one bucket */
        return (uint32_t)(m + 5) * 60000u;
    } else {
        /* snap to upper bucket */
        uint8_t up = (uint8_t)(((m + 5) / 5) * 5);
        if (up < 5) up = 5;
        if (up > 75) up = 75;
        return (uint32_t)up * 60000u;
    }
}

/* -- Minute & pattern helpers (shared so mask and animation stay in sync) - */
static inline uint8_t minutes_ceil_from_ms(uint32_t ms_left) {
    uint32_t m = (ms_left + 59999u) / 60000u; /* ceil */
    if (m > 55) m = 55;
    return (uint8_t)m;
}
static inline uint8_t pattern_index_from_minutes(uint8_t m) {
    /* 0: 1-minute special
     * 1..15: 5-minute buckets up to 75
     */
    if (m <= 1) return 0;
    if (m > 75) m = 75;

    /* Ceil to nearest 5: 2..5->1, 6..10->2, ... 71..75->15 */
    uint8_t bucket = (uint8_t)((m + 4) / 5);
    if (bucket < 1)  bucket = 1;
    if (bucket > 15) bucket = 15;
    return bucket;
}
static uint32_t __attribute__((unused)) ms_from_setting(uint8_t minutes) {
    return (uint32_t)minutes * 60000u;
}

/* -- Breathing animation period table ------------------------------------- */
/* Returns one full bright→dim→bright cycle duration (ms) by minute bucket */
static inline uint16_t pulse_period_for_min(uint8_t m) {
    if (m >= 5) return 1300; /* slightly slower overall (tunable) */
    if (m == 4) return 1000;
    if (m == 3) return  800;
    if (m == 2) return  600;
    return 450;              /* m == 1 (calmer than before) */
}

/* -- Helper prototypes ---------------------------------------------------- */
static void set_led_mask(uint8_t mask, HSV_t hsv);

#if 1
/* -- Breathing tip -------------------------------------------------------- */
static int8_t rightmost_active_led_from_mask(uint8_t mask) {
    /* If wiring is reversed, “rightmost” in physical space is the lower i. */
    bool search_high_to_low = !POMO_LEDS_REVERSED;

    if (search_high_to_low) {
        for (int8_t i = POMO_LED_COUNT - 1; i >= 0; i--) {
            uint8_t bit_index = POMO_LEDS_REVERSED ? i : (POMO_LED_COUNT - 1 - i);
            if ((mask >> bit_index) & 0x1) return (int8_t)(POMO_LED_FIRST + i);
        }
    } else {
        for (uint8_t i = 0; i < POMO_LED_COUNT; i++) {
            uint8_t bit_index = POMO_LEDS_REVERSED ? i : (POMO_LED_COUNT - 1 - i);
            if ((mask >> bit_index) & 0x1) return (int8_t)(POMO_LED_FIRST + i);
        }
    }
    return -1;
}
#endif

/* Soft triangle-wave pulse on a single LED (keeps hue/sat from base_hsv) */
static void run_subtle_tip_pulse_at(uint8_t led_index, HSV_t base_hsv) {
    const uint16_t period = POMO_TIP_PERIOD_MS;
    uint32_t t     = timer_read32();
    uint32_t phase = period ? (t % period) : 0;
    uint32_t half  = period / 2;
    uint16_t tri   = (phase <= half)
        ? (uint16_t)(phase * 100 / (half ? half : 1))
        : (uint16_t)((period - phase) * 100 / (half ? half : 1));
    uint8_t base_v = rgblight_get_val();
    uint8_t v_lo   = (uint8_t)((uint16_t)base_v * POMO_TIP_LOW_PCT  / 100);
    uint8_t v_hi   = (uint8_t)((uint16_t)base_v * POMO_TIP_HIGH_PCT / 100);
    uint8_t v      = (uint8_t)(v_lo + ((uint16_t)(v_hi - v_lo) * tri) / 100);
    HSV_t h = base_hsv; h.v = v;
    rgblight_sethsv_at(h.h, h.s, h.v, led_index);
}

/* Soft triangle-wave pulse that keeps hue/sat from base_hsv */
static void run_subtle_tip_pulse(uint8_t mask, HSV_t base_hsv) {
    // pick a period (use your existing constant if you have one)
    const uint16_t period = POMO_TIP_PERIOD_MS;  // or whatever you already use

    uint32_t t     = timer_read32();
    uint32_t phase = period ? (t % period) : 0;

    /* Triangle wave 0..100..0 */
    uint32_t half    = period / 2;
    uint16_t tri_pct = (phase <= half)
        ? (uint16_t)(phase * 100 / (half ? half : 1))
        : (uint16_t)((period - phase) * 100 / (half ? half : 1));

    /* Brightness range is a fraction of the current global value */
    uint8_t base_v  = rgblight_get_val();
    uint8_t v_lo    = (uint8_t)((uint16_t)base_v * POMO_TIP_LOW_PCT  / 100);
    uint8_t v_hi    = (uint8_t)((uint16_t)base_v * POMO_TIP_HIGH_PCT / 100);
    uint8_t v       = (uint8_t)(v_lo + ((uint16_t)(v_hi - v_lo) * tri_pct) / 100);

    HSV_t h = base_hsv;
    h.v = v;

    set_led_mask(mask, h); /* unchanged: use your existing mask */
}

/* -- Rendering helpers ---------------------------------------------------- */

static void set_led_mask(uint8_t mask, HSV_t hsv) {
    for (uint8_t i = 0; i < POMO_LED_COUNT; i++) {
        uint8_t bit_index = POMO_LEDS_REVERSED ? i : (POMO_LED_COUNT - 1 - i);
        uint8_t on        = (mask >> bit_index) & 0x1;
        uint8_t led_index = POMO_LED_FIRST + i;
        if (on) {
            rgblight_sethsv_at(hsv.h, hsv.s, hsv.v, led_index);
        } else {
            rgblight_sethsv_at(0, 0, 0, led_index);
        }
    }
}

/* Triple/double blink engines (mask-driven) */
static void run_blink(uint32_t total_ms, uint8_t cycles, uint8_t mask, HSV_t hsv) {
    uint32_t t          = timer_elapsed32(anim_started_at);
    uint32_t phase_cnt  = cycles * 2;
    uint32_t slot       = total_ms / phase_cnt;
    uint32_t phase      = (slot ? (t / slot) : 0);

    if (t >= total_ms) { anim_gate = true; return; }

    bool high   = (phase % 2) == 0;
    uint8_t vhi = hsv.v;
    uint8_t vlo = (uint8_t)((uint16_t)hsv.v * POMO_BLINK_LOW_RATIO / 100);

    HSV_t h = hsv;
    h.v = high ? vhi : vlo;
    set_led_mask(mask, h);
}

/* Breathing under 5 minutes (minute-based ramp) */
static void run_pulse(uint8_t minute_bucket, uint8_t mask, HSV_t hsv) {
    uint8_t  m      = minute_bucket;
    uint16_t period = pulse_period_for_min(m);

    uint32_t t     = timer_read32();
    uint32_t phase = period ? (t % period) : 0;

    /* Triangle wave 0..100..0 */
    uint32_t half    = period / 2;
    uint16_t tri_pct = (phase <= half)
        ? (uint16_t)(phase * 100 / (half ? half : 1))
        : (uint16_t)((period - phase) * 100 / (half ? half : 1));

    /* 1:00 distinct but a touch calmer (shallower low; period already slower above) */
    uint8_t low_pct  = (m == 1) ? 30 : POMO_PULSE_LOW_PCT;  /* default LOW is 35 for 5..2 */
    uint8_t high_pct = (m == 1) ? 100: POMO_PULSE_HIGH_PCT;

    uint8_t base_v = rgblight_get_val();
    uint8_t v_lo   = (uint8_t)((uint16_t)base_v * low_pct  / 100);
    uint8_t v_hi   = (uint8_t)((uint16_t)base_v * high_pct / 100);
    uint8_t v      = (uint8_t)(v_lo + ((uint16_t)(v_hi - v_lo) * tri_pct) / 100);

    HSV_t h = hsv; h.v = v;
    set_led_mask(mask, h);
}

/* Simple 6-LED rainbow wave for POMO_RAINBOW */
static void run_rainbow_wave(void) {
    uint32_t t = timer_elapsed32(anim_started_at);
    if (t >= POMO_RAINBOW_MS) { anim_gate = true; return; }
    /* Walk hue across the 6 LEDs over time */
    uint16_t base_h = (uint16_t)((t / 6) % 256);
    for (uint8_t i = 0; i < POMO_LED_COUNT; i++) {
        uint16_t h = (base_h + (i * 32)) % 256;
        rgblight_sethsv_at(h, 255, rgblight_get_val(), POMO_LED_FIRST + i);
    }
}

/* Rainbow over only the LEDs enabled by `mask` (bits b5..b0) */
static void run_rainbow_over_mask(uint8_t mask) {
    uint32_t t      = timer_elapsed32(anim_started_at);
    uint16_t base_h = (uint16_t)((t / 6) % 256);

    for (uint8_t i = 0; i < POMO_LED_COUNT; i++) {
        uint8_t bit_index = POMO_LEDS_REVERSED ? i : (POMO_LED_COUNT - 1 - i);
        bool    on        = (mask >> bit_index) & 0x1;
        uint8_t led_index = POMO_LED_FIRST + i;

        if (on) {
            uint16_t h = (base_h + (i * 32)) % 256; /* per-LED offset */
            rgblight_sethsv_at(h, 255, rgblight_get_val(), led_index);
        } else {
            rgblight_sethsv_at(0, 0, 0, led_index);
        }
    }
}

/* Forward declaration so stop_period can call it */
static inline bool visual_lock_active(void);

/* -- Period control ------------------------------------------------------- */
static void start_period(pomo_state_t next, uint8_t minutes) {
    /* Clamp minutes to 1–75 for safety */
    minutes = clamp_1_to_75(minutes);

    /* Set state */
    pstate = next;

    /* Compute total/remaining ms */
    period_ms_total = (uint32_t)minutes * 60000u;
    period_ms_left  = period_ms_total;

    /* Reset animation timing */
    anim_started_at = timer_read32();
    anim_gate       = false;

    /* Reset sweep state so housekeeping can start a fresh sweep */
    in_start_sweep   = false;
    sweep_completed  = false;
    sweep_wrapped    = false;
    sweep_current    = 1;
    sweep_target     = pattern_index_from_minutes(minutes);
    sweep_started_at = 0;
    sweep_last_step  = 0;

    /* Clear preview / pause state */
    preview_kind     = PV_NONE;
    preview_blinked  = false;
    timer_paused     = false;
}

static void stop_period(void) {
    /* Return to OFF state and clear timers */
    pstate          = PSTATE_OFF;
    period_ms_total = 0;
    period_ms_left  = 0;

    /* Reset sweep / preview / pause state */
    in_start_sweep  = false;
    sweep_completed = false;
    sweep_wrapped   = false;
    preview_kind    = PV_NONE;
    preview_blinked = false;
    timer_paused    = false;

    /* Restore standing layer’s baseline lighting if nothing else owns LEDs */
    if (!visual_lock_active()) {
        uint8_t layer = get_highest_layer(layer_state);
        HSV16   hsv   = cp_color_get_layer_hsv(layer);
        uint8_t val   = rgblight_get_val();
        uint8_t mode  = cp_color_get_layer_anim(layer);

        rgblight_enable_noeeprom();
        rgblight_mode_noeeprom(mode);
        rgblight_sethsv_noeeprom(hsv.h, hsv.s, val);
    }
}

/* -- Caps effect (forward decls so start/stop can call into it) ----------- */
static bool    caps_active    = false;
static bool    caps_suspended = false;

static inline bool visual_lock_active(void) {
    return (pstate != PSTATE_OFF) || cp_color_active();
}

/* Forward prototype so there's no implicit declaration */
static void caps_effect_set(bool on);

/* -------------------------------------------------------------------------
 * Timer pause management
 * ------------------------------------------------------------------------- */
static inline bool timer_config_layer_held(void) {
    /* Holding Work layer pauses during WORK; holding Break layer pauses during BREAK */
    return ((pstate == PSTATE_WORK)  && work_config_layer_on()) ||
           ((pstate == PSTATE_BREAK) && break_config_layer_on());
}
static void update_timer_pause(void) {
    bool held = timer_config_layer_held();
    if (held && !was_cfg_layer) {
        timer_paused = true;   /* just pressed → pause */
    } else if (!held && was_cfg_layer) {
        timer_paused = false;  /* just released → resume */
    }
    was_cfg_layer = held;
}

/* -------------------------------------------------------------------------
 * Encoders: while on CP layers 8/9/10, hijack for selection/edit
 * ------------------------------------------------------------------------- */
bool encoder_update_user(uint8_t index, bool clockwise) {
    if (layer_state_cmp(layer_state, 8) ||
        layer_state_cmp(layer_state, 9) ||
        layer_state_cmp(layer_state, 10)) {
        bool left = (index == 0);  /* flip to (index == 1) if your hardware is swapped */
        if (left) {
            tap_code(clockwise ? CP_LAYER_INC : CP_LAYER_DEC);
        } else {
            if      (layer_state_cmp(layer_state, 8)) tap_code(clockwise ? CP_HUE_INC  : CP_HUE_DEC);
            else if (layer_state_cmp(layer_state, 9)) tap_code(clockwise ? CP_SAT_INC  : CP_SAT_DEC);
            else                                       tap_code(clockwise ? CP_ANIM_INC : CP_ANIM_DEC);
        }
        return false;
    }
    /* ... your fallback encoder logic */
    return true;
}

/* -------------------------------------------------------------------------
 * Startup animation – 4 rolls, integer-only
 * ------------------------------------------------------------------------- */

#define STARTUP_ROLL_MS          400u
#define STARTUP_ROLL_COUNT       8u
#define STARTUP_STEPS_PER_ROLL   POMO_LED_COUNT
#define STARTUP_TOTAL_STEPS      (STARTUP_ROLL_COUNT * STARTUP_STEPS_PER_ROLL)

static bool     startup_anim_active     = false;
static uint32_t startup_anim_started_at = 0;

static void startup_anim_begin(void) {
    startup_anim_active     = true;
    startup_anim_started_at = timer_read32();
}

/* 
 * 4 rolls:
 *  - Rolls 0–2: single “I” runs left→right, background glows brighter each roll,
 *               and the “I” is slightly brighter than the background.
 *  - Roll 3:    IOOOOO, IIOOOO, ... IIIIII at final brightness.
 */
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
    HSV_t   hsv    = HSV_LITERAL(HSV_MID1ORANGE);

    if (roll < (STARTUP_ROLL_COUNT - 1u)) {
        /* Rolls 0..6:
         *  - baseline rises smoothly from 0 up toward ~3/4 of target
         *  - highlight is a bit above baseline, capped at target
         */
        uint16_t base_scaled = (uint16_t)target * (uint16_t)roll / (STARTUP_ROLL_COUNT); /* 0.. ~7/8 */
        uint16_t hi_scaled   = base_scaled + (uint16_t)target / 3u;
        if (hi_scaled > target) hi_scaled = target;

        uint8_t baseline_v  = (uint8_t)base_scaled;
        uint8_t highlight_v = (uint8_t)hi_scaled;

        for (uint8_t i = 0; i < POMO_LED_COUNT; i++) {
            uint8_t bit_index = POMO_LEDS_REVERSED ? i : (POMO_LED_COUNT - 1 - i);
            uint8_t led_index = POMO_LED_FIRST + i;
            uint8_t v         = (bit_index == pos_rev) ? highlight_v : baseline_v;

            hsv.v = v;
            rgblight_sethsv_at(hsv.h, hsv.s, hsv.v, led_index);
        }
    } else {
        /* Final roll (roll 7):
         *  - baseline is high (e.g. ~3/4 target)
         *  - filled side uses full target brightness
         *  - unfilled side still glows at baseline (no black)
         */
        uint8_t baseline_v  = (uint8_t)((uint16_t)target * 3u / 4u);
        uint8_t highlight_v = target;

        for (uint8_t i = 0; i < POMO_LED_COUNT; i++) {
            uint8_t bit_index = POMO_LEDS_REVERSED ? i : (POMO_LED_COUNT - 1 - i);
            uint8_t led_index = POMO_LED_FIRST + i;

            /* Reverse-direction fill: head moves with pos_rev */
            uint8_t v = (bit_index >= pos_rev) ? highlight_v : baseline_v;

            hsv.v = v;
            rgblight_sethsv_at(hsv.h, hsv.s, hsv.v, led_index);
        }
    }
}

/* -------------------------------------------------------------------------
 * Main tick (housekeeping): CP owns LEDs while active; else Pomodoro/idle
 * ------------------------------------------------------------------------- */
void housekeeping_task_user(void) {
    static bool was_cp_layers = false;

    /* If startup anim is running, let it own the LEDs */
    if (startup_anim_active) {
        startup_anim_tick();
        startup_anim_was_active = true;
        return;
    }

    /* We were animating on the last scan, but not anymore:
     * re-assert the base layer style so we don't get "stuck" in a weird state.
     */
    if (startup_anim_was_active) {
        startup_anim_was_active = false;

        /* Decide which layer's style to show.
         * If you want *always* default layer 0, just hard-code layer = 0 here.
         * Otherwise use the current highest layer, but treat CP layers as 0.
         */
        uint8_t layer = get_highest_layer(layer_state);
        if (layer == 8 || layer == 9 || layer == 10) {
            layer = 0;  /* CP edit layers shouldn't define idle style */
        }

        HSV16  c    = cp_color_get_layer_hsv(layer);
        uint8_t v   = rgblight_get_val();
        uint8_t mode= cp_color_get_layer_anim(layer);

        rgblight_mode_noeeprom(mode);
        rgblight_sethsv_noeeprom(c.h, c.s, v);
    }

    bool on_cp_layers =
        layer_state_cmp(layer_state, 8) ||
        layer_state_cmp(layer_state, 9) ||
        layer_state_cmp(layer_state, 10);

    /* During startup animation, let it fully own the LEDs */
    if (startup_anim_active) {
        startup_anim_tick();
        return;
    }

    if (on_cp_layers) {
        cp_color_ensure_active();
    } else {
        if (was_cp_layers) {
            /* Just exited CP: stop preview, persist, and apply chosen anim/color once */
            cp_color_release();
            if (!caps_active) {
                uint8_t layer = get_highest_layer(layer_state);
                HSV16   c     = cp_color_get_layer_hsv(layer);
                uint8_t v     = rgblight_get_val();
                uint8_t mode  = cp_color_get_layer_anim(layer);
                rgblight_enable_noeeprom();
                rgblight_mode_noeeprom(mode);
                rgblight_sethsv_noeeprom(c.h, c.s, v);
            }
            /* If using LED ranges, re-enable full strip: */
            /* rgblight_set_effect_range(0, RGBLED_NUM); */
        }
    }

    was_cp_layers = on_cp_layers;

    if (cp_color_active()) {
        cp_color_task();
        return;
    }

    /* Immediate display when holding a timer-config layer (timer OFF) */
    if (pstate == PSTATE_OFF) {
        if (work_config_layer_on() || break_config_layer_on()) {
            bool    is_break = break_config_layer_on();
            HSV_t   hsv      = is_break ? break_color_white() : work_color_from_layer();
            uint8_t mins     = is_break ? break_setting_min   : work_setting_min;
            uint8_t mask     = PATS[pattern_index_from_minutes(mins)];
            run_subtle_tip_pulse(mask, hsv); /* gentle breathe while editing */
            return; /* take over rendering while layer is held */
        }

        /* Preview window (double-blink then steady) */
        if (preview_kind != PV_NONE) {
            bool    expired = timer_expired32(timer_read32(), preview_until);
            HSV_t   hsv     = (preview_kind == PV_BREAK) ? break_color_white() : work_color_from_layer();
            uint8_t mins    = (preview_kind == PV_BREAK) ? break_setting_min   : work_setting_min;
            uint8_t mask    = PATS[pattern_index_from_minutes(mins)];

            if (!preview_blinked) {
                anim_started_at = timer_read32();
                anim_gate       = false;
                run_blink(POMO_DOUBLE_BLINK_MS, 2, mask, hsv);
                if (anim_gate) preview_blinked = true;
            } else {
                set_led_mask(mask, hsv);
            }
            if (expired) {
                preview_kind    = PV_NONE;
                preview_blinked = false;
            }
            return;
        }

        /* Idle OFF — only paint if mode is STATIC; otherwise let rgblight animate */
        if (rgblight_get_mode() == RGBLIGHT_MODE_STATIC_LIGHT) {
            HSV_t layer = work_color_from_layer();
            set_led_mask(LED_ALL_MASK, layer);
        }
        return;
    }

    /* Transitional RAINBOW state */
    if (pstate == PSTATE_RAINBOW) {
        run_rainbow_wave();
        if (anim_gate) {
            /* Rainbow finished -> switch to BREAK (auto) */
            anim_gate = false;
            start_period(PSTATE_BREAK, break_setting_min);
        }
        return;
    }

    /* Start sweep animation when toggled ON */
    if (!in_start_sweep && !sweep_completed && period_ms_left == period_ms_total) {
        in_start_sweep   = true;
        sweep_current    = 1;  /* IOOOOO */
        sweep_target     = pattern_index_from_minutes(minutes_ceil_from_ms(period_ms_left));
        sweep_started_at = timer_read32();
        sweep_last_step  = 0;
        sweep_wrapped    = false;

        /* If starting a 1-minute session, drive the sweep to index 1 for a full lap */
        if (sweep_target == 0) sweep_target = 1;
    }

    /* Handle the start sweep (fills LED bar once at start) */
    if (in_start_sweep) {
        HSV_t sweep_hsv = work_color_from_layer();
        set_led_mask(PATS[sweep_current], sweep_hsv);

        uint32_t elapsed = timer_elapsed32(sweep_started_at);
        uint32_t step    = elapsed / POMO_SWEEP_STEP_MS;

        while (sweep_last_step < step) {
            if (sweep_current == 11) {
                sweep_current = 1;
                sweep_wrapped = true;
            } else {
                sweep_current++;
            }
            if (sweep_wrapped && (sweep_current == sweep_target)) {
                in_start_sweep  = false;
                sweep_completed = true;
                break;
            }
            sweep_last_step++;
        }
        return; /* hold control here until sweep finishes */
    }

    /* Timer pause handling (detect hold/release of config layers) */
    update_timer_pause();

    /* Tick timer (skip countdown when paused) */
    static uint32_t last_tick = 0;
    uint32_t now = timer_read32();
    if (last_tick == 0) last_tick = now;
    uint32_t dt = timer_elapsed32(last_tick);
    if (dt >= 50) { /* 20 Hz granularity is plenty */
        last_tick = now;
        if (!timer_paused) {
            if (period_ms_left > dt) period_ms_left -= dt; else period_ms_left = 0;
        }
    }

    /* Check for end of period (Work → Rainbow, Break → Stop) */
    if (period_ms_left == 0) {
        if (pstate == PSTATE_WORK) {
            pstate          = PSTATE_RAINBOW;
            anim_started_at = timer_read32();
            anim_gate       = false;
            return;
        } else if (pstate == PSTATE_BREAK) {
            stop_period(); /* clears to standing layer state immediately */
            return;
        }
    }

    /* Render current remaining time as LED pattern */
    uint8_t m    = minutes_ceil_from_ms(period_ms_left);
    uint8_t mask = PATS[pattern_index_from_minutes(m)];
    HSV_t   hsv  = (pstate == PSTATE_BREAK) ? break_color_white() : work_color_from_layer();

    /* While PAUSED: breathe (same affordance as edit-hold while OFF) */
    if (timer_paused) {
        run_subtle_tip_pulse(mask, hsv);
        return;
    }

    /* CapsLock overlay (only when not paused) */
    if (host_keyboard_led_state().caps_lock) {
        run_rainbow_over_mask(mask);  /* only LEDs that are “on” for the timer */
        return;                       /* skip normal render this frame */
    }

    if (m <= 5) {
        /* Breathing under 5 minutes (uses same bucket) */
        run_pulse(m, mask, hsv);
    } else {
        /* Draw the progress bar */
        set_led_mask(mask, hsv);
        /* Subtle “alive” cue: breathe the rightmost active LED only */
        int8_t tip = rightmost_active_led_from_mask(mask);
        if (tip >= 0) {
            run_subtle_tip_pulse_at((uint8_t)tip, hsv);
        }
    }
}

/* -------------------------------------------------------------------------
 * Key handling
 * ------------------------------------------------------------------------- */
bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    /* Let Color Picker consume its keycodes first */
    if (!cp_color_process(keycode, record)) return false;

    if (!record->event.pressed) return true;

    switch (keycode) {
        case POMO_TOGGLE: {
            if (pstate == PSTATE_OFF) {
                /* Start WORK immediately from stored work_setting (allow 1 minute) */
                start_period(PSTATE_WORK, clamp_1_to_75(work_setting_min));
            } else {
                stop_period();
            }
            return false;
        }

        case POMO_WORK_DEC:
        case POMO_WORK_INC: {
            if (pstate == PSTATE_WORK) {
                /* Live adjust (no blink). Jump to adjacent 5-min boundary. */
                period_ms_left = (keycode == POMO_WORK_DEC) ? step_down_ms(period_ms_left)
                                                            : step_up_ms(period_ms_left);
                /* Clamp within 5..75 and also update "setting" memory. */
                uint8_t new_min = (uint8_t)((period_ms_left + 59999) / 60000);
                work_setting_min = clamp_1_to_75(((new_min + 2) / 5) * 5);
                return false;
            }
            if (pstate == PSTATE_BREAK) {
                /* Ignore work adjust during break; spec keeps adjustments scoped. */
                return false;
            }

            /* TIMER OFF */
            if (work_config_layer_on()) {
                /* IMMEDIATE commit while the Work config layer is held (no preview/double-blink) */
                if (keycode == POMO_WORK_DEC) {
                    work_setting_min = clamp_1_to_75(minus5_or_to1(work_setting_min));  /* includes 5→1 */
                } else {
                    work_setting_min = clamp_1_to_75(plus5_from1(work_setting_min));    /* 1→5, then +5 */
                }
                return false;  /* rendering handled in housekeeping (immediate display) */
            }

            /* Not in Work config layer: preview-window behavior */
            {
                uint32_t now      = timer_read32();
                bool     in_win   = (preview_kind == PV_WORK) && !timer_expired32(now, preview_until);

                if (!in_win) {
                    /* First press (or expired): show current selection for a few seconds */
                    preview_kind     = PV_WORK;
                    preview_until    = now + POMO_PREVIEW_MS;
                    preview_blinked  = false;
                    return false;
                } else {
                    /* Second press inside the window: COMMIT and show updated selection */
                    if (keycode == POMO_WORK_DEC) {
                        work_setting_min = clamp_1_to_75(minus5_or_to1(work_setting_min));
                    } else {
                        work_setting_min = clamp_1_to_75(plus5_from1(work_setting_min));
                    }
                    preview_until   = now + POMO_PREVIEW_MS;
                    preview_blinked = true;  /* skip the double-blink; steady mask after commit */
                    return false;
                }
            }
        }

        case POMO_BREAK_DEC:
        case POMO_BREAK_INC: {
            if (pstate == PSTATE_BREAK) {
                /* Live adjust during break */
                period_ms_left = (keycode == POMO_BREAK_DEC) ? step_down_ms(period_ms_left)
                                                             : step_up_ms(period_ms_left);
                uint8_t new_min = (uint8_t)((period_ms_left + 59999) / 60000);
                break_setting_min = clamp_1_to_75(((new_min + 2) / 5) * 5);
                return false;
            }
            if (pstate == PSTATE_WORK) {
                /* Ignore break adjust during work; spec keeps adjustments scoped. */
                return false;
            }

            /* TIMER OFF */
            if (break_config_layer_on()) {
                /* IMMEDIATE commit while the Break config layer is held (no preview/double-blink) */
                if (keycode == POMO_BREAK_DEC) {
                    break_setting_min = clamp_1_to_75(minus5_or_to1(break_setting_min));
                } else {
                    break_setting_min = clamp_1_to_75(plus5_from1(break_setting_min));
                }
                return false;  /* rendering handled in housekeeping (immediate display) */
            }

            /* Not in Break config layer: preview-window behavior */
            {
                uint32_t now    = timer_read32();
                bool     in_win = (preview_kind == PV_BREAK) && !timer_expired32(now, preview_until);

                if (!in_win) {
                    /* First press (or expired): show current selection for a few seconds */
                    preview_kind     = PV_BREAK;
                    preview_until    = now + POMO_PREVIEW_MS;
                    preview_blinked  = false;
                    return false;
                } else {
                    /* Second press inside the window: COMMIT and show updated selection */
                    if (keycode == POMO_BREAK_DEC) {
                        break_setting_min = clamp_1_to_75(minus5_or_to1(break_setting_min));
                    } else {
                        break_setting_min = clamp_1_to_75(plus5_from1(break_setting_min));
                    }
                    preview_until   = now + POMO_PREVIEW_MS;
                    preview_blinked = true;  /* skip the double-blink; steady mask after commit */
                    return false;
                }
            }
        }
        
        case CP_BOOT_TOG:
            cp_boot_anim_enabled = !cp_boot_anim_enabled;

#ifdef EE_CP_BOOT_ANIM_ENABLED
            eeprom_update_byte((void *)EE_CP_BOOT_ANIM_ENABLED,
                                cp_boot_anim_enabled ? 1 : 0);
#endif

            HSV_t hsv = HSV_LITERAL(HSV_MID1ORANGE);
            hsv.v = rgblight_get_val();

            if (cp_boot_anim_enabled) {
                set_led_mask(LED_ALL_MASK, hsv);
            } else {
                hsv.v = 0;
                set_led_mask(LED_ALL_MASK, hsv);
            }
            return false;

        case CP_BOOT_PLAY:
            /* Play the startup animation on demand */
            if (!startup_anim_active) {
                startup_anim_begin();
            }
            return false;

    }

    return true; /* fall-through for other keycodes */
}

/* -------------------------------------------------------------------------
 * Non-timer functionality (must remain)
 * ------------------------------------------------------------------------- */

/* -- Helpers to keep current brightness (V) when reapplying layer color -- */
#ifndef H_OF
#  define H_OF(h, s, v) (h)
#endif
#ifndef S_OF
#  define S_OF(h, s, v) (s)
#endif
#ifndef V_OF
#  define V_OF(h, s, v) (v)
#endif

/* 3-arg version: pass explicit h,s,v; preserves current V */
#ifndef SET_HS_KEEP_V3
#  define SET_HS_KEEP_V3(h, s, v)                          \
    do {                                                   \
        uint8_t __v = rgblight_get_val();                  \
        rgblight_sethsv_noeeprom((h), (s), __v);           \
    } while (0)
#endif

/* Tuple forwarder: pass a single (h,s,v) macro like (15,255,255) */
#ifndef SET_HS_KEEP_V_TUPLE
#  define SET_HS_KEEP_V_TUPLE(T)  SET_HS_KEEP_V3 T
#endif

static void caps_effect_set(bool on) {
    if (visual_lock_active()) {
        /* Don’t change rgblight mode while Pomodoro/CP are drawing per-LED */
        if (on) {
            caps_suspended = true;
        } else {
            caps_suspended = false;
        }
        return;
    }

    /* Normal behavior when not visually locked */
    if (on && !caps_active) {
        /* Turning Caps ON: apply Caps style from the Color Picker */
        caps_active = true;

        HSV16   hsv  = cp_color_get_caps_hsv();
        uint8_t val  = rgblight_get_val();
        uint8_t mode = cp_color_get_caps_anim();

        rgblight_enable_noeeprom();
        rgblight_mode_noeeprom(mode);
        rgblight_sethsv_noeeprom(hsv.h, hsv.s, val);

    } else if (!on && caps_active) {
        /* Turning Caps OFF: restore the standing layer's style */
        caps_active = false;

        uint8_t layer = get_highest_layer(layer_state);
        HSV16   base  = cp_color_get_layer_hsv(layer);
        uint8_t val   = rgblight_get_val();
        uint8_t mode  = cp_color_get_layer_anim(layer);

        rgblight_enable_noeeprom();
        rgblight_mode_noeeprom(mode);
        rgblight_sethsv_noeeprom(base.h, base.s, val);
    }
}

/* -- Layer coloring (keeps brightness sticky) ----------------------------- */

layer_state_t layer_state_set_user(layer_state_t state) {
    uint8_t layer = get_highest_layer(state);
    static uint8_t last_layer = LED_ALL_MASK;

    /* Don’t apply a static color when entering CP edit layers (8/9/10);
       let CP paint the layer-ID mask immediately instead. */
    if (layer == 8 || layer == 9 || layer == 10) {
        last_layer = layer;
        return state;
    }

    /* While the startup animation is running, don’t fight it. */
    if (startup_anim_active) {
        last_layer = layer;  /* keep tracking so we don’t “re-flash” later */
        return state;
    }

    /* Preserve Caps rainbow: while Caps is active, don't overwrite rgblight mode/HSV. */
    if (caps_active) {
        last_layer = layer;  /* still update our tracker so future changes work */
        return state;
    }

    if (layer != last_layer) {
        last_layer = layer;

        HSV16  c    = cp_color_get_layer_hsv(layer);
        uint8_t v   = rgblight_get_val();              // preserve brightness
        uint8_t mode= cp_color_get_layer_anim(layer);  // per-layer effect
        rgblight_mode_noeeprom(mode);
        rgblight_sethsv_noeeprom(c.h, c.s, v);
    }
    return state;
}

/* -- CapsLock LED hook: triggers rainbow swirl when Caps toggles ---------- */
bool led_update_user(led_t state) {
    caps_effect_set(state.caps_lock);
    return true;  /* don't block other LED updates */
}

/* -- Init ----------------------------------------------------------------- */
void keyboard_post_init_user(void) {
    rgblight_enable_noeeprom();
    rgblight_mode_noeeprom(RGBLIGHT_MODE_STATIC_LIGHT);
    SET_HS_KEEP_V_TUPLE(HSV_MID1ORANGE);

    /* Load boot animation toggle from EEPROM */
    boot_anim_load_config();

    /* Color Picker init */
    cp_color_init();

    /* Caps init in case Caps was on at boot */
    caps_effect_set(host_keyboard_led_state().caps_lock);

    /* One-time startup animation if enabled */
    if (cp_boot_anim_enabled) {
        startup_anim_begin();
    }
}
