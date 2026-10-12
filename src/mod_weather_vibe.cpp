// mod_weather_vibe.cpp — AzerothCore module entry point
// Registers all script hooks and initialises the module.

#include "mod_weather_vibe.h"
#include "mod_wv_core.h"
#include "mod_wv_engine.h"
#include "mod_wv_world.h"

#include "Chat.h"
#include "Player.h"
#include "ScriptMgr.h"

// Forward declaration for the registration function defined in its own TU.
void RegisterWeatherVibeCommands();

void WeatherVibe_ApplyConfig(bool reload)
{
    WeatherVibe::ConfigView cfg;

    sWeatherVibeCore.LoadConfig(cfg);

    if (!sWeatherVibeCore.IsEnabled())
    {
        sWeatherVibeEngine.Clear();
        return;
    }

    sWeatherVibeWorld.LoadConfig(cfg);
    sWeatherVibeEngine.Load(cfg, reload);
}

// ============================================================
// PlayerScript — tells a client the weather of where it now stands
//
// The zone and area hooks run inside map updates, possibly on a map thread: they only read the
// module's state, and write nothing but the player's own record of what it was last sent.
// Bots have no client and are skipped.
// ============================================================
class WeatherVibe_PlayerScript : public PlayerScript
{
public:
    WeatherVibe_PlayerScript() : PlayerScript("WeatherVibe_PlayerScript",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_UPDATE_ZONE, PLAYERHOOK_ON_UPDATE_AREA,
          PLAYERHOOK_ON_MAP_CHANGED }) {}

    // The weather itself needs no push here: Player::UpdateZone runs during login and
    // reaches OnPlayerUpdateZone below.
    void OnPlayerLogin(Player* player) override
    {
        if (sWeatherVibeCore.IsEnabled() && sWeatherVibeCore.IsAnnounceEnabled())
            ChatHandler(player->GetSession()).SendSysMessage("This server is running the |cff4CFF00WeatherVibe |rmodule.");
    }

    void OnPlayerLogout(Player* player) override
    {
        sWeatherVibeCore.SetDebugSubscriber(player->GetGUID(), false);
        sWeatherVibeCore.SetAuditioning(player->GetGUID(), false);
    }

    // A loading screen: the client starts its weather over, so the zone hook that follows
    // must send whatever is up, even if it is what this client was last told.
    void OnPlayerMapChanged(Player* player) override
    {
        if (WeatherVibeCore::HasClient(player))
            WeatherVibeCore::Forget(player);
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 newArea) override
    {
        if (!sWeatherVibeCore.IsEnabled() || !WeatherVibeCore::HasClient(player))
            return;

        sWeatherVibeEngine.SendCurrentTo(player, newZone, newArea, true);
    }

    void OnPlayerUpdateArea(Player* player, uint32 oldArea, uint32 newArea) override
    {
        if (!sWeatherVibeCore.IsEnabled() || !WeatherVibeCore::HasClient(player))
            return;

        sWeatherVibeEngine.OnAreaChange(player, oldArea, newArea);
    }
};

// ============================================================
// WorldScript — startup, config reload, and the engine's tick
// ============================================================
class WeatherVibe_WorldScript : public WorldScript
{
public:
    WeatherVibe_WorldScript() : WorldScript("WeatherVibe_WorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) {}

    // The first config load happens before the DBC stores exist, and zone validation needs
    // them — so the initial load waits for OnStartup and only reloads are handled here.
    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            WeatherVibe_ApplyConfig(true);
    }

    void OnStartup() override
    {
        WeatherVibe_ApplyConfig(false);
    }

    void OnUpdate(uint32 diff) override
    {
        if (sWeatherVibeCore.IsEnabled())
            sWeatherVibeEngine.Update(diff);
    }
};

// ============================================================
// Module entry point — called by AzerothCore loader
// ============================================================
void Addmod_weather_vibeScripts()
{
    new WeatherVibe_PlayerScript();
    new WeatherVibe_WorldScript();

    RegisterWeatherVibeCommands();
}
