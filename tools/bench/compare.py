#!/usr/bin/env python3
"""Compare two sets of benchmark games: speed, computer player handicap and where the games diverge.

    python3 compare.py BASE_RESULTS_FOLDER NEW_RESULTS_FOLDER

Both folders hold one sub folder per game (<seed>-<rep>) with result.json and stats.jsonl (directly or in max-port/).
Games are paired by folder name, so run both sets with the same seeds.

The handicap figures are per game sums of the stats.jsonl team turn lines:
- cutoffs: how often a time limited computer player loop stopped with work left, per 1000 time checks.
- left: computer player reminders left undone when the thinking time ran out, per 100 team turns.
- path lag: ticks from a path search dispatch to its delivery, mean and maximum.
- timer: team turns ended by the 45 s turn timer instead of the computer player.
A new build must not be worse than the base on these, lower is better for all of them.
"""

import json
import statistics
import sys
from pathlib import Path


def load(game_dir):
    for path in (game_dir / 'stats.jsonl', game_dir / 'max-port' / 'stats.jsonl'):
        if path.exists():
            return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    return []


def handicap(lines):
    turns = [line for line in lines if line['type'] == 'team_turn']
    checks = sum(check for line in turns for check, _ in line['budget_checks_cutoffs'].values())
    cutoffs = sum(cutoff for line in turns for _, cutoff in line['budget_checks_cutoffs'].values())
    left = sum(line['reminders_left_at_cutoffs'] for line in turns)
    delivered = sum(line['paths']['delivered_in_tick'] + line['paths']['delivered_between_ticks'] for line in turns)
    lag_sum = sum(line['paths']['latency_ticks_mean'] * (line['paths']['delivered_in_tick'] +
                                                         line['paths']['delivered_between_ticks']) for line in turns)
    return {'team_turns': len(turns),
            'cutoffs_per_1000': 1000 * cutoffs / checks if checks else 0.0,
            'left_per_100_turns': 100 * left / len(turns) if turns else 0.0,
            'path_lag_mean': lag_sum / delivered if delivered else 0.0,
            'path_lag_max': max((line['paths']['latency_ticks_max'] for line in turns), default=0),
            'timer_ends': sum(line['end_turn']['by_timer'] for line in turns),
            'ticks': sum(line['ticks'] for line in turns)}


def divergence(base, new):
    """The first team turn whose checksum or random number draws differ, None if all compared turns match."""
    base_turns = [line for line in base if line['type'] == 'team_turn']
    new_turns = [line for line in new if line['type'] == 'team_turn']
    for a, b in zip(base_turns, new_turns):
        if (a['checksum'], a['rng_draws'], a['rng_state']) != (b['checksum'], b['rng_draws'], b['rng_state']):
            return f"turn {a['turn']} {a['team']}"
    return f'none in {min(len(base_turns), len(new_turns))} team turns'


def speed(game_dir, lines):
    """Wall time, turns and seconds per turn. The game's own clock when stats.jsonl has the game line (the log
    polling of bench.py is too coarse for fast games), else the logged turn times."""
    result = json.loads((game_dir / 'result.json').read_text())
    times = {int(k): v for k, v in result['turn_times'].items()}
    games = [line for line in lines if line['type'] == 'game']
    if games and games[-1].get('turns'):
        game = games[-1]
        return result['wall'], game['turns'], (game['wall_s'] - game['setup_s']) / game['turns']
    per_turn = (times[max(times)] - times[1]) / (len(times) - 1) if len(times) > 1 else float('nan')
    return result['wall'], len(times), per_turn


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)

    base_dir, new_dir = Path(sys.argv[1]), Path(sys.argv[2])
    names = sorted({path.parent.name for path in base_dir.glob('*/result.json')} &
                   {path.parent.name for path in new_dir.glob('*/result.json')})
    rows = []

    print(f'base {base_dir}\nnew  {new_dir}\n')
    print(f"{'game':6} {'':4} {'wall s':>8} {'turns':>5} {'s/turn':>7} {'cutoffs/1000':>12} {'left/100':>8} "
          f"{'lag mean':>8} {'lag max':>7} {'timer':>5}  diverges at")

    for name in names:
        base, new = load(base_dir / name), load(new_dir / name)
        for label, folder, lines in (('base', base_dir, base), ('new', new_dir, new)):
            wall, turns, per_turn = speed(folder / name, lines)
            h = handicap(lines)
            rows.append((label, wall, turns, per_turn, h))
            print(f"{name:6} {label:4} {wall:8.1f} {turns:5} {per_turn:7.2f} {h['cutoffs_per_1000']:12.3f} "
                  f"{h['left_per_100_turns']:8.1f} {h['path_lag_mean']:8.3f} {h['path_lag_max']:7} "
                  f"{h['timer_ends']:5}" + (f'  {divergence(base, new)}' if label == 'new' else ''))

    for label in ('base', 'new'):
        mine = [row for row in rows if row[0] == label]
        if mine:
            print(f"\n{label}: mean s/turn {statistics.mean(row[3] for row in mine):.2f}, "
                  f"mean cutoffs/1000 {statistics.mean(row[4]['cutoffs_per_1000'] for row in mine):.3f}, "
                  f"left/100 {statistics.mean(row[4]['left_per_100_turns'] for row in mine):.1f}, "
                  f"path lag max {max(row[4]['path_lag_max'] for row in mine)}, "
                  f"timer ends {sum(row[4]['timer_ends'] for row in mine)}")

    base_speed = [row[3] for row in rows if row[0] == 'base']
    new_speed = [row[3] for row in rows if row[0] == 'new']
    if base_speed and new_speed:
        print(f'\nspeedup in s/turn: {statistics.mean(base_speed) / statistics.mean(new_speed):.2f}x')


if __name__ == '__main__':
    main()
