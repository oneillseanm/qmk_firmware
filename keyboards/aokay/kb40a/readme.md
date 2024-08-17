# KB-40-A

![KB-40-A illustration](https://i.imgur.com/a/vZifIro.png)

## Description

A 40% keyboard kit with a staggered, 12-U layout.

* Keyboard Maintainer: [Sean O'Neill](https://oneillseanm.github.io)
* Hardware Supported: Proprietary PCB using ATmega32U4 controller
* Hardware Availability: [KB-40-A info page](https://oneillseanm.github.io/kb40a.html)

## Setup

Make example for this keyboard (after setting up your build environment):

    make aokay/kb40a:default

Flashing example for this keyboard:

    make aokay/kb40a:default:flash

See the [build environment setup](https://docs.qmk.fm/#/getting_started_build_tools) and the [make instructions](https://docs.qmk.fm/#/getting_started_make_guide) for more information. Brand new to QMK? Start with our [Complete Newbs Guide](https://docs.qmk.fm/#/newbs).

## Bootloader

Enter the bootloader in two ways:

* **Keycode in layout**: Using the default firmware, press `FN+Backspace` which is assigned to `QK_BOOT`.
* **Physical reset button**: Briefly press the button on the front of the PCB
