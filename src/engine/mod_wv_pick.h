#ifndef MOD_WV_PICK_H
#define MOD_WV_PICK_H

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

    Family GetFamily(WeatherState state);
    // 0 = standalone (Fine, Fog, Thunders), 1 = light, 2 = medium, 3 = heavy.
    uint8  GetTier(WeatherState state);
    // Two states whose grade means the same thing to the eye, so one can become the other by
    // simply moving the grade: two strengths of the same rain, snow or sandstorm, or a clear
    // sky and fog (neither has particles; both are only a matter of how heavy the air looks).
    bool   IsContinuousPair(WeatherState a, WeatherState b);

    // A thunderstorm grows out of real rain when the table has some: it then counts as the top
    // of the rain ladder. A table that uses thunder on its own (volcanic rumble, arcane storms)
    // keeps it as a standalone state.
    bool   IsThunderCoupled(WeatherTable const& table);

    // Whether `candidate` can follow `current` without skipping a step.
    //   strict: calm and light weather (Fine, Fog, lone thunder, any light tier) mix freely;
    //           medium and heavy are reached and left one tier at a time, in both directions.
    //   legacy: only escalation is stepped; Fine, Fog and Thunders are always reachable.
    bool   IsNaturalTransition(WeatherState current, WeatherState candidate, bool thunderCoupled, bool strict);

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
