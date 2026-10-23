#include "mod_weather_vibe.h"
#include "mod_wv_core.h"
#include "mod_wv_engine.h"
#include "mod_wv_world.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include <algorithm>

using Acore::ChatCommands::ChatCommandTable;
using Acore::ChatCommands::Console;
using namespace WeatherVibe;

namespace
{
    void Reply(ChatHandler* handler, std::string const& text)
    {
        handler->SendSysMessage("|cff00ff00WeatherVibe:|r " + text);
    }

    bool Fail(ChatHandler* handler, std::string const& text)
    {
        Reply(handler, text);
        handler->SetSentErrorMessage(true);
        return false;
    }

    // ============================================================
    // Runtime permission gate for the read-only commands.
    // WeatherVibeCommand = 0  -> GM only (default)
    // WeatherVibeCommand = 1  -> everyone
    // Anything that changes the weather is GM-only in the command table itself.
    // ============================================================
    bool MayRead(ChatHandler* handler)
    {
        if (handler->IsConsole() || sWeatherVibeCore.PlayersMayRead())
            return true;

        if (handler->GetSession()->GetSecurity() >= SEC_GAMEMASTER)
            return true;

        return Fail(handler, "Commands are restricted to GMs.");
    }

    bool Ready(ChatHandler* handler)
    {
        if (sWeatherVibeCore.IsEnabled())
            return true;

        return Fail(handler, "Module is disabled in config.");
    }

    // "here" is the profile the caller stands in; anything else is a profile name.
    Profile* ProfileArg(ChatHandler* handler, std::string const& token)
    {
        if (token.empty() || ToLower(token) == "here")
        {
            Player* player = handler->GetPlayer();
            if (!player)
            {
                Fail(handler, "Name a profile: 'here' needs a character.");
                return nullptr;
            }

            Profile* profile = sWeatherVibeEngine.FindProfileFor(player->GetZoneId(), player->GetAreaId());
            if (!profile)
                Fail(handler, "No weather profile covers where you stand (see .wvibe where).");
            return profile;
        }

        Profile* profile = sWeatherVibeEngine.FindProfile(token);
        if (!profile)
            Fail(handler, "Unknown profile '" + token + "' (.wvibe status lists them).");
        return profile;
    }

    bool StateArg(ChatHandler* handler, std::string const& token, WeatherState& state)
    {
        if (WeatherVibeCore::ParseState(token, state))
            return true;

        return Fail(handler, "Invalid weather state '" + token + "'. Use .wvibe help for valid states.");
    }

    // Everywhere a profile's weather is seen, by name.
    std::string CoverageOf(Profile const& profile)
    {
        std::string out;
        for (uint32 zoneId : profile.zoneIds)
            out += (out.empty() ? "" : ", ") + WeatherVibeCore::AreaName(zoneId);
        for (uint32 areaId : profile.areaIds)
            out += (out.empty() ? "" : ", ") + WeatherVibeCore::AreaName(areaId);
        return out;
    }
}

// ============================================================
// CommandScript class
// Each handler owns its validation, feedback and dispatch.
// ============================================================
class WeatherVibe_CommandScript : public CommandScript
{
public:
    WeatherVibe_CommandScript() : CommandScript("WeatherVibe_CommandScript") {}

    // ----------------------------------------------------------
    // .wvibe status [here|<profile>]
    // ----------------------------------------------------------
    static bool HandleWvibeStatus(ChatHandler* handler, Optional<std::string> target)
    {
        if (!MayRead(handler) || !Ready(handler))
            return false;

        SeasonBlend const blend = sWeatherVibeCore.GetSeasonBlend();
        std::string season = WeatherVibeCore::SeasonName(blend.primary);
        if (blend.secondaryShare > 0.005f)
            season += Acore::StringFormat(" (+{:.0f}% {})", blend.secondaryShare * 100.0f,
                WeatherVibeCore::SeasonName(blend.secondary));

        Reply(handler, "season " + season + " | daypart "
            + WeatherVibeCore::DayPartName(sWeatherVibeCore.GetCurrentDayPart()));

        if (target)
        {
            Profile* profile = ProfileArg(handler, *target);
            if (!profile)
                return false;

            handler->SendSysMessage(sWeatherVibeEngine.Describe(*profile));
            handler->SendSysMessage("  covers: " + CoverageOf(*profile));
            handler->SendSysMessage(Acore::StringFormat("  thunder here is {}, its tempest {} | rain looks: {} | sandstorm looks: {}",
                WeatherVibeCore::WeatherStateName(profile->thunderAs),
                WeatherVibeCore::WeatherStateName(WeatherVibeEngine::ResolveThunder(*profile, WV_STATE_TEMPEST)),
                WeatherVibeCore::RainSkinName(profile->skins.rain), WeatherVibeCore::SandSkinName(profile->skins.sand)));
            handler->SendSysMessage(Acore::StringFormat("  Waystone block {} | thunder made by the {}{}",
                sWeatherVibeCore.IsExtensionEnabled() ? "on" : "off",
                WeatherVibeCore::ThunderSourceName(sWeatherVibeCore.GetThunderSource()),
                profile->hasFlashColor ? Acore::StringFormat(" | lightning colour {:06X}", profile->flashColor) : std::string()));

            if (!profile->neighbors.empty())
            {
                std::string neighbours;
                for (uint16 index : profile->neighbors)
                {
                    Profile const& other = sWeatherVibeEngine.GetProfiles()[index];
                    neighbours += (neighbours.empty() ? "" : ", ") + other.name + " ("
                        + WeatherVibeCore::WeatherStateName(other.live.targetState) + ")";
                }
                handler->SendSysMessage("  neighbours: " + neighbours);
            }

            return true;
        }

        std::vector<Profile const*> sorted;
        for (Profile const& profile : sWeatherVibeEngine.GetProfiles())
            sorted.push_back(&profile);

        if (sorted.empty())
        {
            Reply(handler, "No profiles are loaded.");
            return true;
        }

        std::sort(sorted.begin(), sorted.end(), [](Profile const* a, Profile const* b) { return a->name < b->name; });

        for (Profile const* profile : sorted)
            handler->SendSysMessage(sWeatherVibeEngine.Describe(*profile));

        return true;
    }

    // ----------------------------------------------------------
    // .wvibe where
    // ----------------------------------------------------------
    static bool HandleWvibeWhere(ChatHandler* handler)
    {
        if (!MayRead(handler))
            return false;

        Player* player = handler->GetPlayer();
        if (!player)
            return Fail(handler, "No player found (use in-game only).");

        uint32 const zoneId = player->GetZoneId();
        uint32 const areaId = player->GetAreaId();

        Reply(handler, Acore::StringFormat("zone {} ({}) | area {} ({})",
            zoneId, WeatherVibeCore::AreaName(zoneId), areaId, WeatherVibeCore::AreaName(areaId)));

        Profile const* profile = sWeatherVibeEngine.Resolve(zoneId, areaId);
        if (!profile)
        {
            handler->SendSysMessage("  no weather profile here");
            return true;
        }

        bool const byArea = std::find(profile->areaIds.begin(), profile->areaIds.end(), areaId) != profile->areaIds.end();
        handler->SendSysMessage(Acore::StringFormat("  profile {} (by {})", profile->name, byArea ? "area" : "zone"));
        handler->SendSysMessage("  " + sWeatherVibeEngine.Describe(*profile));
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe set <zoneId> <state> <percentage:0..100>
    // Sends the zone's profile there, then lets it carry on.
    // ----------------------------------------------------------
    static bool HandleWvibeSet(ChatHandler* handler, uint32 zoneId, std::string stateToken, float percentage)
    {
        if (!Ready(handler))
            return false;

        WeatherState state = WEATHER_STATE_FINE;
        if (!StateArg(handler, stateToken, state))
            return false;

        if (Profile* profile = sWeatherVibeEngine.FindProfileFor(zoneId, 0))
        {
            sWeatherVibeEngine.Steer(*profile, state, percentage);
            Reply(handler, "steering " + sWeatherVibeEngine.Describe(*profile));
            return true;
        }

        float const raw = sWeatherVibeCore.PercentToRaw(sWeatherVibeCore.GetCurrentDayPart(), state, percentage);
        uint32 const reached = sWeatherVibeEngine.PushToZone(zoneId, state, raw);
        Reply(handler, Acore::StringFormat("zone {} has no profile; sent once to {} client(s), nothing will hold it.", zoneId, reached));
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe setRaw <zoneId> <state> <grade:0..1>
    // Calibration: snaps to a client grade and stays there until released. The id sent is
    // still chosen by the grade (WireState); `audition` is the way to see one exact id.
    // ----------------------------------------------------------
    static bool HandleWvibeSetRaw(ChatHandler* handler, uint32 zoneId, std::string stateToken, float rawGrade)
    {
        if (!Ready(handler))
            return false;

        WeatherState state = WEATHER_STATE_FINE;
        if (!StateArg(handler, stateToken, state))
            return false;

        if (Profile* profile = sWeatherVibeEngine.FindProfileFor(zoneId, 0))
        {
            sWeatherVibeEngine.ForceRaw(*profile, state, rawGrade);
            Reply(handler, sWeatherVibeEngine.Describe(*profile));
            return true;
        }

        uint32 const reached = sWeatherVibeEngine.PushToZone(zoneId, state, rawGrade);
        Reply(handler, Acore::StringFormat("zone {} has no profile; sent once to {} client(s), nothing will hold it.", zoneId, reached));
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe force <here|profile> <state> <percentage> [minutes]
    // ----------------------------------------------------------
    static bool HandleWvibeForce(ChatHandler* handler, std::string target, std::string stateToken, float percentage, Optional<uint32> minutes)
    {
        if (!Ready(handler))
            return false;

        Profile* profile = ProfileArg(handler, target);
        if (!profile)
            return false;

        WeatherState state = WEATHER_STATE_FINE;
        if (!StateArg(handler, stateToken, state))
            return false;

        sWeatherVibeEngine.Force(*profile, state, percentage, std::min(minutes.value_or(0), 24u * 60u));
        Reply(handler, sWeatherVibeEngine.Describe(*profile));
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe resume [here|profile|all]
    // ----------------------------------------------------------
    static bool HandleWvibeResume(ChatHandler* handler, Optional<std::string> target)
    {
        if (!Ready(handler))
            return false;

        if (!target || ToLower(*target) == "all")
        {
            uint32 released = 0;
            for (Profile const& profile : sWeatherVibeEngine.GetProfiles())
            {
                if (!profile.live.forced)
                    continue;

                sWeatherVibeEngine.Resume(*sWeatherVibeEngine.FindProfile(profile.name));
                ++released;
            }

            Reply(handler, Acore::StringFormat("released {} locked profile(s).", released));
            return true;
        }

        Profile* profile = ProfileArg(handler, *target);
        if (!profile)
            return false;

        sWeatherVibeEngine.Resume(*profile);
        Reply(handler, profile->name + " released.");
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe next [here|profile]
    // ----------------------------------------------------------
    static bool HandleWvibeNext(ChatHandler* handler, Optional<std::string> target)
    {
        if (!Ready(handler))
            return false;

        Profile* profile = ProfileArg(handler, target.value_or("here"));
        if (!profile)
            return false;

        sWeatherVibeEngine.SkipHold(*profile);
        Reply(handler, profile->name + " will pick its next weather now.");
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe reload
    // Re-reads the config files and re-applies this module only. `.reload config` works too.
    // ----------------------------------------------------------
    static bool HandleWvibeReload(ChatHandler* handler)
    {
        if (!sConfigMgr->Reload())
            return Fail(handler, "Could not re-read the config files.");

        WeatherVibe_ApplyConfig(true);

        if (!sWeatherVibeCore.IsEnabled())
        {
            Reply(handler, "config re-read; the module is disabled.");
            return true;
        }

        Reply(handler, Acore::StringFormat("config re-read: {} profile(s); running weather was kept. Warnings, if any, are in the server log.",
            sWeatherVibeEngine.GetProfiles().size()));
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe debug <on|off>
    // ----------------------------------------------------------
    static bool HandleWvibeDebug(ChatHandler* handler, std::string onOff)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return Fail(handler, "In-game only. For the server log, set WeatherVibe.Debug = 1.");

        std::string const mode = ToLower(onOff);
        if (mode != "on" && mode != "off")
            return Fail(handler, "Use .wvibe debug on or .wvibe debug off.");

        sWeatherVibeCore.SetDebugSubscriber(player->GetGUID(), mode == "on");
        Reply(handler, mode == "on"
            ? "you will see the engine's decisions for the weather you stand in."
            : "debug feed off.");
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe audition <clientWeatherId> [raw:0..1]   |   .wvibe audition off
    // Shows any weather id the client knows (Weather.dbc) to the caller alone. Nothing about
    // the zone changes; the caller simply stops receiving its weather until "off".
    // ----------------------------------------------------------
    static bool HandleWvibeAudition(ChatHandler* handler, std::string idOrOff, Optional<float> rawGrade)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return Fail(handler, "In-game only.");

        // Either way the client is about to show something the module did not send it.
        WeatherVibeCore::Forget(player);

        if (ToLower(idOrOff) == "off")
        {
            sWeatherVibeCore.SetAuditioning(player->GetGUID(), false);
            sWeatherVibeEngine.SendCurrentTo(player, player->GetZoneId(), player->GetAreaId());
            Reply(handler, "back to the weather of where you stand.");
            return true;
        }

        uint32 weatherId = 0;
        if (!ParseUInt(idOrOff, weatherId) || weatherId > 0xFFFF)
            return Fail(handler, "Use .wvibe audition <clientWeatherId> [grade 0..1], or .wvibe audition off.");

        float const grade = std::clamp(rawGrade.value_or(0.6f), 0.0f, 1.0f);

        sWeatherVibeCore.SetAuditioning(player->GetGUID(), true);
        WeatherVibeCore::SendWeather(player, WeatherState(weatherId), grade);

        Reply(handler, Acore::StringFormat("showing client weather {} at grade {:.2f} ({:.0f}% of full density), to you only. "
            "It stays until .wvibe audition off.", weatherId, grade, Density(grade) * 100.0f));
        handler->SendSysMessage("  the id picks the sound; the grade what falls (nothing at or below 0.25) and how dark the sky is (full from 0.25)");
        handler->SendSysMessage("  extra looks: 104 ash mist | 103 ember dust | 90 dark rain | 100 red rain | 86 rain with the thunder bed for its sound");
        handler->SendSysMessage("  silent rows: 2 rain | 61 snow (what thin drizzle and flurries are sent as)");
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe lightning
    // Drops one bolt close by, whether or not strikes are switched on, to see what it looks like.
    // ----------------------------------------------------------
    static bool HandleWvibeLightning(ChatHandler* handler)
    {
        if (!Ready(handler))
            return false;

        Player* player = handler->GetPlayer();
        if (!player)
            return Fail(handler, "In-game only.");

        uint32 const entry = sWeatherVibeWorld.GetLightningEntry();

        if (!sWeatherVibeWorld.StrikeLightningNear(player, 12.0f, 25.0f))
            return Fail(handler, Acore::StringFormat("no bolt: you are indoors, there is no open ground within 25 yards, "
                "or game object {} is not in the world DB.", entry));

        Reply(handler, Acore::StringFormat("a bolt (game object {}) fell within 25 yards. Strikes during storms are {} "
            "(WeatherVibe.Lightning.Enable).", entry, sWeatherVibeWorld.IsLightningEnabled() ? "on" : "off"));
        return true;
    }

    // ----------------------------------------------------------
    // .wvibe simulate <here|profile> [hours]
    // ----------------------------------------------------------
    static bool HandleWvibeSimulate(ChatHandler* handler, std::string target, Optional<uint32> hours)
    {
        if (!Ready(handler))
            return false;

        Profile* profile = ProfileArg(handler, target);
        if (!profile)
            return false;

        uint32 const span = std::clamp(hours.value_or(48), 1u, 2000u);
        std::array<float, WV_STATES> const share = sWeatherVibeEngine.Simulate(*profile, span);

        Reply(handler, Acore::StringFormat("{} over {}h as things stand now ({}, {}):", profile->name, span,
            WeatherVibeCore::SeasonName(sWeatherVibeCore.GetCurrentSeason()),
            WeatherVibeCore::DayPartName(sWeatherVibeCore.GetCurrentDayPart())));

        std::vector<size_t> order;
        for (size_t i = 0; i < WV_STATES; ++i)
            if (share[i] > 0.0005f)
                order.push_back(i);

        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return share[a] > share[b]; });

        for (size_t index : order)
            handler->SendSysMessage(Acore::StringFormat("  {:5.1f}%  {}", share[index] * 100.0f,
                WeatherVibeCore::WeatherStateName(WeatherVibeCore::StateAt(index))));

        if (order.empty())
            handler->SendSysMessage("  nothing to pick from in this season.");

        return true;
    }

    // ----------------------------------------------------------
    // .wvibe help
    // ----------------------------------------------------------
    static bool HandleWvibeHelp(ChatHandler* handler)
    {
        if (!MayRead(handler))
            return false;

        Reply(handler, "commands:");
        handler->SendSysMessage("  .wvibe status [here|profile]  (current weather, time to next change)");
        handler->SendSysMessage("  .wvibe where  (zone and area ids, and the profile that covers them)");
        handler->SendSysMessage("  .wvibe set [zoneId] [state] [pct:0..100]  (go there, then carry on)");
        handler->SendSysMessage("  .wvibe force [here|profile] [state] [pct] [minutes]  (go there and stay)");
        handler->SendSysMessage("  .wvibe setRaw [zoneId] [state] [grade:0..1]  (calibration: snap to a client grade and stay)");
        handler->SendSysMessage("  .wvibe resume [here|profile|all]  |  .wvibe next [here|profile]");
        handler->SendSysMessage("  .wvibe simulate [here|profile] [hours]  |  .wvibe debug on|off  |  .wvibe reload");
        handler->SendSysMessage("  .wvibe audition [clientWeatherId] [grade] | audition off  (look at any client weather, you only)");
        handler->SendSysMessage("  .wvibe lightning  (drop one bolt beside you)");
        Reply(handler, "states (number or name):");
        handler->SendSysMessage("Clear | 0 Fine | 1 Fog | 3 LightRain | 4 MediumRain | 5 HeavyRain");
        handler->SendSysMessage("6 LightSnow | 7 MediumSnow | 8 HeavySnow");
        handler->SendSysMessage("22 LightSandstorm | 41 MediumSandstorm | 42 HeavySandstorm");
        handler->SendSysMessage("86 Thunders (the storm that suits the place) | ThunderSnow | DryThunders");
        handler->SendSysMessage("Tempest (the worst storm the place gets) | Blizzard");
        return true;
    }

    // ----------------------------------------------------------
    // Command table – the read-only commands are registered at SEC_PLAYER and gated at runtime
    // by MayRead(); everything that changes the weather is GM-only here.
    // ----------------------------------------------------------
    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable wvibeTable =
        {
            { "status",   HandleWvibeStatus,   SEC_PLAYER,        Console::Yes },
            { "show",     HandleWvibeStatus,   SEC_PLAYER,        Console::Yes },
            { "where",    HandleWvibeWhere,    SEC_PLAYER,        Console::No  },
            { "help",     HandleWvibeHelp,     SEC_PLAYER,        Console::Yes },
            { "set",      HandleWvibeSet,      SEC_GAMEMASTER,    Console::Yes },
            { "setRaw",   HandleWvibeSetRaw,   SEC_GAMEMASTER,    Console::Yes },
            { "force",    HandleWvibeForce,    SEC_GAMEMASTER,    Console::Yes },
            { "resume",   HandleWvibeResume,   SEC_GAMEMASTER,    Console::Yes },
            { "next",     HandleWvibeNext,     SEC_GAMEMASTER,    Console::Yes },
            { "simulate", HandleWvibeSimulate, SEC_GAMEMASTER,    Console::Yes },
            { "audition", HandleWvibeAudition, SEC_GAMEMASTER,    Console::No  },
            { "lightning", HandleWvibeLightning, SEC_GAMEMASTER,  Console::No  },
            { "debug",    HandleWvibeDebug,    SEC_GAMEMASTER,    Console::No  },
            { "reload",   HandleWvibeReload,   SEC_ADMINISTRATOR, Console::Yes },
        };
        static ChatCommandTable root =
        {
            { "wvibe", wvibeTable }
        };
        return root;
    }
};

// ============================================================
// Registration — called from Addmod_weather_vibeScripts()
// ============================================================
void RegisterWeatherVibeCommands()
{
    new WeatherVibe_CommandScript();
}
