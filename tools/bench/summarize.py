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


def load_stats(game_dir):
    """The game line of stats.jsonl (in the game folder or its max-port sub folder), None without statistics."""
    for path in (game_dir / 'stats.jsonl', game_dir / 'max-port' / 'stats.jsonl'):
        if path.exists():
            lines = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
            games = [line for line in lines if line['type'] == 'game']
            return games[-1] if games else None
    return None


def stats_line(g):
    ticks = max(g['ticks'], 1)
    cutoffs = sum(cutoff for _, cutoff in g['budget_checks_cutoffs'].values())
    checks = sum(check for check, _ in g['budget_checks_cutoffs'].values())
    paths = g['paths']
    process = g.get('process', {})
    return (f"ms/tick {1000 * g['wall_s'] / ticks:5.1f} ticks {g['ticks']:6} tick work {100 * g['tick_s'] / g['wall_s']:4.1f}% "
            f"present {100 * g['present_s'] / g['wall_s']:4.1f}% | AI cutoffs {cutoffs}/{checks} "
            f"reminders left {g['reminders_left_at_cutoffs']} | paths {paths['dispatched']} "
            f"lag ticks mean {paths['latency_ticks_mean']:.2f} max {paths['latency_ticks_max']} "
            f"ms mean {paths['latency_ms_mean']:.1f} | ends idle {g['end_turn']['by_idle_limit']} "
            f"timer {g['end_turn']['by_timer']} | cpu {process.get('user_s', 0):.0f}s "
            f"rss {process.get('peak_rss_kb', 0) / 1024:.0f}MB")


for label in sys.argv[1:]:
    games = sorted(Path(label).glob('*/result.json'))
    print(f'== {label}: {len(games)} games')
    walls, per_turn, first, full = [], [], [], []
    for path in games:
        r = json.loads(path.read_text())
        times = {int(k): v for k, v in r['turn_times'].items()}
        turns = len(times)
        stats = load_stats(path.parent)
        if stats and stats.get('turns'):
            # exact: the game's own clock from the first team turn to the end, the log polling is too coarse for
            # fast games
            spt = (stats['wall_s'] - stats['setup_s']) / stats['turns']
        else:
            # time from the first logged turn to the last: excludes the start up and the game setup
            spt = (times[max(times)] - times[1]) / (turns - 1) if turns > 1 else float('nan')
        status = 'CRASH' if r['crashed'] else f"exit {r['exit_code']}"
        ended = r['result'].split(':')[0] if r['result'] else 'no game over line'
        print(f"  {path.parent.name:8} {status:7} wall {r['wall']:7.1f}s turns {turns:3} "
              f"s/turn {spt:5.1f} setup+turn1 {times.get(1, float('nan')):5.1f}s  {ended}")
        if stats:
            print(f"           {stats_line(stats)}")
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
