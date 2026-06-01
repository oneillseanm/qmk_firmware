
# QMK

## Introduction

Ever since I got a dz60 with a keymap and I'm using this branch of the QMK
project to build and flash it onto my keyboard. It seems Windows and
Mac users have a slick UI but linux is stuck on the command line.

There are three components of QMK: the qmk source tree, the qmk
configurator, the qmk command line application.

## The QMK Source Tree

This is just a clone of the official source tree. My clone has a
branch named kleemann that has all of my customizations.

When you get a new keyboard you generally find in within the
`keyboards/` directory and then make a new directory within keymaps
that has all of your customizations.

E.g.: `~/qmk_configurator/keyboards/dz60/keymaps/kleemann/`

### Building the Original Project

Here are the orignal steps for cloning the project:

    <clone qmk project in github>
    git clone https://github.com/sizezero/qmk_firmware.git
    
    I don't think the next command is necessary. Everything is done from the sizezero repo.
    git remote add upstream https://github.com/qmk/qmk_firmware.git
    
    git remote -v
    git branch kleemann
    git checkout kleemann
    git push -u origin kleemann

### Updating Branch from Upstream

From the github web page first go to the master and "sync fork" then
go to the kleemann branch and "sync fork". Local copies then need a
"git pull"

June 2024: I tried this but when I tried to "sync fork" kleemann from
the web site it said I either had to lose over 10 commits from
kleemann, open a pull request, or resolve merge problems from the
command line. I'm currently working on flashing Sean a new version of
JRIS so I'm going to wait before investigating this.

I ended up doing a git merge from the command line. The new strategy is:

    <from github "sync fork" on master>
    <from local, checked out branch: kleemann, jris, other>
    git fetch
    git merge master

The above didn't seem to work. I had to do a "sync fork" of jris65
from the web site.

## QMK Configurator

This is an externally hosted web application that allows you to make
keyboard layouts. I can't see a reason _not_ to use this. It produeces
`.json` files but can also compile `.hex` files that are eventually
flashed. It's good enough for me to create a `.json` file and then
compile to `.hex` myself. Place the `.json` file here:

    keyboards/dz60/keymaps/kleemann/keymap.json

## QMK Command Line Application

Getting this working requires both a checked out copy of the qmk
source tree and a separately compiled python application called
qmk. The python program reads from the config file
`~/.config/qmk/qmk.ini` in order to be associated with the source
tree. Because of this, the source tree should not be too out of
date. My source tree is `~/qmk_firmware_kleemann/`

    sudo apt install libhidapi-dev libhidapi-hidraw0 libhidapi-libusb0
    sudo apt install python3-hidapi

    mkdir python-qmk
    python3 -m venv python-qmk
    python-qmk/bin/python3 -m pip install qmk
    ( cd bin ; ln -s ../keyboard/python-qmk/bin/qmk qmk )

In order to flash with qmk the normal unix user needs permissions to
access the USB device. See [these
docs](https://docs.qmk.fm/#/faq_build?id=linux-udev-rules) which add a
new set of rules to `/etc/udev/rules.d/` The most reliable way to put
the new rules into effect is to just reboot the system.

# Keyboards

## dz60

Once:

    make git-submodules

Whenever:

    make clean
    make dz60:default
    make dz60:kleemann

Plug in the keyboard and press the button in the bottom with a paper
clip for a few seconds.

    make dz60:kleemann:flash

This should immediately update the keyboard at which point you can use
the keyboard to type into the computer.

Instead of the paper clip you could also use the reset button
(middlespace + backspace + ESC) but I haven't tried that yet. I think
you have to do the sequence before plugging the keyboard in.

## KBD75

The build for the KBD75 is similar.

    make clean
    make kbdfans/kbd75/rev2:kleemann

The KBD75 needs the case removed to access the reset button so it's easier to use a keystroke to reset. First:

    sudo dmesg -w

Then middle space + ESC. You should see `ATm32u4DFU` up in the dmesg output.

    make kbdfans/kbd75/rev2:kleemann:flash

## Contra 40%

The Contra uses a Pro Micro or Sea Micro as the controller. This
requires the caterina bootloader but this appears to be all setup in
the contra/ keyboard project. There are multiple ways to reset it:

- Short the ground and reset. Reset appears to be the third pin the the left of the usb port. I'm not sure about ground.
- Press the reset button on the sea micro. Some people say you need to press it twice.
- Press the reset button the PCB after it is socketed.
- Configure the layout with a reset code.

Here's what the Sea Micro looks like. Reset button doesn't seem to change what it displays.

    [33157.731843] usb 3-4: new full-speed USB device number 7 using xhci_hcd
    [33157.882061] usb 3-4: New USB device found, idVendor=03eb, idProduct=2ff4, bcdDevice= 0.00
    [33157.882070] usb 3-4: New USB device strings: Mfr=1, Product=2, SerialNumber=3
    [33157.882073] usb 3-4: Product: ATm32U4DFU
    [33157.882075] usb 3-4: Manufacturer: ATMEL
    [33157.882077] usb 3-4: SerialNumber: 1.0.0

I made a very simple layout with two layers. The reset is the lower
right key plus the upper left key. Lower right key can be tested by
pressing various top row keys to produce fn keys.

    make clean
    make contra:kleemann
    make contra:kleemann:flash

The flash did not recognize the controller. It seems that the Contra
is configured to look for the Pro Micro which is very different than
the Sea Micro. Changing the bootloader seemed to flash it and get it
to be recognized

There is both a kleemann layout and a test layout. The test layout is
simpler and good at verifying PCB connection and switches.

## Corne Classic

    make clean
    make crkbd/rev1:kleemann
    make crkbd/rev1:kleemann:flash

All builds say 97% full which means I only have 500 bytes left. Huh, I
swaped keyboard.c with keyboard.json and it dropped to 83%. Ok,
included old default/keymaps.c minus the keymap and we're back to 96%
Maybe this is due to the crazy oled and lighting stuff. First get it
working, then play with all this crazy stuff.

## Aeroboard 70

### One Time

Double press the PCB button while the keyboard is plugged in. This is
difficult so you may have to rest the PCB upside down with the edges
on some books while the cable is close enough to the daughter
board. The button is rediculously tiny so use some small, non-metal
tool to actually press the button. If you do this correctly, a USB
drive will conntect to your computer.

To flash the drive, simply copy a `.uf2` file to the drive. It will
flash and restart.

The files you copy onto the drive do not appear to actually show up in
future reads of the drive's contents. If you read from the USB drive,
you will always see the original flashed contents.

If you add

    BOOTMAGIC_ENABLE = yes

to `rules.mk`, you will be able to bring up the USB drive for flashing
on a normal `QK_BOOT` (Reset) keycode without having to open up the
keyboard and double press the PCB.

### Regular Build

    make clean
    make eason/aeroboard:default
    make eason/aeroboard:kleemann

Press reset to bring up the USB drive.

Copy the compiled `.uf2` file in the QMK root directory to the flash
drive.

# Reviung 41

    make clean
    make reviung/reviung41:default
    make reviung/reviung41:kleemann

Long press the reset button to bring up the USB drive.

Copy the compiled `.uf2` file in the QMK root directory to the flash
drive.

# Corchim

This is a corne in a nice case.

    make clean
    make crkbd/rev1:kleemann_corchim
    make crkbd/rev1:kleemann_corchim:flash

It seems that in order to bring up a USB drive to copy to, the reset
button has to be double tapped.

# Misc Notes

## Home Row Mods

There is a whole thing about doing [home row mods.](https://precondition.github.io/home-row-mods) The gist is that you set your home keys to mod tap keys so that long press acts like the mod and tap acts like the home row letter.

It turns out you can't just do this and have your keyboard work. There will be lots of mistyped keys as single presses are sometimes interpreted as mods and long presses are sometimes interpreted as characters. The solution is to set a bunch QMK compile flags that change how keypresses are interpreted.

There are lots of options but right now I'm starting with just a few:

    #define IGNORE_MOD_TAP_INTERRUPT
    #define PERMISSIVE_HOLD
    #define RETRO_TAPPING

These settings can be in many different places but I've put them in `keymaps/<keymapname>/config.h`

Right now I'm only testing this on my writing keyboard. If it works well I'll move them to my work keyboard as well.

## March 2022

I've been having a bunch of mis-types. I'm going to try a couple tweaks.

### 1

The tapping term is the maxximum time that a key should be held to register. This is used a lot for Tap and Hold settings and it's best visualized in the following diagram:

https://cdn.discordapp.com/attachments/663573863480950808/757162393209012304/modtap.pdf

TAPPING_TERM is used to define the differences between presses, holds, and other actions. It basically represents the longest time period that a key would be normally pressed and considerred a normal, pressed key. Longer than this and the press is considerred a hold. PERMISSIVE_HOLD sets an infinite tapping term so it really shouldn't be used. It is much better to customize the tapping term based on your own personal typing style.

So what is the correct value for typing term? The default is 200MS. Most people choose a value between 150MS and 220MS. Higher values of tapping term will result in accidental alphas and lower values will result in accidental mods. It is easiest to troubeshoot by setting the default to the high side, and then lowering it until there are no problems. The default tapping term needs to be built into the config.h but the modifications occur by assigning DT_PRINT, DT_UP, AND DT_DOWN to extra keys. I'll be assigning them as the bottom leftmost keys on the space mod layer: DOWN, UP, PRINT. rules.mk also needs a DYNAMIC_TYPING_TERM_ENABLE = yes.

To test this, first try long strings of lower case letters then practice on text containing capital letters. If you get accidental characters, lower the typing term and not the new value. After doing this, I started to run into problems at 160ms and 150ms. To give me some buffer, I'm setting my default to 175ms.

### 2

I was bugged that holding down a LT for a while and then releasing it acted as a keypress. It turns out that I had enabled this annoying behavior with RETRO_TAPPING. I got rid of it.

### 3

IGNORE_MOD_TAP_INTERRUPT is now enabled by default in QMK so no
defines are required in order to use tap hold correctly.
