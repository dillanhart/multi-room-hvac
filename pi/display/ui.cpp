#include "ui.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

// ---------------------------------------------------------------- state

namespace {

struct mode_choice {
    const char* mode;    // API name
    const char* label;
    lv_color_t color;
};

// left to right on the mode screen
const mode_choice MODES[] = {
    {"heat", "Heat", colors::heat_red},
    {"cool", "Cool", colors::cool_blue},
    {"both", "Both", colors::orange},
    {"auto", "Auto", colors::ui_grey},
};

struct ui_state {
    api_client* api = nullptr;
    std::string icon_dir;

    // TrueType Montserrat at any size (the built-in fonts stop at 48)
    std::vector<char> ttf;          // must outlive the fonts made from it
    const lv_font_t* font_huge;     // main screen temp
    const lv_font_t* font_big;      // set points, the temp's N/A
    const lv_font_t* font_medium;   // forecast temps

    screen_enums current = screen_enums::main;
    lv_obj_t* screen = nullptr;
    uint64_t shown_version = 0;

    // what the edit screens change; sent by the set point screen's Done
    settings_edit original, edit;

    // widgets that change without rebuilding their screen (null when not on it)
    lv_obj_t* main_temp = nullptr;
    lv_obj_t* main_weather = nullptr;   // right half
    std::string main_weather_key;       // what main_weather shows now: rebuilt only if this changes
    std::vector<forecast_entry> shown_forecast;
    lv_obj_t* heat_label = nullptr;
    lv_obj_t* ac_label = nullptr;
    lv_obj_t* night_label = nullptr;
    lv_obj_t* fan_button = nullptr;
    lv_obj_t* mode_buttons[std::size(MODES)] = {};
};

ui_state ui;

}   // namespace

// ---------------------------------------------------------------- building blocks

static void plain(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);    // no theme: every colour on screen is set here
    lv_obj_set_scrollable(obj, false);
}

static lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const std::string& text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text.c_str());
    return l;
}

// an oval with a coloured outline; filled = solid background with dark text
static void style_oval(lv_obj_t* button, lv_color_t color, bool filled) {
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_bg_opa(button, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(button, filled ? LV_OPA_80 : LV_OPA_30, LV_STATE_PRESSED);   // touch feedback
    lv_obj_t* text = lv_obj_get_child(button, 0);
    if (text) lv_obj_set_style_text_color(text, filled ? colors::background : color, 0);
}

static lv_obj_t* oval_button(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t color,
                             const char* text, lv_event_cb_t on_click, void* user_data = nullptr) {
    lv_obj_t* b = lv_button_create(parent);
    plain(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, color, 0);
    lv_obj_set_ext_click_area(b, 4);   // a little forgiveness for a resistive panel
    lv_obj_t* l = label(b, &lv_font_montserrat_20, color, text);
    lv_obj_center(l);
    style_oval(b, color, false);
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, user_data);
    return b;
}

static lv_obj_t* done_button(lv_obj_t* parent, lv_event_cb_t on_click) {
    return oval_button(parent, 198, 188, 112, 44, colors::done, "Done", on_click);
}

// a big arrow: red up, blue down. Tap area larger than the glyph.
static lv_obj_t* arrow_button(lv_obj_t* parent, int center_x, int y, bool up, lv_event_cb_t on_click) {
    lv_color_t color = up ? colors::heat_red : colors::cool_blue;
    lv_obj_t* b = lv_button_create(parent);
    plain(b);
    lv_obj_set_size(b, 88, 56);
    lv_obj_set_pos(b, center_x - 44, y);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_t* l = label(b, &lv_font_montserrat_48, color, up ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, nullptr);
    return b;
}

// a set point number, centred on (center_x, center_y). Long values ("70.5") get a smaller font to fit.
static void set_number(lv_obj_t* l, float value, int center_x, int center_y) {
    std::string text = format_setpoint(value);
    lv_obj_set_style_text_font(l, text.size() <= 2 ? ui.font_big : ui.font_medium, 0);
    lv_label_set_text(l, text.c_str());
    lv_obj_update_layout(l);
    lv_obj_set_pos(l, center_x - lv_obj_get_width(l) / 2, center_y - lv_obj_get_height(l) / 2);
}

// The Lottie widget still uses its buffer in its destructor, which runs after
// LV_EVENT_DELETE: free it on the next timer cycle instead (found with ASan)
static void free_icon_buffer(lv_event_t* e) {
    lv_async_call([](void* buffer) { lv_draw_buf_destroy(static_cast<lv_draw_buf_t*>(buffer)); },
                  lv_event_get_user_data(e));
}

// an animated Meteocons icon, size x size, top-centred at (center_x, top)
static lv_obj_t* weather_icon(lv_obj_t* parent, const std::string& name, int size, int center_x, int top) {
    std::string path = ui.icon_dir + "/" + name + ".json";
    if (!std::ifstream(path)) {
        std::fprintf(stderr, "icon missing: %s\n", path.c_str());
        path = ui.icon_dir + "/not-available.json";
    }
    lv_obj_t* icon = lv_lottie_create(parent);
    lv_draw_buf_t* buffer = lv_draw_buf_create(size, size, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    lv_lottie_set_draw_buf(icon, buffer);
    lv_obj_add_event_cb(icon, free_icon_buffer, LV_EVENT_DELETE, buffer);
    lv_lottie_set_src_file(icon, path.c_str());
    if (!ANIMATE_ICONS) lv_anim_delete(icon, nullptr);   // hold the first frame
    lv_obj_set_pos(icon, center_x - size / 2, top);
    return icon;
}

// a clickable, invisible region
static lv_obj_t* tap_area(lv_obj_t* parent, int x, int y, int w, int h, lv_event_cb_t on_click) {
    lv_obj_t* a = lv_obj_create(parent);
    plain(a);
    lv_obj_set_pos(a, x, y);
    lv_obj_set_size(a, w, h);
    lv_obj_set_clickable(a, true);
    lv_obj_add_event_cb(a, on_click, LV_EVENT_CLICKED, nullptr);
    return a;
}

static lv_color_t status_color(display_status s) {
    switch (s) {
        case display_status::heating:
        case display_status::em_heating:  return colors::heat_red;
        case display_status::cooling:     return colors::cool_blue;
        case display_status::circulating: return colors::fan_green;
        case display_status::idle:
        case display_status::unknown:     return colors::text;
    }
    return colors::text;
}

// ---------------------------------------------------------------- main

static void open_setpoints(lv_event_t*) {
    api_snapshot snap = ui.api->snapshot();
    if (!snap.settings) return;   // control program not reached yet: nothing to edit
    ui.original = ui.edit = *snap.settings;
    ui_show(screen_enums::main_temperature);
}

static void update_main(const api_snapshot& snap) {
    // left: room temp in the status colour, or N/A
    if (snap.main_temp) {
        lv_obj_set_style_text_font(ui.main_temp, ui.font_huge, 0);
        lv_label_set_text(ui.main_temp, format_temp(*snap.main_temp).c_str());
        lv_obj_set_style_text_color(ui.main_temp, status_color(snap.status), 0);
    } else {
        lv_obj_set_style_text_font(ui.main_temp, ui.font_big, 0);
        lv_label_set_text(ui.main_temp, "N/A");
        lv_obj_set_style_text_color(ui.main_temp, colors::text, 0);
    }
    lv_obj_center(ui.main_temp);

    // right: one hour's icon and temp, or nothing. Rebuilt only when it changes, so the
    // animation isn't restarted on every poll.
    bool have = (int)snap.forecast.size() > MAIN_FORECAST_INDEX;
    const forecast_entry* hour = have ? &snap.forecast[MAIN_FORECAST_INDEX] : nullptr;
    std::string key = hour ? icon_name(hour->condition, hour->is_day) + "|" + format_temp(hour->temp) : "none";
    if (key == ui.main_weather_key) return;
    ui.main_weather_key = key;
    lv_obj_clean(ui.main_weather);
    if (!hour) return;   // no forecast yet: left blank
    // Meteocons art has wide margins: a big box for a mid-size picture
    weather_icon(ui.main_weather, icon_name(hour->condition, hour->is_day), 150, 80, 6);
    lv_obj_t* t = label(ui.main_weather, ui.font_medium, colors::text, format_temp(hour->temp) + "\xC2\xB0");   // °
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 156);
}

static void build_main(lv_obj_t* s) {
    lv_obj_t* left = tap_area(s, 0, 0, 160, 240, open_setpoints);
    ui.main_temp = label(left, ui.font_huge, colors::text, "");
    ui.main_weather = tap_area(s, 160, 0, 160, 240, [](lv_event_t*) { ui_show(screen_enums::weather_1); });
    ui.main_weather_key.clear();
    update_main(ui.api->snapshot());
}

// ---------------------------------------------------------------- forecast pages

static void build_forecast(lv_obj_t* s, int first) {
    ui.shown_forecast = ui.api->snapshot().forecast;
    const std::vector<forecast_entry>& hours = ui.shown_forecast;
    // three columns: hour, icon, temp
    for (int i = 0; i < 3 && first + i < (int)hours.size() && first + i < FORECAST_HOURS; i++) {
        const forecast_entry& h = hours[first + i];
        int center_x = 53 + i * 107;
        std::string when = first + i == 0 ? "Now" : format_hour(h.time);
        lv_obj_t* t = label(s, &lv_font_montserrat_20, colors::text, when);
        lv_obj_set_pos(t, center_x - 50, 10);
        lv_obj_set_width(t, 100);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        weather_icon(s, icon_name(h.condition, h.is_day), 110, center_x, 26);
        lv_obj_t* temp = label(s, ui.font_medium, colors::text, format_temp(h.temp) + "\xC2\xB0");
        lv_obj_update_layout(temp);
        lv_obj_set_pos(temp, center_x - lv_obj_get_width(temp) / 2, 132);
    }

    // one arrow: forward on the first page, back on the second
    bool page_one = first == 0;
    lv_event_cb_t next = [](lv_event_t*) { ui_show(screen_enums::weather_2); };
    lv_event_cb_t back = [](lv_event_t*) { ui_show(screen_enums::weather_1); };
    oval_button(s, 10, 188, 76, 44, colors::done, page_one ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT,
                page_one ? next : back);
    done_button(s, [](lv_event_t*) { ui_show(screen_enums::main); });
}

// ---------------------------------------------------------------- set points

constexpr int HEAT_X = 56, AC_X = 164, NUMBER_Y = 120;

static void refresh_setpoints() {
    set_number(ui.heat_label, ui.edit.heat_point, HEAT_X, NUMBER_Y);
    set_number(ui.ac_label, ui.edit.ac_point, AC_X, NUMBER_Y);
}

static void style_fan() {
    style_oval(ui.fan_button, colors::fan_green, ui.edit.fan_on);
}

static void send_and_close(lv_event_t*) {
    api_change change{setpoints_request(ui.original, ui.edit), fan_request(ui.original, ui.edit)};
    if (change.setpoints || change.fan) ui.api->submit(change);
    ui_show(screen_enums::main);
}

static void build_setpoints(lv_obj_t* s) {
    ui.heat_label = label(s, ui.font_big, colors::text, "");
    ui.ac_label = label(s, ui.font_big, colors::text, "");
    arrow_button(s, HEAT_X, 6, true, [](lv_event_t*) { if (step_heat(ui.edit, +1)) refresh_setpoints(); });
    arrow_button(s, HEAT_X, 178, false, [](lv_event_t*) { if (step_heat(ui.edit, -1)) refresh_setpoints(); });
    arrow_button(s, AC_X, 6, true, [](lv_event_t*) { if (step_ac(ui.edit, +1)) refresh_setpoints(); });
    arrow_button(s, AC_X, 178, false, [](lv_event_t*) { if (step_ac(ui.edit, -1)) refresh_setpoints(); });
    refresh_setpoints();

    // right side, top to bottom
    oval_button(s, 224, 6, 90, 48, colors::text, "Mode", [](lv_event_t*) { ui_show(screen_enums::season_mode); });
    ui.fan_button = oval_button(s, 224, 66, 90, 48, colors::fan_green, "Fan", [](lv_event_t*) {
        ui.edit.fan_on = !ui.edit.fan_on;
        style_fan();
    });
    style_fan();
    oval_button(s, 224, 126, 90, 48, colors::text, "Night",
                [](lv_event_t*) { ui_show(screen_enums::night_temperature); });
    oval_button(s, 224, 186, 90, 48, colors::done, "Done", send_and_close);
}

// ---------------------------------------------------------------- night difference

constexpr int NIGHT_X = 110;

static void refresh_night() {
    set_number(ui.night_label, ui.edit.night_difference, NIGHT_X, NUMBER_Y);
}

static void build_night(lv_obj_t* s) {
    lv_obj_set_pos(label(s, &lv_font_montserrat_20, colors::ui_grey, "Night difference"), 12, 8);
    ui.night_label = label(s, ui.font_big, colors::text, "");
    arrow_button(s, NIGHT_X, 30, true, [](lv_event_t*) { if (step_night(ui.edit, +1)) refresh_night(); });
    arrow_button(s, NIGHT_X, 154, false, [](lv_event_t*) { if (step_night(ui.edit, -1)) refresh_night(); });
    refresh_night();
    done_button(s, [](lv_event_t*) { ui_show(screen_enums::main_temperature); });
}

// ---------------------------------------------------------------- mode

static void style_modes() {
    for (size_t i = 0; i < std::size(MODES); i++)
        style_oval(ui.mode_buttons[i], MODES[i].color, ui.edit.mode == MODES[i].mode);
}

static void build_mode(lv_obj_t* s) {
    for (size_t i = 0; i < std::size(MODES); i++) {
        ui.mode_buttons[i] = oval_button(s, 8 + (int)i * 78, 92, 70, 56, MODES[i].color, MODES[i].label,
                                         [](lv_event_t* e) {
                                             auto* choice = static_cast<const mode_choice*>(lv_event_get_user_data(e));
                                             ui.edit.mode = choice->mode;   // one selected at a time
                                             style_modes();
                                         },
                                         const_cast<mode_choice*>(&MODES[i]));
    }
    style_modes();
    done_button(s, [](lv_event_t*) { ui_show(screen_enums::main_temperature); });
}

// ---------------------------------------------------------------- switching and the timer

screen_enums ui_current() {
    return ui.current;
}

void ui_show(screen_enums screen) {
    lv_obj_t* old = ui.screen;
    ui.screen = lv_obj_create(nullptr);
    plain(ui.screen);
    lv_obj_set_style_bg_color(ui.screen, colors::background, 0);
    lv_obj_set_style_bg_opa(ui.screen, LV_OPA_COVER, 0);

    ui.current = screen;
    ui.main_temp = ui.main_weather = ui.heat_label = ui.ac_label = ui.night_label = ui.fan_button = nullptr;
    switch (screen) {
        case screen_enums::main:              build_main(ui.screen); break;
        case screen_enums::main_temperature:  build_setpoints(ui.screen); break;
        case screen_enums::night_temperature: build_night(ui.screen); break;
        case screen_enums::weather_1:         build_forecast(ui.screen, 0); break;
        case screen_enums::weather_2:         build_forecast(ui.screen, 2); break;
        case screen_enums::season_mode:       build_mode(ui.screen); break;
    }
    lv_screen_load(ui.screen);
    // usually called from a button on the old screen: delete it after the event finishes
    if (old) lv_obj_delete_async(old);
}

static void tick(lv_timer_t*) {
    if (ui.current != screen_enums::main && lv_display_get_inactive_time(nullptr) > SCREEN_TIMEOUT_MS) {
        ui_show(screen_enums::main);   // unsent edits are dropped: open_setpoints copies fresh ones
        return;
    }
    api_snapshot snap = ui.api->snapshot();
    if (snap.version == ui.shown_version) return;
    ui.shown_version = snap.version;
    // edit screens keep showing the edit, not live values
    if (ui.current == screen_enums::main) update_main(snap);
    if ((ui.current == screen_enums::weather_1 || ui.current == screen_enums::weather_2) &&
        snap.forecast != ui.shown_forecast)
        ui_show(ui.current);
}

static std::vector<char> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void ui_start(api_client& api, const std::string& asset_dir) {
    ui.api = &api;
    ui.icon_dir = asset_dir + "/icons";

    std::string font_path = asset_dir + "/fonts/Montserrat-Medium.ttf";
    ui.ttf = read_file(font_path);
    if (ui.ttf.empty()) {
        std::fprintf(stderr, "font missing: %s (using the built-in 48 px font)\n", font_path.c_str());
        ui.font_huge = ui.font_big = ui.font_medium = &lv_font_montserrat_48;
    } else {
        ui.font_huge = lv_tiny_ttf_create_data(ui.ttf.data(), ui.ttf.size(), 120);
        ui.font_big = lv_tiny_ttf_create_data(ui.ttf.data(), ui.ttf.size(), 76);
        ui.font_medium = lv_tiny_ttf_create_data(ui.ttf.data(), ui.ttf.size(), 44);
    }

    ui_show(screen_enums::main);
    lv_timer_create(tick, 250, nullptr);
}
