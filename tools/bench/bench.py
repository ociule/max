#!/usr/bin/env python3
"""Run hidden AI vs AI games and record their wall clock time.

    python3 bench.py --run-dir /workspace/run --bin bin/max-port --label base --seeds 1,2,3 --turns 100 \
        [--parallel 3] [-- extra game args]

The run folder holds the game binary (--bin is relative to it), PATCHES.RES, the JSON assets and the WRL maps; the
game runs with it as the working folder. Make the test map with tools/make_test_map.py --out SNOW_1.WRL --seed 1.

Every game runs in its own XDG_DATA_HOME (<run-dir>/results/<label>/<seed>-<rep>/), so games never share settings
or logs.
The game log has no timestamps: the runner polls it and stamps each "Turn N" line as it appears, which gives the
time per turn. results/<label>/<seed>-<rep>/result.json holds the totals, the per turn times and the game over line.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

RUN = Path.cwd()
TURN_RE = re.compile(r'Command line: Turn (\d+):')


def run_game(binary, out_dir, seed, turns, extra, timeout):
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in ('max-port',):
        target = out_dir / old
        if target.exists():
            subprocess.run(['rm', '-rf', str(target)], check=True)

    env = dict(os.environ, XDG_DATA_HOME=str(out_dir))
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:alloc_dealloc_mismatch=0')
    args = [str(RUN / binary), '--hidden', '--turns', str(turns), '--seed', str(seed), *extra]
    log = out_dir / 'max-port' / 'stdout.txt'

    start = time.monotonic()
    with open(out_dir / 'stderr.txt', 'w') as err:
        game = subprocess.Popen(args, cwd=RUN, env=env, stdout=subprocess.DEVNULL, stderr=err)

    turn_times = {}
    position = 0
    result_line = ''
    while True:
        finished = game.poll() is not None
        if log.exists():
            with open(log, 'r', errors='replace') as stream:
                stream.seek(position)
                chunk = stream.read()
                # only consume complete lines
                cut = chunk.rfind('\n') + 1
                position += len(chunk[:cut].encode('utf-8', errors='replace'))
                now = time.monotonic() - start
                for line in chunk[:cut].splitlines():
                    match = TURN_RE.search(line)
                    if match:
                        turn_times[int(match.group(1))] = now
                    if 'Game over' in line:
                        result_line = line.split('Command line: ', 1)[-1]
        if finished:
            break
        if time.monotonic() - start > timeout:
            game.kill()
            game.wait()
            break
        time.sleep(0.25)

    wall = time.monotonic() - start
    crash_dir = out_dir / 'max-port' / 'crashes'
    crashed = crash_dir.exists() and any(crash_dir.iterdir())
    result = {'seed': seed, 'turns': turns, 'args': args, 'exit_code': game.returncode, 'wall': wall,
              'turns_logged': len(turn_times), 'turn_times': turn_times, 'result': result_line, 'crashed': crashed}
    (out_dir / 'result.json').write_text(json.dumps(result, indent=1))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--run-dir', default='.', help='run folder (default: the working folder)')
    parser.add_argument('--bin', required=True, help='game binary, relative to the run folder')
    parser.add_argument('--label', required=True)
    parser.add_argument('--seeds', default='1')
    parser.add_argument('--reps', type=int, default=1, help='games per seed')
    parser.add_argument('--turns', type=int, default=100)
    parser.add_argument('--parallel', type=int, default=1)
    parser.add_argument('--timeout', type=float, default=4 * 3600)
    parser.add_argument('extra', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    global RUN
    RUN = Path(args.run_dir).resolve()
    extra = args.extra[1:] if args.extra[:1] == ['--'] else args.extra

    jobs = [(int(seed), rep) for rep in range(args.reps) for seed in args.seeds.split(',')]
    lock = threading.Lock()

    def worker():
        while True:
            with lock:
                if not jobs:
                    return
                seed, rep = jobs.pop(0)
            out_dir = RUN / 'results' / args.label / f'{seed}-{rep}'
            r = run_game(args.bin, out_dir, seed, args.turns, extra, args.timeout)
            with lock:
                print(f"{args.label} seed {seed} rep {rep}: exit {r['exit_code']} wall {r['wall']:.1f}s "
                      f"turns {r['turns_logged']} crashed {r['crashed']} | {r['result'][:120]}", flush=True)

    threads = [threading.Thread(target=worker) for _ in range(args.parallel)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()


if __name__ == '__main__':
    sys.exit(main())
