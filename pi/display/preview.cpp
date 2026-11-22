/*
 * preview.cpp - desktop check of the screens, no Pi needed: renders into memory with
 * a fake clock, fake API data and scripted taps, and writes each step to a .ppm.
 *
 *   python3 compile.py --preview      (builds ./display_preview and runs it)
 *   ./display_preview <out_dir>       default: preview_out/
 *
 * Exits non-zero if a scripted step doesn't land on the screen it should.
 */

#include "lvgl.h"
#include "api_client.h"
#include "display_constants.h"
#include "ui.h"

#include <cstdio>
#include <filesystem>
#include <string>

static uint32_t fake_ms = 0;
static uint32_t clock_ms() { return fake_ms; }

static uint16_t frame[SCREEN_WIDTH * SCREEN_HEIGHT];   // RGB565, as on the panel

static struct { int x = 0, y = 0; bool pressed = false; } finger;

static void read_finger(lv_indev_t*, lv_indev_data_t* data) {
    data->point.x = finger.x;
    data->point.y = finger.y;
    data->state = finger.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void flush(lv_display_t* disp, const lv_area_t*, uint8_t*) {
    lv_display_flush_ready(disp);   // direct mode: frame already holds the image
}

static void run_ms(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
        fake_ms += 10;
        lv_timer_handler();
    }
}

static void tap(int x, int y) {
    finger = {x, y, true};
    run_ms(100);
    finger.pressed = false;
    run_ms(100);
}

static std::string out_dir = "preview_out";
static int failures = 0;

static void save(const std::string& name) {
    lv_refr_now(nullptr);
    std::string path = out_dir + "/" + name + ".ppm";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    for (uint16_t px : frame) {
        unsigned char rgb[3] = {(unsigned char)((px >> 11) << 3), (unsigned char)(((px >> 5) & 0x3F) << 2),
                                (unsigned char)((px & 0x1F) << 3)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    std::printf("wrote %s\n", path.c_str());
}

static void expect(screen_enums want, const char* step) {
    if (ui_current() != want) {
        std::printf("FAIL: %s: on screen %d, expected %d\n", step, (int)ui_current(), (int)want);
        failures++;
    }
}

int main(int argc, char** argv) {
    if (argc > 1) out_dir = argv[1];
    std::filesystem::create_directories(out_dir);
    setenv("TZ", "America/Chicago", 1);

    lv_init();
    lv_tick_set_cb(clock_ms);
    lv_display_t* disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, frame, nullptr, sizeof frame, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush);
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_finger);

    long long hour = 1'795'000'000 / 3600 * 3600;
    api_snapshot data;
    data.status = display_status::heating;
    data.main_temp = 68.6f;
    data.settings = settings_edit{68, 72, 2, "auto", false};
    data.forecast = {{hour, 41.4f, "sct", true}, {hour + 3600, 43, "rain_showers", true},
                     {hour + 7200, 44, "tsra", true}, {hour + 10800, 42, "bkn", false},
                     {hour + 14400, 39, "snow", false}};

    api_client api;   // never started: no thread, no network
    api.set_snapshot(data);
    ui_start(api, "assets");
    run_ms(1500);     // into the icon animation
    save("01_main_heating");

    // status colours and N/A
    for (auto [status, name] : {std::pair{display_status::cooling, "02_main_cooling"},
                                std::pair{display_status::circulating, "03_main_circulating"}}) {
        data.status = status;
        api.set_snapshot(data);
        run_ms(500);
        save(name);
    }
    api_snapshot empty;
    api.set_snapshot(empty);
    run_ms(500);
    save("04_main_no_data");
    data.status = display_status::idle;
    api.set_snapshot(data);
    run_ms(500);

    // forecast pages
    tap(240, 120);
    expect(screen_enums::weather_1, "tap forecast half");
    run_ms(1500);
    save("05_forecast_page_1");
    tap(48, 210);
    expect(screen_enums::weather_2, "forecast arrow");
    run_ms(1500);
    save("06_forecast_page_2");
    tap(254, 210);
    expect(screen_enums::main, "forecast Done");

    // set points: heat up 5 pushes AC, fan on, mode both, night +1, Done sends
    tap(80, 120);
    expect(screen_enums::main_temperature, "tap temp half");
    save("07_setpoints");
    for (int i = 0; i < 5; i++) tap(56, 34);
    tap(269, 90);     // fan
    save("08_setpoints_heat_up_fan_on");
    tap(269, 30);     // mode
    expect(screen_enums::season_mode, "Mode button");
    tap(8 + 2 * 78 + 35, 120);   // Both
    save("09_mode_both");
    tap(254, 210);
    expect(screen_enums::main_temperature, "mode Done");
    tap(269, 150);    // night
    expect(screen_enums::night_temperature, "Night button");
    tap(110, 58);
    save("10_night");
    tap(254, 210);
    expect(screen_enums::main_temperature, "night Done");
    save("11_setpoints_back");
    tap(269, 210);    // Done
    expect(screen_enums::main, "set point Done");

    std::vector<api_change> sent = api.take_submitted();
    std::printf("sent %zu change(s)\n", sent.size());
    for (const api_change& c : sent)
        std::printf("  setpoints: %s\n  fan: %s\n", c.setpoints ? c.setpoints->dump().c_str() : "-",
                    c.fan ? c.fan->dump().c_str() : "-");
    if (sent.size() != 1 || !sent[0].setpoints || !sent[0].fan ||
        *sent[0].setpoints != json{{"heat_point", 73.0}, {"ac_point", 75.0}, {"night_difference", 3.0}, {"mode", "both"}} ||
        *sent[0].fan != json{{"fan", "on"}}) {
        std::printf("FAIL: Done sent the wrong change\n");
        failures++;
    }

    // timeout: edits dropped, nothing sent
    tap(80, 120);
    tap(56, 34);
    run_ms(SCREEN_TIMEOUT_MS + 1000);
    expect(screen_enums::main, "30 s timeout");
    if (!api.take_submitted().empty()) {
        std::printf("FAIL: timeout sent a change\n");
        failures++;
    }

    // no set points yet: the temp half does nothing
    api.set_snapshot(empty);
    run_ms(500);
    tap(80, 120);
    expect(screen_enums::main, "tap with no set points");

    std::printf(failures ? "%d FAILED\n" : "all steps OK\n", failures);
    return failures ? 1 : 0;
}
