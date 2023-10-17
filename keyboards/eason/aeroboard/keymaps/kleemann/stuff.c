#include QMK_KEYBOARD_H

void keyboard_post_init_user(void) {
    rgblight_enable_noeeprom();
    rgblight_sethsv_noeeprom(85, 0xff, 0x40); // h=85=green, s=0xff=sharp color, v=0x40 dim
}

layer_state_t layer_state_set_user(layer_state_t state) {
    switch (get_highest_layer(state)) {
    case 0: // gaming/normal main layer
    case 1: // gaming/normal FN layer
    case 6: // gaming/normal toggle layeer
        rgblight_sethsv_noeeprom(85, 0xff, 0x40); // h=85=green, s=0xff=sharp color, v=0x40 dim
        break;
    default: //  for any other layers, or the default layer
        rgblight_sethsv_noeeprom(0, 0xff, 0x40); // h=0=red, s=0xff=sharp color, v=0x40 dim
        break;
    }
    return state;
}
