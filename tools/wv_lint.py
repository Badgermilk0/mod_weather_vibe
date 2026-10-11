#!/usr/bin/env python3
"""Offline check of a mod_weather_vibe config file.

    python tools/wv_lint.py conf/mod_weather_vibe.conf.dist [--stats]

Reads the file the way the module does (trailing "# comments" stripped, numbered keys in
numeric order) and reports what would be wrong or pointless once the server loads it:

  ERROR    the module would reject or misread the line
  WARN     it loads, but probably not as intended
  NOTE     worth a look

It cannot check that a zone or area id is the place you think it is - the server validates
ids against AreaTable at startup and `.wvibe where` prints them in game.

Exit status is 1 when there is an ERROR or a WARN, 0 otherwise.
"""
import argparse
import collections
import re
import sys

STATES = {
    0: 'Fine', 1: 'Fog',
    3: 'LightRain', 4: 'MediumRain', 5: 'HeavyRain',
    6: 'LightSnow', 7: 'MediumSnow', 8: 'HeavySnow',
    22: 'LightSandstorm', 41: 'MediumSandstorm', 42: 'HeavySandstorm',
    86: 'Thunders',
}
STATE_BY_NAME = {name.lower(): sid for sid, name in STATES.items()}
FAMILY = {0: 'fine', 1: 'fog', 3: 'rain', 4: 'rain', 5: 'rain', 6: 'snow', 7: 'snow', 8: 'snow',
          22: 'sand', 41: 'sand', 42: 'sand', 86: 'thunder'}
TIER = {3: 1, 4: 2, 5: 3, 6: 1, 7: 2, 8: 3, 22: 1, 41: 2, 42: 3}
LADDERS = ((3, 4, 5), (6, 7, 8), (22, 41, 42))
SEASONS = ('Spring', 'Summer', 'Autumn', 'Winter')
DAYPARTS = ('MORNING', 'AFTERNOON', 'EVENING', 'NIGHT')
TURNS = ('Rain', 'Snow', 'Sandstorm', 'Storm', 'Fog', 'Clear')
TRANSITION_KEYS = ('Step.TimeSeconds.Min', 'Step.TimeSeconds.Max', 'StepSize.Perc', 'Max.Changes.Per.Hour',
                   'Max.Consecutive.Same', 'MinHoldSeconds')
RETIRED_TRANSITION_KEYS = ('SeasonBlendMinutes',)

# What a profile's header comment may promise, and the family that would break the promise.
# "almost no rain" and the like are not promises.
THEME_CLAIMS = (
    (re.compile(r'(?<!almost )\bno snow\b'), 'no snow', 'snow'),
    (re.compile(r'(?<!almost )\bno rain\b'), 'no rain', 'rain'),
    (re.compile(r'(?<!almost )\bno sand\b'), 'no sand', 'sand'),
    (re.compile(r'(?<!almost )\bno thunder'), 'no thunder', 'thunder'),
    (re.compile(r'(?<!almost )\bno fog\b'), 'no fog', 'fog'),
)


class Report:
    def __init__(self):
        self.items = []

    def add(self, level, line, text):
        self.items.append((level, line, text))

    def count(self, level):
        return sum(1 for item in self.items if item[0] == level)


def parse_state(token):
    token = token.strip()
    if token.isdigit():
        return int(token) if int(token) in STATES else None
    return STATE_BY_NAME.get(token.lower())


def to_float(token):
    try:
        return float(token)
    except ValueError:
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('conf')
    parser.add_argument('--stats', action='store_true', help='print the share of each kind of weather per profile and season')
    args = parser.parse_args()

    with open(args.conf, 'rb') as handle:
        raw = handle.read()
    text = raw.decode('utf-8', errors='replace')

    report = Report()
    if b'\r\n' in raw:
        report.add('NOTE', 0, 'file has CRLF line endings (the shipped file uses LF)')

    values = {}                                   # key -> (value, line)
    headers = {}                                  # profile -> lower-cased header comment text
    pending_header = []
    in_header = False

    for number, line in enumerate(text.split('\n'), 1):
        stripped = line.strip()

        # A profile's banner:  "# Name  -  description" between "#====" rules.
        if stripped.startswith('#===='):
            in_header = not in_header
            if in_header:
                pending_header = []
            elif pending_header:
                match = re.match(r'^#\s+(\w+)\s{2}-\s{2}', pending_header[0])
                if match:
                    headers[match.group(1)] = ' '.join(h.lstrip('# ').strip() for h in pending_header).lower()
            continue
        if in_header and stripped.startswith('#'):
            pending_header.append(stripped)
            continue

        if not stripped or stripped[0] in '#[':
            continue
        if '=' not in stripped:
            report.add('ERROR', number, 'not a "key = value" line: ' + stripped)
            continue

        key, value = (part.strip() for part in stripped.split('=', 1))
        if not key.startswith('WeatherVibe'):
            continue
        if key in values:
            report.add('WARN', number, f'{key} is set twice (first on line {values[key][1]}); AzerothCore keeps the first')
            continue

        keeps_hash = key.startswith('WeatherVibe.Remarks.') and key.count('.') == 3
        if not keeps_hash:
            value = value.split('#', 1)[0].strip()
        values[key] = (value.replace('"', ''), number)

    # ---- collect ----------------------------------------------------------------------------
    profiles = collections.defaultdict(lambda: {s: [] for s in SEASONS})   # name -> season -> rows
    transitions = collections.defaultdict(dict)
    neighbours = {}
    zone_map, area_map = {}, {}
    holidays = []
    known_keys = ('WeatherVibe.Enable', 'WeatherVibe.Announce', 'WeatherVibe.Debug', 'WeatherVibeCommand',
                      'WeatherVibe.Profile.Enable', 'WeatherVibe.Profile.ReApply.PerSec', 'WeatherVibe.Pace',
                      'WeatherVibe.Transition.TickSeconds', 'WeatherVibe.Escalation.Strict', 'WeatherVibe.FogBridge.Boost',
                      'WeatherVibe.Front.Strength', 'WeatherVibe.Season', 'WeatherVibe.Season.BlendDays',
                      'WeatherVibe.DayPart.Mode', 'WeatherVibe.Hook.Enable', 'WeatherVibe.Hook.ResyncMinutes',
                      'WeatherVibe.Remarks.Enable', 'WeatherVibe.Remarks.Chance',
                      'WeatherVibe.Remarks.CooldownMinutes', 'WeatherVibe.Remarks.Range')

    def entry(key, value, line):
        """weight,state,min,max -> (weight, state, min, max) or None (reported)."""
        parts = [p.strip() for p in value.split(',')]
        if len(parts) != 4:
            report.add('ERROR', line, f'{key} = {value}: want weight,state,minPct,maxPct')
            return None
        weight, low, high = to_float(parts[0]), to_float(parts[2]), to_float(parts[3])
        state = parse_state(parts[1])
        if weight is None or low is None or high is None:
            report.add('ERROR', line, f'{key} = {value}: weight and percentages must be numbers')
            return None
        if state is None:
            report.add('ERROR', line, f'{key} = {value}: "{parts[1]}" is not a weather state')
            return None
        if weight <= 0:
            report.add('ERROR', line, f'{key} = {value}: weight must be above 0')
            return None
        if high < low:
            report.add('WARN', line, f'{key} = {value}: minimum is above maximum (typo?) - the module swaps them')
            low, high = high, low
        if not (0 <= low <= 100 and 0 <= high <= 100):
            report.add('WARN', line, f'{key} = {value}: percentages are clamped to 0..100')
        return weight, state, low, high

    for key, (value, line) in values.items():
        match = re.match(r'^WeatherVibe\.Profile\.(\w+)\.(Spring|Summer|Autumn|Winter)\.(\d+)$', key)
        if match:
            row = entry(key, value, line)
            if row:
                profiles[match.group(1)][match.group(2)].append((int(match.group(3)), row, line))
            continue

        match = re.match(r'^WeatherVibe\.Profile\.(\w+)\.Transition\.(.+)$', key)
        if match:
            name, setting = match.groups()
            if setting in RETIRED_TRANSITION_KEYS:
                report.add('NOTE', line, f'{key} is retired and ignored (seasons blend by calendar: WeatherVibe.Season.BlendDays)')
            elif setting not in TRANSITION_KEYS:
                report.add('WARN', line, f'{key}: unknown transition setting')
            elif to_float(value) is None:
                report.add('ERROR', line, f'{key} = {value}: not a number')
            else:
                transitions[name][setting] = float(value)
            continue

        match = re.match(r'^WeatherVibe\.Profile\.(\w+)\.Neighbors$', key)
        if match:
            neighbours[match.group(1)] = ([n.strip() for n in value.split(',') if n.strip()], line)
            continue

        match = re.match(r'^WeatherVibe\.Profile\.(\w+)\.DayPartWeight\.(\w+)\.(\w+)$', key) or \
            re.match(r'^WeatherVibe\.()DayPartWeight\.(\w+)\.(\w+)$', key)
        if match:
            if match.group(2) not in DAYPARTS:
                report.add('ERROR', line, f'{key}: daypart must be one of {", ".join(DAYPARTS)}')
            elif match.group(3).lower() not in STATE_BY_NAME:
                report.add('ERROR', line, f'{key}: "{match.group(3)}" is not a weather state name')
            elif to_float(value) is None or float(value) < 0:
                report.add('ERROR', line, f'{key} = {value}: want a multiplier of 0 or more')
            continue

        match = re.match(r'^WeatherVibe\.(Zone|Area)Profile\.(\d+)$', key)
        if match:
            parts = [p.strip() for p in value.split(',')]
            if len(parts) != 2 or not parts[0].isdigit() or int(parts[0]) == 0 or not parts[1]:
                report.add('ERROR', line, f'{key} = {value}: want <id>,<ProfileName>')
                continue
            target = zone_map if match.group(1) == 'Zone' else area_map
            ident = int(parts[0])
            if ident in target:
                report.add('WARN', line, f'{key}: {match.group(1).lower()} {ident} is already mapped to {target[ident][0]} (line {target[ident][1]}); the later one wins')
            target[ident] = (parts[1], line)
            continue

        match = re.match(r'^WeatherVibe\.Holiday\.(\d+)\.(\w+)\.(\d+)$', key)
        if match:
            row = entry(key, value, line)
            if row:
                holidays.append((int(match.group(1)), match.group(2), row, line))
            continue

        if re.match(r'^WeatherVibe\.Intensity\.InternalRange\.(\w+)\.(\w+)$', key):
            daypart, state_name = key.split('.')[-2:]
            parts = [to_float(p) for p in value.split(',')]
            if daypart not in DAYPARTS or state_name.lower() not in STATE_BY_NAME:
                report.add('ERROR', line, f'{key}: unknown daypart or state')
            elif len(parts) != 2 or None in parts:
                report.add('ERROR', line, f'{key} = {value}: want <min>, <max>')
            elif not (0 <= parts[0] <= parts[1] <= 1):
                report.add('WARN', line, f'{key} = {value}: want 0 <= min <= max <= 1')
            continue

        if re.match(r'^WeatherVibe\.Hold\.(\w+)$', key):
            if key.split('.')[-1].lower() not in STATE_BY_NAME:
                report.add('ERROR', line, f'{key}: not a weather state name')
            continue

        if re.match(r'^WeatherVibe\.DayPart\.(MORNING|AFTERNOON|EVENING|NIGHT)\.Start$', key):
            if not re.match(r'^\d{1,2}(:\d{2})?$', value):
                report.add('ERROR', line, f'{key} = {value}: want HH:MM')
            continue

        match = re.match(r'^WeatherVibe\.Remarks\.(\w+)\.(\d+)$', key)
        if match:
            if match.group(1) not in TURNS:
                report.add('ERROR', line, f'{key}: kind must be one of {", ".join(TURNS)}')
            elif len(value) > 200:
                report.add('WARN', line, f'{key}: a spoken line over 200 characters may be cut off')
            continue

        if key not in known_keys:
            report.add('WARN', line, f'{key}: the module does not read this key')

    # ---- profiles ---------------------------------------------------------------------------
    mapped = {name for name, _ in zone_map.values()} | {name for name, _ in area_map.values()}

    for name in sorted(mapped - set(profiles)):
        lines = [l for n, l in list(zone_map.values()) + list(area_map.values()) if n == name]
        report.add('ERROR', lines[0], f'profile "{name}" is mapped but has no entries')
    for name in sorted(set(profiles) - mapped):
        first = min(row[2] for season in profiles[name].values() for row in season)
        report.add('WARN', first, f'profile "{name}" is defined but no zone or area maps to it, so it never runs')

    for name, seasons in sorted(profiles.items()):
        given = [s for s in SEASONS if seasons[s]]
        if 0 < len(given) < len(SEASONS) and len(given) != 1:
            report.add('NOTE', seasons[given[0]][0][2], f'{name}: no entries for {", ".join(s for s in SEASONS if not seasons[s])}; those seasons borrow another')

        if name not in transitions and name in mapped:
            report.add('NOTE', seasons[given[0]][0][2], f'{name}: no Transition.* settings; the module defaults apply')

        for season in given:
            rows = seasons[season]
            line = rows[0][2]
            indexes = sorted(row[0] for row in rows)
            if indexes != list(range(1, len(indexes) + 1)):
                report.add('NOTE', line, f'{name}.{season}: entry numbers are not 1..{len(indexes)} ({indexes}) - fine, but check nothing was lost')

            present = collections.Counter(row[1][1] for row in rows)
            for state, count in present.items():
                if count > 1:
                    report.add('WARN', line, f'{name}.{season}: {STATES[state]} appears {count} times; merge the rows')

            for ladder in LADDERS:
                for tier in (1, 2):
                    if ladder[tier] in present and ladder[tier - 1] not in present:
                        report.add('WARN', line, f'{name}.{season}: {STATES[ladder[tier]]} has no {STATES[ladder[tier - 1]]} below it; '
                                                 'weather escalates a tier at a time, so it will rarely be reached')

            # With medium/heavy rain in the table, thunder grows out of that rain; otherwise it stands alone.
            if not any(TIER.get(state, 0) <= 1 for state in present):
                report.add('WARN', line, f'{name}.{season}: nothing lighter than medium - there is no way to ease off')

        theme = headers.get(name, '')
        families = collections.defaultdict(list)
        for season in given:
            for _, row, line in seasons[season]:
                families[FAMILY[row[1]]].append((season, line))
        for pattern, phrase, family in THEME_CLAIMS:
            if pattern.search(theme) and families.get(family):
                where = ', '.join(f'{s} (line {l})' for s, l in families[family])
                report.add('WARN', families[family][0][1], f'{name}: its header says "{phrase}" but it has {family}: {where}')

        ts = transitions.get(name, {})
        if ts.get('Step.TimeSeconds.Min', 0) > ts.get('Step.TimeSeconds.Max', 1e9):
            report.add('WARN', 0, f'{name}: Step.TimeSeconds.Min is above Max; the module swaps them')
        if 'Max.Changes.Per.Hour' in ts and not 1 <= ts['Max.Changes.Per.Hour'] <= 120:
            report.add('WARN', 0, f'{name}: Max.Changes.Per.Hour is clamped to 1..120')

    for name, (others, line) in sorted(neighbours.items()):
        if name not in profiles:
            report.add('WARN', line, f'Neighbors given for "{name}", which is not a profile')
        for other in others:
            if other not in profiles:
                report.add('WARN', line, f'{name}.Neighbors names "{other}", which is not a profile')
            elif other == name:
                report.add('NOTE', line, f'{name}.Neighbors lists itself')

    for holiday_id, name, row, line in holidays:
        if name not in profiles:
            report.add('WARN', line, f'holiday {holiday_id} row for "{name}", which is not a profile')

    # ---- output -----------------------------------------------------------------------------
    order = {'ERROR': 0, 'WARN': 1, 'NOTE': 2}
    for level, line, message in sorted(report.items, key=lambda item: (order[item[0]], item[1])):
        print(f'{level:5} {"line " + str(line) if line else "":>10}  {message}')

    print(f'\n{len(profiles)} profile(s), {len(zone_map)} zone(s), {len(area_map)} area(s), '
          f'{sum(len(v[0]) for v in neighbours.values())} border(s), {len(holidays)} holiday row(s): '
          f'{report.count("ERROR")} error(s), {report.count("WARN")} warning(s), {report.count("NOTE")} note(s)')

    if args.stats:
        print('\nShare of weight by kind of weather (before time-of-day, fronts and escalation):')
        for name, seasons in sorted(profiles.items()):
            for season in SEASONS:
                rows = seasons[season]
                if not rows:
                    continue
                total = sum(row[1][0] for row in rows)
                share = collections.Counter()
                for _, row, _ in rows:
                    share[FAMILY[row[1]]] += row[0]
                parts = '  '.join(f'{family} {100 * weight / total:.0f}%' for family, weight in share.most_common())
                print(f'  {name:20} {season:7} {parts}')

    return 1 if report.count('ERROR') or report.count('WARN') else 0


if __name__ == '__main__':
    sys.exit(main())
