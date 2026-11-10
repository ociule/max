#!/usr/bin/env python3
"""Print the unit animation tables (the D_* resources) of an original MAX.RES.

    python3 tools/dump_frame_tables.py path/to/MAX.RES

Hidden games do not load MAX.RES and use a stand-in copy of these tables (src/unit.cpp). The tables pace part of the
game logic: an explosion lives until its last frame, a bridge rises frame by frame. This script prints the original
values so the stand-in can match them exactly. It only reads the file and needs nothing but Python 3.

The game reads a D_* resource as signed bytes (src/unit.cpp, Unit::Unit): image base and count, turret image base and
count, firing image base and count, connector image base and count, then, for turret units, eight x/y turret mount
offsets. The effect sprites' own frame counts are printed as a cross-check of the image counts.
"""

import hashlib
import struct
import sys

# sprites whose frame sequences pace game logic, for the cross-check
EFFECT_SPRITES = ['LNDEXPLD', 'AIREXPLD', 'SEAEXPLD', 'BLDEXPLD', 'HITEXPLD', 'RKTSMOKE', 'TRPBUBLE', 'BRIDGE',
                  'RADAR', 'SCANNER', 'AWAC']


def read_index(data):
    magic, offset, size = struct.unpack_from('<4sii', data, 0)
    if magic != b'RES0':
        raise SystemExit(f'not a resource file (header {magic!r})')

    index = {}
    for position in range(offset, offset + size, 16):
        tag, data_offset, data_size = struct.unpack_from('<8sii', data, position)
        name = tag.split(b'\0', 1)[0].decode('ascii', errors='replace')
        # the game keeps the first entry of a name
        index.setdefault(name, (data_offset, data_size))

    return index


def signed(values):
    return [value - 256 if value > 127 else value for value in values]


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)

    with open(sys.argv[1], 'rb') as stream:
        data = stream.read()

    index = read_index(data)
    tables = sorted(name for name in index if name.startswith('D_'))

    print(f'# file {sys.argv[1]}')
    print(f'# size {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}')
    print(f'# {len(index)} resources, {len(tables)} D_* tables')
    print('#')
    print('# name: size | image base count | turret base count | firing base count | connector base count | '
          'turret offsets x,y for angles 0-7 | raw bytes')

    for name in tables:
        offset, size = index[name]
        raw = data[offset:offset + size]
        values = signed(raw)
        head = values[:8] + [0] * (8 - len(values[:8]))
        offsets = values[8:24]
        pairs = ' '.join(f'{offsets[i]},{offsets[i + 1]}' for i in range(0, len(offsets) - 1, 2))
        print(f'{name}: {size} | {head[0]} {head[1]} | {head[2]} {head[3]} | {head[4]} {head[5]} | '
              f'{head[6]} {head[7]} | {pairs or "-"} | {raw.hex()}')

    print('#')
    print('# effect sprite frame counts (first 16 bit word of the sprite)')

    for name in EFFECT_SPRITES:
        if name in index:
            offset, size = index[name]
            count = struct.unpack_from('<H', data, offset)[0] if size >= 2 else None
            print(f'sprite {name}: {count} frames, {size} bytes')
        else:
            print(f'sprite {name}: missing')


if __name__ == '__main__':
    main()
