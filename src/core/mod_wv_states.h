#ifndef MOD_WV_STATES_H
#define MOD_WV_STATES_H

#include "Define.h"
#include "Weather.h"          // WeatherState enum

// The client has no thunderstorm. Its weather id 86, which the core calls THUNDERS, is rain
// with the Dark Portal's ambience loop (Weather.dbc: effect type rain, sound "DarkPortal").
// So thunder is the module's own: one of three states, each sent to the client as weather it
// can draw (WeatherVibeCore::WireState) while the server plays the thunder itself
// (WeatherVibeWorld::Thunder).
//   WEATHER_STATE_THUNDERS   a rainstorm      -> heavy rain
//   WV_STATE_THUNDER_SNOW    thundersnow      -> heavy snow
//   WV_STATE_DRY_THUNDERS    a dry storm      -> fog: a dark sky, nothing falling
// A profile row that says 86 becomes whichever of the three suits the profile (see
// WeatherVibeEngine::ResolveThunder). The other two have no client id and exist only here;
// their numbers are free in Weather.dbc and are never sent.
constexpr WeatherState WV_STATE_THUNDER_SNOW = WeatherState(186);
constexpr WeatherState WV_STATE_DRY_THUNDERS = WeatherState(286);

#endif // MOD_WV_STATES_H
