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

    // One stretch of a transition: a state shown while its grade goes from -> to.
    //   streamed: the server moves the grade in small eased steps. Right for rain, snow and a
    //             sky, which the client updates in place.
    //   one shot: the grade is set to its end value at the start and left alone, because the
    //             stock client restarts a sandstorm from nothing on every packet about it.
    //             The client does the fading; the leg only waits for it.
    struct Leg
    {
        WeatherState state = WEATHER_STATE_FINE;
        float        from = 0.0f;
        float        to = 0.0f;
        uint32       ms = 0;
        bool         oneShot = false;
    };

    // How to get from one weather to another.
    struct TransitionPlan
    {
        Leg    legs[2];
        uint8  legCount = 0;
        uint32 durationMs = 0;
        bool   keepGrade = false;   // nothing is to change on screen: stay at the current grade
    };

    enum class Phase : uint8 { HOLD, TRANSITION };

    // Live weather of a profile — shared by every zone and area mapped to it.
    struct LiveState
    {
        bool         started = false;

        // What clients are shown right now.
        WeatherState state = WEATHER_STATE_FINE;
        float        raw = 0.0f;

        // Where the weather is settled, or heading, and what it was when it set out.
        WeatherState fromState = WEATHER_STATE_FINE;
        WeatherState targetState = WEATHER_STATE_FINE;
        float        targetPct = 100.0f;
        float        targetRaw = 0.0f;
        WeatherState settledState = WEATHER_STATE_FINE;   // last state it came to rest on
        DayPart      band = DayPart::MORNING;             // whose intensity band the target was mapped with

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
        uint32 thunderTimerMs = 0;    // next clap while a storm is up; 0 = none scheduled
        uint32 rumbleTimerMs = 0;     // next look for a storm in earshot (here, coming, next door)
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

        // What a row that says "Thunders" (86) means here: a storm in the profile's own weather.
        // A "Tempest" row is the worst of that weather (see ResolveThunder).
        WeatherState              thunderAs = WEATHER_STATE_THUNDERS;
        std::vector<uint32>       thunderSounds; // this profile's own claps; empty = the global ones
        WeatherSkins              skins;         // what its rain and sandstorm look like
        bool                      hasFlashColor = false;
        uint32                    flashColor = 0;  // 0xRRGGBB of its lightning on a Waystone client

        LiveState                 live;
    };
}

// ============================================================
// WeatherVibeEngine — singleton
// Owns the profiles, runs their weather, and decides who is told.
//
// Threading: Update() and every mutating call run on the world thread while the map threads are
// idle. The zone/area hooks call Resolve()/SendCurrentTo() from map threads: they only read the
// engine, and write nothing but the record of what was last sent, which lives with the player
// they are running for.
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
    // enteredZone: the player has just changed zone. The core may have sent that client weather
    // of its own on the way in, so what it was last told by the module is repeated where
    // repeating is free (rain, snow, a sky); a sandstorm is only ever sent on a change.
    void SendCurrentTo(Player* player, uint32 zoneId, uint32 areaId, bool enteredZone = false) const;
    void OnAreaChange(Player* player, uint32 oldArea, uint32 newArea) const;
    void ForEachListener(WeatherVibe::Profile const& profile, std::function<void(Player*)> const& visit) const;
    // One-shot push to the clients in a zone the module has no profile for. Returns how many got it.
    uint32 PushToZone(uint32 zoneId, WeatherState state, float rawGrade) const;

    // 86 means "this profile's kind of thunder", and Tempest the worst of it: a tempest of rain,
    // a blizzard, or the darkest dry storm. Every other state is itself.
    static WeatherState ResolveThunder(WeatherVibe::Profile const& profile, WeatherState state)
    {
        if (state == WEATHER_STATE_THUNDERS)
            return profile.thunderAs;
        if (state == WV_STATE_TEMPEST)
            return profile.thunderAs == WEATHER_STATE_THUNDERS ? WV_STATE_TEMPEST
                : profile.thunderAs == WV_STATE_THUNDER_SNOW ? WV_STATE_BLIZZARD : WV_STATE_DRY_THUNDERS;
        return state;
    }

    // What the client is sent for a profile right now: the id and grade every client reads,
    // and the extension block a Waystone client reads besides. Read-only, safe from the hooks.
    WeatherVibe::Wire WireOf(WeatherVibe::Profile const& profile) const;

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
    // A storm on screen or on its way here, or (Thunder.Rumble.Neighbors) in a neighbouring profile.
    bool StormInEarshot(WeatherVibe::Profile const& profile) const;
    // A neighbouring profile has a storm up or coming.
    bool StormNextDoor(WeatherVibe::Profile const& profile) const;
    void AdvanceTransition(WeatherVibe::Profile& profile, uint32 diff);
    void PickNext(WeatherVibe::Profile& profile);
    // mayStay: a pick of the profile's own may decide nothing needs to change on screen (a
    // re-pick of the same sandstorm, say). A GM's request always takes effect.
    void Begin(WeatherVibe::Profile& profile, WeatherState state, float pct, bool mayStay);
    void Settle(WeatherVibe::Profile& profile);
    void MarkDirty(WeatherVibe::Profile& profile);
    void FlushDirty();

    void BuildTable(WeatherVibe::Profile const& profile, WeatherVibe::WeatherTable& out, bool useFronts) const;
    WeatherVibe::TransitionPlan PlanTransition(WeatherVibe::Profile const& profile,
        WeatherState fromState, float fromRaw, WeatherState toState, float toRaw, bool mayStay) const;
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
    bool   _reapplyDue = false;         // the next broadcast pass repeats rain and sky to everyone
    uint32 _reconcileTimerMs = 0;
    bool   _reconcileDue = false;       // the next broadcast pass checks every client, changed or not
    float  _holdMult[WV_STATES];
    float  _dayPartWeight[WV_DAYPARTS][WV_STATES];

    bool _anyDirty = false;
    WeatherVibe::WeatherTable _scratchTable;
    std::vector<WeatherVibe::Wire> _scratchWires;   // one per profile, for a broadcast pass
};

#define sWeatherVibeEngine WeatherVibeEngine::Instance()

#endif // MOD_WV_ENGINE_H
