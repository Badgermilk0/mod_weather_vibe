#include "mod_wv_world.h"
#include "mod_wv_engine.h"

#include "CellImpl.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"

#include <cmath>
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

    // ── Thunder ──
    // Sound ids are SoundEntries.dbc rows whose files are in the stock client:
    //   4936   Blasted Lands lightning bolt (3 variants, loud)
    //   13021  Storm Peaks lightning        (3 variants)
    //   9843   Netherstorm lightning crack  (7 variants, quieter)
    //   10007  the same cracks, quieter still
    auto readSounds = [&](char const* key, char const* def)
        {
            std::vector<uint32> out;
            for (std::string const& token : Split(cfg.Text(key, def), ','))
            {
                uint32 soundId = 0;
                if (!token.empty() && ParseUInt(token, soundId) && soundId)
                    out.push_back(soundId);
            }
            return out;
        };

    _thunderEnabled = cfg.Bool("WeatherVibe.Thunder.Enable", true);
    uint32 thunderMin = std::clamp(cfg.UInt("WeatherVibe.Thunder.IntervalSeconds.Min", 12), 3u, 3600u);
    uint32 thunderMax = std::clamp(cfg.UInt("WeatherVibe.Thunder.IntervalSeconds.Max", 70), 3u, 3600u);
    if (thunderMax < thunderMin) std::swap(thunderMin, thunderMax);
    _thunderMinMs = thunderMin * 1000;
    _thunderMaxMs = thunderMax * 1000;
    _thunderNear = readSounds("WeatherVibe.Thunder.Sounds.Near", "4936, 13021");
    _thunderFar = readSounds("WeatherVibe.Thunder.Sounds.Far", "9843, 10007");

    // ── Lightning strikes ──
    _lightningEnabled = cfg.Bool("WeatherVibe.Lightning.Enable", false);
    _lightningBroken = false;
    _lightningEntry = cfg.UInt("WeatherVibe.Lightning.GameObject", 190569);
    _lightningChance = std::clamp(cfg.Float("WeatherVibe.Lightning.Chance", 35.0f), 0.0f, 100.0f);
    _lightningDespawnSec = std::clamp(cfg.UInt("WeatherVibe.Lightning.DespawnSeconds", 3), 1u, 30u);
    _lightningMinDist = std::clamp(cfg.Float("WeatherVibe.Lightning.Distance.Min", 35.0f), 5.0f, 200.0f);
    _lightningMaxDist = std::clamp(cfg.Float("WeatherVibe.Lightning.Distance.Max", 90.0f), _lightningMinDist, 250.0f);
}

WeatherTurn WeatherVibeWorld::ClassifyTurn(WeatherState previous, WeatherState now)
{
    if (previous == now)
        return WeatherTurn::NONE;

    Family const was = GetFamily(previous);
    Family const is = GetFamily(now);

    if (is == Family::THUNDER || (GetTier(now) == 3 && GetTier(previous) < 3))
        return WeatherTurn::STORM;

    // A storm easing back into the rain or snow it was made of is not news.
    switch (is)
    {
    case Family::FINE:      return WeatherTurn::CLEAR;
    case Family::FOG:       return WeatherTurn::FOG;
    case Family::RAIN:      return (was == Family::RAIN || previous == WEATHER_STATE_THUNDERS) ? WeatherTurn::NONE : WeatherTurn::RAIN;
    case Family::SNOW:      return (was == Family::SNOW || previous == WV_STATE_THUNDER_SNOW) ? WeatherTurn::NONE : WeatherTurn::SNOW;
    case Family::SANDSTORM: return was == Family::SANDSTORM ? WeatherTurn::NONE : WeatherTurn::SANDSTORM;
    default:                return WeatherTurn::NONE;
    }
}

// ─────────────────────────────────────────────────────────────
// Thunder and lightning
// ─────────────────────────────────────────────────────────────

uint32 WeatherVibeWorld::NextThunderDelayMs(float intensity) const
{
    float const storm = std::clamp(intensity, 0.0f, 1.0f);
    float const gap = float(_thunderMaxMs) - storm * float(_thunderMaxMs - _thunderMinMs);
    return uint32(std::max(1000.0f, gap * frand(0.6f, 1.4f)));
}

void WeatherVibeWorld::Thunder(Profile const& profile, float intensity)
{
    if (!_thunderEnabled)
        return;

    // A profile's own sounds if it has them (Netherstorm crackles); otherwise loud entries are
    // likelier the harder the storm, which reads as the storm being closer.
    std::vector<uint32> const* pool = &profile.thunderSounds;
    if (pool->empty())
    {
        bool const overhead = roll_chance_f(25.0f + 60.0f * std::clamp(intensity, 0.0f, 1.0f));
        pool = overhead ? &_thunderNear : &_thunderFar;
        if (pool->empty())
            pool = overhead ? &_thunderFar : &_thunderNear;
    }

    uint32 const soundId = pool->empty() ? 0 : (*pool)[urand(0, uint32(pool->size()) - 1)];

    struct Strike { uint32 mapId; float x; float y; };
    std::vector<Strike> strikes;

    sWeatherVibeEngine.ForEachListener(profile, [&](Player* player)
        {
            if (_lightningEnabled && !_lightningBroken)
            {
                // A bolt already fell close by this clap: its own thunder reaches this far.
                for (Strike const& strike : strikes)
                    if (strike.mapId == player->GetMapId() && player->GetExactDist2d(strike.x, strike.y) < 80.0f)
                        return;

                if (roll_chance_f(_lightningChance) && StrikeLightningNear(player, _lightningMinDist, _lightningMaxDist))
                {
                    strikes.push_back({ player->GetMapId(), player->GetPositionX(), player->GetPositionY() });
                    return;
                }
            }

            if (soundId)
                player->PlayDirectSound(soundId, player);
        });
}

bool WeatherVibeWorld::StrikeLightningNear(Player* player, float minDist, float maxDist)
{
    Map* map = player->FindMap();
    if (!map || _lightningBroken || !player->IsOutdoors())
        return false;

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        float const angle = frand(0.0f, 2.0f * float(M_PI));
        float const dist = frand(minDist, maxDist);
        float const x = player->GetPositionX() + dist * std::cos(angle);
        float const y = player->GetPositionY() + dist * std::sin(angle);

        // The ground under the sky there: searched from well above the player, so a hillside
        // or a rooftop is found rather than the cellar beneath it.
        float const z = map->GetHeight(player->GetPhaseMask(), x, y, player->GetPositionZ() + 40.0f, true, 150.0f);
        if (z <= INVALID_HEIGHT || map->IsInWater(player->GetPhaseMask(), x, y, z, 2.0f))
            continue;

        // A native, unowned, self-removing object: the bolt model and its positional thunder.
        if (map->SummonGameObject(_lightningEntry, x, y, z, angle, 0.0f, 0.0f, 0.0f, 0.0f, _lightningDespawnSec))
            return true;

        // The template is not in this world DB. Say so once and stop asking.
        _lightningBroken = true;
        LOG_ERROR("module", "[WeatherVibe] lightning game object {} could not be spawned; strikes are off until the config is reloaded "
            "(WeatherVibe.Lightning.GameObject)", _lightningEntry);
        return false;
    }

    return false;
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

        // Listeners are told 86 for any kind of thunder, whatever the client is drawing.
        Weather weather(map, zoneId, &noScript);
        sScriptMgr->OnWeatherChange(&weather, WeatherVibeCore::HookState(profile.live.state), profile.live.raw);
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
