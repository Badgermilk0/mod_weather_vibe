#ifndef MOD_WV_WORLD_H
#define MOD_WV_WORLD_H

#include "mod_wv_core.h"

#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

namespace WeatherVibe
{
    struct Profile;
}

// How the weather turned, as someone standing in it would put it.
enum class WeatherTurn : uint8
{
    NONE = 0,
    RAIN,
    SNOW,
    SANDSTORM,
    STORM,
    FOG,
    CLEAR,
    COUNT
};

// ============================================================
// WeatherVibeWorld — singleton
// What the world does about the weather: the hook other scripts listen to, and the
// occasional remark from someone standing nearby. World thread only.
// ============================================================
class WeatherVibeWorld
{
public:
    static WeatherVibeWorld& Instance();

    WeatherVibeWorld(WeatherVibeWorld const&) = delete;
    WeatherVibeWorld& operator=(WeatherVibeWorld const&) = delete;

    void LoadConfig(WeatherVibe::ConfigView const& cfg);

    // A profile came to rest on new weather.
    void OnSettled(WeatherVibe::Profile const& profile, WeatherState previous);

    // Fires the core's weather-change hook for every open-world zone of the profile. mod-ale
    // turns it into the Lua event WEATHER_EVENT_ON_CHANGE (25): (event, zoneId, state, grade).
    void FireHook(WeatherVibe::Profile const& profile) const;

    // How often a profile re-announces its weather to hook listeners (0 = only when it changes).
    uint32 GetResyncMs() const { return _hookEnabled ? _resyncMs : 0; }

    static WeatherTurn  ClassifyTurn(WeatherState previous, WeatherState now);
    static char const*  TurnName(WeatherTurn turn);

private:
    WeatherVibeWorld() = default;

    void Remark(WeatherVibe::Profile const& profile, WeatherTurn turn);

    bool   _hookEnabled = true;
    uint32 _resyncMs = 10 * 60 * 1000;

    bool   _remarksEnabled = true;
    float  _remarkChance = 15.0f;        // percent, per player, per turn of the weather
    uint32 _remarkCooldownSec = 30 * 60;
    float  _remarkRange = 12.0f;

    std::vector<std::string> _remarkLines[size_t(WeatherTurn::COUNT)];
    std::unordered_map<ObjectGuid, time_t> _lastRemark;
};

#define sWeatherVibeWorld WeatherVibeWorld::Instance()

#endif // MOD_WV_WORLD_H
