/*
 * display_constants.h - colours, screens, timings and hardware settings for the
 * display program. Screen layout lives in ui.cpp, next to the code that uses it.
 */

#ifndef DISPLAY_CONSTANTS_H
#define DISPLAY_CONSTANTS_H

#include "lvgl.h"
#include "display_logic.h"   // touch_calibration

// colours used in the UI
namespace colors {
    const lv_color_t background = lv_color_make(0, 0, 0);       // background color
    const lv_color_t text       = lv_color_make(255, 255, 255);    // text / icon colors
    const lv_color_t heat_red   = lv_color_make(244, 99, 108);     // heat color
    const lv_color_t cool_blue  = lv_color_make(3, 155, 229);      // cooling color
    const lv_color_t fan_green  = lv_color_make(144, 224, 149);    // fan color
    const lv_color_t ui_grey    = lv_color_make(102, 98, 98);      // grey: auto mode
    const lv_color_t dark_blue  = lv_color_make(9, 85, 122);       // dark blue
    const lv_color_t orange     = lv_color_make(250, 129, 74);     // orange: both mode
    const lv_color_t done       = lv_color_make(144, 164, 174);    // light blue-grey: Done buttons
}

// the screens. weather_1 / weather_2 are the forecast's two pages (hours 0-2, 2-4).
enum class screen_enums {
    main,
    main_temperature,    // set points
    night_temperature,
    weather_1,
    weather_2,
    season_mode
};

constexpr int SCREEN_WIDTH = 320;
constexpr int SCREEN_HEIGHT = 240;

// ---- behaviour
constexpr uint32_t SCREEN_TIMEOUT_MS = 30 * 1000;     // no touch this long on any other screen -> main, edits dropped
constexpr int FORECAST_HOURS = 5;                     // hours on the forecast pages
constexpr int MAIN_FORECAST_INDEX = 0;                // main screen's forecast: 0 = the hour containing now
constexpr bool ANIMATE_ICONS = true;                  // false: icons hold their first frame (less CPU and SPI)

// ---- control program API (pi/api.h); API_PORT comes from ../constants.h
constexpr const char* API_HOST = "127.0.0.1";
constexpr int API_TIMEOUT_MS = 2000;                  // per connect / read: a stopped control program fails fast
constexpr int POLL_INTERVAL_MS = 5000;                // status, readings, set points
constexpr int WEATHER_POLL_INTERVAL_MS = 5 * 60 * 1000;   // while there's a forecast
constexpr int WEATHER_EMPTY_POLL_MS = 30 * 1000;          // while there isn't (fresh start, fetch failing)
constexpr int READING_MAX_AGE_MS = 3 * 60 * 1000;     // older than this (API unreachable) -> N/A

// ---- hardware
constexpr const char* FRAMEBUFFER = "/dev/fb1";
constexpr const char* CONSOLE_TTY = "/dev/tty1";      // switched to graphics mode while running
constexpr const char* TOUCH_SPI = "/dev/spidev1.0";
constexpr uint32_t TOUCH_SPI_HZ = 1000000;            // XPT2046 max is ~2 MHz
constexpr const char* GPIO_CHIP = "/dev/gpiochip0";
constexpr unsigned TOUCH_IRQ_GPIO = 16;               // T_IRQ: low while pressed

// raw readings at the edges, from the corner table in log.md (2026-11-18), averaged per edge
constexpr touch_calibration TOUCH_CAL = {
    .raw_y_left = 475, .raw_y_right = 3575,
    .raw_x_top = 435, .raw_x_bottom = 3600,
    .width = SCREEN_WIDTH, .height = SCREEN_HEIGHT,
};
constexpr int TOUCH_SAMPLES = 7;                      // per axis per read
constexpr int TOUCH_RAW_MIN = 150;                    // outside this: a stray read (4095, near 0)
constexpr int TOUCH_RAW_MAX = 3950;
constexpr int TOUCH_MAX_SPREAD = 80;                  // samples in a burst must agree this closely
constexpr int TOUCH_SETTLE_MOVE = 60;                 // first two reads after touch-down must agree (raw units)

#endif
