// pick_test.cpp — offline behaviour test for the selection rules.
//
// Links the module's real src/engine/mod_wv_pick.cpp (nothing else from the server) and runs it
// over every season table in a conf: 80,000 picks per table. It fails if a state is never
// reached or the weather repeats a state past its limit, and prints how far each state's share
// of picks is from its share of weight.
//
// Not part of the module build (AzerothCore only compiles src/). Build it by hand with the
// core's include directories on the path, e.g. with MSVC:
//
//     cl /std:c++20 /EHsc /utf-8 /O2 /DNOMINMAX @<file with one /I"dir" per AC include dir>
//        tools\pick_test.cpp src\engine\mod_wv_pick.cpp /Fe:pick_test.exe
//     pick_test.exe conf\mod_weather_vibe.conf.dist        (strict rules)
//     pick_test.exe conf\mod_weather_vibe.conf.dist 0      (legacy rules, for comparison)
//
// frand() is supplied here, so the server's Random.cpp is not needed.
#include "mod_wv_pick.h"
#include "Random.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <regex>
#include <string>
#include <vector>

using namespace WeatherVibe;

static std::mt19937 gRng(12345);
float frand(float min, float max) { return std::uniform_real_distribution<float>(min, max)(gRng); }

static char const* Name(WeatherState s)
{
    switch (s)
    {
    case WEATHER_STATE_FINE:             return "Fine";
    case WEATHER_STATE_FOG:              return "Fog";
    case WEATHER_STATE_LIGHT_RAIN:       return "LightRain";
    case WEATHER_STATE_MEDIUM_RAIN:      return "MediumRain";
    case WEATHER_STATE_HEAVY_RAIN:       return "HeavyRain";
    case WEATHER_STATE_LIGHT_SNOW:       return "LightSnow";
    case WEATHER_STATE_MEDIUM_SNOW:      return "MediumSnow";
    case WEATHER_STATE_HEAVY_SNOW:       return "HeavySnow";
    case WEATHER_STATE_LIGHT_SANDSTORM:  return "LightSandstorm";
    case WEATHER_STATE_MEDIUM_SANDSTORM: return "MediumSandstorm";
    case WEATHER_STATE_HEAVY_SANDSTORM:  return "HeavySandstorm";
    case WEATHER_STATE_THUNDERS:         return "Thunders";
    case WV_STATE_THUNDER_SNOW:          return "ThunderSnow";
    case WV_STATE_DRY_THUNDERS:          return "DryThunders";
    default:                             return "?";
    }
}

// A row's state: a number, or one of the two names that have no number worth remembering.
static WeatherState ParseState(std::string const& token)
{
    if (token == "ThunderSnow") return WV_STATE_THUNDER_SNOW;
    if (token == "DryThunders") return WV_STATE_DRY_THUNDERS;
    return WeatherState(std::stoul(token));
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: pick_test <conf> [strict=1]\n");
        return 2;
    }

    PickRules rules;
    rules.strict = argc < 3 || std::string(argv[2]) != "0";

    std::map<std::string, WeatherTable> tables;   // "Profile.Season" -> rows
    std::map<std::string, uint32> maxConsec;      // profile -> Max.Consecutive.Same

    std::ifstream in(argv[1]);
    std::regex row(R"(^WeatherVibe\.Profile\.(\w+)\.(Spring|Summer|Autumn|Winter)\.\d+\s*=\s*([\d.]+)\s*,\s*(\d+|ThunderSnow|DryThunders)\s*,\s*([\d.]+)\s*,\s*([\d.]+))");
    std::regex consec(R"(^WeatherVibe\.Profile\.(\w+)\.Transition\.Max\.Consecutive\.Same\s*=\s*(\d+))");
    std::string line;
    std::smatch m;

    while (std::getline(in, line))
    {
        if (std::regex_search(line, m, row))
        {
            WeatherEntry entry;
            entry.weight = std::stof(m[3]);
            entry.state = ParseState(m[4]);
            entry.minPct = std::stof(m[5]);
            entry.maxPct = std::stof(m[6]);
            tables[m[1].str() + "." + m[2].str()].push_back(entry);
        }
        else if (std::regex_search(line, m, consec))
            maxConsec[m[1].str()] = uint32(std::stoul(m[2]));
    }

    // "Thunders" (86) means the storm that suits the profile, decided from all its tables —
    // the same rule as WeatherVibeEngine::LoadProfile. (Thunder.Style overrides are not read here.)
    std::map<std::string, std::pair<bool, bool>> weatherOf;   // profile -> (has rain, has snow)
    for (auto const& [name, table] : tables)
    {
        auto& [hasRain, hasSnow] = weatherOf[name.substr(0, name.find('.'))];
        for (WeatherEntry const& entry : table)
        {
            if (GetTier(entry.state) == 0)
                continue;
            hasRain = hasRain || GetFamily(entry.state) == Family::RAIN;
            hasSnow = hasSnow || GetFamily(entry.state) == Family::SNOW;
        }
    }

    for (auto& [name, table] : tables)
    {
        auto const& [hasRain, hasSnow] = weatherOf[name.substr(0, name.find('.'))];
        WeatherState const thunderAs = hasRain ? WEATHER_STATE_THUNDERS : hasSnow ? WV_STATE_THUNDER_SNOW : WV_STATE_DRY_THUNDERS;
        for (WeatherEntry& entry : table)
            if (entry.state == WEATHER_STATE_THUNDERS)
                entry.state = thunderAs;
    }

    struct Skew { double ratio; std::string what; double expected, got; };
    std::vector<Skew> skews;
    int problems = 0;
    constexpr int PICKS = 80000;

    for (auto const& [name, table] : tables)
    {
        std::string const profile = name.substr(0, name.find('.'));
        uint32 const limit = maxConsec.count(profile) ? maxConsec[profile] : 3;

        float total = 0.0f;
        std::map<WeatherState, int> hits;
        std::map<WeatherState, float> weight;
        for (WeatherEntry const& entry : table)
        {
            total += entry.weight;
            weight[entry.state] += entry.weight;
        }

        WeatherState current = table[size_t(PickWeighted(table))].state;
        uint32 consecCount = 1, longestRun = 1;

        for (int i = 0; i < PICKS; ++i)
        {
            int const index = PickEntry(table, current, limit, consecCount, rules);
            if (index < 0)
            {
                std::printf("FAIL  %-30s nothing to pick from %s\n", name.c_str(), Name(current));
                ++problems;
                break;
            }

            WeatherState const next = table[size_t(index)].state;
            consecCount = (next == current) ? consecCount + 1 : 1;
            longestRun = std::max(longestRun, consecCount);
            current = next;
            ++hits[next];
        }

        for (auto const& [state, w] : weight)
        {
            double const expected = w / total, got = double(hits[state]) / PICKS;
            if (hits[state] == 0)
            {
                std::printf("FAIL  %-30s %s (weight %.0f) is never reached\n", name.c_str(), Name(state), w);
                ++problems;
            }
            skews.push_back({ got / expected, name + " " + Name(state), expected * 100, got * 100 });
        }

        // Balancing may hold a state for up to twice its limit (see PickEntry).
        uint32 const hardLimit = rules.strict ? std::max(limit * 2, limit + 1) : limit;
        if (longestRun > hardLimit + 1)
        {
            std::printf("RUN   %-30s same state %u times in a row (limit %u)\n", name.c_str(), longestRun, hardLimit);
            ++problems;
        }
    }

    std::sort(skews.begin(), skews.end(), [](Skew const& a, Skew const& b) { return a.ratio < b.ratio; });

    size_t within25 = 0, within50 = 0, underHalf = 0, underQuarter = 0, overDouble = 0;
    for (Skew const& s : skews)
    {
        if (s.ratio >= 0.75 && s.ratio <= 1.25) ++within25;
        if (s.ratio >= 0.5 && s.ratio <= 1.5) ++within50;
        if (s.ratio < 0.5) ++underHalf;
        if (s.ratio < 0.25) ++underQuarter;
        if (s.ratio > 2.0) ++overDouble;
    }

    std::printf("%zu season tables, %zu states, %s rules: %d hard problem(s)\n",
        tables.size(), skews.size(), rules.strict ? "strict" : "legacy", problems);
    std::printf("  share of picks against share of weight:\n");
    std::printf("    within 25%%: %zu   within 50%%: %zu   under half: %zu   under a quarter: %zu   over double: %zu\n",
        within25, within50, underHalf, underQuarter, overDouble);

    std::printf("  most under-picked:\n");
    for (size_t i = 0; i < std::min<size_t>(8, skews.size()); ++i)
        std::printf("    %-40s wanted %5.1f%%  got %5.1f%%\n", skews[i].what.c_str(), skews[i].expected, skews[i].got);

    std::printf("  most over-picked:\n");
    for (size_t i = 0; i < std::min<size_t>(5, skews.size()); ++i)
    {
        Skew const& s = skews[skews.size() - 1 - i];
        std::printf("    %-40s wanted %5.1f%%  got %5.1f%%\n", s.what.c_str(), s.expected, s.got);
    }

    return problems ? 1 : 0;
}
