#!/usr/bin/env python3
"""Run a batch of Jev against classic games in parallel and print one result table.

    python3 tools/jev_batch.py --label b1
    python3 tools/jev_batch.py --label b1 --games 0:42,3:7,6:13 --turns 100
    python3 tools/jev_batch.py --summarize b1          the table of a batch run before

Each game is world:seed, played with tools/jev_match.py as runs/LABEL-wWORLD-sSEED/. The table goes to the screen and
to runs/LABEL.md, the numbers to runs/LABEL.json. For the Jev team (red unless --controllers says otherwise):
rank, points, asset worth against the best other team, the turn its first factory stood, the turn it first had
points, units and buildings lost, questions asked and their input tokens.
"""

import argparse
import json
import re
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNS = ROOT / 'runs'
TEAMS = ['red', 'green', 'blue', 'gray']
FACTORIES = ('Heavy Vehicle Plant', 'Light Vehicle Plant', 'Air Units Plant', 'Shipyard', 'Training Hall')


def run_game(label, world, seed, turns, controllers, mock):
    name = f'{label}-w{world}-s{seed}'
    command = [sys.executable, str(ROOT / 'tools' / 'jev_match.py'), '--name', name, '--force'] + \
              (['--mock'] if mock else []) + \
              ['--', '--controllers', controllers, '--world', str(world), '--seed', str(seed), '--turns', str(turns)]
    subprocess.run(command)
    return name


def summarize_game(name, jev_team):
    run = RUNS / name
    log = (run / 'game.log').read_text(errors='replace') if (run / 'game.log').exists() else ''
    result = {'name': name, 'turns_played': 0}
    team_line = re.compile(r'(\w+) units (\d+) \(\d+ with classic tasks\) buildings (\d+) points (\d+) worth (\d+)')

    for line in log.splitlines():
        match = re.search(r'Command line: Turn (\d+):(.*)', line)
        if match:
            turn = int(match.group(1))
            result['turns_played'] = turn
            teams = {m.group(1): m for m in team_line.finditer(match.group(2))}
            if jev_team in teams:
                own = teams[jev_team]
                result['units'], result['buildings'] = int(own.group(2)), int(own.group(3))
                result['points'], result['worth'] = int(own.group(4)), int(own.group(5))
                if int(own.group(4)) > 0 and 'first_points_turn' not in result:
                    result['first_points_turn'] = turn
                others = [m for team, m in teams.items() if team != jev_team]
                if others:
                    best = max(others, key=lambda m: (int(m.group(4)), int(m.group(5))))
                    result['enemy_points'], result['enemy_worth'] = int(best.group(4)), int(best.group(5))
                    result['enemy_units'], result['enemy_buildings'] = int(best.group(2)), int(best.group(3))

        match = re.search(rf'Jev {jev_team}: turn (\d+), \d+ units, buildings:(.*)', line)
        if match and 'first_factory_turn' not in result and any(f'{name} (' in match.group(2) for name in FACTORIES):
            result['first_factory_turn'] = int(match.group(1))

        match = re.search(r'Command line: Game over on turn (\d+):(.*)', line)
        if match:
            result['game_over_turn'] = int(match.group(1))
            for part in match.group(2).split(';'):
                fields = re.match(r'\s*(\w+) rank (\d+)(.*)', part)
                if fields and fields.group(1) == jev_team:
                    result['rank'] = int(fields.group(2))
                    lost = re.search(r'units and buildings lost (\d+)', fields.group(3))
                    result['lost'] = int(lost.group(1)) if lost else None
                elif fields:
                    lost = re.search(r'units and buildings lost (\d+)', fields.group(3))
                    result.setdefault('enemy_lost', int(lost.group(1)) if lost else None)

    questions = 0
    tokens = 0
    if (run / 'questions.jsonl').exists():
        for line in (run / 'questions.jsonl').read_text().splitlines():
            entry = json.loads(line)
            if entry['request'].get('team') == jev_team:
                questions += 1
                tokens += (entry['reply'].get('usage') or {}).get('input_tokens', 0)
    result['questions'], result['input_tokens'] = questions, tokens
    return result


def table(results):
    def cell(row, key):
        value = row.get(key)
        return '-' if value is None else str(value)

    header = ('| game | turns | rank | points | enemy points | worth | enemy worth | units / buildings | '
              'enemy units / buildings | first factory | first points | lost | enemy lost | questions | cost |')
    lines = [header, '|' + '---|' * (header.count('|') - 1)]
    for row in results:
        cost = row.get('input_tokens', 0) * 0.042 / 1e6
        lines.append('| ' + ' | '.join([
            row['name'], cell(row, 'turns_played'), cell(row, 'rank'), cell(row, 'points'), cell(row, 'enemy_points'),
            cell(row, 'worth'), cell(row, 'enemy_worth'), f"{cell(row, 'units')} / {cell(row, 'buildings')}",
            f"{cell(row, 'enemy_units')} / {cell(row, 'enemy_buildings')}", cell(row, 'first_factory_turn'),
            cell(row, 'first_points_turn'), cell(row, 'lost'), cell(row, 'enemy_lost'), cell(row, 'questions'),
            f'${cost:.3f}']) + ' |')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--label', help='batch name')
    parser.add_argument('--games', default='0:42,3:7,6:13', help='comma separated world:seed list')
    parser.add_argument('--turns', type=int, default=100)
    parser.add_argument('--controllers', default='jev,classic')
    parser.add_argument('--mock', action='store_true', help='mock Jev answers, to test the batch itself')
    parser.add_argument('--summarize', metavar='LABEL', help='only print the table of an earlier batch')
    args = parser.parse_args()

    jev_team = TEAMS[args.controllers.split(',').index('jev')]
    games = [tuple(int(x) for x in game.split(':')) for game in args.games.split(',')]
    label = args.summarize or args.label

    if not label:
        parser.error('give --label or --summarize')

    names = [f'{label}-w{world}-s{seed}' for world, seed in games]

    if not args.summarize:
        threads = [threading.Thread(target=run_game, args=(label, world, seed, args.turns, args.controllers, args.mock))
                   for world, seed in games]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

    results = [summarize_game(name, jev_team) for name in names]
    text = table(results)
    (RUNS / f'{label}.md').write_text(text + '\n')
    (RUNS / f'{label}.json').write_text(json.dumps(results, indent=2))
    print(text)


if __name__ == '__main__':
    main()
