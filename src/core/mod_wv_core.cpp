#include "mod_wv_core.h"
#include "mod_wv_pick.h"

#include "Config.h"
#include "DataMap.h"
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
        { WV_STATE_DRY_THUNDERS,          "DryThunders",     { 0.20f, 0.25f } }    // how dark the sky gets
    } };

    // The last thing a client was told, kept on the player (see WeatherVibeCore::Deliver).
    struct SentWeather : public DataMap::Base
    {
        WeatherState wireId = WEATHER_STATE_FINE;
        float        grade = 0.0f;
    };

    char const* const SENT_KEY = "WeatherVibe.Sent";

    // Weather.dbc rows the core has no name for.
    constexpr WeatherState CLIENT_WEATHER_DARK_RAIN = WeatherState(90);    // rain motion, leaf-brown streaks, no sound
    constexpr WeatherState CLIENT_WEATHER_RED_RAIN = WeatherState(100);    // rain motion, red drops
    constexpr WeatherState CLIENT_WEATHER_EMBER = WeatherState(103);       // sandstorm motion, orange
    constexpr WeatherState CLIENT_WEATHER_ASH = WeatherState(104);         // sandstorm motion, black mist, no sound
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

            // The client's two thresholds make some of a band pointless; say so once per state.
            if (dp != 0)
                continue;

            bool const skyOnly = WeatherVibe::GroundOf(kStates[i].state) == WeatherVibe::Ground::SKY;
            if (skyOnly && range.max > WeatherVibe::CLIENT_PARTICLE_FLOOR + 0.001f)
                LOG_WARN("server.loading", "[WeatherVibe] {} is only a sky, and the sky is fully stormy at grade 0.25: "
                    "the part of its range above 0.25 ({:.2f}) changes nothing", kStates[i].name, range.max);
            else if (!skyOnly && range.min < m_loopLightFrom - 0.001f)
                LOG_WARN("server.loading", "[WeatherVibe] {} starts at grade {:.2f}, but nothing falls at or below 0.25 and it is "
                    "sent as plain sky below {:.2f}: its low percentages will show no weather", kStates[i].name, range.min, m_loopLightFrom);
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
void WeatherVibeCore::SendWeather(Player* player, WeatherState wireId, float rawGrade)
{
    WorldPackets::Misc::Weather weather(wireId, ClampRaw(wireId, rawGrade));
    player->SendDirectMessage(weather.Write());
}

bool WeatherVibeCore::Deliver(Player* player, WeatherState wireId, float rawGrade, WeatherVibe::Tell why)
{
    using WeatherVibe::Tell;

    float const grade = ClampRaw(wireId, rawGrade);
    SentWeather* sent = player->CustomData.Get<SentWeather>(SENT_KEY);

    if (sent && sent->wireId == wireId)
    {
        float const off = std::fabs(sent->grade - grade);

        if (RestartsOnPacket(wireId))
        {
            // Never said twice - and a player who only walked into much the same snow keeps
            // the snow they have, rather than watch it fade out and come back a shade thicker.
            if (off < (why == Tell::CHANGE ? WeatherVibe::GRADE_EPSILON : WeatherVibe::RESTART_NOT_WORTH))
                return false;
        }
        else if (why != Tell::REPEAT && off < WeatherVibe::GRADE_EPSILON)
            return false;   // the client is already showing exactly this
    }

    // A rain id with a texture of its own (the dark and red skins) leaves that texture on
    // ordinary rain afterwards. Any packet that is not rain clears it, so send the sky first.
    if (sent && (sent->wireId == CLIENT_WEATHER_DARK_RAIN || sent->wireId == CLIENT_WEATHER_RED_RAIN)
        && wireId >= WEATHER_STATE_LIGHT_RAIN && wireId <= WEATHER_STATE_HEAVY_RAIN)
        SendWeather(player, WEATHER_STATE_FINE, grade);

    SendWeather(player, wireId, grade);

    if (!sent)
        sent = player->CustomData.GetDefault<SentWeather>(SENT_KEY);
    sent->wireId = wireId;
    sent->grade = grade;
    return true;
}

void WeatherVibeCore::Forget(Player* player)
{
    player->CustomData.Erase(SENT_KEY);
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
    return s == WEATHER_STATE_THUNDERS || s == WV_STATE_THUNDER_SNOW || s == WV_STATE_DRY_THUNDERS;
}

WeatherState WeatherVibeCore::WireState(WeatherState state, WeatherSkins skins, float rawGrade) const
{
    using WeatherVibe::Ground;

    Ground const ground = WeatherVibe::GroundOf(state);

    // A sky is a sky. Fine has no row in the client's table and Fog has one with no particles
    // and no sound: to the client they are the same thing, a sky dimmed by the grade.
    if (ground == Ground::SKY)
        return state == WEATHER_STATE_FINE ? WEATHER_STATE_FINE : WEATHER_STATE_FOG;

    // Too little is falling to see. The id would still switch its sound loop on at once, so
    // send the sky alone: it looks the same, and it is quiet.
    if (rawGrade < m_loopLightFrom)
        return WEATHER_STATE_FINE;

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
    return IsThunder(state) ? WEATHER_STATE_THUNDERS : state;
}

bool WeatherVibeCore::RestartsOnPacket(WeatherState wireId)
{
    switch (wireId)
    {
    case WEATHER_STATE_LIGHT_SNOW:
    case WEATHER_STATE_MEDIUM_SNOW:
    case WEATHER_STATE_HEAVY_SNOW:
    case WEATHER_STATE_LIGHT_SANDSTORM:
    case WEATHER_STATE_MEDIUM_SANDSTORM:
    case WEATHER_STATE_HEAVY_SANDSTORM:
    case CLIENT_WEATHER_ASH:
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
