# WeatherVibe (AzerothCore module)

[![Intro!](https://i3.ytimg.com/vi/iCn14KLCerY/maxresdefault.jpg)](https://www.youtube.com/watch?v=iCn14KLCerY)

Weather that belongs to the place it falls on. Each zone has a **profile**: what the sky does
there in each season, how hard, and how quickly it changes. Elwynn is mild with misty mornings,
Duskwood never quite clears, Stranglethorn gets real downpours, the Storm Peaks get real
blizzards. Weather builds and eases a step at a time, drifts between neighbouring zones, and
turns gradually with the calendar.

This is a fork of [hermensbas/mod_weather_vibe](https://github.com/hermensbas/mod_weather_vibe).
The profiles and the idea are that project's; the engine, delivery and world features were
reworked here. Demos of the original: <https://youtu.be/GRyTk5umEno>, <https://youtu.be/23jTJcT-n44>.

> **Required:** set `ActivateWeather = 0` in `worldserver.conf`. Otherwise the core's own
> weather fights this module. The module logs an error at startup if it is still on.

---

## What it does

- **Profiles per zone.** Weighted weather tables per season, per profile. Zones that share a
  profile share one sky: Stormwind and Elwynn, Orgrimmar and Durotar, a battleground and the
  zone it stands for.
- **Every map.** Open world, battlegrounds, raids and dungeons all work the same way.
- **Only real players are told.** Playerbots never receive a weather packet.
- **Smooth changes.** Intensity moves continuously in small eased steps. Light rain becomes
  medium rain without a dip; a clear sky clouds over before the first drops.
- **One step at a time.** From a calm sky weather starts light; medium follows light, heavy
  follows medium, and it eases off the same way. A thunderstorm grows out of real rain.
- **Weights mean what they say.** A state with 10% of a table's weight is in the sky about 10%
  of the time, including the heavy tiers that stepping would otherwise starve.
- **Time of day.** Mist at dawn and after dark, thunder in the afternoon and evening.
- **Seasons that turn gradually.** Around each equinox and solstice the two seasons' tables
  are mixed.
- **Weather fronts.** Heavy weather in a neighbouring zone makes the same weather likelier
  here, if this zone has that kind of weather at all. A desert never inherits snow.
- **Microclimates.** A single area can have its own profile inside its zone: the plague
  cauldron fields, a valley where the blizzard never stops.
- **Holidays.** Snow over Elwynn at Winter Veil, mist at Hallow's End, clear skies at Midsummer.
- **The world notices.** Now and then an NPC standing near you remarks that the weather has
  turned. Lua scripts are told too, so events can follow the weather.
- **Live tuning.** `.wvibe reload` re-reads the config without resetting the sky.

---

## Installation

1. Put the module in your AzerothCore `modules` folder and rebuild.
2. In `worldserver.conf` set `ActivateWeather = 0`.
3. Install `mod_weather_vibe.conf.dist` in your config's `modules` folder and start the server.

### The `.conf` and the `.conf.dist`

AzerothCore only reads a key from `mod_weather_vibe.conf` if `mod_weather_vibe.conf.dist` has
the same key. Changing a value in your `.conf` always works. **Adding** a line does not, unless
you add it to the `.conf.dist` too. This matters here because much of the config is numbered
lines you are meant to add to: zone mappings, profile rows, areas, remarks.

The simplest setup is to keep no `.conf` and edit the installed `.conf.dist`.

### Upgrading from an older version

- Replace your `.conf` with the new `.conf.dist` and re-apply your own changes. An old `.conf`
  still loads, but it overrides the corrected profile data and the new defaults with the old
  values.
- `Transition.SeasonBlendMinutes` is retired (seasons now blend by calendar). An old `.conf`
  that still has those lines logs "Found incorrect option" for each at startup; delete them.
- `WeatherVibe.Debug` no longer prints anything to players. It writes to the server log.

---

## Commands

Read-only commands are GM-only unless `WeatherVibeCommand = 1`. Commands that change the
weather are always GM-only; `reload` needs administrator level.

| Command | What it does |
|---|---|
| `.wvibe status` | Season, daypart, and one line per profile |
| `.wvibe status here` / `<profile>` | That profile in detail: state, intensity, time to the next change, what it covers, its neighbours |
| `.wvibe where` | Zone and area ids and names where you stand, and the profile that covers them |
| `.wvibe set <zoneId> <state> <pct>` | Send that zone's profile there; afterwards it carries on by itself |
| `.wvibe force <here\|profile> <state> <pct> [minutes]` | Go there and stay, for that long or until released |
| `.wvibe resume [here\|profile\|all]` | Release a forced profile |
| `.wvibe next [here\|profile]` | Pick the next weather now |
| `.wvibe setRaw <zoneId> <state> <raw>` | Calibration: snap to a raw grade (0..1) and stay there until `resume` |
| `.wvibe simulate <here\|profile> [hours]` | Run the real selection rules and print how much of the time each state gets |
| `.wvibe debug on\|off` | See the engine's decisions in chat for the weather you stand in |
| `.wvibe reload` | Re-read the config files and re-apply this module; running weather is kept |

`<state>` is a number or a name: `0 Fine`, `1 Fog`, `3 LightRain`, `4 MediumRain`,
`5 HeavyRain`, `6 LightSnow`, `7 MediumSnow`, `8 HeavySnow`, `22 LightSandstorm`,
`41 MediumSandstorm`, `42 HeavySandstorm`, `86 Thunders`.

`set`, `force` and `setRaw` act on the whole profile, so synced zones stay in sync.

---

## Configuration

Everything is in `mod_weather_vibe.conf.dist`, with a comment on each key. The outline:

### Profiles

```ini
# WeatherVibe.Profile.<Name>.<Season>.<N> = weight, state, intensityMinPct, intensityMaxPct
WeatherVibe.Profile.DunMorogh.Spring.1=10,0,40,70
WeatherVibe.Profile.DunMorogh.Spring.2=15,1,30,60
WeatherVibe.Profile.DunMorogh.Spring.4=40,6,30,70
```

A row is one kind of weather: how often (its weight against the other rows) and how strong
(a percentage range). For `Fine` the percentage is how clear the sky is: 100 is cloudless, low
is overcast. A profile may leave a season out; it then borrows a neighbouring one.

Per profile, under `WeatherVibe.Profile.<Name>.`:

| Key | Meaning |
|---|---|
| `Transition.Max.Changes.Per.Hour` | How often the weather changes |
| `Transition.MinHoldSeconds` | Least time weather stays once it has arrived |
| `Transition.Step.TimeSeconds.Min/Max`, `Transition.StepSize.Perc` | How long a change takes: one step of time per `StepSize.Perc` of intensity travelled |
| `Transition.Max.Consecutive.Same` | Same state in a row before a different one is wanted |
| `DayPartWeight.<DAYPART>.<State>` | This profile's own time-of-day multiplier |
| `Neighbors` | Profiles whose storms drift here |

### Where a profile applies

```ini
WeatherVibe.ZoneProfile.7=12,ElwynnForest      # zone id, profile
WeatherVibe.ZoneProfile.8=1519,ElwynnForest    # Stormwind shares Elwynn's sky
WeatherVibe.AreaProfile.1=199,PlagueCauldron   # one area inside a zone
```

An area's profile beats its zone's. Use the id of the innermost area; `.wvibe where` prints
it. Ids are checked at startup, and anything unknown is reported in the server log.

### Global

| Key | Default | Meaning |
|---|---|---|
| `WeatherVibe.Pace` | 1.0 | Multiplies every profile's changes per hour. 0.5 = weather lasts twice as long |
| `WeatherVibe.Hold.<State>` | 0.6 to 1.5 | How long each kind of weather lingers. Storms pass, fine spells last |
| `WeatherVibe.Escalation.Strict` | 1 | Step a tier at a time in both directions, and balance picks to the weights |
| `WeatherVibe.FogBridge.Boost` | 1.5 | Extra fog weight when the sky is clear or the weather light |
| `WeatherVibe.Front.Strength` | 1.5 | How strongly neighbours' storms pull. 0 = off |
| `WeatherVibe.Season.BlendDays` | 30 | Days either side of a season boundary over which tables mix. 0 = overnight |
| `WeatherVibe.DayPartWeight.<DAYPART>.<State>` | see file | Time-of-day multiplier for all profiles |
| `WeatherVibe.Intensity.InternalRange.<DAYPART>.<State>` | see file | The raw grade band a state's 0..100% maps onto |
| `WeatherVibe.Holiday.<holidayId>.<Profile>.<N>` | see file | Extra rows while a holiday runs |
| `WeatherVibe.Remarks.*` | on, 15%, 30 min | NPC remarks and their lines |
| `WeatherVibe.Hook.Enable` / `.ResyncMinutes` | 1 / 10 | The weather-change hook for other scripts |

### Check before you deploy

```bash
python tools/wv_lint.py conf/mod_weather_vibe.conf.dist
python tools/wv_lint.py conf/mod_weather_vibe.conf.dist --stats   # share of each kind of weather
```

Reports typos (a minimum above a maximum), duplicate rows, a profile that contradicts its own
header ("no snow" with a snow row), tiers that can rarely be reached, unmapped profiles and
unknown keys. It reads only the file; it cannot tell whether an id is the place you meant.

---

## Reacting to the weather from Lua

When a profile's weather settles, the module fires the core's weather-change hook for each of
its open-world zones. With mod-ale that is server event 25:

```lua
local RAIN = { [3] = true, [4] = true, [5] = true, [86] = true }

RegisterServerEvent(25, function(event, zoneId, state, grade)
    if zoneId == 12 and RAIN[state] then
        -- it is raining in Elwynn
    end
end)
```

The event repeats every `Hook.ResyncMinutes`, so a script loaded with `.reload ale` learns the
current weather without waiting for it to change. Battleground and raid zones do not fire it.

Do not set weather from Lua with `Map:SetWeather`. It creates a core weather object that keeps
regenerating on its own and fights this module in that zone. Use `.wvibe force` or `.wvibe set`.

---

## Calibrating to your client

What a grade looks like is decided by the client, and clients differ (HD patches especially).
Two defaults are worth five minutes of checking, standing outdoors in a mapped zone:

| Try | Look for | If not |
|---|---|---|
| `.wvibe setRaw <zone> 0 0.0`, then `0 0.3` | The sky at 0.3 is duller but nothing falls | Lower Fine's `InternalRange` maximum |
| `.wvibe setRaw <zone> 3 0.6`, then `4 0.6` | Light and medium rain look the same at the same grade | Make the Light/Medium/Heavy ranges contiguous instead of overlapping |
| `.wvibe setRaw <zone> 3 0.25` | Rain is just barely visible | Raise or lower the rain ranges' minimum to where it first shows |
| `.wvibe setRaw <zone> 86 0.3` | What Thunders draws on your client | Adjust its range, or its weights |

`.wvibe resume` when done.

---

## Troubleshooting

**The weather keeps snapping back.** `ActivateWeather` is still 1, or a script is calling
`Map:SetWeather`.

**A zone has no weather.** `.wvibe where` shows whether a profile covers it. Check the server
log from startup for a warning about that id or profile.

**I added a mapping and it does nothing.** It is only in your `.conf`. Add it to the
`.conf.dist` too (see above).

**Weather changes too often, or not often enough.** `WeatherVibe.Pace` for everything; a
profile's `Max.Changes.Per.Hour` and `MinHoldSeconds` for one place; `WeatherVibe.Hold.<State>`
for one kind of weather.

**It is never heavy enough, or too often.** `.wvibe simulate <profile> 200` prints what the
table really delivers. Change the weights and `.wvibe reload`.

**A boss fight's weather.** Zones without a profile are left to the core, so scripted weather
in raids and dungeons shows as intended. If you map a zone that a core script also drives, the
profile wins.

---

## How it works

[DESIGN.md](DESIGN.md) has the architecture and the reasons behind it.

## License

Same license as the upstream project; see [LICENSE](LICENSE).
