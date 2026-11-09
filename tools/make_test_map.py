#!/usr/bin/env python3
"""Write an open M.A.X. test map: land with clusters of rock, water around the edges.

    python3 tools/make_test_map.py --out SNOW_1.WRL --seed 1

The file has the layout the port's World loader reads (src/world.cpp): "WRL", the layer count (1) and the
size, the minimap, a tile number per cell, the tile count, 64x64 tile images, a 256 color palette and one pass
value per tile (0 land, 1 water, 2 coast, 3 blocked). Every tile is a single color, so the map has no original game
art. The planet file names are fixed by the game, so the output replaces one of them (SNOW_1.WRL is world 0).
"""

import argparse
import random
import struct

TILE_SIZE = 64

# tile number: (pass value, palette index, rgb); the World loader takes palette entries 64 to 159 from the map
TILES = {
    'land': (0, 64, (96, 140, 64)),
    'water': (1, 65, (40, 80, 160)),
    'coast': (2, 66, (190, 170, 110)),
    'rock': (3, 67, (110, 105, 100)),
}
ORDER = ['land', 'water', 'coast', 'rock']


def make_terrain(size, border, rocks, seed):
    """Water within border cells of the edge, one ring of coast, land inside, rock clusters on the land."""
    rng = random.Random(seed)
    terrain = [['land'] * size for _ in range(size)]

    for y in range(size):
        for x in range(size):
            edge = min(x, y, size - 1 - x, size - 1 - y)
            if edge < border:
                terrain[y][x] = 'water'
            elif edge == border:
                terrain[y][x] = 'coast'

    # keep rock off the coast and its neighbors so every shore stays reachable from the land
    low, high = border + 3, size - border - 4
    for _ in range(rocks):
        cx, cy, radius = rng.randint(low, high), rng.randint(low, high), rng.randint(1, 3)
        for y in range(cy - radius, cy + radius + 1):
            for x in range(cx - radius, cx + radius + 1):
                if low <= x <= high and low <= y <= high and (x - cx) ** 2 + (y - cy) ** 2 <= radius * radius:
                    terrain[y][x] = 'rock'

    return terrain


def write_wrl(path, terrain):
    size = len(terrain)
    cells = [terrain[y][x] for y in range(size) for x in range(size)]

    palette = bytearray(256 * 3)
    for name in ORDER:
        _, index, rgb = TILES[name]
        palette[index * 3:index * 3 + 3] = bytes(rgb)

    with open(path, 'wb') as out:
        out.write(b'WRL')
        out.write(struct.pack('<H', 1))
        out.write(struct.pack('<hh', size, size))
        out.write(bytes(TILES[name][1] for name in cells))
        out.write(struct.pack(f'<{len(cells)}H', *(ORDER.index(name) for name in cells)))
        out.write(struct.pack('<H', len(ORDER)))
        for name in ORDER:
            out.write(bytes([TILES[name][1]]) * (TILE_SIZE * TILE_SIZE))
        out.write(palette)
        out.write(bytes(TILES[name][0] for name in ORDER))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, help='output file, e.g. SNOW_1.WRL')
    parser.add_argument('--seed', type=int, default=1, help='seed for the rock placement')
    parser.add_argument('--size', type=int, default=112, help='width and height in cells (the game expects 112)')
    parser.add_argument('--border', type=int, default=4, help='cells of water along each edge')
    parser.add_argument('--rocks', type=int, default=60, help='number of rock clusters')
    parser.add_argument('--show', action='store_true', help='print the map as text')
    args = parser.parse_args()

    terrain = make_terrain(args.size, args.border, args.rocks, args.seed)
    write_wrl(args.out, terrain)

    counts = {name: sum(row.count(name) for row in terrain) for name in ORDER}
    print(f'{args.out}: {args.size}x{args.size}, ' + ', '.join(f'{n} {c}' for n, c in counts.items()))
    if args.show:
        symbol = {'land': '.', 'water': '~', 'coast': ',', 'rock': '#'}
        for row in terrain:
            print(''.join(symbol[name] for name in row))


if __name__ == '__main__':
    main()
