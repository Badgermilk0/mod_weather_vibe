#ifndef MOD_WV_WORLD_H
#define MOD_WV_WORLD_H

#include "mod_wv_core.h"

#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

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

    // --- Thunder. The stock client has no thunderstorm (see mod_wv_states.h): it draws the
    //     rain, snow or dark sky. A Waystone client is asked for its own lightning instead
    //     (WeatherVibeEngine::WireOf); WeatherVibe.Client.Thunder says which is used. ---
    // Thunder at all (WeatherVibe.Thunder.Enable).
    bool   IsThunderOn() const { return _thunderEnabled; }
    // The server's own claps and rumble: unless every listener is left to the client.
    bool   IsThunderEnabled() const
    {
        return _thunderEnabled && sWeatherVibeCore.GetThunderSource() != ThunderSource::CLIENT;
    }
    // Time to the next clap; shorter as the storm (0..1) builds.
    uint32 NextThunderDelayMs(float intensity) const;
    // One clap, the same for everyone standing in the profile outdoors; under a roof a near
    // clap is heard as a far one. With lightning switched on, a listener may get a bolt
    // nearby instead, which brings its own thunder.
    void   Thunder(WeatherVibe::Profile const& profile, float intensity);
    // The rumble: the client's own long storm bed (Thunder.Rumble.Sound) under the claps and the
    // rain, played to each listener once per Rumble.Seconds. Tracked per player, so walking
    // from one stormy profile into another never stacks two of them.
    bool   IsRumbleEnabled() const { return IsThunderEnabled() && _rumbleSound; }
    bool   RumbleFromNeighbors() const { return _rumbleNeighbors; }
    void   Rumble(WeatherVibe::Profile const& profile);
    // Drops a lightning bolt on open ground between minDist and maxDist yards from the player.
    // Works whether or not strikes are switched on, so `.wvibe lightning` can show one.
    bool   StrikeLightningNear(Player* player, float minDist, float maxDist);
    bool   IsLightningEnabled() const { return _lightningEnabled; }
    uint32 GetLightningEntry() const { return _lightningEntry; }

private:
    WeatherVibeWorld() = default;

    void Remark(WeatherVibe::Profile const& profile, WeatherTurn turn);

    bool   _hookEnabled = true;
    uint32 _resyncMs = 10 * 60 * 1000;

    bool   _thunderEnabled = true;
    uint32 _thunderMinMs = 12 * 1000;
    uint32 _thunderMaxMs = 70 * 1000;
    std::vector<uint32> _thunderNear;    // loud sound entries: a storm overhead
    std::vector<uint32> _thunderFar;     // quiet ones: a storm some way off

    uint32 _rumbleSound = 4994;          // "DarkPortal": the 59 s bed the client pairs with weather 86
    uint32 _rumbleMs = 58 * 1000;
    bool   _rumbleNeighbors = true;
    std::unordered_map<ObjectGuid, uint64> _lastRumble;   // game-time ms of each player's last one

    bool   _lightningEnabled = false;
    bool   _lightningBroken = false;     // the game object is missing from the world DB
    uint32 _lightningEntry = 190569;     // "Acherus Lightning": a bolt model with positional thunder
    float  _lightningChance = 35.0f;     // percent, per listener, per clap
    uint32 _lightningDespawnSec = 3;
    float  _lightningMinDist = 35.0f;
    float  _lightningMaxDist = 90.0f;

    bool   _remarksEnabled = true;
    float  _remarkChance = 15.0f;        // percent, per player, per turn of the weather
    uint32 _remarkCooldownSec = 30 * 60;
    float  _remarkRange = 12.0f;

    std::vector<std::string> _remarkLines[size_t(WeatherTurn::COUNT)];
    std::unordered_map<ObjectGuid, time_t> _lastRemark;
};

#define sWeatherVibeWorld WeatherVibeWorld::Instance()

#endif // MOD_WV_WORLD_H
