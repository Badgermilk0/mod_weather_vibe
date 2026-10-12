#ifndef MOD_WV_CORE_H
#define MOD_WV_CORE_H

#include "mod_wv_states.h"

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
constexpr size_t WV_STATES = 14;   // the weather states the module drives (see kStates)

// What a profile's rain or sandstorm looks like. The rows, tiers and weights stay the same;
// only the weather id the client is sent changes (Weather.dbc has these as extra rows).
enum class RainSkin : uint8
{
    RAIN = 0,   // ids 3/4/5
    DARK,       // id 90: dark streaks, silent
    RED         // id 100: red drops
};

enum class SandSkin : uint8
{
    SAND = 0,   // ids 22/41/42
    ASH,        // id 104: black mist, silent
    EMBER       // id 103: orange dust
};

struct WeatherSkins
{
    RainSkin rain = RainSkin::RAIN;
    SandSkin sand = SandSkin::SAND;
};

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
    // ── The client's own rules, read from its weather code (DESIGN.md has the addresses) ──
    //
    //   sound      is chosen by the weather id alone, the moment the packet arrives;
    //   particles  depend on the grade alone: density = (grade - 0.25) / 0.75, nothing at or
    //              below 0.25, whatever the id;
    //   the sky    depends on the first quarter of the grade: storm = 4 * min(grade, 0.25).
    //
    // Everything about what is sent, and when, follows from these.

    // Nothing falls at or below this grade, and the sky is fully stormy from here up.
    constexpr float CLIENT_PARTICLE_FLOOR = 0.25f;
    // The client moves towards a new grade by itself, this much per second.
    constexpr float CLIENT_GRADE_PER_SECOND = 0.1f;
    // A change of grade smaller than this is not worth a packet.
    constexpr float GRADE_EPSILON = 0.004f;
    // Snow and sandstorm fade out and start over on every packet about them. A change of grade
    // smaller than this, within one sound loop, is not worth that.
    constexpr float RESTART_NOT_WORTH = 0.08f;

    // Why a client is being told the weather.
    enum class Tell : uint8
    {
        CHANGE,    // the weather moved: send, unless the client was last told exactly this
        ARRIVAL,   // the player moved: as CHANGE, but snow or sandstorm close enough to what the
                   // client already shows (RESTART_NOT_WORTH) is left alone
        REPEAT     // as ARRIVAL, and rain or a sky is said again even when unchanged, for when
                   // something else may have sent this client weather in between
    };

    // How much is falling, 0..1, at a grade.
    inline float Density(float grade)
    {
        float const density = (grade - CLIENT_PARTICLE_FLOOR) / (1.0f - CLIENT_PARTICLE_FLOOR);
        return density < 0.0f ? 0.0f : (density > 1.0f ? 1.0f : density);
    }

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
    float GetLoopLightFrom() const { return m_loopLightFrom; }

    // ── Delivery ──
    // The raw packet: a client weather id and a grade. Nothing is remembered.
    static void SendWeather(Player* player, WeatherState wireId, float rawGrade);

    // Tells a client what it should be showing — only if that differs from the last thing it
    // was told. This matters most for snow and sandstorm: the client restarts those from
    // nothing on every packet about them, even an identical one. The record lives with the
    // player (CustomData), so it is only ever touched by the thread that owns the player at
    // that moment: its map thread in the zone and area hooks, the world thread otherwise.
    // Returns true when a packet went out.
    static bool Deliver(Player* player, WeatherState wireId, float rawGrade,
        WeatherVibe::Tell why = WeatherVibe::Tell::CHANGE);
    // The record is no longer to be trusted (new map, something else sent weather): the next
    // Deliver always sends.
    static void Forget(Player* player);

    // True for a character with a client behind it. Playerbot sessions have no socket.
    static bool HasClient(Player const* player);

    // State accessors
    bool IsEnabled()         const { return m_enabled; }
    bool IsAnnounceEnabled() const { return m_announce; }
    bool IsProfileEnabled()  const { return m_profileEnabled; }
    bool IsDebug()           const { return m_debug; }
    bool PlayersMayRead()    const { return m_playersMayRead; }
    // worldserver.conf still has ActivateWeather = 1: the core sends its own weather packets.
    bool CoreWeatherIsOn()   const { return m_coreWeatherOn; }

    // GMs who asked to see the engine's decisions for the weather they are standing in.
    // Only touched from the world thread (commands, logout).
    void SetDebugSubscriber(ObjectGuid guid, bool on);
    bool IsDebugSubscriber(ObjectGuid guid) const { return m_debugSubscribers.count(guid) != 0; }
    std::unordered_set<ObjectGuid> const& GetDebugSubscribers() const { return m_debugSubscribers; }

    // GMs looking at a client weather id with `.wvibe audition`. The module sends them nothing
    // until they switch it off, so what they are looking at stays put. Written on the world
    // thread only; the zone hook reads it.
    void SetAuditioning(ObjectGuid guid, bool on);
    bool IsAuditioning(ObjectGuid guid) const { return !m_auditioning.empty() && m_auditioning.count(guid) != 0; }

    // --- What a module state means to the client, and to listeners ---
    static bool IsThunder(WeatherState s);
    // The weather id the client is sent for a module state at a grade, in a profile's skins.
    // The id is what picks the sound, so it follows what is actually falling: below the
    // audible grade it is plain sky (identical to look at, and silent), and above it the
    // light, medium or heavy loop by how much there is — not by the state's name.
    WeatherState WireState(WeatherState state, WeatherSkins skins, float rawGrade) const;
    // The id the weather-change hook reports: any kind of thunder is 86, everything else itself.
    static WeatherState HookState(WeatherState state);
    // Whether the client restarts the whole effect on every packet about this id (snow and
    // sandstorm: its "what is showing?" check only knows rain).
    static bool RestartsOnPacket(WeatherState wireId);

    static bool ParseRainSkin(std::string const& token, RainSkin& out);
    static bool ParseSandSkin(std::string const& token, SandSkin& out);
    static char const* RainSkinName(RainSkin skin);
    static char const* SandSkinName(SandSkin skin);

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
    bool          m_coreWeatherOn = false;
    DayPartStarts m_starts;

    // Cached daypart/season mode — resolved once on config load
    bool    m_dayPartAuto = true;
    DayPart m_dayPartFixed = DayPart::MORNING;
    bool    m_seasonAuto = true;
    Season  m_seasonFixed = Season::SPRING;
    float   m_seasonBlendDays = 30.0f;

    // Per-daypart per-state intensity bands
    Range m_stateRanges[WV_DAYPARTS][WV_STATES];

    // The grade from which each sound loop is used (WeatherVibe.Sound.From.*). Below the
    // first, precipitation is too thin to hear and is sent as sky.
    float m_loopLightFrom = 0.29f;
    float m_loopMediumFrom = 0.50f;
    float m_loopHeavyFrom = 0.68f;

    std::unordered_set<ObjectGuid> m_debugSubscribers;
    std::unordered_set<ObjectGuid> m_auditioning;

    static constexpr float kMinGrade = 0.0001f;
    static constexpr float kMaxGrade = 0.9999f;
};

// Convenience macro (mirrors AzerothCore idiom)
#define sWeatherVibeCore WeatherVibeCore::Instance()

#endif // MOD_WV_CORE_H
