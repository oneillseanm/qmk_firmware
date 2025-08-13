/*
Copyright 2024 @sizezero

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "quantum.h"
#include "deferred_exec.h"

#include "kb40a.h"

uint32_t my_callback(uint32_t trigger_time, void *cb_arg) {
    // if the leftmost toggle key is set to caps lock, then make sure it is in sync
    // with our caps lock state
    sync_caps_lock(0, 4, 10);
    // if the rightmost toggle key depressed and is set to TG(), then toggle to
    // layer 2
    sync_toggle_layer(0, 4, 11, 2);

    // If the toggle keys are mapped to different values the above code will not
    // cause any problems (the calls will do nothing) however you may want to write
    // custom code that performs the correct behavior on starting depending on the
    // new functionality of the toggle keys.
    
    return 0; // don't repeat this callback
}

void keyboard_post_init_user(void) {
    // debug_enable = true;
    // debug_matrix = true;
    // debug_keyboard = true;
    defer_exec(2000, my_callback, NULL);
}
