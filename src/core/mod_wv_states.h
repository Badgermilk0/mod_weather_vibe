#ifndef MOD_WV_STATES_H
#define MOD_WV_STATES_H

#include "Define.h"
#include "Weather.h"          // WeatherState enum

// The client has no thunderstorm. Its weather id 86, which the core calls THUNDERS, is rain
// with the Dark Portal's ambience loop (Weather.dbc: effect type rain, sound "DarkPortal").
// So thunder is the module's own: one of three states, each sent to the client as weather it
// can draw (WeatherVibeCore::WireState) while the thunder comes from the server
// (WeatherVibeWorld::Thunder) or, on a Waystone client, from the client's own lightning.
//   WEATHER_STATE_THUNDERS   a rainstorm      -> heavy rain
//   WV_STATE_THUNDER_SNOW    thundersnow      -> heavy snow
//   WV_STATE_DRY_THUNDERS    a dry storm      -> fog: a dark sky, nothing falling
// A profile row that says 86 becomes whichever of the three suits the profile (see
// WeatherVibeEngine::ResolveThunder). The other two have no client id and exist only here;
// their numbers are free in Weather.dbc and are never sent.
constexpr WeatherState WV_STATE_THUNDER_SNOW = WeatherState(186);
constexpr WeatherState WV_STATE_DRY_THUNDERS = WeatherState(286);

// The ends of the scale, also the module's own and never sent as such:
//   WV_STATE_CLEAR      not a cloud in the sky        -> id 0 at grade 0
//   WV_STATE_TEMPEST    the worst storm a place gets  -> the heaviest rain, the most lightning
//   WV_STATE_BLIZZARD   the worst snow                -> the heaviest snow
// A row that says Tempest becomes a Blizzard where the profile's storms are snow, and a dry
// storm where they are neither (ResolveThunder).
constexpr WeatherState WV_STATE_CLEAR = WeatherState(300);
constexpr WeatherState WV_STATE_BLIZZARD = WeatherState(308);
constexpr WeatherState WV_STATE_TEMPEST = WeatherState(386);

#endif // MOD_WV_STATES_H
