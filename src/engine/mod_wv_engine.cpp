#include "mod_wv_engine.h"
#include "mod_wv_world.h"

#include "Chat.h"
#include "DBCStores.h"
#include "GameEventMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cmath>

using namespace WeatherVibe;

// ─────────────────────────────────────────────────────────────
// Local helpers
// ─────────────────────────────────────────────────────────────

namespace
{
    // Snow and sandstorm: the client restarts the effect from nothing on every packet about
    // it, so those are sent once per real change and the client is left to do the fading.
    bool IsOneShot(Ground ground)
    {
        return ground == Ground::SNOW || ground == Ground::SAND;
    }

    // How often every client's last packet is checked against what it should be showing.
    constexpr uint32 RECONCILE_MS = 20000;

    // How often a profile looks for a storm in earshot; the rumble itself runs much longer.
    constexpr uint32 RUMBLE_CHECK_MS = 5000;

    // How long the client itself takes to get from one grade to another.
    uint32 ClientFadeMs(float from, float to)
    {
        return uint32(std::fabs(to - from) / CLIENT_GRADE_PER_SECOND * 1000.0f);
    }

    // Parse "weight,state,minPct,maxPct" from a config value.
    bool ParseProfileEntry(std::string const& raw, WeatherEntry& out, bool& swapped)
    {
        std::vector<std::string> parts = Split(raw, ',');
        if (parts.size() != 4)
            return false;

        float weight = 0.0f, minPct = 0.0f, maxPct = 0.0f;
        WeatherState state = WEATHER_STATE_FINE;

        if (!ParseFloat(parts[0], weight) || !WeatherVibeCore::ParseState(parts[1], state) ||
            !ParseFloat(parts[2], minPct) || !ParseFloat(parts[3], maxPct))
            return false;

        swapped = maxPct < minPct;
        if (swapped) std::swap(minPct, maxPct);

        out.weight = weight;
        out.state = state;
        out.minPct = std::clamp(minPct, 0.0f, 100.0f);
        out.maxPct = std::clamp(maxPct, 0.0f, 100.0f);
        return out.weight > 0.0f;
    }

    // Where a transition stands after elapsedMs. A streamed leg is eased, so it starts and
    // ends gently; a one-shot leg is at its end value from its first moment.
    void Evaluate(TransitionPlan const& plan, uint32 elapsedMs, WeatherState& state, float& raw)
    {
        uint32 legStart = 0;

        for (uint8 i = 0; i < plan.legCount; ++i)
        {
            Leg const& leg = plan.legs[i];
            bool const last = (i + 1 == plan.legCount);

            if (!last && elapsedMs >= legStart + leg.ms)
            {
                legStart += leg.ms;
                continue;
            }

            state = leg.state;
            if (leg.oneShot || !leg.ms)
            {
                raw = leg.to;
                return;
            }

            float const t = std::clamp(float(elapsedMs - legStart) / float(leg.ms), 0.0f, 1.0f);
            raw = leg.from + (leg.to - leg.from) * (t * t * (3.0f - 2.0f * t));
            return;
        }
    }

    std::string Duration(uint32 ms)
    {
        uint32 const seconds = ms / 1000;
        if (seconds >= 60)
            return Acore::StringFormat("{}m{:02}s", seconds / 60, seconds % 60);
        return Acore::StringFormat("{}s", seconds);
    }

    // The top-level zone an area belongs to (itself, when it is one).
    uint32 ZoneOfArea(uint32 areaId)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
        if (!area)
            return 0;
        return area->zone ? area->zone : area->ID;
    }
}

// ─────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────

WeatherVibeEngine& WeatherVibeEngine::Instance()
{
    static WeatherVibeEngine instance;
    return instance;
}

void WeatherVibeEngine::Clear()
{
    _profiles.clear();
    _zoneToProfile.clear();
    _areaToProfile.clear();
    _anyDirty = false;
}

// ─────────────────────────────────────────────────────────────
// Load — profiles and mappings from config
// ─────────────────────────────────────────────────────────────

void WeatherVibeEngine::LoadGlobals(ConfigView const& cfg)
{
    _autoEngine = sWeatherVibeCore.IsProfileEnabled();
    _rules.strict = cfg.Bool("WeatherVibe.Escalation.Strict", true);
    _rules.fogBridge = std::clamp(cfg.Float("WeatherVibe.FogBridge.Boost", 1.5f), 0.0f, 10.0f);
    _pace = std::clamp(cfg.Float("WeatherVibe.Pace", 1.0f), 0.1f, 10.0f);
    _frontStrength = std::clamp(cfg.Float("WeatherVibe.Front.Strength", 1.5f), 0.0f, 10.0f);
    _tickMs = std::clamp(cfg.UInt("WeatherVibe.Transition.TickSeconds", 4), 1u, 60u) * 1000;

    // Legacy safety net; delivery no longer depends on it.
    _reapplyMs = cfg.UInt("WeatherVibe.Profile.ReApply.PerSec", 0) * 1000;
    _reapplyTimerMs = _reapplyMs;

    // Storms pass, fine spells linger.
    static constexpr float kDefaultHold[WV_STATES] =
    {
        1.50f,                 // Fine
        1.25f,                 // Fog
        1.00f, 1.00f, 0.75f,   // rain
        1.00f, 1.00f, 0.75f,   // snow
        1.00f, 1.00f, 0.75f,   // sandstorm
        0.60f, 0.60f, 0.60f    // Thunders, ThunderSnow, DryThunders
    };

    for (size_t i = 0; i < WV_STATES; ++i)
    {
        char const* stateName = WeatherVibeCore::WeatherStateName(WeatherVibeCore::StateAt(i));

        _holdMult[i] = std::clamp(cfg.Float(std::string("WeatherVibe.Hold.") + stateName, kDefaultHold[i]), 0.1f, 10.0f);

        for (size_t dp = 0; dp < WV_DAYPARTS; ++dp)
        {
            std::string key = std::string("WeatherVibe.DayPartWeight.")
                + WeatherVibeCore::DayPartTokenUpper(DayPart(dp)) + "." + stateName;
            _dayPartWeight[dp][i] = std::max(0.0f, cfg.Float(key, 1.0f));
        }
    }

    // Thunder is thunder: unless the conf says otherwise, thundersnow and dry storms keep the
    // hours and the staying power set for Thunders.
    size_t const thunders = size_t(WeatherVibeCore::StateIndex(WEATHER_STATE_THUNDERS));
    for (WeatherState kind : { WV_STATE_THUNDER_SNOW, WV_STATE_DRY_THUNDERS })
    {
        size_t const i = size_t(WeatherVibeCore::StateIndex(kind));
        char const* kindName = WeatherVibeCore::WeatherStateName(kind);

        if (!cfg.Has(std::string("WeatherVibe.Hold.") + kindName))
            _holdMult[i] = _holdMult[thunders];

        for (size_t dp = 0; dp < WV_DAYPARTS; ++dp)
        {
            std::string key = std::string("WeatherVibe.DayPartWeight.")
                + WeatherVibeCore::DayPartTokenUpper(DayPart(dp)) + "." + kindName;
            if (!cfg.Has(key))
                _dayPartWeight[dp][i] = _dayPartWeight[dp][thunders];
        }
    }
}

bool WeatherVibeEngine::LoadProfile(ConfigView const& cfg, Profile& profile) const
{
    std::string const base = "WeatherVibe.Profile." + profile.name + ".";
    bool anyEntries = false;

    for (size_t s = 0; s < WV_SEASONS; ++s)
    {
        std::string const seasonKey = base + WeatherVibeCore::SeasonName(Season(s));

        for (auto const& [index, value] : cfg.Numbered(seasonKey))
        {
            WeatherEntry entry;
            bool swapped = false;

            if (!ParseProfileEntry(value, entry, swapped))
            {
                LOG_WARN("server.loading", "[WeatherVibe] bad entry '{}.{} = {}' (want weight,state,minPct,maxPct)",
                    seasonKey, index, value);
                continue;
            }

            if (swapped)
                LOG_WARN("server.loading", "[WeatherVibe] '{}.{} = {}' has its minimum above its maximum (typo?), using {:.0f}-{:.0f}",
                    seasonKey, index, value, entry.minPct, entry.maxPct);

            profile.seasons[s].push_back(entry);
            anyEntries = true;
        }
    }

    if (!anyEntries)
        return false;

    auto key = [&](char const* suffix) { return base + "Transition." + suffix; };

    uint32 stepMin = cfg.UInt(key("Step.TimeSeconds.Min"), 15);
    uint32 stepMax = cfg.UInt(key("Step.TimeSeconds.Max"), 30);
    if (stepMax < stepMin) std::swap(stepMin, stepMax);

    TransitionSettings& ts = profile.transition;
    ts.stepTimeMinMs = stepMin * 1000;
    ts.stepTimeMaxMs = stepMax * 1000;
    ts.stepSizePct = std::clamp(cfg.Float(key("StepSize.Perc"), 10.0f), 0.5f, 50.0f);
    ts.changesPerHr = std::clamp(cfg.UInt(key("Max.Changes.Per.Hour"), 6), 1u, 120u);
    ts.maxConsecSame = cfg.UInt(key("Max.Consecutive.Same"), 3);
    ts.minHoldMs = cfg.UInt(key("MinHoldSeconds"), 30) * 1000;

    // Time-of-day pattern: the global table, unless this profile says otherwise.
    for (size_t dp = 0; dp < WV_DAYPARTS; ++dp)
    {
        for (size_t i = 0; i < WV_STATES; ++i)
        {
            std::string overrideKey = base + "DayPartWeight." + WeatherVibeCore::DayPartTokenUpper(DayPart(dp))
                + "." + WeatherVibeCore::WeatherStateName(WeatherVibeCore::StateAt(i));

            profile.dayPartWeight[dp][i] = cfg.Has(overrideKey)
                ? std::max(0.0f, cfg.Float(overrideKey, _dayPartWeight[dp][i]))
                : _dayPartWeight[dp][i];
        }
    }

    // ── Thunder: a storm in the profile's own weather. With rain in its tables it is a
    //    rainstorm, with only snow it is thundersnow, with neither a dry storm. ──
    bool hasRain = false, hasSnow = false;
    for (WeatherTable const& table : profile.seasons)
    {
        for (WeatherEntry const& entry : table)
        {
            if (GetTier(entry.state) == 0)
                continue;
            if (GetFamily(entry.state) == Family::RAIN) hasRain = true;
            if (GetFamily(entry.state) == Family::SNOW) hasSnow = true;
        }
    }

    profile.thunderAs = hasRain ? WEATHER_STATE_THUNDERS : hasSnow ? WV_STATE_THUNDER_SNOW : WV_STATE_DRY_THUNDERS;

    std::string const style = ToLower(cfg.Text(base + "Thunder.Style", "auto"));
    if (style == "rain")      profile.thunderAs = WEATHER_STATE_THUNDERS;
    else if (style == "snow") profile.thunderAs = WV_STATE_THUNDER_SNOW;
    else if (style == "dry")  profile.thunderAs = WV_STATE_DRY_THUNDERS;
    else if (style != "auto")
        LOG_WARN("server.loading", "[WeatherVibe] '{}Thunder.Style = {}' is not rain, snow, dry or auto", base, style);

    for (WeatherTable& table : profile.seasons)
        for (WeatherEntry& entry : table)
            entry.state = ResolveThunder(profile, entry.state);

    profile.thunderSounds.clear();
    for (std::string const& token : Split(cfg.Text(base + "Thunder.Sounds"), ','))
    {
        uint32 soundId = 0;
        if (token.empty())
            continue;
        if (ParseUInt(token, soundId) && soundId)
            profile.thunderSounds.push_back(soundId);
        else
            LOG_WARN("server.loading", "[WeatherVibe] '{}Thunder.Sounds' has '{}', which is not a sound id", base, token);
    }

    // ── Skins: what its rain and its sandstorm look like ──
    std::string skin = cfg.Text(base + "Rain.Skin");
    if (!skin.empty() && !WeatherVibeCore::ParseRainSkin(skin, profile.skins.rain))
        LOG_WARN("server.loading", "[WeatherVibe] '{}Rain.Skin = {}' is not rain, dark or red", base, skin);

    skin = cfg.Text(base + "Sandstorm.Skin");
    if (!skin.empty() && !WeatherVibeCore::ParseSandSkin(skin, profile.skins.sand))
        LOG_WARN("server.loading", "[WeatherVibe] '{}Sandstorm.Skin = {}' is not sand, ash or ember", base, skin);

    return true;
}

void WeatherVibeEngine::LoadNeighbors(ConfigView const& cfg)
{
    auto link = [](Profile& profile, uint16 other)
        {
            if (std::find(profile.neighbors.begin(), profile.neighbors.end(), other) == profile.neighbors.end())
                profile.neighbors.push_back(other);
        };

    for (uint16 i = 0; i < _profiles.size(); ++i)
    {
        std::string const listKey = "WeatherVibe.Profile." + _profiles[i].name + ".Neighbors";
        std::string const list = cfg.Text(listKey);
        if (list.empty())
            continue;

        for (std::string const& name : Split(list, ','))
        {
            if (name.empty())
                continue;

            Profile* other = FindProfile(name);
            if (!other)
            {
                LOG_WARN("server.loading", "[WeatherVibe] '{}' names unknown neighbour '{}'", listKey, name);
                continue;
            }

            uint16 const j = uint16(other - _profiles.data());
            if (j == i)
                continue;

            // Borders have two sides: naming a neighbour once is enough.
            link(_profiles[i], j);
            link(_profiles[j], i);
        }
    }
}

void WeatherVibeEngine::ValidateProfile(Profile const& profile) const
{
    // A medium or heavy tier with nothing lighter beneath it can only be reached by breaking
    // the escalation rule, so in practice it almost never appears.
    static constexpr WeatherState kLadders[3][3] =
    {
        { WEATHER_STATE_LIGHT_RAIN,      WEATHER_STATE_MEDIUM_RAIN,      WEATHER_STATE_HEAVY_RAIN },
        { WEATHER_STATE_LIGHT_SNOW,      WEATHER_STATE_MEDIUM_SNOW,      WEATHER_STATE_HEAVY_SNOW },
        { WEATHER_STATE_LIGHT_SANDSTORM, WEATHER_STATE_MEDIUM_SANDSTORM, WEATHER_STATE_HEAVY_SANDSTORM }
    };

    for (size_t s = 0; s < WV_SEASONS; ++s)
    {
        WeatherTable const& table = profile.seasons[s];
        auto has = [&](WeatherState state)
            {
                return std::any_of(table.begin(), table.end(), [&](WeatherEntry const& e) { return e.state == state; });
            };

        for (auto const& ladder : kLadders)
        {
            for (int tier = 1; tier < 3; ++tier)
            {
                if (has(ladder[tier]) && !has(ladder[tier - 1]))
                    LOG_WARN("server.loading", "[WeatherVibe] {}.{}: {} has no {} below it and will rarely be reached",
                        profile.name, WeatherVibeCore::SeasonName(Season(s)),
                        WeatherVibeCore::WeatherStateName(ladder[tier]),
                        WeatherVibeCore::WeatherStateName(ladder[tier - 1]));
            }
        }
    }
}

void WeatherVibeEngine::Load(ConfigView const& cfg, bool reload)
{
    std::vector<Profile> previous = std::move(_profiles);
    Clear();

    LoadGlobals(cfg);

    // ── Mappings: a profile exists for every name a zone or an area points at ──
    struct Mapped { std::vector<uint32> zones; std::vector<uint32> areas; };
    std::vector<std::string> order;
    std::unordered_map<std::string, Mapped> mapped;

    auto readMappings = [&](char const* baseKey, bool isArea)
        {
            for (auto const& [index, value] : cfg.Numbered(baseKey))
            {
                std::vector<std::string> parts = Split(value, ',');
                uint32 id = 0;

                if (parts.size() != 2 || !ParseUInt(parts[0], id) || id == 0 || parts[1].empty())
                {
                    LOG_WARN("server.loading", "[WeatherVibe] bad mapping '{}.{} = {}' (want <id>,<ProfileName>)",
                        baseKey, index, value);
                    continue;
                }

                auto [it, inserted] = mapped.try_emplace(parts[1]);
                if (inserted)
                    order.push_back(parts[1]);

                (isArea ? it->second.areas : it->second.zones).push_back(id);
            }
        };

    readMappings("WeatherVibe.ZoneProfile", false);
    readMappings("WeatherVibe.AreaProfile", true);

    if (order.empty())
    {
        LOG_INFO("server.loading", "[WeatherVibe] no zone profiles configured");
        return;
    }

    for (std::string const& name : order)
    {
        Profile profile;
        profile.name = name;
        profile.zoneIds = mapped[name].zones;
        profile.areaIds = mapped[name].areas;

        if (!LoadProfile(cfg, profile))
        {
            LOG_WARN("server.loading", "[WeatherVibe] profile '{}' has no entries, skipping", name);
            continue;
        }

        _profiles.push_back(std::move(profile));
    }

    // ── Holiday overlays: WeatherVibe.Holiday.<holidayId>.<Profile>.<N> ──
    {
        std::string const prefix = "WeatherVibe.Holiday.";
        for (std::string const& key : cfg.KeysWithPrefix(prefix))
        {
            std::vector<std::string> parts = Split(key.substr(prefix.size()), '.');
            uint32 holidayId = 0, index = 0;
            std::string const value = cfg.Text(key);

            if (parts.size() != 3 || !ParseUInt(parts[0], holidayId) || !ParseUInt(parts[2], index))
            {
                LOG_WARN("server.loading", "[WeatherVibe] bad holiday key '{}' (want WeatherVibe.Holiday.<holidayId>.<Profile>.<N>)", key);
                continue;
            }

            Profile* profile = FindProfile(parts[1]);
            HolidayEntry holiday;
            bool swapped = false;
            holiday.holidayId = holidayId;

            if (!profile || !ParseProfileEntry(value, holiday.entry, swapped))
            {
                LOG_WARN("server.loading", "[WeatherVibe] holiday entry '{} = {}' ignored ({})", key, value,
                    profile ? "want weight,state,minPct,maxPct" : "unknown profile");
                continue;
            }

            holiday.entry.state = ResolveThunder(*profile, holiday.entry.state);
            profile->holidays.push_back(holiday);
        }
    }

    // ── Lookups, with a look at whether each id means what the conf thinks it means ──
    for (uint16 i = 0; i < _profiles.size(); ++i)
    {
        Profile const& profile = _profiles[i];

        for (uint32 zoneId : profile.zoneIds)
        {
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
            if (!area)
                LOG_WARN("server.loading", "[WeatherVibe] zone {} ('{}') is not in AreaTable, so this mapping does nothing",
                    zoneId, profile.name);
            else if (area->zone)
                LOG_WARN("server.loading", "[WeatherVibe] {} '{}' ('{}') is a sub-area of zone {}; map it with WeatherVibe.AreaProfile instead",
                    zoneId, WeatherVibeCore::AreaName(zoneId), profile.name, area->zone);

            auto [it, inserted] = _zoneToProfile.try_emplace(zoneId, i);
            if (!inserted)
            {
                LOG_WARN("server.loading", "[WeatherVibe] zone {} is mapped to both '{}' and '{}'; using '{}'",
                    zoneId, _profiles[it->second].name, profile.name, profile.name);
                it->second = i;
            }
        }

        for (uint32 areaId : profile.areaIds)
        {
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
            if (!area)
                LOG_WARN("server.loading", "[WeatherVibe] area {} ('{}') is not in AreaTable, so this mapping does nothing",
                    areaId, profile.name);

            auto [it, inserted] = _areaToProfile.try_emplace(areaId, i);
            if (!inserted)
            {
                LOG_WARN("server.loading", "[WeatherVibe] area {} is mapped to both '{}' and '{}'; using '{}'",
                    areaId, _profiles[it->second].name, profile.name, profile.name);
                it->second = i;
            }
        }
    }

    LoadNeighbors(cfg);

    // ── Live weather: carried over by name on a reload, otherwise the world starts mid-weather ──
    uint32 const resync = sWeatherVibeWorld.GetResyncMs();
    uint32 carried = 0;

    for (size_t i = 0; i < _profiles.size(); ++i)
    {
        Profile& profile = _profiles[i];

        auto old = std::find_if(previous.begin(), previous.end(),
            [&](Profile const& p) { return p.name == profile.name; });

        if (reload && old != previous.end() && old->live.started)
        {
            profile.live = old->live;
            ++carried;
        }
        else
            WarmStart(profile);

        // Spread the re-announcements so they do not all land on one tick.
        if (resync && !(reload && old != previous.end()))
            profile.live.resyncTimerMs = uint32(uint64(resync) * i / _profiles.size());

        ValidateProfile(profile);

        std::string where;
        for (uint32 zoneId : profile.zoneIds) { where += where.empty() ? "" : ", "; where += std::to_string(zoneId); }
        for (uint32 areaId : profile.areaIds) { where += where.empty() ? "area " : ", area "; where += std::to_string(areaId); }

        LOG_INFO("server.loading", "[WeatherVibe] profile '{}' -> [{}], {} neighbour(s), starts on {}",
            profile.name, where, profile.neighbors.size(),
            profile.live.started ? WeatherVibeCore::WeatherStateName(profile.live.state) : "nothing (no usable entries)");
    }

    LOG_INFO("server.loading", "[WeatherVibe] {} profile(s) active, {} zone(s), {} area(s){}",
        _profiles.size(), _zoneToProfile.size(), _areaToProfile.size(),
        reload ? Acore::StringFormat(", {} kept their weather", carried) : std::string());

    if (!reload)
        return;

    // Mappings may have moved under people's feet: tell every client where it stands now.
    for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld() || session->IsSocketClosed() || player->IsBeingTeleported())
            continue;

        SendCurrentTo(player, player->GetZoneId(), player->GetAreaId());
    }
}

// The world should already have weather when the first player logs in, and not the same
// weather everywhere: settle each profile on a weighted pick, part-way through its hold.
void WeatherVibeEngine::WarmStart(Profile& profile)
{
    LiveState& live = profile.live;
    live = LiveState{};

    WeatherState state = WEATHER_STATE_FINE;
    float pct = 100.0f;

    if (_autoEngine)
    {
        WeatherTable table;
        BuildTable(profile, table, false);

        int const index = PickWeighted(table);
        if (index < 0)
            return;

        state = table[size_t(index)].state;
        pct = frand(table[size_t(index)].minPct, table[size_t(index)].maxPct);
    }

    live.started = true;
    live.band = sWeatherVibeCore.GetCurrentDayPart();
    live.state = live.targetState = live.settledState = state;
    live.targetPct = pct;
    live.raw = live.targetRaw = sWeatherVibeCore.PercentToRaw(live.band, state, pct);
    live.phase = Phase::HOLD;
    live.holdMs = uint32(float(CalcHoldMs(profile, state, 0)) * frand(0.15f, 1.0f));
}

// ─────────────────────────────────────────────────────────────
// What may come next: the effective table for a profile right now
// ─────────────────────────────────────────────────────────────

void WeatherVibeEngine::BuildTable(Profile const& profile, WeatherTable& out, bool useFronts) const
{
    out.clear();

    // ── Season: this season's rows, with the neighbouring season mixed in near its boundary ──
    SeasonBlend const blend = sWeatherVibeCore.GetSeasonBlend();
    WeatherTable const* primary = &profile.seasons[size_t(blend.primary)];
    WeatherTable const* secondary = &profile.seasons[size_t(blend.secondary)];
    float share = (blend.secondary == blend.primary || secondary->empty()) ? 0.0f : blend.secondaryShare;

    if (primary->empty())
    {
        // A profile may leave a season out; borrow another rather than stall.
        share = 0.0f;
        if (!secondary->empty())
            primary = secondary;
        else
            for (WeatherTable const& table : profile.seasons)
                if (!table.empty()) { primary = &table; break; }
    }

    for (WeatherEntry const& entry : *primary)
    {
        out.push_back(entry);
        out.back().weight *= (1.0f - share);
    }

    if (share > 0.0f)
    {
        for (WeatherEntry const& entry : *secondary)
        {
            out.push_back(entry);
            out.back().weight *= share;
        }
    }

    // ── Holidays add their own rows while they run ──
    for (HolidayEntry const& holiday : profile.holidays)
        if (IsHolidayActive(HolidayIds(holiday.holidayId)))
            out.push_back(holiday.entry);

    // ── Time of day ──
    size_t const dp = size_t(sWeatherVibeCore.GetCurrentDayPart());
    for (WeatherEntry& entry : out)
    {
        int const index = WeatherVibeCore::StateIndex(entry.state);
        if (index >= 0)
            entry.weight *= profile.dayPartWeight[dp][size_t(index)];
    }

    // ── Fronts: heavy weather next door makes the same kind of weather likelier here.
    //    Only rows the table already has are boosted, so a desert never inherits snow. ──
    if (!useFronts || _frontStrength <= 0.0f || profile.neighbors.empty())
        return;

    float rain = 0.0f, snow = 0.0f, sand = 0.0f;

    for (uint16 index : profile.neighbors)
    {
        LiveState const& other = _profiles[index].live;
        if (!other.started)
            continue;

        // A storm next door counts as the heavy weather it is drawn as; a dry one carries nothing.
        if (other.targetState == WEATHER_STATE_THUNDERS) { rain += 1.0f; continue; }
        if (other.targetState == WV_STATE_THUNDER_SNOW)  { snow += 1.0f; continue; }

        if (GetTier(other.targetState) < 2)
            continue;

        switch (GetFamily(other.targetState))
        {
        case Family::RAIN:      rain += 1.0f; break;
        case Family::SNOW:      snow += 1.0f; break;
        case Family::SANDSTORM: sand += 1.0f; break;
        default: break;
        }
    }

    float const count = float(profile.neighbors.size());

    for (WeatherEntry& entry : out)
    {
        float pressure = 0.0f;

        switch (GetFamily(entry.state))
        {
        case Family::RAIN:      pressure = rain / count; break;
        case Family::SNOW:      pressure = snow / count; break;
        case Family::SANDSTORM: pressure = sand / count; break;
        case Family::THUNDER:
            if (entry.state == WEATHER_STATE_THUNDERS)     pressure = rain / count;
            else if (entry.state == WV_STATE_THUNDER_SNOW) pressure = snow / count;
            break;
        default: break;
        }

        entry.weight *= 1.0f + _frontStrength * pressure;
    }
}

// ─────────────────────────────────────────────────────────────
// Scheduling & transition
// ─────────────────────────────────────────────────────────────

// Plan the way from one weather to another, as the client will actually show it.
//
// The client draws nothing at or below grade 0.25 (the floor) and has the sky fully stormy from
// there up, so the floor is where every kind of weather meets every other: a sky clouds over
// up to it, precipitation thins down to it.
//
//   same kind of thing on screen (two rains, two skies):
//       the grade moves straight across; nothing restarts. The state swaps halfway.
//   one kind to another:
//       the old one is eased to the floor, the new one builds from it.
//
// Rain and a sky are streamed in small steps. Snow and sandstorm cannot be: the client restarts
// them from nothing on every packet, so they get one packet with the final grade, and when
// they are what is being left, no packet at all — the client fades them by itself the moment
// it is told about anything else.
//
// A streamed leg is paced by the profile's Step.* keys: one step of time per StepSize.Perc of
// the range it crosses (a sky's range is the first quarter of the grade, precipitation's the
// rest), and never faster than the client's own 0.1 per second.
TransitionPlan WeatherVibeEngine::PlanTransition(Profile const& profile, WeatherState fromState, float fromRaw,
    WeatherState toState, float toRaw, bool mayStay) const
{
    TransitionSettings const& ts = profile.transition;
    TransitionPlan plan;

    Ground const here = GroundOf(fromState);
    Ground const there = GroundOf(toState);
    float const floor = CLIENT_PARTICLE_FLOOR;

    auto stream = [&](WeatherState state, float from, float to)
        {
            if (std::fabs(to - from) < GRADE_EPSILON)
                return;

            Leg& leg = plan.legs[plan.legCount++];
            leg.state = state;
            leg.from = from;
            leg.to = to;

            float const span = (GroundOf(state) == Ground::SKY) ? floor : (1.0f - floor);
            float const travelPct = std::fabs(to - from) / span * 100.0f;
            uint32 const steps = std::max(1u, uint32(std::ceil(travelPct / ts.stepSizePct)));
            for (uint32 i = 0; i < steps; ++i)
                leg.ms += urand(ts.stepTimeMinMs, ts.stepTimeMaxMs);

            // An eased leg moves half as fast again at its middle as on average.
            leg.ms = std::max(leg.ms, uint32(float(ClientFadeMs(from, to)) * 1.5f));
            plan.durationMs += leg.ms;
        };

    auto oneShot = [&](WeatherState state, float from, float to)
        {
            Leg& leg = plan.legs[plan.legCount++];
            leg.state = state;
            leg.from = from;
            leg.to = to;
            leg.oneShot = true;

            // The client's own doing: whatever was falling fades out, the effect is rebuilt,
            // the new grade fades in. Wait that long, and a little for the last particles.
            leg.ms = ClientFadeMs(std::max(from, floor), floor) + ClientFadeMs(floor, std::max(to, floor)) + 4000;
            plan.durationMs += leg.ms;
        };

    bool const sameGrade = std::fabs(toRaw - fromRaw) < GRADE_EPSILON;

    if (here == there)
    {
        if (IsOneShot(there))
        {
            // More of the same snow or sandstorm. A packet costs a visible restart, so one is
            // only spent on a change worth seeing or hearing: a profile's own re-pick of the
            // same state stays as it is, and so does a nudge within one sound loop.
            bool const sameLoop = sWeatherVibeCore.WireState(fromState, profile.skins, fromRaw)
                == sWeatherVibeCore.WireState(toState, profile.skins, toRaw);

            if (!sameGrade && mayStay && (fromState == toState || (sameLoop && std::fabs(toRaw - fromRaw) < RESTART_NOT_WORTH)))
                plan.keepGrade = true;
            else if (!sameGrade)
                oneShot(toState, fromRaw, toRaw);

            return plan;
        }

        if (fromState == toState)
            stream(toState, fromRaw, toRaw);
        else
        {
            float const middle = (fromRaw + toRaw) * 0.5f;
            stream(fromState, fromRaw, middle);
            stream(toState, middle, toRaw);
        }

        return plan;
    }

    // Leaving. (A sky only ever leaves for precipitation here, so "to the floor" is clouding over.)
    if (!IsOneShot(here))
        stream(fromState, fromRaw, floor);

    // Arriving.
    if (IsOneShot(there))
        oneShot(toState, IsOneShot(here) ? fromRaw : floor, toRaw);
    else
        stream(toState, floor, toRaw);

    return plan;
}

// The profile's interval between changes (~3600000/changesPerHr, +/-25%) is the whole budget:
// transition plus hold. What is left after the transition is the hold, never less than
// MinHoldSeconds, then stretched or shortened by the kind of weather it is.
uint32 WeatherVibeEngine::CalcHoldMs(Profile const& profile, WeatherState state, uint32 transitionMs) const
{
    TransitionSettings const& ts = profile.transition;

    float const interval = (3600000.0f / float(ts.changesPerHr)) * frand(0.75f, 1.25f) / _pace;
    float hold = std::max(float(ts.minHoldMs), interval - float(transitionMs));

    int const index = WeatherVibeCore::StateIndex(state);
    if (index >= 0)
        hold *= _holdMult[size_t(index)];

    return uint32(std::clamp(hold, 1000.0f, 24.0f * 3600000.0f));
}

void WeatherVibeEngine::MarkDirty(Profile& profile)
{
    profile.live.dirty = true;
    _anyDirty = true;
}

void WeatherVibeEngine::Begin(Profile& profile, WeatherState state, float pct, bool mayStay)
{
    LiveState& live = profile.live;
    DayPart const dp = sWeatherVibeCore.GetCurrentDayPart();

    // A GM asking for 86 gets this profile's kind of thunder, as its own rows do.
    state = ResolveThunder(profile, state);

    // The daypart's band is applied here, once: a hold does not shift when the clock crosses
    // into the next daypart.
    live.band = dp;
    live.targetState = state;
    live.targetPct = std::clamp(pct, 0.0f, 100.0f);
    live.targetRaw = sWeatherVibeCore.PercentToRaw(dp, state, live.targetPct);
    live.plan = PlanTransition(profile, live.state, live.raw, state, live.targetRaw, mayStay);

    if (live.plan.keepGrade)
    {
        // Snow or sandstorm that is close enough to what is already falling: leave it be.
        live.targetRaw = live.raw;
        live.targetPct = sWeatherVibeCore.RawToPercent(dp, state, live.raw);
    }

    live.holdMs = CalcHoldMs(profile, state, live.plan.durationMs);
    live.elapsedMs = 0;
    live.tickTimerMs = 0;

    if (!live.plan.durationMs)
        Settle(profile);
    else
        live.phase = Phase::TRANSITION;
}

void WeatherVibeEngine::Settle(Profile& profile)
{
    LiveState& live = profile.live;
    WeatherState const previous = live.settledState;

    // After a one-shot leg, or a pick that stays as it is, there is nothing new to show.
    if (live.state != live.targetState || std::fabs(live.raw - live.targetRaw) >= GRADE_EPSILON)
        MarkDirty(profile);

    live.phase = Phase::HOLD;
    live.state = live.targetState;
    live.raw = live.targetRaw;
    live.settledState = live.targetState;

    Debug(profile, Acore::StringFormat("settled on {} {:.0f}% (grade {:.2f}), {}",
        WeatherVibeCore::WeatherStateName(live.state), live.targetPct, live.raw,
        live.forced ? std::string("locked") : "holding " + Duration(live.holdMs)));

    sWeatherVibeWorld.OnSettled(profile, previous);
}

void WeatherVibeEngine::AdvanceTransition(Profile& profile, uint32 diff)
{
    LiveState& live = profile.live;

    live.elapsedMs += diff;
    if (live.elapsedMs >= live.plan.durationMs)
    {
        Settle(profile);
        return;
    }

    if (live.tickTimerMs > diff)
    {
        live.tickTimerMs -= diff;
        return;
    }
    live.tickTimerMs = _tickMs;

    WeatherState state = live.state;
    float raw = live.raw;
    Evaluate(live.plan, live.elapsedMs, state, raw);

    if (state != live.state || std::fabs(raw - live.raw) >= GRADE_EPSILON)
    {
        live.state = state;
        live.raw = raw;
        MarkDirty(profile);
    }
}

void WeatherVibeEngine::PickNext(Profile& profile)
{
    LiveState& live = profile.live;

    BuildTable(profile, _scratchTable, true);

    int const index = PickEntry(_scratchTable, live.settledState, profile.transition.maxConsecSame,
        live.consecSame, _rules);

    if (index < 0)
    {
        live.holdMs = 60000;   // nothing to pick from right now; look again in a minute
        return;
    }

    WeatherEntry const entry = _scratchTable[size_t(index)];
    WeatherState const from = live.settledState;

    live.consecSame = (entry.state == from) ? live.consecSame + 1 : 1;
    Begin(profile, entry.state, frand(entry.minPct, entry.maxPct), true);

    if (live.phase == Phase::TRANSITION)
        Debug(profile, Acore::StringFormat("{} -> {} {:.0f}% over {}{}, then holding {}",
            WeatherVibeCore::WeatherStateName(from), WeatherVibeCore::WeatherStateName(live.targetState),
            live.targetPct, Duration(live.plan.durationMs),
            live.plan.legs[live.plan.legCount - 1].oneShot ? " (sent once; the client fades it)" : "",
            Duration(live.holdMs)));
}

void WeatherVibeEngine::UpdateProfile(Profile& profile, uint32 diff)
{
    LiveState& live = profile.live;
    if (!live.started)
        return;

    if (uint32 const resync = sWeatherVibeWorld.GetResyncMs())
    {
        if (live.resyncTimerMs <= diff)
        {
            live.resyncTimerMs = resync;
            if (live.phase == Phase::HOLD)
                sWeatherVibeWorld.FireHook(profile);
        }
        else
            live.resyncTimerMs -= diff;
    }

    // A storm is on screen: the client only draws the rain, snow or dark sky, so the thunder
    // is ours to play. Claps come closer together as the storm builds.
    if (WeatherVibeCore::IsThunder(live.state) && sWeatherVibeWorld.IsThunderEnabled())
    {
        if (live.thunderTimerMs > diff)
            live.thunderTimerMs -= diff;
        else
        {
            float const intensity = sWeatherVibeCore.RawToPercent(live.band, live.state, live.raw) / 100.0f;

            // 0 means the storm has only just come up: wait part of a gap before the first one.
            if (live.thunderTimerMs)
                sWeatherVibeWorld.Thunder(profile, intensity);

            uint32 const gap = sWeatherVibeWorld.NextThunderDelayMs(intensity);
            live.thunderTimerMs = std::max(1u, live.thunderTimerMs ? gap : gap / 2);
        }
    }
    else
        live.thunderTimerMs = 0;

    // Under the claps, the long low rumble: a storm here, one on its way, or one next door.
    if (sWeatherVibeWorld.IsRumbleEnabled())
    {
        if (live.rumbleTimerMs > diff)
            live.rumbleTimerMs -= diff;
        else
        {
            live.rumbleTimerMs = RUMBLE_CHECK_MS;
            if (StormInEarshot(profile))
                sWeatherVibeWorld.Rumble(profile);
        }
    }

    if (live.forced && live.forcedTimed)
    {
        if (live.forcedMs <= diff)
        {
            live.forced = false;
            live.forcedTimed = false;
            live.forcedMs = 0;
            live.holdMs = 0;
            Debug(profile, "override ran out, back to the profile");
        }
        else
            live.forcedMs -= diff;
    }

    if (live.phase == Phase::TRANSITION)
    {
        AdvanceTransition(profile, diff);
        return;
    }

    if (live.forced || !_autoEngine)
        return;

    if (live.holdMs > diff)
    {
        live.holdMs -= diff;
        return;
    }

    PickNext(profile);
}

void WeatherVibeEngine::Update(uint32 diff)
{
    if (_profiles.empty())
        return;

    if (_reapplyMs)
    {
        if (_reapplyTimerMs <= diff)
        {
            _reapplyTimerMs = _reapplyMs;
            _reapplyDue = true;
        }
        else
            _reapplyTimerMs -= diff;
    }

    // Snow and sandstorm are sent once, so a client that missed that one packet would keep the
    // old weather until the next change. Every so often each client's record is held against
    // what it should be showing; nothing is sent to the ones that are right.
    if (_reconcileTimerMs <= diff)
    {
        _reconcileTimerMs = RECONCILE_MS;
        _reconcileDue = true;
    }
    else
        _reconcileTimerMs -= diff;

    for (Profile& profile : _profiles)
        UpdateProfile(profile, diff);

    FlushDirty();
}

// ─────────────────────────────────────────────────────────────
// Delivery
// ─────────────────────────────────────────────────────────────

// One pass over the connected clients for everything that changed this tick. Bot sessions are
// never registered with the session manager, so they are not walked at all, and a player is
// found wherever they are — continent, battleground or raid.
void WeatherVibeEngine::FlushDirty()
{
    if (!_anyDirty && !_reapplyDue && !_reconcileDue)
        return;

    bool const everyone = _reapplyDue || _reconcileDue;
    Tell const unchanged = _reapplyDue ? Tell::REPEAT : Tell::ARRIVAL;

    for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
    {
        // A player between maps is told on arrival, by the zone hook.
        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld() || session->IsSocketClosed() || player->IsBeingTeleportedFar())
            continue;

        if (sWeatherVibeCore.IsAuditioning(player->GetGUID()))
            continue;

        Profile const* profile = Resolve(player->GetZoneId(), player->GetAreaId());
        if (profile && (everyone || profile->live.dirty))
            WeatherVibeCore::Deliver(player, WireOf(*profile), profile->live.raw,
                profile->live.dirty ? Tell::CHANGE : unchanged);
    }

    for (Profile& profile : _profiles)
        profile.live.dirty = false;

    _anyDirty = false;
    _reapplyDue = false;
    _reconcileDue = false;
}

// An area's own profile beats its zone's.
Profile const* WeatherVibeEngine::Resolve(uint32 zoneId, uint32 areaId) const
{
    if (!_areaToProfile.empty())
    {
        auto it = _areaToProfile.find(areaId);
        if (it != _areaToProfile.end() && _profiles[it->second].live.started)
            return &_profiles[it->second];
    }

    auto it = _zoneToProfile.find(zoneId);
    if (it != _zoneToProfile.end() && _profiles[it->second].live.started)
        return &_profiles[it->second];

    return nullptr;
}

void WeatherVibeEngine::SendCurrentTo(Player* player, uint32 zoneId, uint32 areaId, bool enteredZone) const
{
    // A GM looking at a weather id with `.wvibe audition` keeps it until they switch it off.
    if (sWeatherVibeCore.IsAuditioning(player->GetGUID()))
        return;

    if (Profile const* profile = Resolve(zoneId, areaId))
    {
        // With the core's weather left on, it has just sent this client a packet of its own:
        // whatever the module said last no longer stands.
        if (enteredZone && sWeatherVibeCore.CoreWeatherIsOn())
            WeatherVibeCore::Forget(player);

        // Walking from one zone into another on the same profile sends nothing new to a client
        // in snow, which is the point: a packet would make the snow fade out and come back.
        WeatherVibeCore::Deliver(player, WireOf(*profile), profile->live.raw, enteredZone ? Tell::REPEAT : Tell::ARRIVAL);
        return;
    }

    // Not a place the module runs. The client keeps showing the last zone's weather until told
    // otherwise, so clear it — then let the core repeat any weather a script set for this zone
    // (a boss storm, an event), which that clear would otherwise have wiped. What the client
    // shows now is no longer the module's to know.
    WeatherVibeCore::SendWeather(player, WEATHER_STATE_FINE, 0.0f);

    if (Map* map = player->FindMap())
        map->SendZoneWeather(zoneId, player);

    WeatherVibeCore::Forget(player);
}

void WeatherVibeEngine::OnAreaChange(Player* player, uint32 oldArea, uint32 newArea) const
{
    if (_areaToProfile.empty())
        return;

    // On a zone change the area hook fires first; the zone hook that follows does the sending.
    uint32 const zoneId = player->GetZoneId();
    if (ZoneOfArea(oldArea) != zoneId)
        return;

    if (Resolve(zoneId, oldArea) != Resolve(zoneId, newArea))
        SendCurrentTo(player, zoneId, newArea);
}

bool WeatherVibeEngine::StormInEarshot(Profile const& profile) const
{
    auto storm = [](LiveState const& live)
        {
            return live.started && (WeatherVibeCore::IsThunder(live.state) || WeatherVibeCore::IsThunder(live.targetState));
        };

    if (storm(profile.live))
        return true;

    if (sWeatherVibeWorld.RumbleFromNeighbors())
        for (uint16 index : profile.neighbors)
            if (storm(_profiles[index].live))
                return true;

    return false;
}

void WeatherVibeEngine::ForEachListener(Profile const& profile, std::function<void(Player*)> const& visit) const
{
    for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld() || session->IsSocketClosed() || player->IsBeingTeleported())
            continue;

        if (Resolve(player->GetZoneId(), player->GetAreaId()) == &profile)
            visit(player);
    }
}

uint32 WeatherVibeEngine::PushToZone(uint32 zoneId, WeatherState state, float rawGrade) const
{
    uint32 count = 0;

    for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld() || session->IsSocketClosed() || player->GetZoneId() != zoneId)
            continue;

        WeatherVibeCore::SendWeather(player, sWeatherVibeCore.WireState(state, WeatherSkins{}, rawGrade), rawGrade);
        WeatherVibeCore::Forget(player);
        ++count;
    }

    return count;
}

// ─────────────────────────────────────────────────────────────
// Commands
// ─────────────────────────────────────────────────────────────

Profile* WeatherVibeEngine::FindProfile(std::string const& name)
{
    std::string const wanted = ToLower(Trim(name));

    for (Profile& profile : _profiles)
        if (ToLower(profile.name) == wanted)
            return &profile;

    return nullptr;
}

Profile* WeatherVibeEngine::FindProfileFor(uint32 zoneId, uint32 areaId)
{
    return const_cast<Profile*>(Resolve(zoneId, areaId));
}

void WeatherVibeEngine::Steer(Profile& profile, WeatherState state, float pct)
{
    LiveState& live = profile.live;
    live.started = true;
    live.forced = false;
    live.forcedTimed = false;
    live.forcedMs = 0;
    live.consecSame = 1;

    Begin(profile, state, pct, false);
}

void WeatherVibeEngine::Force(Profile& profile, WeatherState state, float pct, uint32 minutes)
{
    LiveState& live = profile.live;
    live.started = true;
    live.forced = true;
    live.forcedTimed = minutes > 0;
    live.forcedMs = minutes * 60000;
    live.consecSame = 1;

    Begin(profile, state, pct, false);
    live.holdMs = 0;   // once released, the profile picks again straight away
}

void WeatherVibeEngine::ForceRaw(Profile& profile, WeatherState state, float rawGrade)
{
    LiveState& live = profile.live;
    live.started = true;
    live.forced = true;
    live.forcedTimed = false;
    live.forcedMs = 0;

    state = ResolveThunder(profile, state);

    live.phase = Phase::HOLD;
    live.band = sWeatherVibeCore.GetCurrentDayPart();
    live.state = live.targetState = live.settledState = state;
    live.raw = live.targetRaw = WeatherVibeCore::ClampRaw(state, rawGrade);
    live.targetPct = sWeatherVibeCore.RawToPercent(live.band, state, live.raw);
    live.holdMs = 0;
    live.consecSame = 1;
    MarkDirty(profile);
}

void WeatherVibeEngine::Resume(Profile& profile)
{
    LiveState& live = profile.live;
    live.forced = false;
    live.forcedTimed = false;
    live.forcedMs = 0;

    if (live.phase == Phase::HOLD)
        live.holdMs = 0;
}

void WeatherVibeEngine::SkipHold(Profile& profile)
{
    Resume(profile);
    profile.live.holdMs = 0;   // mid-transition: no hold once it lands
}

std::string WeatherVibeEngine::Describe(Profile const& profile) const
{
    LiveState const& live = profile.live;
    if (!live.started)
        return profile.name + ": not running (no usable weather entries)";

    std::string out = Acore::StringFormat("{}: {} {:.0f}%", profile.name,
        WeatherVibeCore::WeatherStateName(live.state),
        sWeatherVibeCore.RawToPercent(live.band, live.state, live.raw));

    if (live.phase == Phase::TRANSITION)
        out += Acore::StringFormat(" -> {} {:.0f}%, {} to go",
            WeatherVibeCore::WeatherStateName(live.targetState), live.targetPct,
            Duration(live.plan.durationMs - std::min(live.elapsedMs, live.plan.durationMs)));
    else if (!live.forced && _autoEngine)
        out += ", next change in " + Duration(live.holdMs);

    if (live.forced)
        out += live.forcedTimed ? " [locked for " + Duration(live.forcedMs) + "]" : " [locked until .wvibe resume]";
    else if (!_autoEngine)
        out += " [profiles are off: manual only]";

    // What that is to the client: the id picks the sound, the grade what is drawn.
    WeatherState const wire = WireOf(profile);
    out += Acore::StringFormat(" | client: id {} at grade {:.2f}, ", uint32(wire), live.raw);

    if (GroundOf(live.state) == Ground::SKY)
        out += Acore::StringFormat("sky {:.0f}% stormy", std::min(live.raw, CLIENT_PARTICLE_FLOOR) / CLIENT_PARTICLE_FLOOR * 100.0f);
    else if (wire == WEATHER_STATE_FINE)
        out += "stormy sky, too little falling to send";
    else
        out += Acore::StringFormat("{:.0f}% of full density", Density(live.raw) * 100.0f);

    return out;
}

std::array<float, WV_STATES> WeatherVibeEngine::Simulate(Profile const& profile, uint32 hours) const
{
    std::array<double, WV_STATES> spent{};
    std::array<float, WV_STATES> share{};

    DayPart const dp = sWeatherVibeCore.GetCurrentDayPart();
    TransitionSettings const& ts = profile.transition;

    // The calendar, the clock and the neighbours stand still for a simulation.
    WeatherTable table;
    BuildTable(profile, table, true);

    WeatherState state = profile.live.settledState;
    float raw = profile.live.targetRaw;
    uint32 consec = 1;

    double const total = double(hours) * 3600000.0;
    double elapsed = 0.0;

    while (elapsed < total)
    {
        int const index = PickEntry(table, state, ts.maxConsecSame, consec, _rules);
        if (index < 0)
            break;

        WeatherEntry const& entry = table[size_t(index)];
        float const pct = frand(entry.minPct, entry.maxPct);
        float const toRaw = sWeatherVibeCore.PercentToRaw(dp, entry.state, pct);

        TransitionPlan const plan = PlanTransition(profile, state, raw, entry.state, toRaw, true);
        uint32 const hold = CalcHoldMs(profile, entry.state, plan.durationMs);

        // Each stretch of the way there counts for the state shown during it.
        for (uint8 i = 0; i < plan.legCount; ++i)
        {
            int const legIndex = WeatherVibeCore::StateIndex(plan.legs[i].state);
            if (legIndex >= 0)
                spent[size_t(legIndex)] += plan.legs[i].ms;
        }

        int const toIndex = WeatherVibeCore::StateIndex(entry.state);
        if (toIndex >= 0)
            spent[size_t(toIndex)] += hold;

        consec = (entry.state == state) ? consec + 1 : 1;
        state = entry.state;
        if (!plan.keepGrade)
            raw = toRaw;
        elapsed += double(plan.durationMs) + double(hold);
    }

    if (elapsed > 0.0)
        for (size_t i = 0; i < WV_STATES; ++i)
            share[i] = float(spent[i] / elapsed);

    return share;
}

// ─────────────────────────────────────────────────────────────
// Debug feed
// ─────────────────────────────────────────────────────────────

// WeatherVibe.Debug writes the engine's decisions to the server log. A GM who ran
// `.wvibe debug on` also sees them in chat, for the weather they are standing in. Nobody else does.
void WeatherVibeEngine::Debug(Profile const& profile, std::string const& line) const
{
    if (sWeatherVibeCore.IsDebug())
        LOG_INFO("module.weathervibe", "[WeatherVibe] {}: {}", profile.name, line);

    auto const& subscribers = sWeatherVibeCore.GetDebugSubscribers();
    if (subscribers.empty())
        return;

    std::string const text = "|cff00ff00WeatherVibe:|r " + profile.name + ": " + line;

    for (ObjectGuid guid : subscribers)
    {
        Player* gm = ObjectAccessor::FindConnectedPlayer(guid);
        if (!gm || !gm->IsInWorld())
            continue;

        if (Resolve(gm->GetZoneId(), gm->GetAreaId()) == &profile)
            ChatHandler(gm->GetSession()).SendSysMessage(text);
    }
}
