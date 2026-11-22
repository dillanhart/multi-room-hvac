/*
 * display.cpp - the thermostat's touchscreen: shows the room temp and forecast,
 * edits set points, mode, fan and night difference. A client of the control
 * program's API (pi/api.h) over localhost; owns the panel and touch, nothing else.
 *
 *   ui.cpp            the screens
 *   api_client.cpp    the API, on a worker thread
 *   touch.cpp         the touch controller (SPI1 + GPIO 16)
 *   display_logic.cpp set point steps, icons, parsing, touch filtering (unit tested)
 *
 * Build and run on the Pi: python3 compile.py (or ./deploy.sh display-run).
 * SIGTERM / SIGINT: stops cleanly and gives the console back.
 */

#include "lvgl.h"
#include "api_client.h"
#include "display_constants.h"
#include "touch.h"
#include "ui.h"

#include <climits>        // PATH_MAX
#include <csignal>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <linux/kd.h>     // KDSETMODE, KD_GRAPHICS, KD_TEXT
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int) {
    stop_requested = 1;
}

// LVGL needs a millisecond clock for animations, refresh timing and timers
static uint32_t now_ms() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// assets/ next to the executable, wherever it's started from (systemd, deploy.sh)
static std::string asset_dir() {
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return "assets";
    std::string exe(path, n);
    return exe.substr(0, exe.rfind('/')) + "/assets";
}

// stop the kernel console drawing on the panel (cursor, kernel messages) while we run.
// Returns the tty to restore, or -1.
static int console_to_graphics() {
    int tty = open(CONSOLE_TTY, O_RDWR);
    if (tty < 0 || ioctl(tty, KDSETMODE, KD_GRAPHICS) < 0) {
        perror("console graphics mode (needs root or CAP_SYS_TTY_CONFIG)");
        if (tty >= 0) close(tty);
        return -1;
    }
    return tty;
}

static void console_to_text(int tty) {
    if (tty < 0) return;
    ioctl(tty, KDSETMODE, KD_TEXT);
    close(tty);
}

static void read_touch(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* touch = static_cast<touch_panel*>(lv_indev_get_user_data(indev));
    screen_point p;
    if (touch->read(p)) {
        data->point.x = p.x;
        data->point.y = p.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;   // LVGL keeps the last point for the release
    }
}

int main() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    lv_init();
    lv_tick_set_cb(now_ms);

    lv_display_t* display = lv_linux_fbdev_create();
    if (lv_linux_fbdev_set_file(display, FRAMEBUFFER) != LV_RESULT_OK) {
        std::fprintf(stderr, "can't open %s\n", FRAMEBUFFER);
        return 1;
    }
    int tty = console_to_graphics();

    // no touch: still worth showing the temp and forecast
    touch_panel touch;
    if (touch.open()) {
        lv_indev_t* indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, read_touch);
        lv_indev_set_user_data(indev, &touch);
    } else {
        std::fprintf(stderr, "touch unavailable: display only\n");
    }

    api_client api;
    api.start();
    ui_start(api, asset_dir());

    // LVGL draws inside lv_timer_handler(), which says how long until it needs a call
    while (!stop_requested) {
        uint32_t wait = lv_timer_handler();
        usleep((wait > 33 ? 33 : wait) * 1000);
    }

    std::printf("display: stopping\n");
    api.stop();
    touch.close();
    console_to_text(tty);
    return 0;
}
