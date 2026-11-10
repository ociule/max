#!/usr/bin/env python3
"""Summarize bench.py results.

    python3 summarize.py RESULTS_FOLDER [RESULTS_FOLDER...]

A results folder holds one sub folder per game with its result.json, e.g. /workspace/run/results/baseline.
"""

import json
import statistics
import sys
from pathlib import Path


# two sided 95% Student t quantiles by degrees of freedom
T95 = {1: 12.71, 2: 4.30, 3: 3.18, 4: 2.78, 5: 2.57, 6: 2.45, 7: 2.36, 8: 2.31, 9: 2.26, 10: 2.23, 15: 2.13, 20: 2.09}


def t95(df):
    return T95.get(df) or T95[max(k for k in T95 if k <= df)]


def describe(values):
    mean = statistics.mean(values)
    if len(values) < 2:
        return f'{mean:8.1f}'
    stdev = statistics.stdev(values)
    half = t95(len(values) - 1) * stdev / len(values) ** 0.5
    return f'{mean:8.1f} +-{half:6.1f} (sd {stdev:6.1f}, cv {100 * stdev / mean:4.1f}%)'


for label in sys.argv[1:]:
    games = sorted(Path(label).glob('*/result.json'))
    print(f'== {label}: {len(games)} games')
    walls, per_turn, first, full = [], [], [], []
    for path in games:
        r = json.loads(path.read_text())
        times = {int(k): v for k, v in r['turn_times'].items()}
        turns = len(times)
        # time from the first logged turn to the last: excludes the start up and the game setup
        spt = (times[max(times)] - times[1]) / (turns - 1) if turns > 1 else float('nan')
        status = 'CRASH' if r['crashed'] else f"exit {r['exit_code']}"
        ended = r['result'].split(':')[0] if r['result'] else 'no game over line'
        print(f"  {path.parent.name:8} {status:7} wall {r['wall']:7.1f}s turns {turns:3} "
              f"s/turn {spt:5.1f} setup+turn1 {times.get(1, float('nan')):5.1f}s  {ended}")
        walls.append(r['wall'])
        if turns == r['turns']:
            full.append(r['wall'])
        per_turn.append(spt)
        first.append(times.get(1, float('nan')))
    if walls:
        print(f'  wall time     {describe(walls)}')
        print(f'  s per turn    {describe(per_turn)}')
        if full:
            print(f'  wall time of the {len(full)} games that played all their turns {describe(full)}')
