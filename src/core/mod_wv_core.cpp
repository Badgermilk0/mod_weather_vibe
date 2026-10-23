#include "mod_wv_core.h"
#include "mod_wv_pick.h"

#include "Config.h"
#include "DataMap.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Log.h"
#include "Opcodes.h"
#include "Player.h"
#include "StringConvert.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

// ============================================================
// State table — order is the array index used everywhere
// ============================================================
namespace
{
    struct StateInfo
    {
        WeatherState state;
        char const*  name;           // config token and display name
        Range        defaultRange;   // used when the conf has no InternalRange for it
    };

    constexpr std::array<StateInfo, WV_STATES> kStates =
    { {
        // Sky only: the client's sky is fully stormy at grade 0.25 and nothing above that
        // changes it, so these bands live inside 0..0.25. Fine tops out at half the storm sky.
        { WEATHER_STATE_FINE,             "Fine",            { 0.00f, 0.125f } },
        { WEATHER_STATE_FOG,              "Fog",             { 0.10f, 0.25f } },
        // Precipitation: nothing falls at or below 0.25, so every band starts above it, and
        // the tiers climb with a little overlap so that heavy is always heavier than medium.
        { WEATHER_STATE_LIGHT_RAIN,       "LightRain",       { 0.30f, 0.50f } },
        { WEATHER_STATE_MEDIUM_RAIN,      "MediumRain",      { 0.45f, 0.70f } },
        { WEATHER_STATE_HEAVY_RAIN,       "HeavyRain",       { 0.62f, 0.95f } },
        { WEATHER_STATE_LIGHT_SNOW,       "LightSnow",       { 0.32f, 0.52f } },   // snow is sparse:
        { WEATHER_STATE_MEDIUM_SNOW,      "MediumSnow",      { 0.48f, 0.74f } },   // a fifth of rain's
        { WEATHER_STATE_HEAVY_SNOW,       "HeavySnow",       { 0.66f, 1.00f } },   // particle count
        { WEATHER_STATE_LIGHT_SANDSTORM,  "LightSandstorm",  { 0.30f, 0.50f } },
        { WEATHER_STATE_MEDIUM_SANDSTORM, "MediumSandstorm", { 0.45f, 0.70f } },
        { WEATHER_STATE_HEAVY_SANDSTORM,  "HeavySandstorm",  { 0.62f, 0.95f } },
        { WEATHER_STATE_THUNDERS,         "Thunders",        { 0.60f, 0.95f } },   // a downpour
        { WV_STATE_THUNDER_SNOW,          "ThunderSnow",     { 0.66f, 1.00f } },
        { WV_STATE_DRY_THUNDERS,          "DryThunders",     { 0.20f, 0.25f } },   // how dark the sky gets
        // The ends of the scale: not a wisp of cloud, and the worst a place gets.
        { WV_STATE_CLEAR,                 "Clear",           { 0.00f, 0.02f } },
        { WV_STATE_TEMPEST,               "Tempest",         { 0.88f, 1.00f } },
        { WV_STATE_BLIZZARD,              "Blizzard",        { 0.86f, 1.00f } }
    } };

    // The look of each state on a Waystone client, over its band (WeatherVibe.Look.*), in
    // kStates order. Density multiplies the particles; storm and lightning only mean something
    // for the states with thunder; duck below 0 is not sent.
    struct LookDefault
    {
        Range density;
        Range storm;
        Range lightning;   // strikes per minute
        float duck;
    };

    constexpr std::array<LookDefault, WV_STATES> kLook =
    { {
        { { 1.00f, 1.00f }, { 0.00f, 0.00f }, {  0.0f,  0.0f }, -1.0f },   // Fine
        { { 1.00f, 1.00f }, { 0.00f, 0.00f }, {  0.0f,  0.0f }, -1.0f },   // Fog
        { { 1.00f, 1.00f }, { 0.00f, 0.00f }, {  0.0f,  0.0f }, -1.0f },   // LightRain
        { { 1.00f, 1.00f }, { 0.00f, 0.00f }, {  0.0f,  0.0f }, -1.0f },   // MediumRain
        { { 1.00f, 1.15f }, { 0.00f, 0.00f }, {  0.0f,  0.0f }, -1.0f },   // HeavyRain
        { { 1.00f, 1.00f }, { 0.00f, 0.00f }, {  0.0f,  0.0f }, -1.0f },   // LightSnow
        { { 1.00f, 1.10f }, { 0.00f, 0.00f }, {  0.0f,  0.0f }, -1.0f },   // MediumSnow
        { { 1.05f, 1.30f }, { 0.00f, 0.00f }, {  0.0f,  0.0f }, -1.0f },   // HeavySnow
        { { 1.00f, 1.00f }, { 0.00f, 0.00f }, {  0.0f,  0.0f }, -1.0f },   // LightSandstorm
        { { 1.00f, 1.15f }, { 0.00f, 0.00f }, {  0.0f,  0.0f }, -1.0f },   // MediumSandstorm
        { { 1.10f, 1.50f }, { 0.00f, 0.00f }, {  0.0f,  0.0f }, 0.75f },   // HeavySandstorm
        { { 1.05f, 1.25f }, { 0.35f, 0.80f }, {  2.0f,  6.0f }, -1.0f },   // Thunders
        { { 1.10f, 1.35f }, { 0.15f, 0.40f }, {  0.4f,  1.5f }, -1.0f },   // ThunderSnow
        { { 1.00f, 1.00f }, { 0.25f, 0.60f }, {  1.5f,  4.0f }, -1.0f },   // DryThunders
        { { 1.00f, 1.00f }, { 0.00f, 0.00f }, {  0.0f,  0.0f }, -1.0f },   // Clear
        { { 1.40f, 2.00f }, { 0.85f, 1.00f }, {  9.0f, 16.0f }, 0.85f },   // Tempest
        { { 1.50f, 2.00f }, { 0.00f, 0.00f }, {  0.0f,  0.0f }, 0.80f }    // Blizzard
    } };

    // The last thing a client was told, kept on the player (see WeatherVibeCore::Deliver).
    // Not valid: something else may have spoken to the client since; send whatever comes next.
    struct SentWeather : public DataMap::Base
    {
        WeatherState           wireId = WEATHER_STATE_FINE;
        float                  grade = 0.0f;
        WeatherVibe::ClientExt ext;
        bool                   valid = true;
    };

    char const* const SENT_KEY = "WeatherVibe.Sent";

    // Weather.dbc rows the core has no name for.
    constexpr WeatherState CLIENT_WEATHER_SILENT_RAIN = WeatherState(2);   // rain, the default drop, no sound
    constexpr WeatherState CLIENT_WEATHER_SILENT_SNOW = WeatherState(61);  // snow, the default flake, no sound
    constexpr WeatherState CLIENT_WEATHER_DARK_RAIN = WeatherState(90);    // rain motion, leaf-brown streaks, no sound
    constexpr WeatherState CLIENT_WEATHER_RED_RAIN = WeatherState(100);    // rain motion, red drops
    constexpr WeatherState CLIENT_WEATHER_EMBER = WeatherState(103);       // sandstorm motion, orange
    constexpr WeatherState CLIENT_WEATHER_ASH = WeatherState(104);         // sandstorm motion, black mist, no sound

    // Rain drawn with the default drop.
    bool IsPlainRain(WeatherState wireId)
    {
        return wireId == CLIENT_WEATHER_SILENT_RAIN || (wireId >= WEATHER_STATE_LIGHT_RAIN && wireId <= WEATHER_STATE_HEAVY_RAIN);
    }

    // Keys that carry an id or a colour, compared exactly.
    bool IsExactKey(uint32 key)
    {
        using namespace WeatherVibe;
        return key == EXT_SOUND_KIT || key == EXT_RUMBLE_KIT || key == EXT_NEAR_KIT || key == EXT_FAR_KIT || key == EXT_FLASH_COLOR;
    }
}

bool WeatherVibe::ClientExt::SameAs(ClientExt const& other) const
{
    uint32 const ignored = 1u << EXT_TRANSITION;
    if ((mask & ~ignored) != (other.mask & ~ignored))
        return false;

    for (uint32 key = 0; key < EXT_KEY_COUNT; ++key)
    {
        if (key == EXT_TRANSITION || !(mask & (1u << key)))
            continue;

        float const a = values[key];
        float const b = other.values[key];
        if (IsExactKey(key) ? a != b : std::fabs(a - b) > 0.005f + 0.01f * std::fabs(a))
            return false;
    }

    return true;
}

// ============================================================
// Text + config helpers
// ============================================================
namespace WeatherVibe
{
    std::string Trim(std::string const& s)
    {
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
    }

    std::string StripComment(std::string const& s)
    {
        size_t hash = s.find('#');
        return Trim(hash == std::string::npos ? s : s.substr(0, hash));
    }

    std::vector<std::string> Split(std::string const& s, char sep)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= s.size())
        {
            size_t end = s.find(sep, start);
            if (end == std::string::npos)
                end = s.size();
            out.push_back(Trim(s.substr(start, end - start)));
            start = end + 1;
        }
        return out;
    }

    std::string ToLower(std::string_view s)
    {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return out;
    }

    bool ParseFloat(std::string const& s, float& out)
    {
        Optional<float> v = Acore::StringTo<float>(s);
        if (!v)
            return false;
        out = *v;
        return true;
    }

    bool ParseUInt(std::string const& s, uint32& out)
    {
        Optional<uint32> v = Acore::StringTo<uint32>(s);
        if (!v)
            return false;
        out = *v;
        return true;
    }

    ConfigView::ConfigView()
    {
        _keys = sConfigMgr->GetKeysByString("WeatherVibe");
        std::sort(_keys.begin(), _keys.end());
    }

    bool ConfigView::Has(std::string const& key) const
    {
        return std::binary_search(_keys.begin(), _keys.end(), key);
    }

    std::vector<std::string> ConfigView::KeysWithPrefix(std::string const& prefix) const
    {
        std::vector<std::string> out;
        for (auto it = std::lower_bound(_keys.begin(), _keys.end(), prefix); it != _keys.end(); ++it)
        {
            if (it->compare(0, prefix.size(), prefix) != 0)
                break;
            out.push_back(*it);
        }
        return out;
    }

    std::string ConfigView::RawText(std::string const& key, std::string const& def) const
    {
        if (!Has(key))
            return def;
        return Trim(sConfigMgr->GetOption<std::string>(key, def, false));
    }

    std::string ConfigView::Text(std::string const& key, std::string const& def) const
    {
        if (!Has(key))
            return def;
        return StripComment(sConfigMgr->GetOption<std::string>(key, def, false));
    }

    float ConfigView::Float(std::string const& key, float def) const
    {
        float v = def;
        std::string text = Text(key);
        if (!text.empty() && !ParseFloat(text, v))
            LOG_WARN("server.loading", "[WeatherVibe] '{} = {}' is not a number, using {}", key, text, def);
        return v;
    }

    uint32 ConfigView::UInt(std::string const& key, uint32 def) const
    {
        uint32 v = def;
        std::string text = Text(key);
        if (!text.empty() && !ParseUInt(text, v))
            LOG_WARN("server.loading", "[WeatherVibe] '{} = {}' is not a whole number, using {}", key, text, def);
        return v;
    }

    bool ConfigView::Bool(std::string const& key, bool def) const
    {
        std::string text = ToLower(Text(key));
        if (text.empty())
            return def;
        return text == "1" || text == "true" || text == "yes" || text == "on";
    }

    std::vector<std::pair<uint32, std::string>> ConfigView::Numbered(std::string const& base, bool stripComments) const
    {
        std::string const prefix = base + ".";
        std::vector<std::pair<uint32, std::string>> out;

        for (std::string const& key : KeysWithPrefix(prefix))
        {
            uint32 index = 0;
            if (!ParseUInt(key.substr(prefix.size()), index) || index == 0)
                continue;   // a deeper key such as "<base>.Transition.X"

            std::string value = stripComments ? Text(key) : RawText(key);
            if (!value.empty())
                out.emplace_back(index, std::move(value));
        }

        std::sort(out.begin(), out.end());
        return out;
    }
}

// ============================================================
// Singleton accessor
// ============================================================
WeatherVibeCore& WeatherVibeCore::Instance()
{
    static WeatherVibeCore instance;
    return instance;
}

// ============================================================
// Config loading
// ============================================================
void WeatherVibeCore::LoadConfig(WeatherVibe::ConfigView const& cfg)
{
    m_enabled = cfg.Bool("WeatherVibe.Enable", true);
    m_announce = cfg.Bool("WeatherVibe.Announce", false);
    m_debug = cfg.Bool("WeatherVibe.Debug", false);
    m_profileEnabled = cfg.Bool("WeatherVibe.Profile.Enable", true);
    m_playersMayRead = cfg.UInt("WeatherVibeCommand", 0) == 1;

    if (!m_enabled)
    {
        LOG_INFO("server.loading", "[WeatherVibe] disabled by config");
        return;
    }

    m_coreWeatherOn = sWorld->getBoolConfig(CONFIG_WEATHER);
    if (m_coreWeatherOn)
        LOG_ERROR("module", "WeatherVibe: ActivateWeather is enabled in worldserver.conf, but WeatherVibe is also enabled. This will cause conflicts. Please set ActivateWeather = 0 to let WeatherVibe control weather.");

    // Which sound loop goes with how much is falling. The first is also the grade below
    // which precipitation is sent as plain sky: too thin to see, so it must not be heard.
    m_loopLightFrom = std::clamp(cfg.Float("WeatherVibe.Sound.From.Light", 0.29f), WeatherVibe::CLIENT_PARTICLE_FLOOR, 1.0f);
    m_loopMediumFrom = std::clamp(cfg.Float("WeatherVibe.Sound.From.Medium", 0.50f), m_loopLightFrom, 1.0f);
    m_loopHeavyFrom = std::clamp(cfg.Float("WeatherVibe.Sound.From.Heavy", 0.68f), m_loopMediumFrom, 1.0f);
    // Below the first loop, the silent rows: a drizzle or a few flakes that can be seen.
    m_silentDrizzle = cfg.Bool("WeatherVibe.Sound.SilentDrizzle", true);

    // The Waystone client's extension block, and who makes the thunder.
    m_extension = cfg.Bool("WeatherVibe.Client.Extension", true);

    std::string const source = WeatherVibe::ToLower(cfg.Text("WeatherVibe.Client.Thunder", "client"));
    if (source == "server")    m_thunderSource = ThunderSource::SERVER;
    else if (source == "both") m_thunderSource = ThunderSource::BOTH;
    else
    {
        m_thunderSource = ThunderSource::CLIENT;
        if (source != "client")
            LOG_WARN("server.loading", "[WeatherVibe] 'WeatherVibe.Client.Thunder = {}' is not client, server or both; using client", source);
    }

    // Without the block no client can be asked for lightning: the server plays the thunder.
    if (!m_extension)
        m_thunderSource = ThunderSource::SERVER;

    LoadDayPartConfig(cfg);
    LoadIntensityRangesConfig(cfg);
    LoadLookConfig(cfg);
}

void WeatherVibeCore::LoadLookConfig(WeatherVibe::ConfigView const& cfg)
{
    auto pair = [&](std::string const& key, float& a, float& b)
        {
            std::string const text = cfg.Text(key);
            if (text.empty())
                return;

            std::vector<std::string> parts = WeatherVibe::Split(text, ',');
            float x = 0.0f, y = 0.0f;
            if (parts.size() == 2 && WeatherVibe::ParseFloat(parts[0], x) && WeatherVibe::ParseFloat(parts[1], y))
            {
                a = x;
                b = y;
            }
            else
                LOG_WARN("server.loading", "[WeatherVibe] '{} = {}' is not '<a>, <b>', keeping the default", key, text);
        };

    auto curve = [&](char const* ground, SkyCurve def)
        {
            SkyCurve c = def;
            pair(std::string("WeatherVibe.Look.Sky.") + ground, c.first, c.fullAt);
            c.first = std::clamp(c.first, 0.0f, 1.0f);
            c.fullAt = std::clamp(c.fullAt, 0.05f, 1.0f);
            return c;
        };

    m_skyRain = curve("Rain", { 0.50f, 0.60f });
    m_skySnow = curve("Snow", { 0.40f, 0.75f });
    m_skySand = curve("Sandstorm", { 0.35f, 0.90f });

    for (size_t i = 0; i < WV_STATES; ++i)
    {
        std::string const base = std::string("WeatherVibe.Look.") + kStates[i].name + ".";
        LookDefault const& def = kLook[i];

        Range density = def.density, storm = def.storm, lightning = def.lightning;
        pair(base + "Density", density.min, density.max);
        pair(base + "Storm", storm.min, storm.max);
        pair(base + "Lightning", lightning.min, lightning.max);

        m_lookDensity[i] = { std::clamp(density.min, 0.0f, 2.0f), std::clamp(density.max, 0.0f, 2.0f) };
        m_lookStorm[i] = { std::clamp(storm.min, 0.0f, 1.0f), std::clamp(storm.max, 0.0f, 1.0f) };
        m_lookLightning[i] = { std::clamp(lightning.min, 0.0f, 60.0f), std::clamp(lightning.max, 0.0f, 60.0f) };

        float const duck = cfg.Float(base + "Duck", def.duck);
        m_lookDuck[i] = duck < 0.0f ? -1.0f : std::min(duck, 1.0f);
    }

    m_distantLightning = std::clamp(cfg.Float("WeatherVibe.Look.DistantStorm.Lightning", 0.8f), 0.0f, 10.0f);
    m_distantRumble = std::clamp(cfg.Float("WeatherVibe.Look.DistantStorm.Rumble", 0.35f), 0.0f, 1.0f);
    m_distantNeighbors = cfg.Bool("WeatherVibe.Look.DistantStorm.Neighbors", true);
}

void WeatherVibeCore::LoadDayPartConfig(WeatherVibe::ConfigView const& cfg)
{
    // Resolve daypart mode once — avoid per-tick string comparison
    {
        std::string mode = WeatherVibe::ToLower(cfg.Text("WeatherVibe.DayPart.Mode", "auto"));

        m_dayPartAuto = true;
        if (mode == "morning") { m_dayPartAuto = false; m_dayPartFixed = DayPart::MORNING; }
        else if (mode == "afternoon") { m_dayPartAuto = false; m_dayPartFixed = DayPart::AFTERNOON; }
        else if (mode == "evening") { m_dayPartAuto = false; m_dayPartFixed = DayPart::EVENING; }
        else if (mode == "night") { m_dayPartAuto = false; m_dayPartFixed = DayPart::NIGHT; }
    }

    // Resolve season mode once
    {
        std::string mode = WeatherVibe::ToLower(cfg.Text("WeatherVibe.Season", "auto"));

        m_seasonAuto = true;
        if (mode == "spring") { m_seasonAuto = false; m_seasonFixed = Season::SPRING; }
        else if (mode == "summer") { m_seasonAuto = false; m_seasonFixed = Season::SUMMER; }
        else if (mode == "autumn") { m_seasonAuto = false; m_seasonFixed = Season::AUTUMN; }
        else if (mode == "winter") { m_seasonAuto = false; m_seasonFixed = Season::WINTER; }
    }

    m_seasonBlendDays = std::clamp(cfg.Float("WeatherVibe.Season.BlendDays", 30.0f), 0.0f, 45.0f);

    m_starts.morning = ParseHHMM(cfg.Text("WeatherVibe.DayPart.MORNING.Start", "06:00"), 6 * 60);
    m_starts.afternoon = ParseHHMM(cfg.Text("WeatherVibe.DayPart.AFTERNOON.Start", "12:00"), 12 * 60);
    m_starts.evening = ParseHHMM(cfg.Text("WeatherVibe.DayPart.EVENING.Start", "18:00"), 18 * 60);
    m_starts.night = ParseHHMM(cfg.Text("WeatherVibe.DayPart.NIGHT.Start", "22:00"), 22 * 60);

    ValidateDayPartStarts();
}

void WeatherVibeCore::LoadIntensityRangesConfig(WeatherVibe::ConfigView const& cfg)
{
    for (size_t dp = 0; dp < WV_DAYPARTS; ++dp)
    {
        for (size_t i = 0; i < WV_STATES; ++i)
        {
            Range range = kStates[i].defaultRange;

            std::string key = std::string("WeatherVibe.Intensity.InternalRange.")
                + DayPartTokenUpper(DayPart(dp)) + "." + kStates[i].name;
            std::string text = cfg.Text(key);

            if (!text.empty())
            {
                std::vector<std::string> parts = WeatherVibe::Split(text, ',');
                float a = 0.0f, b = 0.0f;
                if (parts.size() == 2 && WeatherVibe::ParseFloat(parts[0], a) && WeatherVibe::ParseFloat(parts[1], b))
                {
                    if (b < a) { std::swap(a, b); }
                    range = { std::clamp(a, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f) };
                }
                else
                    LOG_WARN("server.loading", "[WeatherVibe] '{} = {}' is not '<min>, <max>', keeping the default", key, text);
            }

            m_stateRanges[dp][i] = range;

            // The client's two thresholds make some of a band pointless; say so once per state.
            if (dp != 0)
                continue;

            // With the silent rows, thin rain and snow are drawn from just above the floor;
            // without them (and for sandstorm, which has none) only from the first loop.
            WeatherVibe::Ground const ground = WeatherVibe::GroundOf(kStates[i].state);
            bool const skyOnly = ground == WeatherVibe::Ground::SKY;
            bool const silentRow = m_silentDrizzle && (ground == WeatherVibe::Ground::RAIN || ground == WeatherVibe::Ground::SNOW);
            float const visibleFrom = silentRow ? WeatherVibe::CLIENT_PARTICLE_FLOOR + 0.01f : m_loopLightFrom;

            if (skyOnly && range.max > WeatherVibe::CLIENT_PARTICLE_FLOOR + 0.001f)
                LOG_WARN("server.loading", "[WeatherVibe] {} is only a sky, and the sky is fully stormy at grade 0.25: "
                    "the part of its range above 0.25 ({:.2f}) changes nothing", kStates[i].name, range.max);
            else if (!skyOnly && range.min < visibleFrom - 0.001f)
                LOG_WARN("server.loading", "[WeatherVibe] {} starts at grade {:.2f}, but nothing is drawn for it below {:.2f}: "
                    "its low percentages will show no weather", kStates[i].name, range.min, visibleFrom);
        }
    }
}

// ============================================================
// Time helpers
// ============================================================
tm WeatherVibeCore::GetLocalTimeSafe()
{
    time_t now = GameTime::GetGameTime().count();
    tm out{};
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&out, &now);
#else
    localtime_r(&now, &out);
#endif
    return out;
}

int WeatherVibeCore::ParseHHMM(std::string const& s, int defMinutes)
{
    int h = 0, m = 0; char colon = 0;
    std::string t = s;
    t.erase(std::remove_if(t.begin(), t.end(), [](unsigned char c) { return std::isspace(c); }), t.end());

    if (std::sscanf(t.c_str(), "%d%c%d", &h, &colon, &m) == 3 && colon == ':' && h >= 0 && h < 24 && m >= 0 && m < 60)
        return h * 60 + m;

    if (std::sscanf(t.c_str(), "%d", &h) == 1 && h >= 0 && h < 24)
        return h * 60;

    return defMinutes;
}

void WeatherVibeCore::ValidateDayPartStarts()
{
    auto clampMinutes = [](int v) { return std::clamp(v, 0, 23 * 60 + 59); };

    m_starts.morning = clampMinutes(m_starts.morning);
    m_starts.afternoon = std::max(clampMinutes(m_starts.afternoon), m_starts.morning + 1);
    m_starts.evening = std::max(clampMinutes(m_starts.evening), m_starts.afternoon + 1);
    m_starts.night = std::max(clampMinutes(m_starts.night), m_starts.evening + 1);
}

// ============================================================
// Day/Season resolution
// ============================================================
DayPart WeatherVibeCore::GetCurrentDayPart() const
{
    if (!m_dayPartAuto) { return m_dayPartFixed; }

    tm  lt = GetLocalTimeSafe();
    int minutes = lt.tm_hour * 60 + lt.tm_min;

    if (minutes >= m_starts.night || minutes < m_starts.morning) { return DayPart::NIGHT; }
    if (minutes >= m_starts.evening) { return DayPart::EVENING; }
    if (minutes >= m_starts.afternoon) { return DayPart::AFTERNOON; }

    return DayPart::MORNING;
}

SeasonBlend WeatherVibeCore::GetSeasonBlend() const
{
    SeasonBlend out;

    if (!m_seasonAuto)
    {
        out.primary = out.secondary = m_seasonFixed;
        return out;
    }

    // Day 78 of the year (0-based) is 20 March, the vernal equinox; each season is a quarter of
    // the year from there. Taking the remainder first keeps 19 March in winter.
    constexpr float seasonDays = 365.0f / 4.0f;

    tm    lt = GetLocalTimeSafe();
    float position = float((lt.tm_yday - 78 + 365) % 365) / seasonDays;   // 0..4
    uint32 index = std::min<uint32>(uint32(position), 3u);
    float  within = std::clamp(position - float(index), 0.0f, 1.0f);      // 0 = first day, 1 = last

    out.primary = Season(index);
    out.secondary = out.primary;

    if (m_seasonBlendDays > 0.0f)
    {
        // At a boundary the two tables weigh the same; BlendDays further in, only this season's.
        float window = m_seasonBlendDays / seasonDays;

        if (within < window)
        {
            out.secondary = Season((index + 3) % 4);
            out.secondaryShare = 0.5f * (1.0f - within / window);
        }
        else if (within > 1.0f - window)
        {
            out.secondary = Season((index + 1) % 4);
            out.secondaryShare = 0.5f * (1.0f - (1.0f - within) / window);
        }
    }

    return out;
}

// ============================================================
// Intensity mapping
// ============================================================
Range WeatherVibeCore::GetRange(DayPart dp, WeatherState state) const
{
    int index = StateIndex(state);
    if (index < 0 || dp >= DayPart::COUNT)
        return Range{ 0.30f, 1.00f };
    return m_stateRanges[size_t(dp)][size_t(index)];
}

float WeatherVibeCore::ClampRaw(WeatherState state, float rawGrade)
{
    float grade = std::clamp(rawGrade, 0.0f, 1.0f);

    // For non-fine states the client needs grade within (0..1) exclusive
    // to actually render the effect; 0.0 shows nothing, 1.0 can glitch.
    if (state != WEATHER_STATE_FINE)
        grade = std::clamp(grade, kMinGrade, kMaxGrade);

    return grade;
}

float WeatherVibeCore::PercentToRaw(DayPart dp, WeatherState state, float percentage) const
{
    float const share = std::clamp(percentage, 0.0f, 100.0f) / 100.0f;
    Range const r = GetRange(dp, state);

    if (WeatherVibe::IsClearSky(state))
        return r.max - share * (r.max - r.min);   // 100% = clear, 0% = as clouded as the state gets

    return r.min + share * (r.max - r.min);
}

float WeatherVibeCore::RawToPercent(DayPart dp, WeatherState state, float rawGrade) const
{
    Range const r = GetRange(dp, state);
    float const width = r.max - r.min;
    if (width < 0.0001f)
        return 100.0f;

    float share = WeatherVibe::IsClearSky(state) ? (r.max - rawGrade) / width : (rawGrade - r.min) / width;
    return std::clamp(share, 0.0f, 1.0f) * 100.0f;
}

// ============================================================
// The look on a Waystone client
// ============================================================
float WeatherVibeCore::LookShare(DayPart dp, WeatherState state, float rawGrade) const
{
    float const pct = RawToPercent(dp, state, rawGrade) / 100.0f;
    return WeatherVibe::IsClearSky(state) ? 1.0f - pct : pct;
}

float WeatherVibeCore::SkyFor(WeatherState state, float rawGrade) const
{
    using WeatherVibe::Ground;

    Ground const ground = WeatherVibe::GroundOf(state);

    // A sky on its own: the stock ramp, fully stormy at the floor.
    if (ground == Ground::SKY)
        return std::clamp(rawGrade / WeatherVibe::CLIENT_PARTICLE_FLOOR, 0.0f, 1.0f);

    // Under rain, snow or sand the stock client is fully stormy at the first drop. Here the sky
    // starts paler and darkens as more falls (eased, so it deepens quickly at first): a shower
    // under a bright sky, a downpour under a black one.
    SkyCurve const& c = ground == Ground::RAIN ? m_skyRain : (ground == Ground::SNOW ? m_skySnow : m_skySand);
    float const t = std::clamp(WeatherVibe::Density(rawGrade) / c.fullAt, 0.0f, 1.0f);
    float sky = c.first + (1.0f - c.first) * t * (2.0f - t);

    // A storm is dark from the moment it is one.
    if (IsThunder(state) || state == WV_STATE_BLIZZARD)
        sky = std::max(sky, 0.9f);

    return std::clamp(sky, 0.0f, 1.0f);
}

// ============================================================
// Delivery
// ============================================================
void WeatherVibeCore::SendWeather(Player* player, WeatherState wireId, float rawGrade, bool abrupt, WeatherVibe::ClientExt const* ext)
{
    using namespace WeatherVibe;

    bool const withBlock = ext && !ext->Empty() && Instance().IsExtensionEnabled();
    uint32 const count = withBlock ? uint32(std::popcount(ext->mask)) : 0;

    WorldPacket data(SMSG_WEATHER, 4 + 4 + 1 + (withBlock ? 4 + 1 + 5 * count : 0));
    data << uint32(wireId);
    data << float(ClampRaw(wireId, rawGrade));
    data << uint8(abrupt ? 1 : 0);

    if (withBlock)
    {
        data << uint32(EXT_MAGIC);
        data << uint8(count);
        for (uint32 key = 0; key < EXT_KEY_COUNT; ++key)
        {
            if (!ext->Has(ExtKey(key)))
                continue;
            data << uint8(key);
            data << float(ext->values[key]);
        }
    }

    player->SendDirectMessage(&data);
}

bool WeatherVibeCore::Deliver(Player* player, WeatherVibe::Wire const& wire, WeatherVibe::Tell why)
{
    using WeatherVibe::Tell;

    WeatherState const wireId = wire.id;
    float const grade = ClampRaw(wireId, wire.grade);
    bool const withBlock = Instance().IsExtensionEnabled();
    SentWeather* sent = player->CustomData.Get<SentWeather>(SENT_KEY);

    if (sent && sent->valid && sent->wireId == wireId)
    {
        float const off = std::fabs(sent->grade - grade);
        bool const sameLook = !withBlock || sent->ext.SameAs(wire.ext);

        if (RestartsOnPacket(wireId))
        {
            // Never said twice - and a player who only walked into much the same sandstorm
            // keeps the one they have, rather than watch it fade out and come back a shade
            // thicker. A change of look alone (a storm flickering next door) is not worth that
            // either, unless the weather itself changed.
            if (off < (why == Tell::CHANGE ? WeatherVibe::GRADE_EPSILON : WeatherVibe::RESTART_NOT_WORTH)
                && (sameLook || why != Tell::CHANGE))
                return false;
        }
        else if (why != Tell::REPEAT && off < WeatherVibe::GRADE_EPSILON && sameLook)
            return false;   // the client is already showing exactly this
    }

    // A rain id with a texture of its own (the dark and red skins) leaves that texture on
    // ordinary rain afterwards. Any packet that is not rain clears it, so send the sky first.
    if (sent && sent->valid && (sent->wireId == CLIENT_WEATHER_DARK_RAIN || sent->wireId == CLIENT_WEATHER_RED_RAIN)
        && IsPlainRain(wireId))
        SendWeather(player, WEATHER_STATE_FINE, grade);

    // No record: the client has just logged in or come through a loading screen. Put the
    // weather straight up, so it is already there when the screen lifts.
    bool const abrupt = !sent;
    WeatherVibe::ClientExt block = wire.ext;
    if (abrupt && !block.Empty())
        block.Set(WeatherVibe::EXT_TRANSITION, 0.0f);

    SendWeather(player, wireId, grade, abrupt, &block);

    if (!sent)
        sent = player->CustomData.GetDefault<SentWeather>(SENT_KEY);
    sent->wireId = wireId;
    sent->grade = grade;
    sent->ext = block;
    sent->valid = true;
    return true;
}

void WeatherVibeCore::Forget(Player* player, bool snapNext)
{
    if (snapNext)
    {
        player->CustomData.Erase(SENT_KEY);
        return;
    }

    if (SentWeather* sent = player->CustomData.Get<SentWeather>(SENT_KEY))
        sent->valid = false;
    else
        player->CustomData.GetDefault<SentWeather>(SENT_KEY)->valid = false;
}

bool WeatherVibeCore::HasClient(Player const* player)
{
    WorldSession const* session = player->GetSession();
    return session && !session->IsSocketClosed();
}

// Both sets are nearly always empty, and the logout hook calls the "off" side for every
// character including bots: leave an empty set untouched.
void WeatherVibeCore::SetDebugSubscriber(ObjectGuid guid, bool on)
{
    if (on)
        m_debugSubscribers.insert(guid);
    else if (!m_debugSubscribers.empty())
        m_debugSubscribers.erase(guid);
}

void WeatherVibeCore::SetAuditioning(ObjectGuid guid, bool on)
{
    if (on)
        m_auditioning.insert(guid);
    else if (!m_auditioning.empty())
        m_auditioning.erase(guid);
}

// ============================================================
// Module state -> client weather id
// ============================================================
bool WeatherVibeCore::IsThunder(WeatherState s)
{
    return s == WEATHER_STATE_THUNDERS || s == WV_STATE_THUNDER_SNOW || s == WV_STATE_DRY_THUNDERS || s == WV_STATE_TEMPEST;
}

char const* WeatherVibeCore::ThunderSourceName(ThunderSource source)
{
    switch (source)
    {
    case ThunderSource::SERVER: return "server";
    case ThunderSource::BOTH:   return "both";
    default:                    return "client";
    }
}

WeatherState WeatherVibeCore::WireState(WeatherState state, WeatherSkins skins, float rawGrade) const
{
    using WeatherVibe::Ground;

    Ground const ground = WeatherVibe::GroundOf(state);

    // A sky is a sky. Fine has no row in the client's table and Fog has one with no particles
    // and no sound: to the client they are the same thing, a sky dimmed by the grade.
    if (ground == Ground::SKY)
        return WeatherVibe::IsClearSky(state) ? WEATHER_STATE_FINE : WEATHER_STATE_FOG;

    // Too little is falling for a sound loop; the id would switch one on at once. Rain and
    // snow have silent rows with the ordinary drop and flake (2 and 61), so a drizzle or a few
    // flakes can be seen without being heard. Below that, and for sand, the sky alone: it
    // looks the same, and it is quiet.
    if (rawGrade < m_loopLightFrom)
    {
        bool const drawn = m_silentDrizzle && rawGrade > WeatherVibe::CLIENT_PARTICLE_FLOOR + 0.005f;

        if (drawn && ground == Ground::RAIN && skins.rain != RainSkin::RED)
            return skins.rain == RainSkin::DARK ? CLIENT_WEATHER_DARK_RAIN : CLIENT_WEATHER_SILENT_RAIN;
        if (drawn && ground == Ground::SNOW)
            return CLIENT_WEATHER_SILENT_SNOW;
        return WEATHER_STATE_FINE;
    }

    // From here the id is chosen for its sound, by how much is falling: a state's name does
    // not decide the loop, the grade does. 0 light, 1 medium, 2 heavy.
    int const loop = rawGrade >= m_loopHeavyFrom ? 2 : (rawGrade >= m_loopMediumFrom ? 1 : 0);

    switch (ground)
    {
    case Ground::RAIN:
        if (skins.rain == RainSkin::DARK) return CLIENT_WEATHER_DARK_RAIN;
        if (skins.rain == RainSkin::RED)  return CLIENT_WEATHER_RED_RAIN;
        return loop == 2 ? WEATHER_STATE_HEAVY_RAIN : (loop == 1 ? WEATHER_STATE_MEDIUM_RAIN : WEATHER_STATE_LIGHT_RAIN);

    case Ground::SNOW:
        return loop == 2 ? WEATHER_STATE_HEAVY_SNOW : (loop == 1 ? WEATHER_STATE_MEDIUM_SNOW : WEATHER_STATE_LIGHT_SNOW);

    case Ground::SAND:
    default:
        if (skins.sand == SandSkin::ASH)   return CLIENT_WEATHER_ASH;
        if (skins.sand == SandSkin::EMBER) return CLIENT_WEATHER_EMBER;

        // The client's table has the sandstorm loops crossed against the core's names:
        // id 22 carries the heavy wind, 41 the light one, 42 the medium one.
        return loop == 2 ? WEATHER_STATE_LIGHT_SANDSTORM : (loop == 1 ? WEATHER_STATE_HEAVY_SANDSTORM : WEATHER_STATE_MEDIUM_SANDSTORM);
    }
}

WeatherState WeatherVibeCore::HookState(WeatherState state)
{
    if (IsThunder(state))
        return WEATHER_STATE_THUNDERS;
    if (state == WV_STATE_CLEAR)
        return WEATHER_STATE_FINE;
    if (state == WV_STATE_BLIZZARD)
        return WEATHER_STATE_HEAVY_SNOW;
    return state;
}

// Weather::SetType (0x7846A0 in the stock 12340 exe) restarts the effect when the type or the
// texture changes, and fills in a default texture when the row has none. Rain and snow have
// defaults, so a second rain or snow packet changes nothing. Sandstorm has none: every packet
// about it reads as a new texture. 104 carries its own texture and does not restart.
bool WeatherVibeCore::RestartsOnPacket(WeatherState wireId)
{
    switch (wireId)
    {
    case WEATHER_STATE_LIGHT_SANDSTORM:
    case WEATHER_STATE_MEDIUM_SANDSTORM:
    case WEATHER_STATE_HEAVY_SANDSTORM:
    case CLIENT_WEATHER_EMBER:
        return true;
    default:
        return false;
    }
}

bool WeatherVibeCore::ParseRainSkin(std::string const& token, RainSkin& out)
{
    std::string const t = WeatherVibe::ToLower(WeatherVibe::Trim(token));
    if (t == "rain") { out = RainSkin::RAIN; return true; }
    if (t == "dark") { out = RainSkin::DARK; return true; }
    if (t == "red")  { out = RainSkin::RED;  return true; }
    return false;
}

bool WeatherVibeCore::ParseSandSkin(std::string const& token, SandSkin& out)
{
    std::string const t = WeatherVibe::ToLower(WeatherVibe::Trim(token));
    if (t == "sand")  { out = SandSkin::SAND;  return true; }
    if (t == "ash")   { out = SandSkin::ASH;   return true; }
    if (t == "ember") { out = SandSkin::EMBER; return true; }
    return false;
}

char const* WeatherVibeCore::RainSkinName(RainSkin skin)
{
    switch (skin)
    {
    case RainSkin::DARK: return "dark";
    case RainSkin::RED:  return "red";
    default:             return "rain";
    }
}

char const* WeatherVibeCore::SandSkinName(SandSkin skin)
{
    switch (skin)
    {
    case SandSkin::ASH:   return "ash";
    case SandSkin::EMBER: return "ember";
    default:              return "sand";
    }
}

// ============================================================
// Name helpers
// ============================================================
WeatherState WeatherVibeCore::StateAt(size_t index)
{
    return kStates[index < WV_STATES ? index : 0].state;
}

int WeatherVibeCore::StateIndex(WeatherState s)
{
    for (size_t i = 0; i < WV_STATES; ++i)
        if (kStates[i].state == s)
            return int(i);
    return -1;
}

char const* WeatherVibeCore::WeatherStateName(WeatherState s)
{
    int index = StateIndex(s);
    if (index >= 0)
        return kStates[size_t(index)].name;

    switch (s)
    {
    case WEATHER_STATE_BLACKRAIN: return "BlackRain";
    case WEATHER_STATE_BLACKSNOW: return "BlackSnow";
    default:                      return "Unknown";
    }
}

bool WeatherVibeCore::ParseState(std::string const& token, WeatherState& out)
{
    std::string const text = WeatherVibe::Trim(token);

    uint32 id = 0;
    if (WeatherVibe::ParseUInt(text, id))
    {
        if (!IsValidWeatherState(id))
            return false;
        out = WeatherState(id);
        return true;
    }

    std::string const lower = WeatherVibe::ToLower(text);
    for (StateInfo const& info : kStates)
    {
        if (lower == WeatherVibe::ToLower(info.name))
        {
            out = info.state;
            return true;
        }
    }

    return false;
}

char const* WeatherVibeCore::DayPartName(DayPart d)
{
    switch (d)
    {
    case DayPart::MORNING:   return "Morning";
    case DayPart::AFTERNOON: return "Afternoon";
    case DayPart::EVENING:   return "Evening";
    case DayPart::NIGHT:     return "Night";
    default:                 return "Unknown";
    }
}

char const* WeatherVibeCore::DayPartTokenUpper(DayPart d)
{
    switch (d)
    {
    case DayPart::MORNING:   return "MORNING";
    case DayPart::AFTERNOON: return "AFTERNOON";
    case DayPart::EVENING:   return "EVENING";
    case DayPart::NIGHT:     return "NIGHT";
    default:                 return "UNKNOWN";
    }
}

char const* WeatherVibeCore::SeasonName(Season s)
{
    switch (s)
    {
    case Season::SPRING: return "Spring";
    case Season::SUMMER: return "Summer";
    case Season::AUTUMN: return "Autumn";
    case Season::WINTER: return "Winter";
    default:             return "Unknown";
    }
}

std::string WeatherVibeCore::AreaName(uint32 areaId)
{
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId))
        if (char const* name = area->area_name[sWorld->GetDefaultDbcLocale()])
            return name;
    return "unknown";
}
