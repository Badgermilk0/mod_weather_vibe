# WeatherVibe — design

Architecture and the reasons behind it. [README.md](README.md) is the admin's guide; this file
is for whoever changes the code.

## Verification status

As of 2026-10-11 the rework described here has been:

- **compile-checked** — every translation unit passes MSVC `cl /Zs` (full front-end analysis)
  against the workspace core, no errors and no warnings in module files;
- **exercised offline** — the selection rules run over all 284 shipped season tables
  (`tools/pick_test.cpp`), and the conf passes `tools/wv_lint.py`;
- **not built into a worldserver and not run in game.** Timing, delivery, the hook, remarks and
  everything the client draws are unverified. The in-game checklist is at the end of this file.

## What the module is

A replacement for the core's weather (`ActivateWeather = 0`). The unit is a **profile**: a named
set of weighted weather tables, one per season, plus transition tuning. Zones and areas are
mapped onto profiles; everything mapped to the same profile shares one live weather state.

## Files

| File | Role |
|---|---|
| `src/mod_weather_vibe.cpp` | Entry point. The `PlayerScript` (zone/area hooks) and the `WorldScript` (startup, reload, tick). `WeatherVibe_ApplyConfig()` |
| `src/core/mod_wv_core.*` | Config view, calendar (season, daypart), percentage-to-grade mapping, the packet, name tables |
| `src/engine/mod_wv_pick.*` | Selection rules only: families, tiers, what may follow what, the weighted draw. No server state |
| `src/engine/mod_wv_engine.*` | Profiles, their live state, transitions, holds, fronts, delivery, GM overrides, simulation |
| `src/world/mod_wv_world.*` | What the world does about it: the weather-change hook and NPC remarks |
| `src/cmd/mod_wv_commands.cpp` | `.wvibe` |
| `tools/wv_lint.py` | Offline conf check |
| `tools/pick_test.cpp` | Offline behaviour test of `mod_wv_pick.cpp` (not part of the build; AzerothCore compiles `src/` only) |

## Decisions

### The profile is the truth; players are told what they stand in

The original kept a per-zone cache of the last packet and broadcast with
`Map::SendZoneMessage`. That had three failures: instanced maps were never reached
(`FindBaseNonInstanceMap`), the cache was only written when somebody received the packet (so an
empty zone served stale weather to the next arrival), and every bot on the map was sent every
packet.

Now a profile holds `(state, raw)` and nothing is cached per zone:

- `WeatherVibeEngine::Resolve(zone, area)` is the single answer to "which profile is this
  player in": the area's profile if it has one, else the zone's.
- A change is delivered by one pass over `sWorldSessionMgr->GetAllSessions()`. Playerbot
  sessions are never registered there, so bots are not even visited, and a player is found on
  whatever map they are.
- The zone hook (`OnPlayerUpdateZone`) sends the resolved profile's weather. It also covers
  login, because `Player::UpdateZone` runs during login.
- The area hook only acts when an area profile is involved and the zone did not change (on a
  zone change the area hook fires first; the zone hook that follows does the sending).
- **Unmapped zones:** the client keeps the previous zone's weather until told otherwise, so the
  hook sends Fine — and then calls `Map::SendZoneWeather()` so the core repeats any weather a
  script set for that zone (Akil'zon, the Lich King, Quel'Danas). The original's bare Fine wiped
  those.

`HasClient()` is `!session->IsSocketClosed()`; that is how a bot is told from a player without
depending on mod-playerbots.

### Transitions happen in raw-grade space

The client draws from the raw grade (0..1); the state id within a family mostly selects sound.
The original stepped a *percentage* and swapped state at 0% or 100% of each state's own band.
With the shipped bands (light 0.25–0.60, medium 0.25–0.70) a Light→Medium "upgrade" went up to
0.60 and restarted at 0.25.

`PlanTransition()` builds one or two legs of grade movement:

| From → to | Path |
|---|---|
| same state | grade moves |
| continuous pair (same rain/snow/sand family, or Fine↔Fog) | grade moves straight across, state id swaps halfway |
| anything else | old state eases to its 0% grade, new state builds from its 0% grade |

`Evaluate()` applies a smoothstep over the whole plan. A tick (`Transition.TickSeconds`,
default 4 s) sends a packet only if the grade moved by more than `RAW_EPSILON`.

The *duration* still comes from the profile's `Step.TimeSeconds.*` and `StepSize.Perc`: one
old-style step of time per `StepSize.Perc` of intensity travelled. Existing profiles keep their
pacing; they just move smoothly inside it.

*Unverified assumption:* that equal grades look alike across a family's three state ids. The
README's calibration table tests it.

### Fine honours its band, inverted

A Fine row's percentage is clearness. `PercentToRaw` maps 100% to the band's minimum (0.00,
clear) and 0% to its maximum (0.30). The original ignored the band and sent `1 - pct`, so the
many profiles authored with Fine at 20–50% went out at raw 0.5–0.8.

The shipped bands make Fine's 0% (0.30) meet precipitation's 0% (0.25). That is what makes
"ease to 0%, swap, build from 0%" read as clouds gathering and then rain.

The daypart's band is applied once, when a target is chosen (`Begin()`), not on each resend. A
hold does not shift when the clock crosses into the next daypart.

### Selection: stepped, and balanced

`IsNaturalTransition()` (strict mode):

- Calm and light weather mix freely: Fine, Fog, thunder used on its own, and the light tier of
  any family.
- Medium and heavy are reached and left one tier at a time within their family.
- **Thunder is coupled to rain** when the table has medium or heavy rain: it then counts as
  the top of the rain ladder (`IsThunderCoupled`). Tables that use thunder alone (Netherstorm,
  Blackrock) keep it as calm weather.

The rule is deliberately **symmetric** (if A may follow B, B may follow A), because the next
part depends on it.

Stepping alone starves the upper tiers. Heavy rain can only be entered from medium rain, and
every state that touches it has many other places to go. Measured with `tools/pick_test.cpp`
over the 1,541 state rows of the shipped conf, comparing each state's share of picks with its
share of weight:

| Rules | within 25% | within 50% | under half | under a quarter |
|---|---|---|---|---|
| original ("legacy") | 641 | 968 | 402 | 225 |
| strict + balanced | 1195 | 1495 | 13 | 0 |

Under the original rules, Elwynn's spring heavy rain (4.3% of the weight) was picked 0.1% of
the time. This was true of the module as it shipped, not something the rework introduced.

`PickEntry()` fixes it with a Metropolis–Hastings acceptance step. Propose a natural neighbour
by weight; accept a move with probability `min(1, reach(here) / reach(there))`, where
`reach(x)` is the total weight of everything that may follow `x`; otherwise stay for another
spell. On a symmetric graph that makes the long-run share of each state proportional to its
weight. A "stay" is a pick of the current state, so it re-rolls the intensity.

`Max.Consecutive.Same` is variety: after that many picks of one state it is no longer offered.
Balancing may still hold a state, up to twice the limit — a long blizzard is what a profile
that weights heavy snow at 30% is asking for. Past that the move is forced.

The three passes (natural + variety, variety only, anything) guarantee a pick whenever the
table has weight. The second pass used to be "natural, ignoring variety", which could repeat a
state forever when its only natural successor was itself.

Residual skew: fog is over-picked by design (`FogBridge.Boost`), and a medium tier above a
thinly weighted light tier stays under-picked (it is a bottleneck no acceptance rule can widen).

`Escalation.Strict = 0` restores the original rules, unbalanced.

### The effective table

`BuildTable()` assembles what a profile may pick from *right now*:

1. **Season.** This season's rows, plus the neighbouring season's scaled by
   `SeasonBlend.secondaryShare` (0 in mid-season, 0.5 on the boundary day; window
   `Season.BlendDays`). A missing season borrows another. This replaced a 10-minute blend
   window that only ever influenced one or two picks, four times a year.
2. **Holidays.** `Holiday.<id>.<Profile>.<N>` rows while `IsHolidayActive(id)`.
3. **Time of day.** Each row's weight times the profile's `dayPartWeight[daypart][state]`.
4. **Fronts.** For each of rain, snow, sandstorm: the share of this profile's neighbours whose
   *target* weather is medium/heavy in that family (or thunder, for rain). Rows of that family
   are multiplied by `1 + Front.Strength × share`. Only rows the table already has are touched,
   so climate is never overridden.

Neighbours are listed once per border; `LoadNeighbors()` makes every link two-way.

### Pacing

`CalcHoldMs()`: the profile's interval (`3600 s / Max.Changes.Per.Hour`, ±25%, divided by
`Pace`) is the budget for transition plus hold. The hold is what is left, never under
`MinHoldSeconds`, then multiplied by `Hold.<State>`. The original's separate hourly budget
counter did the same job as the interval and is gone.

### Warm start

`WarmStart()` settles each profile on a weighted pick, part-way through a hold. The original
started every profile at 0% and faded in, so after a restart the whole world shared one sky.
Nothing is persisted; a restart rolls new weather.

### GM overrides are real state

`Steer` (go there, carry on), `Force` (go there, stay, optionally timed) and `ForceRaw` (snap,
stay) change the profile's live state, so synced zones stay in sync and the engine does not
overwrite them a tick later. `forced` suppresses picks until `Resume` or the timer.

### The hook

`WeatherVibeWorld::FireHook()` calls `sScriptMgr->OnWeatherChange()` per open-world zone of a
profile, on settle and every `Hook.ResyncMinutes`. mod-ale forwards that as
`WEATHER_EVENT_ON_CHANGE` (25). The hook wants a core `Weather*`; a stack `Weather` built on a
static zeroed `WeatherData` supplies one (ScriptId 0, so no `WeatherScript` is looked up; it is
never stored, ticked or sent). Instanced zones have no base map to hang it on and are skipped.

This is the intended seam for gameplay. Effects on stats, skills or spawns belong in scripts
listening to event 25, not in this module.

### Remarks

On a settle that changes the *kind* of weather (`ClassifyTurn`: rain, snow, sandstorm, storm,
fog, clear), each real player in the profile rolls `Remarks.Chance`. If it passes, and the
player is alive, outdoors, out of combat and not on a taxi, the nearest friendly idle humanoid
in line of sight within `Remarks.Range` says a line. One line per player per cooldown, and no
NPC speaks twice in one pass. Lines live in the conf.

## Threading

The invariant everything relies on:

- **All mutation happens on the world thread**: `OnUpdate`, config (re)load, commands
  (`CMSG_MESSAGECHAT` is `PROCESS_THREADUNSAFE`), logout.
- **The zone and area hooks only read.** They run inside `Player::UpdateZone`, which can be a
  map-update thread. Map threads only run inside `sMapMgr->Update()`, during which the world
  thread waits, so a reader never overlaps a writer.

Keep it that way: no per-player bookkeeping in the hooks, no lazy caches written from
`Resolve()` or `SendCurrentTo()`. `Remark()` does grid searches and `Say()`; it is reached only
from `Settle()`, on the world thread.

## Config loading

- `ConfigView` snapshots every `WeatherVibe*` key once and answers prefix queries from that.
  All values are read as text and a trailing `# comment` is stripped — AzerothCore keeps the
  whole rest of the line as the value. (Remark lines are the exception: spoken text keeps `#`.)
- The first `OnAfterConfigLoad` runs before the DBC stores are loaded and id validation needs
  them, so the initial load is in `OnStartup`; `OnAfterConfigLoad` only handles reloads.
- On reload, `Load()` carries each profile's `LiveState` over by name and then re-sends
  everyone their current weather, since mappings may have moved.
- **AzerothCore drops any key in the `.conf` that is not in the `.conf.dist`** (at startup;
  oddly, not on reload). New numbered lines must go in both. It is documented in the README and
  at the top of the conf, and it is why the retired `SeasonBlendMinutes` lines in an old
  `.conf` log "incorrect option".
- Code defaults for the hold multipliers match the conf. The time-of-day table defaults to 1.0
  in code, so without the new conf that feature is neutral.

## Known limits

- A zone mapped here that a core script also drives (Isle of Quel'Danas has both) shows the
  profile's weather; the profile wins on the next change.
- `Map:SetWeather` from Lua, and the Scourge Invasion script, create a core `Weather` object
  that `Map::UpdateWeather` keeps regenerating regardless of `ActivateWeather`. It will fight
  the module in that zone.
- Area ids must be the innermost area. The Argent Tournament grounds needed eight.
- The season table is northern-hemisphere and the same for every zone.
- State 90 (black rain) and 106 (black snow) are not driven; `kStates` is the list.

## Testing

Offline, from the workspace root:

```
python MODULES/mod_weather_vibe/tools/wv_lint.py MODULES/mod_weather_vibe/conf/mod_weather_vibe.conf.dist
```

Compile check: `cl /Zs /std:c++20 /utf-8 /EHsc /permissive- /DNOMINMAX /DWIN32_LEAN_AND_MEAN`
with the core's `src/common` and `src/server` directories (minus `scripts` and `apps`), the
`deps` include directories, boost, OpenSSL and the module's `src` tree on the include path.

Behaviour test: build `tools/pick_test.cpp` together with `src/engine/mod_wv_pick.cpp` (its
header has the command) and run it on the conf. Re-run it after touching `mod_wv_pick.cpp` or
re-weighting many profiles; the table above is the baseline.

In game, after a build and deploy:

1. Startup log: one line per profile, no `[WeatherVibe]` warnings.
2. `.wvibe where` in Elwynn, then Stormwind: same profile, same weather.
3. Enter Arathi Basin: weather matches Arathi Highlands.
4. Fly into a zone with no other players: the weather is already right, no snap a few seconds later.
5. Enter a raid mid-encounter where a boss sets weather: it is not wiped.
6. `.wvibe force here 5 80`, watch `.wvibe status here`: the grade climbs steadily, no dip.
7. `.wvibe debug on` and wait for a Light → Medium change: one continuous move.
8. Edit a weight, `.wvibe reload`: the current weather is kept, the next pick uses the new table.
9. A non-GM character sees no WeatherVibe text with `WeatherVibe.Debug = 1`.
10. Walk into Felstone Field in the Western Plaguelands: the haze thickens; walk out: it lifts.
11. A Lua handler on server event 25 prints the zone and state when weather settles.
12. Stand by a guard through a change of weather a few times: occasionally a line, never spam.
13. The calibration table in the README.

## Where to edit

| To change | Go to |
|---|---|
| What may follow what; the balance | `mod_wv_pick.cpp` — then run `pick_test` |
| How a change looks over time | `PlanTransition()`, `Evaluate()` in `mod_wv_engine.cpp` |
| How long weather lasts | `CalcHoldMs()` |
| What influences a pick | `BuildTable()` |
| Who is told, and when | `FlushDirty()`, `SendCurrentTo()`, `OnAreaChange()` |
| A new config key | read it in `LoadGlobals()` / `LoadProfile()` / `WeatherVibeCore::LoadConfig()`, add it to the conf **and** to `known_keys` or a pattern in `wv_lint.py` |
| A new weather state | `kStates` in `mod_wv_core.cpp`, the family/tier switches in `mod_wv_pick.cpp`, the tables in `wv_lint.py` |
| Remark wording | the conf |
