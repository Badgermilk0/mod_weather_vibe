#include "mod_wv_pick.h"

#include "Random.h"

#include <algorithm>
#include <cstdlib>

namespace WeatherVibe
{
    namespace
    {
        struct Kind
        {
            Family family;
            uint8  tier;
        };

        Kind Classify(WeatherState state, bool thunderCoupled)
        {
            if (state == WEATHER_STATE_THUNDERS && thunderCoupled)
                return { Family::RAIN, 3 };
            return { GetFamily(state), GetTier(state) };
        }
    }

    Family GetFamily(WeatherState state)
    {
        switch (state)
        {
        case WEATHER_STATE_FOG:              return Family::FOG;
        case WEATHER_STATE_LIGHT_RAIN:
        case WEATHER_STATE_MEDIUM_RAIN:
        case WEATHER_STATE_HEAVY_RAIN:       return Family::RAIN;
        case WEATHER_STATE_LIGHT_SNOW:
        case WEATHER_STATE_MEDIUM_SNOW:
        case WEATHER_STATE_HEAVY_SNOW:       return Family::SNOW;
        case WEATHER_STATE_LIGHT_SANDSTORM:
        case WEATHER_STATE_MEDIUM_SANDSTORM:
        case WEATHER_STATE_HEAVY_SANDSTORM:  return Family::SANDSTORM;
        case WEATHER_STATE_THUNDERS:         return Family::THUNDER;
        case WEATHER_STATE_FINE:
        default:                             return Family::FINE;
        }
    }

    uint8 GetTier(WeatherState state)
    {
        switch (state)
        {
        case WEATHER_STATE_LIGHT_RAIN:
        case WEATHER_STATE_LIGHT_SNOW:
        case WEATHER_STATE_LIGHT_SANDSTORM:    return 1;
        case WEATHER_STATE_MEDIUM_RAIN:
        case WEATHER_STATE_MEDIUM_SNOW:
        case WEATHER_STATE_MEDIUM_SANDSTORM:   return 2;
        case WEATHER_STATE_HEAVY_RAIN:
        case WEATHER_STATE_HEAVY_SNOW:
        case WEATHER_STATE_HEAVY_SANDSTORM:    return 3;
        default:                               return 0;
        }
    }

    bool IsContinuousPair(WeatherState a, WeatherState b)
    {
        if (a == b)
            return false;

        if (GetTier(a) > 0 && GetTier(b) > 0)
            return GetFamily(a) == GetFamily(b);

        return (a == WEATHER_STATE_FINE && b == WEATHER_STATE_FOG) || (a == WEATHER_STATE_FOG && b == WEATHER_STATE_FINE);
    }

    bool IsThunderCoupled(WeatherTable const& table)
    {
        for (WeatherEntry const& entry : table)
            if (entry.weight > 0.0f &&
                (entry.state == WEATHER_STATE_MEDIUM_RAIN || entry.state == WEATHER_STATE_HEAVY_RAIN))
                return true;
        return false;
    }

    bool IsNaturalTransition(WeatherState current, WeatherState candidate, bool thunderCoupled, bool strict)
    {
        if (candidate == current)
            return true;

        if (!strict)
        {
            // Legacy rule: standalone always reachable, +/-1 tier within family,
            // cross-family only at tier 1.
            uint8 candidateTier = GetTier(candidate);
            if (candidateTier == 0) return true;

            uint8 currentTier = GetTier(current);
            if (currentTier == 0) return candidateTier == 1;

            if (GetFamily(current) == GetFamily(candidate))
                return std::abs(int(candidateTier) - int(currentTier)) <= 1;

            return candidateTier == 1;
        }

        Kind const from = Classify(current, thunderCoupled);
        Kind const to = Classify(candidate, thunderCoupled);

        // Calm and light weather turn into one another freely: a clear sky, fog, thunder used
        // on its own, and the light tier of any rain, snow or sandstorm.
        if (from.tier <= 1 && to.tier <= 1)
            return true;

        // Medium and heavy are reached, and left, one tier at a time within their own kind.
        // The rule reads the same in both directions, which PickEntry's balancing relies on.
        return from.family == to.family && std::abs(int(to.tier) - int(from.tier)) <= 1;
    }

    bool IsFogBridge(WeatherState current, WeatherState candidate)
    {
        if (candidate != WEATHER_STATE_FOG)
            return false;

        // Fine -> Fog ("something's coming"), light precipitation -> Fog ("clearing up")
        return current == WEATHER_STATE_FINE || GetTier(current) == 1;
    }

    int PickWeighted(WeatherTable const& table)
    {
        float total = 0.0f;
        for (WeatherEntry const& entry : table)
            if (entry.weight > 0.0f)
                total += entry.weight;

        if (total <= 0.0f)
            return -1;

        float roll = frand(0.0f, total);
        float accumulated = 0.0f;
        int last = -1;

        for (size_t i = 0; i < table.size(); ++i)
        {
            if (table[i].weight <= 0.0f)
                continue;

            last = int(i);
            accumulated += table[i].weight;
            if (roll < accumulated)
                return last;
        }

        return last;
    }

    int PickEntry(WeatherTable const& table, WeatherState current, uint32 maxConsecSame,
        uint32 consecCount, PickRules const& rules)
    {
        if (table.empty())
            return -1;

        bool const strict = rules.strict;
        bool const thunderCoupled = strict && IsThunderCoupled(table);

        // Once a state has been picked maxConsecSame times running it is no longer offered.
        // Balancing (below) may still hold the weather where it is — a long blizzard is the
        // point of a profile that asks for one — but only up to twice that count.
        bool const wantChange = consecCount >= maxConsecSame;
        bool const mustChange = consecCount >= std::max(maxConsecSame * 2, maxConsecSame + 1);

        // Total weight of everything that may naturally follow `from`.
        auto reach = [&](WeatherState from)
            {
                float total = 0.0f;
                for (WeatherEntry const& entry : table)
                    if (entry.weight > 0.0f && IsNaturalTransition(from, entry.state, thunderCoupled, strict))
                        total += entry.weight;
                return total;
            };

        WeatherTable eligible(table.size());

        // Pass 0: a natural step, and a different state once the same one has run its course.
        // Pass 1: variety only — taken when the natural steps are a dead end, so the weather
        //         can never get stuck repeating a state it has no stepped way out of.
        // Pass 2: anything with weight.
        for (int pass = 0; pass < 3; ++pass)
        {
            for (size_t i = 0; i < table.size(); ++i)
            {
                WeatherEntry const& entry = table[i];
                eligible[i] = entry;
                eligible[i].weight = 0.0f;

                if (entry.weight <= 0.0f)
                    continue;
                if (pass == 0 && !IsNaturalTransition(current, entry.state, thunderCoupled, strict))
                    continue;
                if (pass < 2 && wantChange && entry.state == current)
                    continue;

                eligible[i].weight = entry.weight;
                if (IsFogBridge(current, entry.state))
                    eligible[i].weight *= rules.fogBridge;
            }

            int const picked = PickWeighted(eligible);
            if (picked < 0)
                continue;

            // Stepping alone starves the upper tiers: heavy rain can only be entered from medium,
            // yet every state that touches it also has plenty of other places to go, so it would
            // turn up a fraction as often as its weight says. Balance that the Metropolis way:
            // a step towards a state with more ways out than the current one is taken only in
            // proportion (reach here / reach there); otherwise the weather stays for another
            // spell. Over time each state then gets the share its weight asks for, without ever
            // skipping a tier.
            if (strict && pass == 0 && !mustChange && table[size_t(picked)].state != current)
            {
                float const here = reach(current);
                float const there = reach(table[size_t(picked)].state);

                if (there > here && frand(0.0f, there) >= here)
                {
                    for (size_t i = 0; i < table.size(); ++i)
                    {
                        eligible[i] = table[i];
                        if (table[i].state != current)
                            eligible[i].weight = 0.0f;
                    }

                    // Stay — unless the table no longer has the current state at all.
                    int const stay = PickWeighted(eligible);
                    if (stay >= 0)
                        return stay;
                }
            }

            return picked;
        }

        return -1;
    }
}
