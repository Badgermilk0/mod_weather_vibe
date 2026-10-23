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
- **Smooth changes.** Rain, snow and the sky move continuously in small eased steps. Light
  rain becomes medium rain without a dip; a clear sky clouds over, a silent drizzle starts,
  then the rain.
- **Built on how the client really draws weather.** The 3.3.5a client's weather code was read,
  not guessed at. Rain is never heard before it can be seen, the sound always matches how much
  is falling, fine days are bright, and sandstorms are not made to flicker (the client
  restarts those every time it is told about them, so it is told once).
- **The whole scale.** From a cloudless sky (`Clear`) through fair days, mist, showers and
  downpours to the absolute storm (`Tempest`) and the `Blizzard`, each where it belongs: deserts
  bake under clear skies and almost never see rain, the gloomy lands never truly clear, the
  jungles, coasts and open sea get the tempests, the high north the blizzards.
- **One step at a time.** From a calm sky weather starts light; medium follows light, heavy
  follows medium, a tempest only grows out of a downpour or a thunderstorm, and it eases off
  the same way. A cloudless sky only follows a fair one.
- **Real thunderstorms.** The stock client has none: what the core calls "Thunders" is rain
  with its thunder bed in place of the rain sound. Here a storm is a downpour, thundersnow or a
  dark dry sky, whichever suits the zone. On the Waystone client it brings the client's own
  lightning - flashes, layered thunder that arrives late from far strikes, fel-green in fel
  lands, violet in Netherstorm, orange through volcanic ash - and storms next door flicker on
  the horizon before they arrive. On a stock client the server plays the thunderclaps and the
  distant rumble instead (`WeatherVibe.Client.Thunder`).
- **Made for the Waystone client, fine on any client.** A Waystone client is also told how
  stormy the sky is apart from how much falls (a shower under a pale sky), how thick it falls
  (a tempest twice as dense), and how long each change takes. A stock client ignores that part
  of the packet.
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
  values - and every profile table was reworked for the new states (Clear, Tempest, Blizzard)
  and for its climate, so an old `.conf` keeps the old climate.
- New in this version: `WeatherVibe.Client.*`, `WeatherVibe.Look.*`,
  `WeatherVibe.Sound.SilentDrizzle`, the `Clear` / `Tempest` / `Blizzard` bands and holds,
  `<Profile>.Lightning.Color`. **Thunder now defaults to the Waystone client's own**
  (`WeatherVibe.Client.Thunder = client`): on a realm where some players use a stock client,
  set it to `both` or they hear no thunder.
- `Transition.SeasonBlendMinutes` is retired (seasons now blend by calendar). An old `.conf`
  that still has those lines logs "Found incorrect option" for each at startup; delete them.
- `WeatherVibe.Debug` no longer prints anything to players. It writes to the server log.
- **Every intensity band changed** (`WeatherVibe.Intensity.InternalRange.*`), and
  `WeatherVibe.Sound.From.*` is new. An old `.conf` keeps the old bands, and those are what
  caused rain you could hear and not see (rain started at grade 0.25, where the client draws
  nothing) and fine days under a storm sky (Fine reached 0.30; the client's sky is fully
  stormy at 0.25). Copy the whole "What the client does with weather" section of the new
  `.conf.dist` over yours. The server log warns at startup about each band the client cannot
  show, and `tools/wv_lint.py` does the same for a file.

---

## Commands

Read-only commands are GM-only unless `WeatherVibeCommand = 1`. Commands that change the
weather are always GM-only; `reload` needs administrator level.

| Command | What it does |
|---|---|
| `.wvibe status` | Season, daypart, and one line per profile |
| `.wvibe status here` / `<profile>` | That profile in detail: state, intensity, time to the next change, what the client is being sent (weather id, grade, how much is falling), what it covers, its neighbours |
| `.wvibe where` | Zone and area ids and names where you stand, and the profile that covers them |
| `.wvibe set <zoneId> <state> <pct>` | Send that zone's profile there; afterwards it carries on by itself |
| `.wvibe force <here\|profile> <state> <pct> [minutes]` | Go there and stay, for that long or until released |
| `.wvibe resume [here\|profile\|all]` | Release a forced profile |
| `.wvibe next [here\|profile]` | Pick the next weather now |
| `.wvibe setRaw <zoneId> <state> <grade>` | Calibration: snap to a client grade (0..1) and stay there until `resume` |
| `.wvibe simulate <here\|profile> [hours]` | Run the real selection rules and print how much of the time each state gets |
| `.wvibe debug on\|off` | See the engine's decisions in chat for the weather you stand in |
| `.wvibe audition <clientWeatherId> [grade]` | Show any weather id the client knows to you alone, at that grade. The zone is not touched. `.wvibe audition off` returns you to it |
| `.wvibe lightning` | Drop one lightning bolt beside you, whether strikes are switched on or not |
| `.wvibe reload` | Re-read the config files and re-apply this module; running weather is kept |

`<state>` is a number or a name: `Clear`, `0 Fine`, `1 Fog`, `3 LightRain`, `4 MediumRain`,
`5 HeavyRain`, `6 LightSnow`, `7 MediumSnow`, `8 HeavySnow`, `22 LightSandstorm`,
`41 MediumSandstorm`, `42 HeavySandstorm`, `86 Thunders`, `ThunderSnow`, `DryThunders`,
`Tempest`, `Blizzard`.

`86 Thunders` means the storm that suits the profile: a rainstorm where its tables have rain,
thundersnow where they have only snow, a dry storm where they have neither. `Tempest` means the
worst of it: a tempest of rain, a blizzard, or the darkest dry storm.

`set`, `force` and `setRaw` act on the whole profile, so synced zones stay in sync.

The state you name is what the weather *is*; the id the client is sent is chosen by how much
is falling, so `.wvibe force here 5 0` (heavy rain at the bottom of its band) is sent as
medium rain, and `.wvibe status here` says so. To see one exact client id, use `audition`.

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
(a percentage range). For `Clear` and `Fine` the percentage is how clear the sky is: 100 is
cloudless, low is as clouded as the state gets. A profile may leave a season out; it then
borrows a neighbouring one.

The kinds of weather, calmest to wildest: `Clear` (not a cloud; lasts, and only borders Fine
and Fog), `Fine` (fair, some cloud), `Fog` (a grey, close sky), the light / medium / heavy tiers
of rain, snow and sandstorm, the three thunder states, and at the top `Tempest` and `Blizzard`,
reached only from the heaviest weather of their kind and gone again quickly.

Per profile, under `WeatherVibe.Profile.<Name>.`:

| Key | Meaning |
|---|---|
| `Transition.Max.Changes.Per.Hour` | How often the weather changes |
| `Transition.MinHoldSeconds` | Least time weather stays once it has arrived |
| `Transition.Step.TimeSeconds.Min/Max`, `Transition.StepSize.Perc` | How long a change of rain, snow or sky takes: one step of time per `StepSize.Perc` of the range crossed. (A sandstorm changes in one client-side fade of a few seconds; see below) |
| `Transition.Max.Consecutive.Same` | Same state in a row before a different one is wanted |
| `DayPartWeight.<DAYPART>.<State>` | This profile's own time-of-day multiplier |
| `Neighbors` | Profiles whose storms drift here |
| `Thunder.Style` | What `86` means here: `auto` (from its tables), `rain`, `snow` or `dry` |
| `Thunder.Sounds` | Its own thunderclaps (sound ids) instead of the global ones; on a Waystone client, its crack and its roll |
| `Lightning.Color` | `0xRRGGBB`: its lightning on a Waystone client |
| `Sandstorm.Skin`, `Rain.Skin` | What its sandstorm or rain looks like; see [Looks](#looks) |

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
| `WeatherVibe.Intensity.InternalRange.<DAYPART>.<State>` | see file | The grade band a state's 0..100% maps onto; see [How the client draws weather](#how-the-client-draws-weather) |
| `WeatherVibe.Sound.From.Light` / `.Medium` / `.Heavy` | 0.29 / 0.50 / 0.68 | The grade from which each sound loop is used |
| `WeatherVibe.Sound.SilentDrizzle` | 1 | Below the light loop, thin rain and snow are sent as the client's silent rows (2, 61): seen, not heard. 0 = plain sky |
| `WeatherVibe.Client.Extension` | 1 | Append the Waystone client's extension block; see [The Waystone client](#the-waystone-client) |
| `WeatherVibe.Client.Thunder` | client | Who makes thunder and lightning: `client`, `server` or `both` |
| `WeatherVibe.Look.*` | see file | What a Waystone client is shown per state: sky, density, lightning, storm, duck; distant storms |
| `WeatherVibe.Holiday.<holidayId>.<Profile>.<N>` | see file | Extra rows while a holiday runs |
| `WeatherVibe.Remarks.*` | on, 15%, 30 min | NPC remarks and their lines |
| `WeatherVibe.Hook.Enable` / `.ResyncMinutes` | 1 / 10 | The weather-change hook for other scripts |
| `WeatherVibe.Thunder.Enable` | 1 | Thunderclaps during storms |
| `WeatherVibe.Thunder.IntervalSeconds.Min` / `.Max` | 12 / 70 | Gap between claps; nearer the minimum as the storm builds |
| `WeatherVibe.Thunder.Sounds.Near` / `.Far` | see file | Loud and quiet clap sounds; loud ones are likelier in a hard storm |
| `WeatherVibe.Thunder.Rumble.Sound` / `.Seconds` / `.Neighbors` | 4994 / 58 / 1 | The long distant-thunder bed under a storm, how often it restarts, and whether a storm next door is heard |
| `WeatherVibe.Lightning.Enable` | 0 | Lightning bolts landing near players during storms |
| `WeatherVibe.Lightning.*` | see file | Which object, how often, how far away, how long it stays |

### Thunder and lightning

A storm is drawn by the client as rain, snow or the darkest sky, as hard as its grade says.
Who makes the thunder is `WeatherVibe.Client.Thunder`:

| Value | Waystone client | Stock client |
|---|---|---|
| `client` (default) | Its own lightning and layered thunder, rate and violence from `Look.<State>.Lightning` / `.Storm` | No thunder |
| `server` | The server's claps, no lightning | The server's claps |
| `both` | Flashes with the server's claps; its own thunder muted | The server's claps |

The server cannot tell the two clients apart, so this is one setting for the realm.

The server's thunder: one clap at a time, the same for everyone in that weather, with the
client's long distant-thunder bed under it. Audition a clap with the core's
`.debug play sound <id>`. The server's lightning bolts are off by default: a clap may come with
a stock game object with a lightning model and its own thunder, placed 35–90 yd from a player on
open ground and removed after a few seconds. Run `.wvibe lightning` to see one before deciding.

### The Waystone client

The Waystone client (`CLIENT/wowdecomp - MODIFIED`) reads an extension block after the stock
packet fields; a stock client skips it, so it is always sent (`Client.Extension = 0` stops it).
What it adds, per state and from `WeatherVibe.Look.*`:

- **The sky apart from the rain.** The stock client is fully stormy at the first drop. Here a
  shower starts under a sky half as dark and darkens as more falls (`Look.Sky.*`); a storm is
  dark from the start. Clouding over before rain meets the rain's own sky, so nothing jumps.
- **Density.** Heavy weather a little thicker, a `Tempest` or `Blizzard` up to twice as thick
  as the stock client can draw.
- **Transitions.** Each streamed step is told to take one tick, so a change is one continuous
  move; a sandstorm's single packet carries its whole fade.
- **Lightning.** While a storm is up, strikes per minute and how violent (`Look.<State>.Lightning`,
  `.Storm`). While one is coming or going, or up in a neighbouring profile, far-off flashes and a
  low rumble (`Look.DistantStorm.*`). Per profile, its colour (`Lightning.Color`) and its
  thunder sounds (`Thunder.Sounds`).
- **Duck.** Under a tempest, a blizzard or a heavy sandstorm the zone's own ambience is turned
  well down.

The Waystone client's own weather settings (`/weather`, the `weather*` CVars) still apply on
top: its volume, thunder level, lightning brightness and so on are the player's.

### Looks

The client can draw four kinds of weather no profile uses yet. A profile can say what its
sandstorm or rain *is*; its rows, weights and pacing stay as they are.

| Setting | Value | Client weather | What the data says it is |
|---|---|---|---|
| `Sandstorm.Skin` | `ash` | 104 | Black mist on the wind, silent |
| `Sandstorm.Skin` | `ember` | 103 | Orange dust |
| `Rain.Skin` | `dark` | 90 | Dark streaks, silent |
| `Rain.Skin` | `red` | 100 | Red drops |

None is switched on, because how they look was read from the client's tables, not seen. Look
first with `.wvibe audition 104 0.6` (then `103`, `90`, `100`), and `.wvibe audition off` when
done. The three volcanic profiles have a `Sandstorm.Skin = sand` line ready to change to `ash`.

### Check before you deploy

```bash
python tools/wv_lint.py conf/mod_weather_vibe.conf.dist
python tools/wv_lint.py conf/mod_weather_vibe.conf.dist --stats   # share of each kind of weather
```

Reports typos (a minimum above a maximum), duplicate rows, a profile that contradicts its own
header ("no snow" with a snow row), tiers that can rarely be reached, unmapped profiles and
unknown keys. Zone and area ids are checked against `tools/area_ids.tsv`, the stock client's
area table: an id that does not exist, a sub-area used as a zone, a zone on a test map. Zones
on instance maps are listed so you can confirm each is the one you meant.

`tools/wv_client_data.py` rebuilds that table from a client folder, and prints the client's
weather table and sound entries. It only reads the client.

```bash
python tools/wv_client_data.py areas   "<client folder>"
python tools/wv_client_data.py weather "<client folder>"
python tools/wv_client_data.py sounds  "<client folder>" "Lightning|Thunder"
```

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

`state` is the module's state, not what the client is drawing: any kind of thunder (a tempest
too) is reported as 86, `Clear` as 0, a `Blizzard` as 8, and a skinned sandstorm is still 22,
41 or 42.

Do not set weather from Lua with `Map:SetWeather`. It creates a core weather object that keeps
regenerating on its own and fights this module in that zone. Use `.wvibe force` or `.wvibe set`.

---

## How the client draws weather

A weather packet is an id and a grade from 0 to 1. What the 3.3.5a client does with them was
read from its own code ([DESIGN.md](DESIGN.md) has the details), and it is not what the names
suggest:

| | Decided by | How |
|---|---|---|
| **Sound** | the id alone | The loop starts the moment the packet arrives, whatever the grade |
| **What falls** | the grade alone | `density = (grade − 0.25) / 0.75`. Nothing falls at or below 0.25, whatever the id |
| **The sky** | the first quarter of the grade | Clear at 0, fully stormy at 0.25, no darker after that |

So the grade has two parts: 0–0.25 is the sky clouding over with nothing falling, and 0.25–1
is how much falls under a sky that is already stormy. Light, medium and heavy rain are the
same raindrops with three different sounds; Fine and Fog are the same sky.

Two more things the client does:

- It **fades by itself**, at a tenth of the grade per second.
- It **restarts a sandstorm every time it is told about one**, even if nothing changed: it
  fades out, is rebuilt, and fades back in. Rain and snow are updated in place. (Its weather
  table has no default texture for sand, so every packet looks like a new one. The Waystone
  client fixes this, but the server cannot tell who is on which client.)

What the module does about it:

- A state's band (`InternalRange`) puts skies inside 0–0.25 and precipitation above it.
- The id is chosen for its sound, by how much is falling (`Sound.From.*`). Below the light
  loop, thin rain and snow are sent as the client's silent rain and snow rows (2 and 61), so a
  drizzle can be seen without being heard, and nothing is ever heard before it can be seen.
- Rain, snow and the sky are moved in small steps. A sandstorm is sent once, with the grade it
  is going to, and never repeated: not on a timer, not when you cross into another zone with
  the same weather, and not when a profile picks the same sandstorm again.
- After a loading screen or a login the weather is sent abruptly, so it is already there.

The consequence to know about: **sandstorms do not build up slowly.** They arrive in a fade of
a few seconds and change the same way. Rain and snow do build.

## Calibrating to your client

The rules above are exact. What a given grade *looks* like is still your client's business,
and clients differ (HD patches especially). Worth a few minutes, standing outdoors in a mapped
zone; `.wvibe status here` shows the id and grade being sent at each step:

| Try | Look for | If not |
|---|---|---|
| `.wvibe setRaw <zone> 3 0.25`, then `0.27`, `0.29`, `0.35` | Nothing falling and silence; a few silent drops; the first thin rain with a quiet loop; plainly raining | If the loop starts before the rain is plain to see, raise `Sound.From.Light` |
| `.wvibe setRaw <zone> 3 0.49`, then `0.51`; `0.67`, then `0.69` | The loop steps up as the rain does | Move `Sound.From.Medium` / `.Heavy` to where each loop suits the rain |
| `.wvibe setRaw <zone> 0 0.0`, then `0 0.125`, then `0 0.25` | Clear; lightly clouded; a full storm sky, nothing falling | That zone has no storm sky (about half do not); nothing to tune |
| `.wvibe force here 0 0` | As dull as a fine day gets. Bright enough? | Lower the `Fine` band's maximum (brighter) or raise it towards 0.25 (gloomier) |
| `.wvibe force here 7 60` in a snow zone, from clear | The sky clouds over, a few flakes, then the snow thickens, without a fade-out | If it comes and goes, something else is sending weather: see Troubleshooting |
| `.wvibe force here 86 80` | A downpour with thunder: the Waystone client's lightning, or (Client.Thunder = server) a clap every half minute or so | `Look.Thunders.*`; `Thunder.IntervalSeconds`, `Thunder.Sounds.*` |
| `.wvibe force here 5 90`, then `.wvibe force here Tempest 100` | The absolute storm: on Waystone twice the rain, lightning every few seconds, most of it close | `Look.Tempest.*` |
| `.wvibe force here Clear 100` | Not a cloud in the sky | That zone's light has no storm sky to clear; nothing to tune |
| `.wvibe audition 104 0.6`, then `103`, `90`, `100` | Whether you like each look | Leave the skins off |
| `.wvibe lightning` | A bolt and its thunder | Try `Lightning.GameObject = 183356`, or leave strikes off |

`.wvibe resume` and `.wvibe audition off` when done.

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

**Thunder is too quiet, too loud or the wrong sound.** `.debug play sound <id>` plays any sound
entry; put the ones you like in `Thunder.Sounds.Near` and `.Far`. Thunder follows the client's
sound-effects volume.

**I can hear rain and nothing is falling, or fine days are dark.** Your `.conf` still has the
old intensity bands. See "Upgrading from an older version"; the server log names each band at
startup.

**A sandstorm fades out and comes back.** The stock client does that whenever it receives a
packet about sand. The module sends one per real change, so something else is sending:
`ActivateWeather` still 1, a script calling `Map:SetWeather`, or
`WeatherVibe.Profile.ReApply.PerSec` set in an old version. One fade when the sandstorm
really changes strength is the client's own and cannot be avoided.

**A sandstorm appears all at once instead of building up.** Yes; see
[How the client draws weather](#how-the-client-draws-weather). Rain and snow build; sand cannot.

**No thunder at all.** On a stock client with `WeatherVibe.Client.Thunder = client` that is
expected: set `both`. On a Waystone client, its `weatherLightning` / `weatherThunder` CVars may
be off.

**A storm is only a drizzle.** Your `.conf` still has an old `Thunders` range (0.15–0.40).

---

## How it works

[DESIGN.md](DESIGN.md) has the architecture and the reasons behind it.

## License

Same license as the upstream project; see [LICENSE](LICENSE).
