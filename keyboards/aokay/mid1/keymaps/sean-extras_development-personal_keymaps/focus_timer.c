#include "focus_timer.h"

#if defined(FOCUS_TIMER_ENABLE) && (FOCUS_TIMER_ENABLE == yes)

#include "quantum.h"
#include "led_controller.h"
#include "mid1_custom_keycodes.h"
#include "keymap_defines.h"


/* -- Config (tweak if needed) --------------------------------------------- */
#define LED_CONTROLLER_NUM_STYLE_TARGETS (LED_CONTROLLER_NUM_LAYERS + 1)  // +1 for caps
#define EE_UNINIT_BYTE 0xFF
#ifndef LED_ALL_MASK
#    define LED_ALL_MASK 0xFF
#endif
#define FOCUS_LED_FIRST         0     /* index of the first of the 8 LEDs used by the focus timer bar */
#define FOCUS_LED_COUNT         8     /* physical LED count */
#define FOCUS_DEFAULT_WORK_MIN  20    /* default work minutes at power-on */
#define FOCUS_DEFAULT_BREAK_MIN 10    /* default break minutes at power-on */
#define FOCUS_PREVIEW_MS        2000  /* preview-window duration when adjusting with timer OFF */
#define FOCUS_RAINBOW_MS        3000  /* duration of rainbow wave on period completion */
#define FOCUS_BLINK_LOW_RATIO   40    /* percent of current brightness for the low phase */
#define FOCUS_LEDS_REVERSED     1     /* set 1 if your bar is wired right->left */
#define FOCUS_PULSE_LOW_PCT     35    /* low phase brightness as % of current V */
#define FOCUS_PULSE_HIGH_PCT    100   /* high phase brightness as % of current V */
#define FOCUS_SWEEP_STEP_MS     90    /* each sweep step duration (tweak to taste) */
#define FOCUS_DOUBLE_BLINK_MS   450   /* preview double-pulse total duration */
#define FOCUS_TIP_PERIOD_MS     1200  /* gentle, slower than sub-5 pulses */
#define FOCUS_TIP_LOW_PCT       8     /* avoid full black to prevent flicker */
#define FOCUS_TIP_HIGH_PCT      90
#define WORK_EDIT_LAYER         6     /* match your keymap.json */
#define BREAK_EDIT_LAYER        7
#ifndef FOCUS_IDLE_PULSE
#   define FOCUS_IDLE_PULSE     0     /* 1=enable idle breathing, 0=keep solid */
#endif

static uint8_t saved_mode __attribute__((unused))     = 0;
static bool    mode_suspended __attribute__((unused)) = false;

static bool timer_paused  = false;
static bool was_cfg_layer = false;   /* edge detect for hold/release */

/* -- Timer-config layer hooks (match your keymap.json layer numbers) ------ */
static inline bool work_config_layer_on(void)  { return layer_state_is(WORK_EDIT_LAYER);  }
static inline bool break_config_layer_on(void) { return layer_state_is(BREAK_EDIT_LAYER); }

/* -- Colors --------------------------------------------------------------- */
typedef struct { uint16_t h; uint8_t s; uint8_t v; } HSV_t;

static inline HSV_t work_color_from_layer(void) {
    HSV_t hsv = { .h = rgblight_get_hue(), .s = rgblight_get_sat(), .v = rgblight_get_val() };
    return hsv;
}
static inline HSV_t break_color_white(void) {
    HSV_t hsv = HSV_LITERAL(HSV_FOCUS_BREAK);
    hsv.v = rgblight_get_val();
    return hsv;
}

/* -- State machine -------------------------------------------------------- */
typedef enum {
    FOCUS_STATE_OFF = 0,
    FOCUS_STATE_WORK,
    FOCUS_STATE_BREAK,
    FOCUS_STATE_RAINBOW  /* transitional animation (0->break) or (break->off→work toggle) */
} focus_state_t;

static focus_state_t focus_state = FOCUS_STATE_OFF;

/* Settings (5-minute steps, stored as minutes) */
static uint8_t work_setting_min  = FOCUS_DEFAULT_WORK_MIN;
static uint8_t break_setting_min = FOCUS_DEFAULT_BREAK_MIN;

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
    bool search_high_to_low = !FOCUS_LEDS_REVERSED;

    if (search_high_to_low) {
        for (int8_t i = FOCUS_LED_COUNT - 1; i >= 0; i--) {
            uint8_t bit_index = FOCUS_LEDS_REVERSED ? i : (FOCUS_LED_COUNT - 1 - i);
            if ((mask >> bit_index) & 0x1) return (int8_t)(FOCUS_LED_FIRST + i);
        }
    } else {
        for (uint8_t i = 0; i < FOCUS_LED_COUNT; i++) {
            uint8_t bit_index = FOCUS_LEDS_REVERSED ? i : (FOCUS_LED_COUNT - 1 - i);
            if ((mask >> bit_index) & 0x1) return (int8_t)(FOCUS_LED_FIRST + i);
        }
    }
    return -1;
}
#endif

/* Soft triangle-wave pulse on a single LED (keeps hue/sat from base_hsv) */
static void run_subtle_tip_pulse_at(uint8_t led_index, HSV_t base_hsv) {
    const uint16_t period = FOCUS_TIP_PERIOD_MS;
    uint32_t t     = timer_read32();
    uint32_t phase = period ? (t % period) : 0;
    uint32_t half  = period / 2;
    uint16_t tri   = (phase <= half)
        ? (uint16_t)(phase * 100 / (half ? half : 1))
        : (uint16_t)((period - phase) * 100 / (half ? half : 1));
    uint8_t base_v = rgblight_get_val();
    uint8_t v_lo   = (uint8_t)((uint16_t)base_v * FOCUS_TIP_LOW_PCT  / 100);
    uint8_t v_hi   = (uint8_t)((uint16_t)base_v * FOCUS_TIP_HIGH_PCT / 100);
    uint8_t v      = (uint8_t)(v_lo + ((uint16_t)(v_hi - v_lo) * tri) / 100);
    HSV_t h = base_hsv; h.v = v;
    rgblight_sethsv_at(h.h, h.s, h.v, led_index);
}

/* Soft triangle-wave pulse that keeps hue/sat from base_hsv */
static void run_subtle_tip_pulse(uint8_t mask, HSV_t base_hsv) {
    // pick a period (use your existing constant if you have one)
    const uint16_t period = FOCUS_TIP_PERIOD_MS;  // or whatever you already use

    uint32_t t     = timer_read32();
    uint32_t phase = period ? (t % period) : 0;

    /* Triangle wave 0..100..0 */
    uint32_t half    = period / 2;
    uint16_t tri_pct = (phase <= half)
        ? (uint16_t)(phase * 100 / (half ? half : 1))
        : (uint16_t)((period - phase) * 100 / (half ? half : 1));

    /* Brightness range is a fraction of the current global value */
    uint8_t base_v  = rgblight_get_val();
    uint8_t v_lo    = (uint8_t)((uint16_t)base_v * FOCUS_TIP_LOW_PCT  / 100);
    uint8_t v_hi    = (uint8_t)((uint16_t)base_v * FOCUS_TIP_HIGH_PCT / 100);
    uint8_t v       = (uint8_t)(v_lo + ((uint16_t)(v_hi - v_lo) * tri_pct) / 100);

    HSV_t h = base_hsv;
    h.v = v;

    set_led_mask(mask, h); /* unchanged: use your existing mask */
}

/* -- Rendering helpers ---------------------------------------------------- */

static void set_led_mask(uint8_t mask, HSV_t hsv) {
    for (uint8_t i = 0; i < FOCUS_LED_COUNT; i++) {
        uint8_t bit_index = FOCUS_LEDS_REVERSED ? i : (FOCUS_LED_COUNT - 1 - i);
        uint8_t on        = (mask >> bit_index) & 0x1;
        uint8_t led_index = FOCUS_LED_FIRST + i;
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
    uint8_t vlo = (uint8_t)((uint16_t)hsv.v * FOCUS_BLINK_LOW_RATIO / 100);

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
    uint8_t low_pct  = (m == 1) ? 30 : FOCUS_PULSE_LOW_PCT;  /* default LOW is 35 for 5..2 */
    uint8_t high_pct = (m == 1) ? 100: FOCUS_PULSE_HIGH_PCT;

    uint8_t base_v = rgblight_get_val();
    uint8_t v_lo   = (uint8_t)((uint16_t)base_v * low_pct  / 100);
    uint8_t v_hi   = (uint8_t)((uint16_t)base_v * high_pct / 100);
    uint8_t v      = (uint8_t)(v_lo + ((uint16_t)(v_hi - v_lo) * tri_pct) / 100);

    HSV_t h = hsv; h.v = v;
    set_led_mask(mask, h);
}

/* Simple 6-LED rainbow wave for FOCUS_RAINBOW */
static void run_rainbow_wave(void) {
    uint32_t t = timer_elapsed32(anim_started_at);
    if (t >= FOCUS_RAINBOW_MS) { anim_gate = true; return; }
    /* Walk hue across the 6 LEDs over time */
    uint16_t base_h = (uint16_t)((t / 6) % 256);
    for (uint8_t i = 0; i < FOCUS_LED_COUNT; i++) {
        uint16_t h = (base_h + (i * 32)) % 256;
        rgblight_sethsv_at(h, 255, rgblight_get_val(), FOCUS_LED_FIRST + i);
    }
}

/* Rainbow over only the LEDs enabled by `mask` (bits b5..b0) */
__attribute__((unused)) static void run_rainbow_over_mask(uint8_t mask) {
    uint32_t t      = timer_elapsed32(anim_started_at);
    uint16_t base_h = (uint16_t)((t / 6) % 256);

    for (uint8_t i = 0; i < FOCUS_LED_COUNT; i++) {
        uint8_t bit_index = FOCUS_LEDS_REVERSED ? i : (FOCUS_LED_COUNT - 1 - i);
        bool    on        = (mask >> bit_index) & 0x1;
        uint8_t led_index = FOCUS_LED_FIRST + i;

        if (on) {
            uint16_t h = (base_h + (i * 32)) % 256; /* per-LED offset */
            rgblight_sethsv_at(h, 255, rgblight_get_val(), led_index);
        } else {
            rgblight_sethsv_at(0, 0, 0, led_index);
        }
    }
}

/* -- Period control ------------------------------------------------------- */
static void start_period(focus_state_t next, uint8_t minutes) {
    /* Clamp minutes to 1–75 for safety */
    minutes = clamp_1_to_75(minutes);

    /* Set state */
    focus_state = next;

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
    focus_state          = FOCUS_STATE_OFF;
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
    if (!led_controller_visual_lock_active()) {
        uint8_t layer = get_highest_layer(layer_state);
        HSV16   hsv   = led_controller_get_layer_hsv(layer);
        uint8_t val   = rgblight_get_val();
        uint8_t mode  = led_controller_get_layer_anim(layer);

        rgblight_enable_noeeprom();
        rgblight_mode_noeeprom(mode);
        rgblight_sethsv_noeeprom(hsv.h, hsv.s, val);
    }
}

/* -- Timer pause management ------------------------------------------------ */
static inline bool timer_config_layer_held(void) {
    /* Holding Work layer pauses during WORK; holding Break layer pauses during BREAK */
    return ((focus_state == FOCUS_STATE_WORK)  && work_config_layer_on()) ||
           ((focus_state == FOCUS_STATE_BREAK) && break_config_layer_on());
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

/* -- Main tick ------------------------------------------------------------- */
void focus_timer_task(void) {

    if (led_controller_active() || led_controller_visual_lock_active()) {
        led_controller_task();
    }

    /* Immediate display when holding a timer-config layer (timer OFF) */
    if (focus_state == FOCUS_STATE_OFF) {
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
                run_blink(FOCUS_DOUBLE_BLINK_MS, 2, mask, hsv);
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
    if (focus_state == FOCUS_STATE_RAINBOW) {
        run_rainbow_wave();
        if (anim_gate) {
            /* Rainbow finished -> switch to BREAK (auto) */
            anim_gate = false;
            start_period(FOCUS_STATE_BREAK, break_setting_min);
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
        uint32_t step    = elapsed / FOCUS_SWEEP_STEP_MS;

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
        if (focus_state == FOCUS_STATE_WORK) {
            focus_state          = FOCUS_STATE_RAINBOW;
            anim_started_at = timer_read32();
            anim_gate       = false;
            return;
        } else if (focus_state == FOCUS_STATE_BREAK) {
            stop_period(); /* clears to standing layer state immediately */
            return;
        }
    }

    /* Render current remaining time as LED pattern */
    uint8_t m    = minutes_ceil_from_ms(period_ms_left);
    uint8_t mask = PATS[pattern_index_from_minutes(m)];
    HSV_t   hsv  = (focus_state == FOCUS_STATE_BREAK) ? break_color_white() : work_color_from_layer();

    /* While PAUSED: breathe (same affordance as edit-hold while OFF) */
    if (timer_paused) {
        run_subtle_tip_pulse(mask, hsv);
        return;
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

/* -- Key handling ---------------------------------------------------------- */
bool focus_timer_process_keycode(uint16_t keycode, keyrecord_t *record) {

    if (!led_controller_process_keycode(keycode, record)) {
        return false;
    }

    if (!record->event.pressed) return true;

    switch (keycode) {
        case FOCUS_TOGGLE: {
            if (focus_state == FOCUS_STATE_OFF) {
                /* Start WORK immediately from stored work_setting (allow 1 minute) */
                start_period(FOCUS_STATE_WORK, clamp_1_to_75(work_setting_min));
            } else {
                stop_period();
            }
            return false;
        }

        case FOCUS_WORK_DEC:
        case FOCUS_WORK_INC: {
            if (focus_state == FOCUS_STATE_WORK) {
                /* Live adjust (no blink). Jump to adjacent 5-min boundary. */
                period_ms_left = (keycode == FOCUS_WORK_DEC) ? step_down_ms(period_ms_left)
                                                            : step_up_ms(period_ms_left);
                /* Clamp within 5..75 and also update "setting" memory. */
                uint8_t new_min = (uint8_t)((period_ms_left + 59999) / 60000);
                work_setting_min = clamp_1_to_75(((new_min + 2) / 5) * 5);
                return false;
            }
            if (focus_state == FOCUS_STATE_BREAK) {
                /* Ignore work adjust during break; spec keeps adjustments scoped. */
                return false;
            }

            /* TIMER OFF */
            if (work_config_layer_on()) {
                /* IMMEDIATE commit while the Work config layer is held (no preview/double-blink) */
                if (keycode == FOCUS_WORK_DEC) {
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
                    preview_until    = now + FOCUS_PREVIEW_MS;
                    preview_blinked  = false;
                    return false;
                } else {
                    /* Second press inside the window: COMMIT and show updated selection */
                    if (keycode == FOCUS_WORK_DEC) {
                        work_setting_min = clamp_1_to_75(minus5_or_to1(work_setting_min));
                    } else {
                        work_setting_min = clamp_1_to_75(plus5_from1(work_setting_min));
                    }
                    preview_until   = now + FOCUS_PREVIEW_MS;
                    preview_blinked = true;  /* skip the double-blink; steady mask after commit */
                    return false;
                }
            }
        }

        case FOCUS_BREAK_DEC:
        case FOCUS_BREAK_INC: {
            if (focus_state == FOCUS_STATE_BREAK) {
                /* Live adjust during break */
                period_ms_left = (keycode == FOCUS_BREAK_DEC) ? step_down_ms(period_ms_left)
                                                             : step_up_ms(period_ms_left);
                uint8_t new_min = (uint8_t)((period_ms_left + 59999) / 60000);
                break_setting_min = clamp_1_to_75(((new_min + 2) / 5) * 5);
                return false;
            }
            if (focus_state == FOCUS_STATE_WORK) {
                /* Ignore break adjust during work; spec keeps adjustments scoped. */
                return false;
            }

            /* TIMER OFF */
            if (break_config_layer_on()) {
                /* IMMEDIATE commit while the Break config layer is held (no preview/double-blink) */
                if (keycode == FOCUS_BREAK_DEC) {
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
                    preview_until    = now + FOCUS_PREVIEW_MS;
                    preview_blinked  = false;
                    return false;
                } else {
                    /* Second press inside the window: COMMIT and show updated selection */
                    if (keycode == FOCUS_BREAK_DEC) {
                        break_setting_min = clamp_1_to_75(minus5_or_to1(break_setting_min));
                    } else {
                        break_setting_min = clamp_1_to_75(plus5_from1(break_setting_min));
                    }
                    preview_until   = now + FOCUS_PREVIEW_MS;
                    preview_blinked = true;  /* skip the double-blink; steady mask after commit */
                    return false;
                }
            }
        }
    }

    return true; /* fall-through for other keycodes */
}

#endif /* FOCUS_TIMER_ENABLE */

#if !defined(FOCUS_TIMER_ENABLE) || (FOCUS_TIMER_ENABLE != yes)

void focus_timer_task(void) {
    /* disabled */
}

bool focus_timer_process_keycode(uint16_t keycode, keyrecord_t *record) {
    return true; /* do nothing, allow other handlers */
}

#endif
