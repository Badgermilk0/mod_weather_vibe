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
constexpr size_t WV_STATES = 17;   // the weather states the module drives (see kStates)

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

    float At(float share) const { return min + (max - min) * share; }
};

// Thunder and lightning: who makes them.
//   CLIENT  a Waystone client draws the lightning and plays the thunder (through the packet's
//           extension block); the server plays nothing. A stock client gets no thunder.
//   SERVER  the server plays 2D claps and the rumble to everyone; Waystone clients are told
//           to draw no lightning of their own.
//   BOTH    the server's claps for everyone, and on a Waystone client the flashes too, with
//           its own thunder and rumble muted so nothing is heard twice.
enum class ThunderSource : uint8
{
    CLIENT = 0,
    SERVER,
    BOTH
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
    // A sandstorm fades out and starts over on every packet about it. A change of grade
    // smaller than this, within one sound loop, is not worth that.
    constexpr float RESTART_NOT_WORTH = 0.08f;

    // ── The Waystone client's extension of SMSG_WEATHER ──
    //
    // After the stock uint32 id, float grade, uint8 abrupt:  uint32 'WVX1', uint8 count, then
    // count x { uint8 key, float value }. A stock client's net layer leaves unread bytes alone,
    // so the block costs it nothing; every packet replaces the previous block, and a packet
    // without one puts the client back on its own defaults. The keys mirror WeatherModel.h in
    // CLIENT/wowdecomp - MODIFIED (its MODIFICATIONS.md has the table with ranges).
    constexpr uint32 EXT_MAGIC = 0x31585657;   // bytes 57 56 58 31

    enum ExtKey : uint8
    {
        EXT_SKY = 1,                 // storm sky blend 0..1, in place of the one the grade implies
        EXT_PARTICLES = 2,           // not used: the grade already says it, for every client alike
        EXT_DENSITY = 3,             // particle count multiplier 0..2
        EXT_LIGHTNING = 4,           // strikes per minute at full weather (0 = none, whatever the id)
        EXT_TRANSITION = 5,          // seconds to reach the new grade, sky and particles
        EXT_VOLUME = 6,
        EXT_STRIKE_MIN = 7,          // nearest a strike lands, yards
        EXT_STRIKE_MAX = 8,          // farthest, yards
        EXT_FLASH = 9,
        EXT_THUNDER = 10,            // thunder volume multiplier (0 = flashes without thunder)
        EXT_NEAR_CHANCE = 11,        // share of strikes within 600 yd
        EXT_BURST = 12,
        EXT_STORM = 13,              // the storm 0..1: near chance, restrikes and rumble follow it
        EXT_SOUND_KIT = 14,
        EXT_RUMBLE_KIT = 15,         // the bed under the weather (0 = none)
        EXT_RUMBLE = 16,             // its volume 0..1
        EXT_NEAR_KIT = 17,           // the crack of a near strike
        EXT_FAR_KIT = 18,            // body and roll of every strike
        EXT_DUCK = 19,               // zone ambience turned down under full weather 0..1
        EXT_INDOOR = 20,
        EXT_FLASH_COLOR = 21,        // 0xRRGGBB carried in the float
        EXT_KEY_COUNT = 22
    };

    struct ClientExt
    {
        uint32 mask = 0;
        float  values[EXT_KEY_COUNT] = {};

        bool  Has(ExtKey key) const { return (mask & (1u << key)) != 0; }
        float Get(ExtKey key, float fallback) const { return Has(key) ? values[key] : fallback; }
        void  Set(ExtKey key, float value) { mask |= 1u << key; values[key] = value; }
        void  Clear(ExtKey key) { mask &= ~(1u << key); }
        bool  Empty() const { return mask == 0; }

        // Whether two blocks say the same thing on screen. The transition time is how a change
        // is made, not what it is, and is left out.
        bool SameAs(ClientExt const& other) const;
    };

    // Everything a client is sent: the stock pair and, for a Waystone client, the block.
    struct Wire
    {
        WeatherState id = WeatherState(0);
        float        grade = 0.0f;
        ClientExt    ext;
    };

    // Why a client is being told the weather.
    enum class Tell : uint8
    {
        CHANGE,    // the weather moved: send, unless the client was last told exactly this
        ARRIVAL,   // the player moved: as CHANGE, but a sandstorm close enough to what the
                   // client already shows (RESTART_NOT_WORTH) is left alone
        REPEAT     // as ARRIVAL, and rain, snow or a sky is said again even when unchanged, for when
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
    // weather is. Fine and Clear run the other way round: 100% is a clear sky (the band's
    // minimum) and 0% is as overcast as the state gets (the band's maximum).
    float PercentToRaw(DayPart dp, WeatherState state, float percentage) const;
    float RawToPercent(DayPart dp, WeatherState state, float rawGrade) const;
    static float ClampRaw(WeatherState state, float rawGrade);
    float GetLoopLightFrom() const { return m_loopLightFrom; }

    // ── What a Waystone client is shown beyond the id and grade (WeatherVibe.Look.*) ──
    // The storm sky for a state at a grade. A sky is the stock ramp; under rain, snow or sand
    // it starts paler than the stock client's (which is fully stormy at the first drop) and
    // darkens as more falls; any storm is dark from the start.
    float SkyFor(WeatherState state, float rawGrade) const;
    // How far into its band (0..1) a state is, on the look tables' scale: the band's own
    // percentage, the right way round for a sky too (0 = calm).
    float LookShare(DayPart dp, WeatherState state, float rawGrade) const;
    float DensityFor(WeatherState state, float share) const { return m_lookDensity[Index(state)].At(share); }
    float StormFor(WeatherState state, float share) const { return m_lookStorm[Index(state)].At(share); }
    float LightningFor(WeatherState state, float share) const { return m_lookLightning[Index(state)].At(share); }
    float DuckFor(WeatherState state) const { return m_lookDuck[Index(state)]; }   // < 0: not sent
    float DistantLightning() const { return m_distantLightning; }
    float DistantRumble() const { return m_distantRumble; }
    bool  DistantFromNeighbors() const { return m_distantNeighbors; }

    // ── Delivery ──
    // The raw packet: a client weather id and a grade, the abrupt flag, and the extension block
    // when there is one and WeatherVibe.Client.Extension allows it. Nothing is remembered.
    static void SendWeather(Player* player, WeatherState wireId, float rawGrade, bool abrupt = false,
        WeatherVibe::ClientExt const* ext = nullptr);

    // Tells a client what it should be showing — only if that differs from the last thing it
    // was told. This matters most for sandstorm: the stock client restarts it from nothing on
    // every packet about it, even an identical one. The record lives with the player
    // (CustomData), so it is only ever touched by the thread that owns the player at that
    // moment: its map thread in the zone and area hooks, the world thread otherwise.
    // A client with no record at all (just logged in, or just through a loading screen) is
    // sent the weather abruptly, so it is there when the screen lifts.
    // Returns true when a packet went out.
    static bool Deliver(Player* player, WeatherVibe::Wire const& wire,
        WeatherVibe::Tell why = WeatherVibe::Tell::CHANGE);
    // The record is no longer to be trusted (something else sent weather): the next Deliver
    // always sends. snapNext: the client has started over (a loading screen), so that packet
    // is abrupt rather than a fade.
    static void Forget(Player* player, bool snapNext = false);

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
    // WeatherVibe.Client.Extension: the 'WVX1' block goes out with the module's packets.
    bool IsExtensionEnabled() const { return m_extension; }
    ThunderSource GetThunderSource() const { return m_thunderSource; }
    static char const* ThunderSourceName(ThunderSource source);

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
    // A storm with lightning: Thunders, ThunderSnow, DryThunders and Tempest.
    static bool IsThunder(WeatherState s);
    // The weather id the client is sent for a module state at a grade, in a profile's skins.
    // The id is what picks the sound, so it follows what is actually falling: below the
    // audible grade it is the silent row of that weather (a drizzle, a few flakes) or plain
    // sky, and above it the light, medium or heavy loop by how much there is — not by the
    // state's name.
    WeatherState WireState(WeatherState state, WeatherSkins skins, float rawGrade) const;
    // The id the weather-change hook reports: any kind of thunder is 86, Clear is Fine and a
    // Blizzard is heavy snow; everything else itself.
    static WeatherState HookState(WeatherState state);
    // Whether the stock client restarts the whole effect on every packet about this id. Only
    // sandstorm does: its effect has no default texture, so the client's "has the texture
    // changed?" test is always yes. (A Waystone client with weatherFixes has that fixed.)
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
    void LoadLookConfig(WeatherVibe::ConfigView const& cfg);
    void ValidateDayPartStarts();

    static size_t Index(WeatherState state) { int i = StateIndex(state); return i < 0 ? 0 : size_t(i); }

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
    bool          m_extension = true;
    ThunderSource m_thunderSource = ThunderSource::CLIENT;
    bool          m_silentDrizzle = true;
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

    // The look (WeatherVibe.Look.*): per ground, the storm sky at the first drop and the share
    // of full density at which it is fully stormy; per state, ranges over the band.
    struct SkyCurve { float first = 0.5f; float fullAt = 0.6f; };
    SkyCurve m_skyRain{ 0.50f, 0.60f };
    SkyCurve m_skySnow{ 0.40f, 0.75f };
    SkyCurve m_skySand{ 0.35f, 0.90f };
    Range    m_lookDensity[WV_STATES];
    Range    m_lookStorm[WV_STATES];
    Range    m_lookLightning[WV_STATES];
    float    m_lookDuck[WV_STATES];
    float    m_distantLightning = 0.8f;
    float    m_distantRumble = 0.35f;
    bool     m_distantNeighbors = true;

    std::unordered_set<ObjectGuid> m_debugSubscribers;
    std::unordered_set<ObjectGuid> m_auditioning;

    static constexpr float kMinGrade = 0.0001f;
    static constexpr float kMaxGrade = 0.9999f;
};

// Convenience macro (mirrors AzerothCore idiom)
#define sWeatherVibeCore WeatherVibeCore::Instance()

#endif // MOD_WV_CORE_H
