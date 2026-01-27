MCU = atmega32u4

FOCUS_TIMER_ENABLE = 0
RGBLIGHT_ENABLE = yes
MOUSEKEY_ENABLE = no

ifeq ($(FOCUS_TIMER_ENABLE),yes)
    SRC += focus_timer.c
endif

SRC += led_controller.c

OPT_DEFS += -DFOCUS_TIMER_ENABLE=$(FOCUS_TIMER_ENABLE)