#!/usr/bin/env python3
"""Read weather-related tables out of a WoW 3.3.5a client. Read-only: the client is never written to.

    python tools/wv_client_data.py areas   <client folder> [-o tools/area_ids.tsv]
    python tools/wv_client_data.py weather <client folder>
    python tools/wv_client_data.py sounds  <client folder> <regex>

  areas    writes the table wv_lint.py checks zone and area ids against:
           id, parent zone (0 = it is a zone), map id, map type, map name, area name
  weather  prints Weather.dbc: every weather id the client can draw, with its particle type,
           ambience sound, colour and texture (this is what `.wvibe audition <id>` shows)
  sounds   prints SoundEntries.dbc rows whose name or folder matches, with their files -
           for choosing thunder sounds (WeatherVibe.Thunder.Sounds.*)

<client folder> is the one holding WoW.exe. Only the stock locale archives are read
(Data/<locale>/patch-<locale>-3.MPQ and older), so custom patch archives do not change the answer;
the server's own DBC files come from the same stock data.
"""
import argparse
import bz2
import glob
import os
import re
import struct
import sys
import zlib

BACKSLASH = chr(92)
MAP_TYPES = {0: 'world', 1: 'dungeon', 2: 'raid', 3: 'battleground', 4: 'arena'}
EFFECTS = {0: 'none', 1: 'rain', 2: 'snow', 3: 'sandstorm'}


# ---- MPQ (just enough of it: hash lookup, zlib/bzip2 sectors, no encryption) -----------------

def _crypt_table():
    table = [0] * 0x500
    seed = 0x00100001
    for index1 in range(0x100):
        index2 = index1
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            high = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            table[index2] = high | (seed & 0xFFFF)
            index2 += 0x100
    return table


CRYPT = _crypt_table()


def mpq_hash(name, hash_type):
    seed1, seed2 = 0x7FED7FED, 0xEEEEEEEE
    for char in name.upper().replace('/', BACKSLASH):
        code = ord(char)
        seed1 = CRYPT[(hash_type << 8) + code] ^ ((seed1 + seed2) & 0xFFFFFFFF)
        seed2 = (code + seed1 + seed2 + (seed2 << 5) + 3) & 0xFFFFFFFF
    return seed1


def mpq_decrypt(data, key):
    count = len(data) // 4
    values = list(struct.unpack('<%dI' % count, data[:count * 4]))
    seed = 0xEEEEEEEE
    for i in range(count):
        seed = (seed + CRYPT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        plain = values[i] ^ ((key + seed) & 0xFFFFFFFF)
        key = ((((~key) & 0xFFFFFFFF) << 0x15) + 0x11111111) & 0xFFFFFFFF | (key >> 0x0B)
        seed = (plain + seed + (seed << 5) + 3) & 0xFFFFFFFF
        values[i] = plain
    return values


def _decompress(data):
    method, payload = data[0], data[1:]
    if method == 0x02:
        return zlib.decompress(payload)
    if method == 0x10:
        return bz2.decompress(payload)
    raise ValueError('MPQ compression 0x%02x is not supported' % method)


class Archive:
    def __init__(self, path):
        self.handle = handle = open(path, 'rb')
        offset = 0
        while True:
            handle.seek(offset)
            magic = handle.read(4)
            if magic == bytes([0x4D, 0x50, 0x51, 0x1A]):
                break
            if len(magic) < 4 or offset > (1 << 20):
                raise ValueError('%s is not an MPQ archive' % path)
            offset += 512

        header = handle.read(40)
        _, _, version, shift, hash_pos, block_pos, hash_count, block_count = struct.unpack('<IIHHIIII', header[:28])
        hi_block = hash_hi = block_hi = 0
        if version >= 1:
            hi_block, hash_hi, block_hi = struct.unpack('<QHH', header[28:40])

        self.base, self.sector, self.hash_count = offset, 512 << shift, hash_count
        handle.seek(offset + hash_pos + (hash_hi << 32))
        self.hashes = mpq_decrypt(handle.read(hash_count * 16), mpq_hash('(hash table)', 3))
        handle.seek(offset + block_pos + (block_hi << 32))
        self.blocks = mpq_decrypt(handle.read(block_count * 16), mpq_hash('(block table)', 3))
        self.block_hi = [0] * block_count
        if hi_block:
            handle.seek(offset + hi_block)
            self.block_hi = list(struct.unpack('<%dH' % block_count, handle.read(block_count * 2)))

    def _find(self, name):
        mask = self.hash_count - 1
        index = start = mpq_hash(name, 0) & mask
        name_a, name_b = mpq_hash(name, 1), mpq_hash(name, 2)
        while True:
            a, b, _, block = self.hashes[4 * index:4 * index + 4]
            if block == 0xFFFFFFFF:
                return None
            if a == name_a and b == name_b and block != 0xFFFFFFFE:
                return block
            index = (index + 1) & mask
            if index == start:
                return None

    def size(self, name):
        block = self._find(name)
        if block is None:
            return None
        _, _, size, flags = self.blocks[4 * block:4 * block + 4]
        return size if flags & 0x80000000 and not flags & 0x02000000 else None

    def read(self, name):
        block = self._find(name)
        if block is None:
            return None
        position, packed, size, flags = self.blocks[4 * block:4 * block + 4]
        position += self.block_hi[block] << 32
        if not flags & 0x80000000 or flags & 0x02000000:
            return None
        if flags & 0x00010000:
            raise ValueError('%s is encrypted' % name)

        handle = self.handle
        handle.seek(self.base + position)
        if flags & 0x01000000:                                  # stored as a single unit
            data = handle.read(packed)
            return _decompress(data) if (flags & 0x200 and packed < size) else data
        if not flags & 0x300:
            return handle.read(size)

        sectors = (size + self.sector - 1) // self.sector
        entries = sectors + 1 + (1 if flags & 0x04000000 else 0)
        offsets = struct.unpack('<%dI' % entries, handle.read(entries * 4))
        parts = []
        for i in range(sectors):
            handle.seek(self.base + position + offsets[i])
            data = handle.read(offsets[i + 1] - offsets[i])
            if len(data) < min(self.sector, size - i * self.sector):
                data = _decompress(data)
            parts.append(data)
        return b''.join(parts)


class Dbc:
    def __init__(self, data):
        if data[:4] != b'WDBC':
            raise ValueError('not a DBC file')
        self.count, self.fields, self.record_size, _ = struct.unpack('<4I', data[4:20])
        self.records = data[20:20 + self.count * self.record_size]
        self.strings = data[20 + self.count * self.record_size:]

    def ints(self, index):
        return struct.unpack('<%dI' % self.fields, self.records[index * self.record_size:(index + 1) * self.record_size])

    def floats(self, index):
        return struct.unpack('<%df' % self.fields, self.records[index * self.record_size:(index + 1) * self.record_size])

    def text(self, offset):
        return self.strings[offset:self.strings.index(bytes([0]), offset)].decode('utf-8', 'replace')


# ---- the client -------------------------------------------------------------------------------

class Client:
    """Stock data only. Tables come from the newest stock locale archive that has them."""

    def __init__(self, folder):
        data = None
        for name in os.listdir(folder):
            if name.lower() == 'data' and os.path.isdir(os.path.join(folder, name)):
                data = os.path.join(folder, name)
        if not data:
            sys.exit('no Data folder under ' + folder)

        locales = [d for d in os.listdir(data) if os.path.isdir(os.path.join(data, d)) and re.fullmatch(r'[a-zA-Z]{4}', d)]
        if not locales:
            sys.exit('no locale folder (enUS, deDE, ...) under ' + data)
        self.locale = locales[0]
        locale_dir = os.path.join(data, self.locale)

        def find(directory, wanted):
            for name in os.listdir(directory):
                if name.lower() == wanted.lower():
                    return os.path.join(directory, name)
            return None

        low = self.locale.lower()
        # Newest first.
        self.table_archives = [p for p in (find(locale_dir, 'patch-%s-3.mpq' % low), find(locale_dir, 'patch-%s-2.mpq' % low),
                                           find(locale_dir, 'patch-%s.mpq' % low), find(locale_dir, 'locale-%s.mpq' % low)) if p]
        self.file_archives = [p for p in (find(data, n) for n in ('patch-3.mpq', 'patch-2.mpq', 'patch.mpq', 'lichking.mpq',
                                                                  'expansion.mpq', 'common-2.mpq', 'common.mpq')) if p]
        self._open = {}

    def _archive(self, path):
        if path not in self._open:
            self._open[path] = Archive(path)
        return self._open[path]

    def table(self, name):
        path = 'DBFilesClient' + BACKSLASH + name + '.dbc'
        for archive in self.table_archives:
            data = self._archive(archive).read(path)
            if data:
                return Dbc(data)
        sys.exit('%s.dbc was not found in the stock archives of %s' % (name, self.locale))

    def file_size(self, path):
        for archive in self.file_archives:
            size = self._archive(archive).size(path)
            if size:
                return size
        return None


def load_sounds(client):
    table = client.table('SoundEntries')
    sounds = {}
    for i in range(table.count):
        ints, floats = table.ints(i), table.floats(i)
        sounds[ints[0]] = {
            'type': ints[1], 'name': table.text(ints[2]),
            'files': [table.text(ints[k]) for k in range(3, 13) if ints[k]],
            'folder': table.text(ints[23]), 'volume': floats[24],
            'min': floats[26], 'cutoff': floats[27],
        }
    return sounds


def cmd_areas(client, args):
    areas = client.table('AreaTable')
    maps = client.table('Map')
    map_info = {}
    for i in range(maps.count):
        ints = maps.ints(i)
        map_info[ints[0]] = (maps.text(ints[1]), ints[2])

    rows = []
    for i in range(areas.count):
        ints = areas.ints(i)
        internal, map_type = map_info.get(ints[1], ('?', -1))
        rows.append((ints[0], ints[2], ints[1], MAP_TYPES.get(map_type, str(map_type)), internal, areas.text(ints[11]).strip() or '(unnamed)'))
    rows.sort()

    with open(args.output, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write('# AreaTable.dbc of a stock 3.3.5a client (%s). Written by tools/wv_client_data.py; do not edit.\n' % client.locale)
        handle.write('# id\tparent\tmap\tmap_type\tmap_name\tname\n')
        for row in rows:
            handle.write('\t'.join(str(value) for value in row) + '\n')
    print('%d areas written to %s' % (len(rows), args.output))


def cmd_weather(client, args):
    table = client.table('Weather')
    sounds = load_sounds(client)
    print('id   particles  sky  colour          sound                              texture')
    for i in range(table.count):
        ints, floats = table.ints(i), table.floats(i)
        sound = sounds.get(ints[1], {}).get('name', '') if ints[1] else '-'
        texture = table.text(ints[7]) if table.fields > 7 else ''
        print('%-4d %-10s %-4s %.1f %.1f %.1f     %-34s %s' % (
            ints[0], EFFECTS.get(ints[2], ints[2]), 'dims' if floats[3] else '-', floats[4], floats[5], floats[6], sound, texture))
    print('\nsky: the row\'s fourth number, 1 ("dims") for ordinary weather and 0 for a few special rows. It is believed to be')
    print('whether the sky shifts to its storm look as the weather builds; that has not been verified.')


def cmd_sounds(client, args):
    pattern = re.compile(args.pattern, re.I)
    for sound_id, sound in sorted(load_sounds(client).items()):
        if not (pattern.search(sound['name']) or pattern.search(sound['folder'])):
            continue
        files = []
        for name in sound['files']:
            size = client.file_size(sound['folder'].rstrip(BACKSLASH) + BACKSLASH + name)
            files.append('%s (%s)' % (name, '%d kB' % (size // 1024) if size else 'MISSING'))
        print('%-6d type %-2d vol %.2f hearing %3.0f-%3.0f yd  %-34s %s' % (
            sound_id, sound['type'], sound['volume'], sound['min'], sound['cutoff'], sound['name'], ', '.join(files)))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)

    areas = sub.add_parser('areas', help='write the area table wv_lint.py checks ids against')
    areas.add_argument('client')
    areas.add_argument('-o', '--output', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'area_ids.tsv'))

    weather = sub.add_parser('weather', help='print every weather id the client can draw')
    weather.add_argument('client')

    sounds = sub.add_parser('sounds', help='print sound entries matching a regex')
    sounds.add_argument('client')
    sounds.add_argument('pattern')

    args = parser.parse_args()
    client = Client(args.client)
    {'areas': cmd_areas, 'weather': cmd_weather, 'sounds': cmd_sounds}[args.command](client, args)


if __name__ == '__main__':
    main()
