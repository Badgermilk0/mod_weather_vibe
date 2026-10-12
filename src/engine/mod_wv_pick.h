#ifndef MOD_WV_PICK_H
#define MOD_WV_PICK_H

#include "mod_wv_states.h"

#include "Define.h"
#include "Weather.h"          // WeatherState enum

#include <vector>

// Selection rules: which weather may follow which, and the weighted draw itself.
// Nothing here knows about profiles, players or timers — it only looks at a table and the
// current state, so `.wvibe simulate` can run exactly what the live engine runs.
namespace WeatherVibe
{
    enum class Family : uint8 { FINE, FOG, RAIN, SNOW, SANDSTORM, THUNDER };

    // One weighted row of a weather table.
    struct WeatherEntry
    {
        float        weight = 0.0f;
        WeatherState state = WEATHER_STATE_FINE;
        float        minPct = 30.0f;
        float        maxPct = 100.0f;
    };

    using WeatherTable = std::vector<WeatherEntry>;

    // How picky the draw is (global config).
    struct PickRules
    {
        bool  strict = true;       // WeatherVibe.Escalation.Strict
        float fogBridge = 1.5f;    // WeatherVibe.FogBridge.Boost
    };

    // What the client actually draws for a state, whatever its sound or its name: rain
    // particles, snow particles, sandstorm particles, or nothing but a sky.
    enum class Ground : uint8 { SKY, RAIN, SNOW, SAND };

    Ground GroundOf(WeatherState state);

    // Any of the three thunder states is Family::THUNDER.
    Family GetFamily(WeatherState state);
    // 0 = standalone (Fine, Fog, thunder), 1 = light, 2 = medium, 3 = heavy.
    uint8  GetTier(WeatherState state);
    // Two states the client draws the same way, so one can become the other by simply moving
    // the grade: strengths of the same rain, snow or sandstorm, with a thunderstorm counting as
    // the rain (or snow) it is drawn as; and the states that are only a sky — a clear one, fog,
    // a dry storm.
    bool   IsContinuousPair(WeatherState a, WeatherState b);

    // A thunderstorm grows out of real weather when the table has it: with medium or heavy rain
    // present, Thunders counts as the top of the rain ladder, and ThunderSnow likewise over
    // medium or heavy snow. Otherwise thunder stands alone, as calm weather that may follow
    // anything light (a cloudburst, or a dry storm that never had a ladder).
    struct ThunderCoupling
    {
        bool rain = false;
        bool snow = false;
    };

    ThunderCoupling GetThunderCoupling(WeatherTable const& table);

    // Whether `candidate` can follow `current` without skipping a step.
    //   strict: calm and light weather (Fine, Fog, lone thunder, any light tier) mix freely;
    //           medium and heavy are reached and left one tier at a time, in both directions.
    //   legacy: only escalation is stepped; Fine, Fog and thunder are always reachable.
    bool   IsNaturalTransition(WeatherState current, WeatherState candidate, ThunderCoupling coupling, bool strict);

    // Fog as the bridge between a clear sky and light precipitation, in either direction.
    bool   IsFogBridge(WeatherState current, WeatherState candidate);

    // Plain weighted draw. Returns an index into the table, or -1 when nothing has weight.
    int    PickWeighted(WeatherTable const& table);

    // Weighted draw that honours escalation and variety. Three passes, loosening each time:
    // natural + variety, variety only, anything. In strict mode the natural pass is balanced
    // so that, over time, each state turns up in proportion to its weight even though the
    // upper tiers can only be entered from the tier below. A pick of the current state means
    // "stay, at a new intensity". Returns an index into the table, or -1.
    int    PickEntry(WeatherTable const& table, WeatherState current, uint32 maxConsecSame,
        uint32 consecCount, PickRules const& rules);
}

#endif // MOD_WV_PICK_H
