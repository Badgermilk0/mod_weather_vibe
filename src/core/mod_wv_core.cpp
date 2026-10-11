#include "mod_wv_core.h"

#include "Config.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Log.h"
#include "MiscPackets.h"
#include "Player.h"
#include "StringConvert.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cctype>
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
        { WEATHER_STATE_FINE,             "Fine",            { 0.00f, 0.30f } },
        { WEATHER_STATE_FOG,              "Fog",             { 0.15f, 0.30f } },
        { WEATHER_STATE_LIGHT_RAIN,       "LightRain",       { 0.25f, 0.60f } },
        { WEATHER_STATE_MEDIUM_RAIN,      "MediumRain",      { 0.25f, 0.70f } },
        { WEATHER_STATE_HEAVY_RAIN,       "HeavyRain",       { 0.25f, 0.90f } },
        { WEATHER_STATE_LIGHT_SNOW,       "LightSnow",       { 0.25f, 0.60f } },
        { WEATHER_STATE_MEDIUM_SNOW,      "MediumSnow",      { 0.25f, 0.70f } },
        { WEATHER_STATE_HEAVY_SNOW,       "HeavySnow",       { 0.25f, 0.90f } },
        { WEATHER_STATE_LIGHT_SANDSTORM,  "LightSandstorm",  { 0.25f, 0.60f } },
        { WEATHER_STATE_MEDIUM_SANDSTORM, "MediumSandstorm", { 0.25f, 0.70f } },
        { WEATHER_STATE_HEAVY_SANDSTORM,  "HeavySandstorm",  { 0.25f, 0.90f } },
        { WEATHER_STATE_THUNDERS,         "Thunders",        { 0.15f, 0.40f } }
    } };
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

    if (sWorld->getBoolConfig(CONFIG_WEATHER))
        LOG_ERROR("module", "WeatherVibe: ActivateWeather is enabled in worldserver.conf, but WeatherVibe is also enabled. This will cause conflicts. Please set ActivateWeather = 0 to let WeatherVibe control weather.");

    LoadDayPartConfig(cfg);
    LoadIntensityRangesConfig(cfg);
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

    if (state == WEATHER_STATE_FINE)
        return r.max - share * (r.max - r.min);   // 100% fine = clear, 0% = overcast

    return r.min + share * (r.max - r.min);
}

float WeatherVibeCore::RawToPercent(DayPart dp, WeatherState state, float rawGrade) const
{
    Range const r = GetRange(dp, state);
    float const width = r.max - r.min;
    if (width < 0.0001f)
        return 100.0f;

    float share = (state == WEATHER_STATE_FINE) ? (r.max - rawGrade) / width : (rawGrade - r.min) / width;
    return std::clamp(share, 0.0f, 1.0f) * 100.0f;
}

// ============================================================
// Delivery
// ============================================================
void WeatherVibeCore::SendWeather(Player* player, WeatherState state, float rawGrade)
{
    WorldPackets::Misc::Weather weather(state, ClampRaw(state, rawGrade));
    player->SendDirectMessage(weather.Write());
}

bool WeatherVibeCore::HasClient(Player const* player)
{
    WorldSession const* session = player->GetSession();
    return session && !session->IsSocketClosed();
}

void WeatherVibeCore::SetDebugSubscriber(ObjectGuid guid, bool on)
{
    if (on)
        m_debugSubscribers.insert(guid);
    else
        m_debugSubscribers.erase(guid);
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
