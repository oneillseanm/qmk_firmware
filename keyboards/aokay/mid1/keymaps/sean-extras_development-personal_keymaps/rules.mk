MCU = atmega32u4

FOCUS_TIMER_ENABLE = no
RGBLIGHT_ENABLE = yes

ifeq ($(FOCUS_TIMER_ENABLE),yes)
    SRC += focus_timer.c
endif

OPT_DEFS += -DFOCUS_TIMER_ENABLE=$(FOCUS_TIMER_ENABLE)

SRC += led_controller.c

OPT_DEFS += -include mid1_custom_keycodes.h
OPT_DEFS += -include keymap_defines.h

MOUSEKEY_ENABLE = no