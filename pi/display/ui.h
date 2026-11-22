/*
 * ui.h - the screens (idea.md -> Display screens):
 *
 *   main --tap left--> main_temperature (set points) --Mode--> season_mode --Done--+
 *     |                  |  ^   |                                                  |
 *     |                  |  |   +--Night--> night_temperature --Done--+            |
 *     |                  |  +-------------------------------------------<----------+
 *     |                  +--Done (the only place changes are sent)--> main
 *     +--tap right--> weather_1 <--arrow--> weather_2 --Done--> main
 *
 * 30 s without a touch on any screen but main -> main, unsent changes dropped.
 */

#ifndef UI_H
#define UI_H

#include <string>
#include "api_client.h"
#include "display_constants.h"

// loads fonts and icons from asset_dir (falls back to built-in fonts if the TTF is
// missing), shows main, and starts the timer that refreshes data and times out screens
void ui_start(api_client& api, const std::string& asset_dir);

void ui_show(screen_enums screen);
screen_enums ui_current();

#endif
