#ifndef MOD_WV_CORE_H
#define MOD_WV_CORE_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Weather.h"          // WeatherState enum

#include <ctime>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

class Player;

// ============================================================
// Enums & POD structs (shared across translation units)
// ============================================================
enum class Season : uint8
{
    SPRING = 0,
    SUMMER,
    AUTUMN,
    WINTER,
    COUNT
};

enum class DayPart : uint8
{
    MORNING = 0,
    AFTERNOON,
    EVENING,
    NIGHT,
    COUNT
};

constexpr size_t WV_SEASONS = size_t(Season::COUNT);
constexpr size_t WV_DAYPARTS = size_t(DayPart::COUNT);
constexpr size_t WV_STATES = 12;   // the weather states the module drives (see kStates)

struct Range
{
    float min = 0.0f;
    float max = 1.0f;
};

struct DayPartStarts
{
    int morning = 6 * 60;      // 06:00
    int afternoon = 12 * 60;   // 12:00
    int evening = 18 * 60;     // 18:00
    int night = 22 * 60;       // 22:00
};

// Where the calendar stands between two seasons. Near an equinox or solstice the neighbouring
// season's table mixes in, so the year turns gradually instead of on one midnight.
struct SeasonBlend
{
    Season primary = Season::SPRING;
    Season secondary = Season::SPRING;
    float  secondaryShare = 0.0f;   // 0..0.5
};

namespace WeatherVibe
{
    std::string Trim(std::string const& s);
    std::string StripComment(std::string const& s);                 // drops a trailing "# ..."
    std::vector<std::string> Split(std::string const& s, char sep);  // trimmed tokens
    std::string ToLower(std::string_view s);
    bool ParseFloat(std::string const& s, float& out);
    bool ParseUInt(std::string const& s, uint32& out);

    // The module's view of the config. AzerothCore keeps everything after '=' as the value, so a
    // trailing "# comment" would otherwise end up inside profile names and numbers; every read
    // here strips it. Keys are snapshotted once so prefix scans do not walk the whole config.
    class ConfigView
    {
    public:
        ConfigView();

        bool Has(std::string const& key) const;
        std::vector<std::string> KeysWithPrefix(std::string const& prefix) const;

        std::string Text(std::string const& key, std::string const& def = "") const;
        std::string RawText(std::string const& key, std::string const& def = "") const;   // keeps '#'
        float       Float(std::string const& key, float def) const;
        uint32      UInt(std::string const& key, uint32 def) const;
        bool        Bool(std::string const& key, bool def) const;

        // Values of "<base>.1", "<base>.2", ... in numeric order, with the index they came from.
        std::vector<std::pair<uint32, std::string>> Numbered(std::string const& base, bool stripComments = true) const;

    private:
        std::vector<std::string> _keys;   // sorted
    };
}

// ============================================================
// WeatherVibeCore — singleton
// Config, calendar, intensity mapping and the packet itself.
// ============================================================
class WeatherVibeCore
{
public:
    static WeatherVibeCore& Instance();

    WeatherVibeCore() = default;
    ~WeatherVibeCore() = default;
    WeatherVibeCore(WeatherVibeCore const&) = delete;
    WeatherVibeCore& operator=(WeatherVibeCore const&) = delete;

    // (Re)reads every core option.
    void LoadConfig(WeatherVibe::ConfigView const& cfg);

    // Calendar
    DayPart     GetCurrentDayPart() const;
    Season      GetCurrentSeason()  const { return GetSeasonBlend().primary; }
    SeasonBlend GetSeasonBlend()    const;

    // Intensity mapping. A percentage (0..100) is how far into the state's configured band the
    // weather is. Fine runs the other way round: 100% is a clear sky (the band's minimum) and
    // 0% is as overcast as "fine" gets (the band's maximum), which is where precipitation begins.
    float PercentToRaw(DayPart dp, WeatherState state, float percentage) const;
    float RawToPercent(DayPart dp, WeatherState state, float rawGrade) const;
    static float ClampRaw(WeatherState state, float rawGrade);

    // Delivery
    static void SendWeather(Player* player, WeatherState state, float rawGrade);
    // True for a character with a client behind it. Playerbot sessions have no socket.
    static bool HasClient(Player const* player);

    // State accessors
    bool IsEnabled()         const { return m_enabled; }
    bool IsAnnounceEnabled() const { return m_announce; }
    bool IsProfileEnabled()  const { return m_profileEnabled; }
    bool IsDebug()           const { return m_debug; }
    bool PlayersMayRead()    const { return m_playersMayRead; }

    // GMs who asked to see the engine's decisions for the weather they are standing in.
    // Only touched from the world thread (commands, logout).
    void SetDebugSubscriber(ObjectGuid guid, bool on);
    bool IsDebugSubscriber(ObjectGuid guid) const { return m_debugSubscribers.count(guid) != 0; }
    std::unordered_set<ObjectGuid> const& GetDebugSubscribers() const { return m_debugSubscribers; }

    // Name helpers (static — no state dependency)
    static char const* WeatherStateName(WeatherState s);     // "LightRain" — also the config token
    static char const* DayPartName(DayPart d);
    static char const* DayPartTokenUpper(DayPart d);
    static char const* SeasonName(Season s);
    static std::string AreaName(uint32 areaId);

    // The states the module drives, in a fixed order usable as an array index.
    static WeatherState StateAt(size_t index);
    static int  StateIndex(WeatherState s);                  // -1 when not driven by the module
    static bool IsValidWeatherState(uint32 value) { return StateIndex(WeatherState(value)) >= 0; }
    // Accepts the numeric id ("3") or the config token ("LightRain"), case-insensitive.
    static bool ParseState(std::string const& token, WeatherState& out);

private:
    void LoadDayPartConfig(WeatherVibe::ConfigView const& cfg);
    void LoadIntensityRangesConfig(WeatherVibe::ConfigView const& cfg);
    void ValidateDayPartStarts();

    Range GetRange(DayPart dp, WeatherState state) const;

    static tm  GetLocalTimeSafe();
    static int ParseHHMM(std::string const& s, int defMinutes);

    // Module state
    bool          m_enabled = true;
    bool          m_announce = false;
    bool          m_profileEnabled = true;
    bool          m_debug = false;
    bool          m_playersMayRead = false;
    DayPartStarts m_starts;

    // Cached daypart/season mode — resolved once on config load
    bool    m_dayPartAuto = true;
    DayPart m_dayPartFixed = DayPart::MORNING;
    bool    m_seasonAuto = true;
    Season  m_seasonFixed = Season::SPRING;
    float   m_seasonBlendDays = 30.0f;

    // Per-daypart per-state intensity bands
    Range m_stateRanges[WV_DAYPARTS][WV_STATES];

    std::unordered_set<ObjectGuid> m_debugSubscribers;

    static constexpr float kMinGrade = 0.0001f;
    static constexpr float kMaxGrade = 0.9999f;
};

// Convenience macro (mirrors AzerothCore idiom)
#define sWeatherVibeCore WeatherVibeCore::Instance()

#endif // MOD_WV_CORE_H
