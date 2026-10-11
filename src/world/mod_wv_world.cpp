#include "mod_wv_world.h"
#include "mod_wv_engine.h"

#include "CellImpl.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "MapMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"

#include <list>
#include <unordered_set>

using namespace WeatherVibe;

namespace
{
    // Someone who could plausibly look up and say something: a living, idle, friendly humanoid
    // that belongs to the world — not a pet, a summon, a trigger or a bot (bots are players).
    struct SpeakerCheck
    {
        SpeakerCheck(Player const* player, float range) : _player(player), _range(range) { }

        bool operator()(Creature* creature) const
        {
            return creature->IsAlive()
                && !creature->IsInCombat()
                && creature->GetCreatureType() == CREATURE_TYPE_HUMANOID
                && !creature->IsPet()
                && !creature->IsTotem()
                && !creature->IsSummon()
                && !creature->IsTrigger()
                && !creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE)
                && creature->IsFriendlyTo(_player)
                && _player->IsWithinDistInMap(creature, _range)
                && creature->IsWithinLOSInMap(_player);
        }

    private:
        Player const* _player;
        float _range;
    };

    Creature* FindSpeaker(Player* player, float range, std::unordered_set<ObjectGuid> const& alreadySpoke)
    {
        std::list<Creature*> nearby;
        SpeakerCheck check(player, range);
        Acore::CreatureListSearcher<SpeakerCheck> searcher(player, nearby, check);
        Cell::VisitObjects(player, searcher, range);

        Creature* nearest = nullptr;
        float nearestDist = 0.0f;

        for (Creature* creature : nearby)
        {
            if (alreadySpoke.count(creature->GetGUID()))
                continue;

            float const dist = player->GetDistance(creature);
            if (!nearest || dist < nearestDist)
            {
                nearest = creature;
                nearestDist = dist;
            }
        }

        return nearest;
    }
}

WeatherVibeWorld& WeatherVibeWorld::Instance()
{
    static WeatherVibeWorld instance;
    return instance;
}

char const* WeatherVibeWorld::TurnName(WeatherTurn turn)
{
    switch (turn)
    {
    case WeatherTurn::RAIN:      return "Rain";
    case WeatherTurn::SNOW:      return "Snow";
    case WeatherTurn::SANDSTORM: return "Sandstorm";
    case WeatherTurn::STORM:     return "Storm";
    case WeatherTurn::FOG:       return "Fog";
    case WeatherTurn::CLEAR:     return "Clear";
    default:                     return "None";
    }
}

void WeatherVibeWorld::LoadConfig(ConfigView const& cfg)
{
    _hookEnabled = cfg.Bool("WeatherVibe.Hook.Enable", true);
    _resyncMs = std::min(cfg.UInt("WeatherVibe.Hook.ResyncMinutes", 10), 1440u) * 60000;

    _remarksEnabled = cfg.Bool("WeatherVibe.Remarks.Enable", true);
    _remarkChance = std::clamp(cfg.Float("WeatherVibe.Remarks.Chance", 15.0f), 0.0f, 100.0f);
    _remarkCooldownSec = cfg.UInt("WeatherVibe.Remarks.CooldownMinutes", 30) * 60;
    _remarkRange = std::clamp(cfg.Float("WeatherVibe.Remarks.Range", 12.0f), 3.0f, 40.0f);

    for (size_t turn = 1; turn < size_t(WeatherTurn::COUNT); ++turn)
    {
        _remarkLines[turn].clear();

        // Spoken text: kept as written, '#' included.
        std::string const base = std::string("WeatherVibe.Remarks.") + TurnName(WeatherTurn(turn));
        for (auto const& [index, line] : cfg.Numbered(base, false))
            _remarkLines[turn].push_back(line);
    }
}

WeatherTurn WeatherVibeWorld::ClassifyTurn(WeatherState previous, WeatherState now)
{
    if (previous == now)
        return WeatherTurn::NONE;

    Family const was = GetFamily(previous);
    Family const is = GetFamily(now);

    if (now == WEATHER_STATE_THUNDERS || (GetTier(now) == 3 && GetTier(previous) < 3))
        return WeatherTurn::STORM;

    switch (is)
    {
    case Family::FINE:      return WeatherTurn::CLEAR;
    case Family::FOG:       return WeatherTurn::FOG;
    case Family::RAIN:      return (was == Family::RAIN || was == Family::THUNDER) ? WeatherTurn::NONE : WeatherTurn::RAIN;
    case Family::SNOW:      return was == Family::SNOW ? WeatherTurn::NONE : WeatherTurn::SNOW;
    case Family::SANDSTORM: return was == Family::SANDSTORM ? WeatherTurn::NONE : WeatherTurn::SANDSTORM;
    default:                return WeatherTurn::NONE;
    }
}

void WeatherVibeWorld::FireHook(Profile const& profile) const
{
    if (!_hookEnabled || !profile.live.started)
        return;

    // The hook wants a core Weather object. This one has no game_weather row behind it:
    // ScriptId 0 means no WeatherScript is looked up, and it never ticks or sends anything.
    static WeatherData const noScript{};

    for (uint32 zoneId : profile.zoneIds)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
        if (!area)
            continue;

        // Open-world zones only; an instanced zone has no single map to speak for it.
        Map* map = sMapMgr->FindBaseNonInstanceMap(area->mapid);
        if (!map)
            continue;

        Weather weather(map, zoneId, &noScript);
        sScriptMgr->OnWeatherChange(&weather, profile.live.state, profile.live.raw);
    }
}

void WeatherVibeWorld::OnSettled(Profile const& profile, WeatherState previous)
{
    FireHook(profile);

    if (!_remarksEnabled)
        return;

    WeatherTurn const turn = ClassifyTurn(previous, profile.live.state);
    if (turn != WeatherTurn::NONE)
        Remark(profile, turn);
}

// When the weather turns, someone standing near a player may say so. Rare, unannounced,
// and never more than one line per player per cooldown.
void WeatherVibeWorld::Remark(Profile const& profile, WeatherTurn turn)
{
    std::vector<std::string> const& lines = _remarkLines[size_t(turn)];
    if (lines.empty() || _remarkChance <= 0.0f)
        return;

    time_t const now = GameTime::GetGameTime().count();

    if (_lastRemark.size() > 512)
        std::erase_if(_lastRemark, [&](auto const& entry) { return now - entry.second >= time_t(_remarkCooldownSec); });

    std::unordered_set<ObjectGuid> spoke;

    sWeatherVibeEngine.ForEachListener(profile, [&](Player* player)
        {
            if (!player->IsAlive() || player->IsInCombat() || player->HasUnitState(UNIT_STATE_IN_FLIGHT) || !player->IsOutdoors())
                return;

            auto last = _lastRemark.find(player->GetGUID());
            if (last != _lastRemark.end() && now - last->second < time_t(_remarkCooldownSec))
                return;

            if (!roll_chance_f(_remarkChance))
                return;

            Creature* speaker = FindSpeaker(player, _remarkRange, spoke);
            if (!speaker)
                return;

            speaker->Say(lines[urand(0, uint32(lines.size()) - 1)], LANG_UNIVERSAL, player);
            spoke.insert(speaker->GetGUID());
            _lastRemark[player->GetGUID()] = now;
        });
}
