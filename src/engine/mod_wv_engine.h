#ifndef MOD_WV_ENGINE_H
#define MOD_WV_ENGINE_H

#include "mod_wv_core.h"
#include "mod_wv_pick.h"

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

namespace WeatherVibe
{
    // Transition tuning (per profile, from config).
    struct TransitionSettings
    {
        uint32 stepTimeMinMs = 15000;
        uint32 stepTimeMaxMs = 30000;
        float  stepSizePct = 10.0f;
        uint32 changesPerHr = 6;
        uint32 maxConsecSame = 3;
        uint32 minHoldMs = 30000;         // minimum time weather lingers after arriving
    };

    // Extra table row that only counts while a holiday runs.
    struct HolidayEntry
    {
        uint32       holidayId = 0;
        WeatherEntry entry;
    };

    // One stretch of a transition: a state shown while its grade moves from -> to.
    struct Leg
    {
        WeatherState state = WEATHER_STATE_FINE;
        float        from = 0.0f;
        float        to = 0.0f;
    };

    // How to get from one weather to another. The grade moves continuously; the state id is
    // swapped where the two legs meet, so there is no jump in what the client draws.
    struct TransitionPlan
    {
        Leg    legs[2];
        uint8  legCount = 1;
        float  split = 1.0f;       // share of the eased progress spent on the first leg
        uint32 durationMs = 0;
    };

    enum class Phase : uint8 { HOLD, TRANSITION };

    // Live weather of a profile — shared by every zone and area mapped to it.
    struct LiveState
    {
        bool         started = false;

        // What clients are shown right now.
        WeatherState state = WEATHER_STATE_FINE;
        float        raw = 0.0f;

        // Where the weather is settled, or heading.
        WeatherState targetState = WEATHER_STATE_FINE;
        float        targetPct = 100.0f;
        float        targetRaw = 0.0f;
        WeatherState settledState = WEATHER_STATE_FINE;   // last state it came to rest on

        Phase          phase = Phase::HOLD;
        TransitionPlan plan;
        uint32         elapsedMs = 0;
        uint32         tickTimerMs = 0;

        uint32 holdMs = 0;            // hold left; during a transition, the hold that follows it
        uint32 consecSame = 1;

        // GM override: the engine keeps its hands off until released.
        bool   forced = false;
        bool   forcedTimed = false;
        uint32 forcedMs = 0;

        uint32 resyncTimerMs = 0;     // next re-announce to hook listeners
        bool   dirty = false;         // changed since the last broadcast pass
    };

    // A named weather profile — loaded once, ticked once, sent to everyone standing in it.
    struct Profile
    {
        std::string               name;
        WeatherTable              seasons[WV_SEASONS];
        std::vector<HolidayEntry> holidays;
        float                     dayPartWeight[WV_DAYPARTS][WV_STATES];
        TransitionSettings        transition;
        std::vector<uint32>       zoneIds;
        std::vector<uint32>       areaIds;
        std::vector<uint16>       neighbors;     // indices into the profile list
        LiveState                 live;
    };
}

// ============================================================
// WeatherVibeEngine — singleton
// Owns the profiles, runs their weather, and decides who is told.
//
// Threading: Update() and every mutating call run on the world thread while the map threads are
// idle. The zone/area hooks call Resolve()/SendCurrentTo() from map threads and only read.
// ============================================================
class WeatherVibeEngine
{
public:
    static WeatherVibeEngine& Instance();

    WeatherVibeEngine(WeatherVibeEngine const&) = delete;
    WeatherVibeEngine& operator=(WeatherVibeEngine const&) = delete;

    // Reads profiles and mappings. On a reload, profiles keep their live weather by name.
    void Load(WeatherVibe::ConfigView const& cfg, bool reload);
    void Clear();
    void Update(uint32 diff);

    // --- Delivery (read-only; safe from the zone/area hooks) ---
    WeatherVibe::Profile const* Resolve(uint32 zoneId, uint32 areaId) const;
    void SendCurrentTo(Player* player, uint32 zoneId, uint32 areaId) const;
    void OnAreaChange(Player* player, uint32 oldArea, uint32 newArea) const;
    void ForEachListener(WeatherVibe::Profile const& profile, std::function<void(Player*)> const& visit) const;
    // One-shot push to the clients in a zone the module has no profile for. Returns how many got it.
    uint32 PushToZone(uint32 zoneId, WeatherState state, float rawGrade) const;

    // --- Commands (world thread) ---
    std::vector<WeatherVibe::Profile> const& GetProfiles() const { return _profiles; }
    WeatherVibe::Profile* FindProfile(std::string const& name);
    WeatherVibe::Profile* FindProfileFor(uint32 zoneId, uint32 areaId);

    void Steer(WeatherVibe::Profile& profile, WeatherState state, float pct);                    // go there, then carry on
    void Force(WeatherVibe::Profile& profile, WeatherState state, float pct, uint32 minutes);    // go there and stay
    void ForceRaw(WeatherVibe::Profile& profile, WeatherState state, float rawGrade);            // snap and stay
    void Resume(WeatherVibe::Profile& profile);
    void SkipHold(WeatherVibe::Profile& profile);

    std::string Describe(WeatherVibe::Profile const& profile) const;
    // Runs the live selection and timing rules for `hours` and reports the share of time per state.
    std::array<float, WV_STATES> Simulate(WeatherVibe::Profile const& profile, uint32 hours) const;

private:
    WeatherVibeEngine() = default;

    void LoadGlobals(WeatherVibe::ConfigView const& cfg);
    bool LoadProfile(WeatherVibe::ConfigView const& cfg, WeatherVibe::Profile& profile) const;
    void LoadNeighbors(WeatherVibe::ConfigView const& cfg);
    void ValidateProfile(WeatherVibe::Profile const& profile) const;
    void WarmStart(WeatherVibe::Profile& profile);

    void UpdateProfile(WeatherVibe::Profile& profile, uint32 diff);
    void AdvanceTransition(WeatherVibe::Profile& profile, uint32 diff);
    void PickNext(WeatherVibe::Profile& profile);
    void Begin(WeatherVibe::Profile& profile, WeatherState state, float pct);
    void Settle(WeatherVibe::Profile& profile);
    void MarkDirty(WeatherVibe::Profile& profile);
    void FlushDirty();

    void BuildTable(WeatherVibe::Profile const& profile, WeatherVibe::WeatherTable& out, bool useFronts) const;
    WeatherVibe::TransitionPlan PlanTransition(WeatherVibe::TransitionSettings const& ts, DayPart dp,
        WeatherState fromState, float fromRaw, WeatherState toState, float toPct, float toRaw) const;
    uint32 CalcHoldMs(WeatherVibe::Profile const& profile, WeatherState state, uint32 transitionMs) const;

    void Debug(WeatherVibe::Profile const& profile, std::string const& line) const;

    std::vector<WeatherVibe::Profile>  _profiles;
    std::unordered_map<uint32, uint16> _zoneToProfile;
    std::unordered_map<uint32, uint16> _areaToProfile;

    // Global tuning
    bool   _autoEngine = true;          // WeatherVibe.Profile.Enable
    WeatherVibe::PickRules _rules;      // WeatherVibe.Escalation.Strict, WeatherVibe.FogBridge.Boost
    float  _pace = 1.0f;
    float  _frontStrength = 1.5f;
    uint32 _tickMs = 4000;
    uint32 _reapplyMs = 0;
    uint32 _reapplyTimerMs = 0;
    float  _holdMult[WV_STATES];
    float  _dayPartWeight[WV_DAYPARTS][WV_STATES];

    bool _anyDirty = false;
    WeatherVibe::WeatherTable _scratchTable;
};

#define sWeatherVibeEngine WeatherVibeEngine::Instance()

#endif // MOD_WV_ENGINE_H
