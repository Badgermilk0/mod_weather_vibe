#ifndef MOD_WEATHER_VIBE_H
#define MOD_WEATHER_VIBE_H

// Registers module scripts.
void Addmod_weather_vibeScripts();

// (Re)applies the module's config. With reload set, running profiles keep their weather.
// World thread only, and not before the DBC stores are loaded.
void WeatherVibe_ApplyConfig(bool reload);

#endif //MOD_WEATHER_VIBE_H
