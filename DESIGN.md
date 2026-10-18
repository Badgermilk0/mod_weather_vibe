# WeatherVibe — design

Architecture and the reasons behind it. [README.md](README.md) is the admin's guide; this file
is for whoever changes the code.

## Verification status

As of 2026-10-12 the rework described here has been:

- **compile-checked** — every translation unit passes MSVC `cl /Zs` (full front-end analysis)
  against the workspace core, no errors and no warnings in module files;
- **exercised offline** — the selection rules run over all 289 shipped season tables
  (`tools/pick_test.cpp`), and the conf passes `tools/wv_lint.py`, ids included;
- **checked against the client's data** — the stock `Weather`, `SoundEntries`, `AreaTable`,
  `Map`, `Light` and `GameObjectDisplayInfo` tables were read from a 3.3.5a client
  (`tools/wv_client_data.py`). That settles what each weather id is, which sounds exist and
  what every id in the conf refers to;
- **checked against the client's code** — the weather handler of the stock 3.3.5a `WoW.exe`
  (build 12340) was disassembled and read. That settles how an id and a grade become sound,
  particles and sky, and what a second packet does to weather already on screen. It is the
  basis of "What the client does with a weather packet" below. It is a reading of machine
  code, not an observation: the numbers are exact, the look is still unseen;
- **partly seen in game** — an earlier build was run and reported back: rain could be heard
  with nothing falling, and fine days were too dark. Both are explained by the client's rules
  and are what the grade bands and `WireState()` now exist to prevent;
- **this version is not built into a worldserver and not run in game.** Timing, delivery, the
  hook, remarks, thunder, lightning and everything the client draws are unverified for it.
  The in-game checklist is at the end of this file.

## What the module is

A replacement for the core's weather (`ActivateWeather = 0`). The unit is a **profile**: a named
set of weighted weather tables, one per season, plus transition tuning. Zones and areas are
mapped onto profiles; everything mapped to the same profile shares one live weather state.

## Files

| File | Role |
|---|---|
| `src/mod_weather_vibe.cpp` | Entry point. The `PlayerScript` (zone/area hooks) and the `WorldScript` (startup, reload, tick). `WeatherVibe_ApplyConfig()` |
| `src/core/mod_wv_states.h` | The module's own thunder states, and why they exist |
| `src/core/mod_wv_core.*` | Config view, calendar (season, daypart), the client's rules as constants, percentage-to-grade mapping, module state to client id (`WireState`), the packet and the per-player record of the last one (`Deliver`), name tables |
| `src/engine/mod_wv_pick.*` | Selection rules only: families, tiers, what the client draws for a state (`GroundOf`), what may follow what, the weighted draw. No server state |
| `src/engine/mod_wv_engine.*` | Profiles, their live state, transitions, holds, fronts, delivery, the thunder timer, GM overrides, simulation |
| `src/world/mod_wv_world.*` | What the world does about it: the weather-change hook, NPC remarks, thunderclaps, lightning strikes |
| `src/cmd/mod_wv_commands.cpp` | `.wvibe` |
| `tools/wv_lint.py` | Offline conf check, ids included |
| `tools/wv_client_data.py` | Reads a client's stock tables (read-only): writes `area_ids.tsv`, prints the weather table and sound entries |
| `tools/area_ids.tsv` | The stock area table, generated; what the linter checks ids against |
| `tools/pick_test.cpp` | Offline behaviour test of `mod_wv_pick.cpp` (not part of the build; AzerothCore compiles `src/` only) |

## What the client actually has

Read from the stock tables; these facts shape several decisions below.

- **A weather id is a row of `Weather.dbc`**: a particle type (none, rain, snow, sandstorm), an
  ambience sound loop, a colour, an optional texture, and one more number that is 1 for
  ordinary weather and 0 for a few special rows. That number scales the storm sky (see the
  next section): at 0 the sky does not darken at all.
- **Light, medium and heavy are one particle type with three sound loops.** Ids 3/4/5 are all
  "rain"; 6/7/8 all "snow". Nothing but the loop differs, so grade alone decides density.
- **Fog (1) has no particles and no sound.** It is only a sky, the same sky Fine (0) gives at
  the same grade.
- **The sandstorm loops are crossed** against the core's names: id 22 ("light") carries
  `SandStormHeavy.wav`, 41 carries the light loop, 42 the medium one.
- **There is no thunderstorm.** Id 86, `WEATHER_STATE_THUNDERS` in the core, is rain particles
  with the ambience `DarkPortal` (sound 4994, `Sound\Ambience\ZoneAmbience\DarkPortal.wav`)
  in place of the rain loop. That file is no hum: 59 s of stereo low rumble with rolls swelling
  out of it (at about 2, 7, 24 and 39 s) - distant thunder, the unmodded game's storm sound.
- **Extra rows nothing here used:** 104 (sandstorm motion, black, `SNOWMIST01` texture, silent),
  103 (sandstorm, orange), 90 (rain motion, `LeafBrown` texture, silent — the core's "black
  rain"), 100 and 108 (rain, `RainDropRed01`). 103 and 105–107 have that last number at 0.
- **378 of 715 lights have a separate storm parameter set.** Only there can the grade darken
  the sky; elsewhere Fine and Fog at any grade look the same.
- **Thunder exists as doodad and spell sounds**, all in stock `common.mpq`. Measured from the
  files (time heard above -30 dB of peak; share of energy below 120 Hz): 8439 / 4936 (Blasted
  Lands bolt, 2.3-4.5 s, 26-32%: a crack and a roll), 13006 (`Shaman_Thunder.wav`, 2.9 s, 31%:
  one boom), 30 (`CallLightning.wav`, 2.5 s, 25%), 9947 / 1485 (`EarthquakeCameraShake.wav` at
  volume 0.2 / 0.4, 3.5 s of flat 66% rumble: the distant roll). 13021 (Storm Peaks, 1.3 s,
  5-24%) is a thin crack, and 9843 / 10007 (Netherstorm crack, 1.5 s, 0%) have no bass at all:
  an electric "bzzt", right for an arcane storm and wrong for a rainstorm. There is no thunder
  roll as such in the data. The played client's `patch-s` SoundEntries leaves all of these as
  stock.
- **Lightning exists as a game object model**: display 6352 (`ZulGurubLightningMadness.mdx`)
  with sound 8439 attached, used by stock objects 190569, 183356 and 180252.

## What the client does with a weather packet

Read from the weather code of the stock 3.3.5a client (`WoW.exe`, build 12340; a client
patched for HD portraits has the same bytes there). Everything about what the module sends,
and when, follows from these six rules. Addresses are for that build.

| Where | What |
|---|---|
| `0x526A69` | the `SMSG_WEATHER` (0x2F4) case of the packet handler at `0x526530` |
| `0x7846A0` | `Weather::Set(effectType, grade, row, smooth, skyFactor)` |
| `0x784850` | the grade moving towards its target, each frame |
| `0x78D170` | the per-frame update, with the switch on particle type |
| `0x7840B0`, `0x7844F0`, `0x784580` | density of rain, snow, sandstorm |
| `0x783B60` | "which effect is showing?" |
| `0x4C8610` | sets the zone ambience loop |

The packet is an id, a grade (0..1) and an "abrupt" flag.

1. **The sound is chosen by the id alone, at once.** The handler takes the row's ambience and
   sets it as the packet arrives. The grade plays no part. No row, or a row without ambience,
   is silence.
2. **What falls depends on the grade alone:** `density = max(0, (grade − 0.25) / 0.75)`. At or
   below 0.25 nothing falls, whatever the id. Full density is about 4,300 raindrops, 860
   snowflakes or 4,000 grains on normal detail (6500, 1300 and 6000 × 0.66).
3. **The sky depends on the first quarter of the grade:** `storm = 4 × min(grade, 0.25) ×
   skyFactor`, where `skyFactor` is the row's last number (1 for ordinary weather and when
   there is no row). The sky is fully stormy from 0.25 up and gets no darker. So Fine (0) and
   Fog (1) are the same thing to the client: no particles, no sound, a sky dimmed by the grade.
4. **The client smooths the grade itself**, linearly, 0.1 per second (the whole range in ten
   seconds), starting from its previous target.
5. **A change of particle type is a restart.** The client fades what is showing down to 0.25,
   waits for the last particles to die, destroys the effect, creates the new one and fades it
   in. Its check for what is showing (`0x783B60`) only knows the rain object, so it answers
   "rain" or "nothing" — and **every snow or sandstorm packet is therefore taken as a change of
   type and restarts the effect, even with the same id and the same grade.** Rain packets are
   not: rain is updated in place.
6. **A rain row with its own texture leaves it behind.** After ids 90 or 100, ordinary rain is
   drawn with their texture until a packet of another type clears it.

What that meant for the module as it was. The first two were reported from a running server;
the rest follow from the rules and were not waited for.

| Effect in game | Cause |
|---|---|
| Rain could be heard with nothing falling | Rain's bands started at 0.25, where density is zero, and every fade between kinds of weather ran the old one down to that point with its id, and so its sound, still set (1, 2) |
| Fine days were dark | Fine ran up to grade 0.30. A zone written as "20–50% clear" was sent 0.15–0.24: a sky 60% to 96% stormy (3) |
| Snow and sandstorm pulse while changing, and restart at zone borders | A step every four seconds, and a resend on every zone change; each one a restart (5) |
| A dry storm has one look | Its band was 0.30–0.60, all of it past the point where the sky stops changing (3) |
| A drizzle can roar | The loop followed the state's name, not how much was falling (1) |

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

### A client is told only what is new to it

Because of rule 5, a packet is not free: an unneeded one makes snow or a sandstorm fade out
and come back. So every packet for a mapped place leaves through one function,
`WeatherVibeCore::Deliver(player, id, grade, why)`, which keeps the last id and grade it sent
that player in the player's own `CustomData` (`SentWeather`, key `WeatherVibe.Sent`) and
compares first. `why` is a `Tell`:

| `Tell` | Used by | Sends when |
|---|---|---|
| `CHANGE` | the broadcast pass, for a profile whose weather moved | the client was not last told exactly this |
| `ARRIVAL` | the area hook, a reload, the reconcile pass | as `CHANGE` — but snow or a sandstorm with the same id and a grade within `RESTART_NOT_WORTH` (0.08) of what the client has is left alone |
| `REPEAT` | the zone hook, `ReApply` | as `ARRIVAL` — and rain or a sky is said again even when unchanged |

What follows from it:

- Walking Dun Morogh → Ironforge, or between any two zones on one profile, sends a client in
  snow nothing. Before, it restarted the snow at every gate.
- The zone hook repeats rain and skies because the core may have sent that client weather of
  its own on the way in (`Player::UpdateZone` calls `Map::SendZoneDynamicInfo` just before the
  hook), and repeating those costs nothing on screen. With `ActivateWeather` left on the core
  always has, so the record is dropped first (`Forget`) and everything is sent.
- The record is dropped on a map change (`OnPlayerMapChanged`: the client starts over behind a
  loading screen), on entering an unmapped zone, after `.wvibe audition`, and after a one-off
  push to an unmapped zone.
- **The reconcile pass.** Snow and sandstorm are sent once, so a client that missed that one
  packet would keep the wrong weather until the next change. Every 20 s (`RECONCILE_MS`)
  `FlushDirty()` holds every client's record against what it should be showing. It sends
  nothing to the ones that are right, which is all of them unless something went wrong.
- **Texture left behind** (rule 6): when the last id sent was a rain skin and ordinary rain is
  due, `Deliver` sends a sky packet first to clear it.

### Module states are not client ids

The engine thinks in module states; `WeatherVibeCore::WireState(state, skins, grade)` turns one
into the id the client is sent (`WeatherVibeEngine::WireOf(profile)`). Since the id is nothing
but the choice of sound (rule 1), it is picked for the sound, by how much is falling:

| What is falling | Grade | Sent as |
|---|---|---|
| nothing — a sky (`Fine`) | any | 0 |
| nothing — a sky (`Fog`, `DryThunders`) | any | 1 |
| rain, snow or sand too thin to see | below `Sound.From.Light` (0.29) | 0: it looks the same, and it is silent |
| light | from `Sound.From.Light` | rain 3, snow 6, sandstorm 41 |
| medium | from `Sound.From.Medium` (0.50) | rain 4, snow 7, sandstorm 42 |
| heavy | from `Sound.From.Heavy` (0.68) | rain 5, snow 8, sandstorm 22 |
| any rain, `Rain.Skin = dark` or `red` | from `Sound.From.Light` | 90 or 100 |
| any sandstorm, `Sandstorm.Skin = ash` or `ember` | from `Sound.From.Light` | 104 or 103 |

A state's name does not choose the loop: `HeavyRain` at 5% of its band is sent as 4, and
`Thunders` (the client's own 86 swaps the rain loop for its thunder bed, so it is never sent;
the server plays that bed itself, see the rumble below) and
`ThunderSnow` climb the same ladder as the rain and snow they are drawn as. The sandstorm ids
are in that order because the client's table has its loops crossed. The first row of
precipitation is what stops sound without particles: at 0.29 about 5% of full density is
falling, some 230 raindrops.

During one stretch of a transition the grade only moves one way, and a hold does not move it
at all, so a threshold is crossed once and the loop never flutters.

The hook reports module states (`HookState`): any thunder is 86, a skinned sandstorm is still
22/41/42. `.wvibe audition` bypasses all of this and sends a raw client id to one GM, who is
then skipped by every send point until they switch it off.

### Thunder is the module's

A row that says 86 means "the storm that suits this profile". `LoadProfile()` decides from all
the profile's tables — rain rows make it `Thunders`, snow without rain `ThunderSnow`, neither
`DryThunders` — and rewrites the rows (`ResolveThunder`); `Thunder.Style` overrides, and a row
may name `ThunderSnow` or `DryThunders` outright. Before this, 17 of the 60 profiles with
thunder had no rain rows at all, seven of them frozen zones, and got raindrops.

While a thunder state is on screen, `UpdateProfile()` runs a timer and
`WeatherVibeWorld::Thunder()` plays a clap: one sound id for the whole profile, sent to each
listener with `PlayDirectSound(id, player)`. The gap runs from `IntervalSeconds.Max` down to
`.Min` as intensity rises. Loud entries (`Sounds.Near`) are favoured in a hard storm and quiet
ones (`Sounds.Far`) in a light one, which is the only "distance" a 2D sound can have. A
listener indoors (`!IsOutdoors()`) gets a `Sounds.Far` clap in place of a near one, since a
2D sound ignores walls; a profile with its own `Thunder.Sounds` plays them everywhere.

**The rumble** (`Thunder.Rumble.*`): under the claps, sound 4994 - the bed the client pairs
with its weather 86 - so the rain loop and the thunder bed play together, which sending 86
could never do. Every 5 s (`RUMBLE_CHECK_MS`) a profile asks `StormInEarshot()`: is its state
or its target a thunder state (a storm on screen, or one building up here), or, with
`Rumble.Neighbors`, is a neighbouring profile's? If so `WeatherVibeWorld::Rumble()` plays the
bed to each listener whose last one began `Rumble.Seconds` (58, the file is 59.4) or more ago.
The time is kept per player (`_lastRumble`), not per profile, so a player walking from one
stormy profile into another, or into the one next door, never hears two at once. A 2D
one-shot cannot be stopped from the server, so the last one runs out on its own, up to a
minute after the storm has gone - heard as the storm moving off. It plays indoors too.

**Lightning** (`Lightning.Enable`, off by default): per listener per clap, a chance to spawn a
stock lightning object with `Map::SummonGameObject` — unowned, removed after
`DespawnSeconds` — on the ground 35–90 yd away, found by a height query from above the player,
never in water, never when the player is indoors. The object has positional thunder, so that
listener gets no 2D clap, and neither does anyone within 80 yd of a bolt from the same clap.
If the object cannot be spawned (the template is missing) strikes switch themselves off until
the next reload and say so once.

### Transitions follow what the client can show

The engine works in the grade (0..1), not in percentages. Every state is put in one of four
groups by what the client draws for it (`GroundOf`): **sky** (Fine, Fog, `DryThunders`),
**rain** (the rain tiers and `Thunders`), **snow** (the snow tiers and `ThunderSnow`) and
**sand**. Grade 0.25 — `CLIENT_PARTICLE_FLOOR` — is where they all meet: below it only the sky
changes, at it the sky is fully stormy and nothing falls yet (rules 2 and 3). A sky clouds
over up to the floor; rain, snow and sand thin down to it.

`PlanTransition()` builds at most two legs, of two kinds:

- a **streamed** leg: the server moves the grade in small eased steps
  (`Transition.TickSeconds`). Right for a sky and for rain, which the client updates in place;
- a **one-shot** leg: the grade is set to where it is going at the start and left there. The
  only thing possible for snow and sandstorm, which restart on every packet (rule 5). The
  client does the fading; the leg just waits for it (`ClientFadeMs`, plus 4 s for the last
  particles).

| From → to | Path |
|---|---|
| sky → sky, rain → rain | one streamed leg; across two states, two halves with the state swapping in the middle |
| sky → rain | the sky streams up to the floor (clouding over), the rain streams up from it |
| rain → sky | the rain streams down to the floor (sent as sky for the last of it, so the sound goes with the last drops), the sky streams down from it |
| sky or rain → snow or sand | the old weather streams to the floor, then one shot |
| snow or sand → sky or rain | nothing is sent about the snow at all: the new weather streams from the floor, and its first packet makes the client fade the snow out by itself |
| snow → snow, sand → sand | one shot. When a profile picks the same state again, or one whose id is the same and grade within `RESTART_NOT_WORTH`, nothing is sent and the grade stays as it is (`TransitionPlan::keepGrade`) |
| snow ↔ sand | one shot |

So heavy rain becomes a thunderstorm by getting heavier, with no lull; snow arrives once and
is not touched again until it really changes; and one client-side dip per real change of snow
intensity is what remains. It cannot be avoided from the server.

A GM's `set` and `force` pass `mayStay = false`: they always take effect, restart or not.

`Evaluate()` gives the state and grade at an elapsed time: smoothstep within a streamed leg,
the end value throughout a one-shot leg. A tick marks the profile dirty only if the grade
moved by more than `GRADE_EPSILON`, so a one-shot leg produces exactly one packet.

A streamed leg's *duration* comes from the profile's `Step.TimeSeconds.*` and `StepSize.Perc`:
one step of time per `StepSize.Perc` of the range the leg is in — the sky's (0–0.25) or
precipitation's (0.25–1). It is never shorter than 1.5 × the client's own time for the
distance (rule 4; smoothstep peaks at 1.5 × its average speed), so the client is never left
chasing the server.

### The bands

`WeatherVibe.Intensity.InternalRange.<DAYPART>.<State>` maps a row's percentage to a grade.
The shipped bands (also the code defaults, `kStates`) are placed by rules 2 and 3:

| State | Band | Is |
|---|---|---|
| Fine | 0.00 – 0.125 | clear to half the storm sky |
| Fog | 0.10 – 0.25 | a grey sky to the full storm sky |
| `DryThunders` | 0.20 – 0.25 | the darkest sky |
| Light / Medium / Heavy rain, sandstorm | 0.30 – 0.50 / 0.45 – 0.70 / 0.62 – 0.95 | 7–33% / 27–60% / 49–93% of full density |
| Light / Medium / Heavy snow | 0.32 – 0.52 / 0.48 – 0.74 / 0.66 – 1.00 | snow has a fifth of rain's particles, so it sits higher |
| `Thunders` | 0.60 – 0.95 | a downpour |
| `ThunderSnow` | 0.66 – 1.00 | a blizzard |

A Fine row's percentage is clearness: `PercentToRaw` maps 100% to the band's minimum and 0% to
its maximum. (The original ignored the band and sent `1 − pct`, so profiles authored with Fine
at 20–50% went out at grade 0.5–0.8.) Fine stops at 0.125 on purpose: the profiles were
written with fine days at 20–60% clear, and with the old 0.30 that was most of a storm sky.
Gloomy zones keep their gloom through their Fog rows.

`LoadIntensityRangesConfig()` warns, once per state, about a sky band reaching past 0.25 and a
precipitation band starting below `Sound.From.Light`; `wv_lint.py` does the same offline. An
admin's own `.conf` from before this still has the old bands and will get those warnings.

The daypart's band is applied once, when a target is chosen (`Begin()`), and remembered
(`LiveState::band`) for anything that later asks how far into its band the weather is. A hold
does not shift when the clock crosses into the next daypart.

### Selection: stepped, and balanced

`IsNaturalTransition()` (strict mode):

- Calm and light weather mix freely: Fine, Fog, thunder used on its own, and the light tier of
  any family.
- Medium and heavy are reached and left one tier at a time within their family.
- **Thunder is coupled to its weather** (`GetThunderCoupling`): with medium or heavy rain in
  the table, `Thunders` counts as the top of the rain ladder; with medium or heavy snow,
  `ThunderSnow` tops the snow ladder. Without them, and always for `DryThunders`, thunder is
  calm weather that may follow anything light.

The rule is deliberately **symmetric** (if A may follow B, B may follow A), because the next
part depends on it.

Stepping alone starves the upper tiers. Heavy rain can only be entered from medium rain, and
every state that touches it has many other places to go. Measured with `tools/pick_test.cpp`
over the 1,559 state rows of the shipped conf, comparing each state's share of picks with its
share of weight:

| Rules | within 25% | within 50% | under half | under a quarter |
|---|---|---|---|---|
| original ("legacy") | 656 | 984 | 402 | 225 |
| strict + balanced | 1226 | 1514 | 12 | 0 |

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
- **The zone and area hooks only read the module's state.** They run inside
  `Player::UpdateZone`, which can be a map-update thread. Map threads only run inside
  `sMapMgr->Update()`, during which the world thread waits, so a reader never overlaps a writer.
- **The one thing a hook writes is the player's own record** (`SentWeather` in that player's
  `CustomData`). A player belongs to one thread at a time — their map's during a map update,
  the world thread otherwise — and `Deliver`/`Forget` are only ever called for the player the
  calling thread owns: the hooks for the player being updated, `FlushDirty()` and the commands
  from the world thread while the maps are idle.

Keep it that way: no module-wide bookkeeping in the hooks, no lazy caches written from
`Resolve()` or `SendCurrentTo()`, and never `Deliver` to another player from inside a hook.
`Remark()` does grid searches and `Say()`; it is reached only from `Settle()`, on the world
thread.

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

- **Snow and sandstorm cannot build up gradually.** Every packet restarts them, so they arrive
  at their grade in one client-side fade (a few seconds) and change by fading out and in. Rain
  and the sky are the only weather the server can move smoothly.
- **Thin precipitation is not drawn.** Below `Sound.From.Light` rain is sent as a stormy sky.
  At that grade about one drop in twenty of a full downpour would be falling.
- **The module only knows what it sent.** Weather sent to a client by anything else — the core
  with `ActivateWeather = 1`, a script through `Map::SetZoneWeather` — is not in the record. The
  zone hook repeats rain and skies to cover it, and drops the record when the core's weather is
  on. What is not covered: a *mapped* zone, in *unchanged snow or sandstorm*, for which a
  script has set zone music, light or weather through the core. The core then sends its own
  weather packet on the way in and the module does not say its snow again until it changes.
- **The skin texture fix is unseen.** Sending a sky packet before ordinary rain (rule 6) is
  what the client's code calls for; it has not been watched working.
- A zone mapped here that a core script also drives (Isle of Quel'Danas has both) shows the
  profile's weather; the profile wins on the next change.
- `Map:SetWeather` from Lua, and the Scourge Invasion script, create a core `Weather` object
  that `Map::UpdateWeather` keeps regenerating regardless of `ActivateWeather`. It will fight
  the module in that zone.
- Area ids must be the innermost area. The Argent Tournament grounds needed eight.
- The season table is northern-hemisphere and the same for every zone.
- Client ids 90, 100, 103 and 104 are reachable only as skins. 106 ("black snow", the Lich
  King's) and the rest of `Weather.dbc` are not driven at all; `.wvibe audition` shows them.
- A 2D clap has no direction or distance and ignores walls. Indoors the far clap stands in
  for a near clap, but it is not muffled: the client's occlusion low-pass only acts on 3D
  sounds (the lightning object's thunder is one).
- The lightning object's look is unverified, and its thunder is whatever sound the model
  carries (8439), not the profile's `Thunder.Sounds`.
- `tools/area_ids.tsv` is the stock table. A server whose DBC files were edited may differ;
  the startup validation is the authority there.

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
6. `.wvibe force here 5 80` from a clear sky, watching `.wvibe status here`: the sky clouds
   over first (id 0, grade rising to 0.25), then rain appears and thickens. **It is silent
   until drops are visible**, then the light loop, the medium, the heavy.
7. `.wvibe force here 0 100` from that rain: the sound stops as the last drops go, not after;
   then the sky clears.
8. `.wvibe debug on` and wait for a Light → Medium change: one continuous move.
9. In Dun Morogh, `.wvibe force here 7 60` from clear: the sky clouds over, then snow arrives
   **once** and stays. Nothing pulses while `.wvibe status here` still shows time to go.
10. Walk Dun Morogh ↔ Ironforge through the gates in snow, and Elwynn ↔ Stormwind in rain:
    nothing restarts, nothing clears.
11. `.wvibe force here 0 100`, then `.wvibe force here 0 0`, on a clear day somewhere with a
    storm sky (Elwynn has one): 0% is lightly clouded, not dark. Then
    `.wvibe force here DryThunders 100`: clearly darker than both.
12. `.wvibe setRaw <zone> 3 0.25`, then `0.29`, `0.35`: nothing; the first thin rain with the
    light loop; plainly raining. If rain is visible before 0.29 or still invisible at it, move
    `WeatherVibe.Sound.From.Light`.
13. Edit a weight, `.wvibe reload`: the current weather is kept, the next pick uses the new table.
14. A non-GM character sees no WeatherVibe text with `WeatherVibe.Debug = 1`.
15. Walk into Felstone Field in the Western Plaguelands: the haze thickens; walk out: it lifts.
16. A Lua handler on server event 25 prints the zone and state when weather settles.
17. Stand by a guard through a change of weather a few times: occasionally a line, never spam.
18. `.wvibe force here 86 80` in Elwynn: a downpour, a clap every half minute or so, no hum.
    The same in Icecrown: snow and thunder, no raindrops. In Netherstorm: a dark sky, crackle.
19. Heavy rain → Thunders with `.wvibe debug on`: the grade only rises; no lull, and the rain
    loop steps up with it.
20. `.wvibe audition 104 0.6`, then 103, 90, 100: each look, to you alone. `.wvibe audition off`.
21. `.wvibe lightning`: a bolt within 25 yards, with its own thunder; gone in a few seconds.
22. Walk Wetlands → Thandol Span → Arathi in rain: the sky does not clear on the bridge.
23. Swim out from Westfall: the sea has weather.
24. Walk into Raven Hill Cemetery: the fog closes in. Light's Hope Chapel: it lifts.
25. Teleport out of a snowing zone and back (`.tele`): the snow is there again on arrival.
26. The calibration table in the README.

## Where to edit

| To change | Go to |
|---|---|
| What may follow what; the balance | `mod_wv_pick.cpp` — then run `pick_test` |
| How a change looks over time | `PlanTransition()`, `Evaluate()` in `mod_wv_engine.cpp` |
| How long weather lasts | `CalcHoldMs()` |
| What influences a pick | `BuildTable()` |
| Who is told, and when | `FlushDirty()`, `SendCurrentTo()`, `OnAreaChange()` in the engine; `Deliver()` and `Tell` in `mod_wv_core.*` decide whether a packet is worth sending |
| Anything about what the client does with a packet | the constants at the top of `mod_wv_core.h` (`CLIENT_PARTICLE_FLOOR`, `CLIENT_GRADE_PER_SECOND`, `RESTART_NOT_WORTH`) and `RestartsOnPacket()` — and the section above that says where each was read |
| Which sound goes with how much rain | `WeatherVibe.Sound.From.*` in the conf; `WireState()` |
| A new config key | read it in `LoadGlobals()` / `LoadProfile()` / `WeatherVibeCore::LoadConfig()`, add it to the conf **and** to `known_keys` or a pattern in `wv_lint.py` |
| A new weather state | `kStates` and `WireState()` in `mod_wv_core.cpp` (and `WV_STATES`), the family/tier/ground switches in `mod_wv_pick.cpp`, `kDefaultHold` in `mod_wv_engine.cpp`, the tables in `wv_lint.py`, `Name()` in `pick_test.cpp` |
| What the client is sent for a state; a new skin | `WireState()` in `mod_wv_core.cpp` — check the id with `wv_client_data.py weather` first |
| When thunder plays, and which sound | the thunder block in `UpdateProfile()`; `Thunder()` in `mod_wv_world.cpp` |
| Where a bolt may land | `StrikeLightningNear()` in `mod_wv_world.cpp` |
| Remark wording | the conf |
