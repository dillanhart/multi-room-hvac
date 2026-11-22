// Toolchain check only: proves LVGL builds and can draw on the panel.
// Shows "71" in white on black for 10 s, then exits. Not the display program;
// delete it (and its line in compile.py) once the real program exists.

#include "lvgl.h"

#include <ctime>
#include <unistd.h>       // usleep
#include <iostream>

// LVGL needs a millisecond clock for animations, refresh timing and timers
static uint32_t now_ms() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main() {
    lv_init();
    lv_tick_set_cb(now_ms);

    lv_display_t* display = lv_linux_fbdev_create();
    if (lv_linux_fbdev_set_file(display, "/dev/fb1") != LV_RESULT_OK) return 1;

    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    lv_obj_t* label = lv_label_create(screen);
    lv_label_set_text(label, "71");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
    lv_obj_center(label);

    // LVGL does its drawing inside lv_timer_handler(), which says how long until it next needs a call
    uint32_t start = now_ms();
    while (now_ms() - start < 10000) {
        uint32_t wait = lv_timer_handler();
        usleep(wait * 1000);
    }
    return 0;
}
