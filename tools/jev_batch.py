#!/usr/bin/env python3
"""Run a batch of Jev against classic games, one at a time by default, and print one result table.

    python3 tools/jev_batch.py --label b1
    python3 tools/jev_batch.py --label b1 --games 0:42,3:7,6:13 --turns 100
    python3 tools/jev_batch.py --label b2 --games dev --model jev-1.14.0
    python3 tools/jev_batch.py --label c1 --games dev --controllers classic,classic --mock --repeats 2
    python3 tools/jev_batch.py --summarize b1          the table of a batch run before

--games takes world:seed pairs or a named set: quick (the usual batch, with --repeats 2), dev (tune on these) or
heldout (confirm only, never tune on these).

Each game is world:seed, played with tools/jev_match.py as runs/LABEL-wWORLD-sSEED/. The table goes to the screen and
to runs/LABEL.md, the numbers to runs/LABEL.json. For the Jev team (red unless --controllers says otherwise):
rank, points, asset worth against the best other team, the turn its first factory stood, the turn it first had
points, units and buildings lost, questions asked and their input tokens. Without a Jev team, --team says which
team the table is about (classic vs classic: the seat baseline).

Each game's stats.jsonl (src/gamestats.hpp) gives the validity columns. The classic AI thinks for a fixed time per
tick, so a busy machine weakens it: "classic cutoffs" is how often its time checks stopped work with work left, per
1000 checks, over the classic teams' turns; compare it with a batch run one game at a time. A game is falsified when
an end of turn wait was abandoned or an assertion failed.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import re
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNS = ROOT / 'runs'
TEAMS = ['red', 'green', 'blue', 'gray']
GAME_SETS = {
    # the usual batch (the user, 2026-11-13): 4 maps, run with --repeats 2; classic as red wins worlds 0 and 3,
    # splits world 6 and loses world 9
    'quick': '0:42,3:7,6:13,9:21',
    'dev': '0:42,3:7,6:13,9:21,12:5,14:33,16:8,18:11,20:17,22:3,1:60,10:44',
    'heldout': '2:29,5:101,8:71,13:90,17:55,21:14',
}
FACTORIES = ('Heavy Vehicle Plant', 'Light Vehicle Plant', 'Air Units Plant', 'Shipyard', 'Training Hall')


# set when a game ends on a Jev billing error: the games after it would get no answers
NO_CREDITS = threading.Event()


def run_game(name, world, seed, turns, controllers, mock, model, max_calls, max_input_tokens, exe):
    if NO_CREDITS.is_set():
        print(f'{name}: not started, Jev has no credits left', flush=True)
        return 2
    command = [sys.executable, str(ROOT / 'tools' / 'jev_match.py'), '--name', name, '--model', model,
               '--max-calls', str(max_calls), '--max-input-tokens', str(max_input_tokens)] + \
              (['--mock'] if mock else []) + (['--exe', exe] if exe else []) + \
              ['--', '--controllers', controllers, '--world', str(world), '--seed', str(seed), '--turns', str(turns)]
    code = subprocess.run(command, check=False).returncode
    if code == 2:
        NO_CREDITS.set()
    return code


def summarize_stats(run, result, classic_teams):
    path = run / 'stats.jsonl'
    if not path.exists():
        return
    checks = cutoffs = left = by_timer = abandoned = assertions = violations = 0
    for line in path.read_text().splitlines():
        try:
            entry = json.loads(line)
        except json.JSONDecodeError:
            continue
        if entry.get('type') != 'team_turn':
            continue
        abandoned += entry['end_turn']['wait_abandoned']
        assertions += entry['assertions']
        violations += entry['consistency']['violations']
        if entry['team'] in classic_teams:
            for site_checks, site_cutoffs in entry['budget_checks_cutoffs'].values():
                checks += site_checks
                cutoffs += site_cutoffs
            left += entry['reminders_left_at_cutoffs']
            by_timer += entry['end_turn']['by_timer']
    result['classic_cutoffs'] = round(1000 * cutoffs / checks, 3) if checks else None
    result['classic_reminders_left'] = left
    result['classic_turns_by_timer'] = by_timer
    result['waits_abandoned'], result['assertions'], result['violations'] = abandoned, assertions, violations
    if abandoned or assertions:
        result['status'] = 'falsified'


def summarize_game(name, jev_team, classic_teams=()):
    run = RUNS / name
    log = (run / 'game.log').read_text(errors='replace') if (run / 'game.log').exists() else ''
    info_path = run / 'run.json'
    info = json.loads(info_path.read_text()) if info_path.exists() else {}
    result = {'name': name, 'turns_played': 0,
              'status': 'complete' if info.get('result') and info.get('exit_code') == 0 else 'incomplete'}
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
        if match:
            # macro: Mining Stations standing, the most at once and when the second one stood
            mines = match.group(2).count('Mining Station (')
            result['mines_max'] = max(result.get('mines_max', 0), mines)
            if mines >= 2 and 'second_mine_turn' not in result:
                result['second_mine_turn'] = int(match.group(1))

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

    # raw materials mined on turns that began with full storage are lost; the income at the end of the game
    full_turns = {}
    if (run / 'questions.jsonl').exists():
        for line in (run / 'questions.jsonl').read_text().splitlines():
            state = json.loads(line).get('request', {}).get('state', {})
            base, mining = state.get('base'), state.get('mining_per_turn')
            if base and mining and state.get('turn') not in full_turns:
                full_turns[state['turn']] = mining['raw'] if base['raw_capacity'] and \
                    base['raw'] >= base['raw_capacity'] else 0
                result['raw_per_turn'] = mining['raw']
    result['raw_lost'] = sum(full_turns.values()) if full_turns else None

    questions = 0
    tokens = 0
    errors = 0
    budget_exhausted = False
    by_question = {}
    commands = []
    if (run / 'questions.jsonl').exists():
        for line in (run / 'questions.jsonl').read_text().splitlines():
            entry = json.loads(line)
            if entry['request'].get('team') == jev_team:
                questions += 1
                used = (entry['reply'].get('usage') or {}).get('input_tokens', 0)
                tokens += used
                name = next(iter(entry['request'].get('questions', {})), '?')
                by_question[name] = by_question.get(name, 0) + used
                if name == 'command':
                    option = entry['request'].get('options', {}).get(entry['reply'].get('choice'), {})
                    commands.append(f"t{entry['request']['state'].get('turn')} {option.get('action', '?')} "
                                    f"{option.get('size', '')}".strip())
                errors += bool(entry['reply'].get('error') or entry['request'].get('template_error'))
                budget_exhausted |= entry['reply'].get('budget_exhausted', False)
    result['questions'], result['input_tokens'] = questions, tokens
    result['tokens_by_question'], result['commands'] = by_question, commands

    # Eco-spheres standing but not working, per turn; air defense at the end
    eco_off = 0
    for line in log.splitlines():
        match = re.search(rf'Jev {jev_team}: turn \d+, \d+ units, buildings:(.*)', line)
        if match and re.search(r'Eco-sphere \((?!Power On)', match.group(1)):
            eco_off += 1
        match = re.search(rf'Command line: Assets of {jev_team}:(.*)', line)
        if match:
            result['anti_air'] = sum(int(n) for n, _ in re.findall(r'(\d+) (Mobile Anti Aircraft|Anti Aircraft) \(',
                                                                      match.group(1)))
    result['eco_off_turns'] = eco_off
    result['errors'], result['budget_exhausted'] = errors, budget_exhausted
    if errors:
        result['status'] = 'budget exhausted' if budget_exhausted else 'errors'
    summarize_stats(run, result, classic_teams)
    return result


def table(results):
    def cell(row, key):
        value = row.get(key)
        return '-' if value is None else str(value)

    header = ('| game | status | turns | rank | points | enemy points | worth | enemy worth | units / buildings | '
              'enemy units / buildings | first factory | first points | mines | second mine | raw per turn | raw lost | '
              'lost | enemy lost | questions | cost | military / construction / production / command cost | '
              'eco off turns | anti air | classic cutoffs | turns by timer | abandoned waits | assertions | '
              'violations |')
    lines = [header, '|' + '---|' * (header.count('|') - 1)]
    for row in results:
        cost = row.get('input_tokens', 0) * 0.042 / 1e6
        lines.append('| ' + ' | '.join([
            row['name'], row.get('status', '?'), cell(row, 'turns_played'), cell(row, 'rank'), cell(row, 'points'), cell(row, 'enemy_points'),
            cell(row, 'worth'), cell(row, 'enemy_worth'), f"{cell(row, 'units')} / {cell(row, 'buildings')}",
            f"{cell(row, 'enemy_units')} / {cell(row, 'enemy_buildings')}", cell(row, 'first_factory_turn'),
            cell(row, 'first_points_turn'), cell(row, 'mines_max'), cell(row, 'second_mine_turn'),
            cell(row, 'raw_per_turn'), cell(row, 'raw_lost'), cell(row, 'lost'),
            cell(row, 'enemy_lost'), cell(row, 'questions'),
            f'${cost:.3f}', ' / '.join(f"${row.get('tokens_by_question', {}).get(name, 0) * 0.042 / 1e6:.3f}"
                                       for name in ('military', 'construction', 'production', 'command')),
            cell(row, 'eco_off_turns'), cell(row, 'anti_air'), cell(row, 'classic_cutoffs'), cell(row, 'classic_turns_by_timer'),
            cell(row, 'waits_abandoned'), cell(row, 'assertions'), cell(row, 'violations')]) + ' |')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--label', help='batch name')
    parser.add_argument('--games', default='0:42,3:7,6:13', help='comma separated world:seed list, or dev or heldout')
    parser.add_argument('--parallel', type=int, default=1, help='games at the same time (default 1)')
    parser.add_argument('--repeats', type=int, default=1, help='games per world:seed (run folders get -rN)')
    parser.add_argument('--team', help='the team the table is about (default: the Jev team)')
    parser.add_argument('--turns', type=int, default=100)
    parser.add_argument('--controllers', default='jev,classic')
    parser.add_argument('--mock', action='store_true', help='mock Jev answers, to test the batch itself')
    parser.add_argument('--model', default='jev-latest')
    parser.add_argument('--max-calls', type=int, default=5000, help='maximum paid requests per game')
    parser.add_argument('--max-input-tokens', type=int, default=8_000_000, help='input-token threshold per game')
    parser.add_argument('--exe', help='the game to run (default: the current build, see jev_match.py)')
    parser.add_argument('--summarize', metavar='LABEL', help='only print the table of an earlier batch')
    args = parser.parse_args()

    controllers = args.controllers.split(',')
    if args.team:
        jev_team = args.team
    elif 'jev' in controllers:
        jev_team = TEAMS[controllers.index('jev')]
    else:
        parser.error('no Jev team: give --team')
    classic_teams = [TEAMS[i] for i, controller in enumerate(controllers) if controller == 'classic']
    games = [tuple(int(x) for x in game.split(':')) for game in GAME_SETS.get(args.games, args.games).split(',')]
    games = [(world, seed, repeat) for world, seed in games for repeat in range(1, args.repeats + 1)]
    label = args.summarize or args.label

    if not label:
        parser.error('give --label or --summarize')

    def game_name(world, seed, repeat):
        return f'{label}-w{world}-s{seed}' + (f'-r{repeat}' if args.repeats > 1 else '')

    names = [game_name(*game) for game in games]

    if not args.summarize:
        if len(set(names)) != len(names):
            parser.error('duplicate world:seed pairs would share a run folder')
        if any((RUNS / name).exists() for name in names):
            parser.error('a run folder already exists; use a new label or --summarize')
        with ThreadPoolExecutor(max_workers=args.parallel) as pool:
            outcomes = list(pool.map(lambda game: run_game(game_name(*game), game[0], game[1], args.turns,
                                                         args.controllers, args.mock, args.model, args.max_calls,
                                                         args.max_input_tokens, args.exe), games))
    else:
        # a summary reads the run folders that exist, with or without repeats
        names = [name for name in names if (RUNS / name).exists()] or \
                sorted(path.name for path in RUNS.glob(f'{label}-w*') if path.is_dir())
        outcomes = []

    results = [summarize_game(name, jev_team, classic_teams) for name in names]
    text = table(results)
    (RUNS / f'{label}.md').write_text(text + '\n')
    (RUNS / f'{label}.json').write_text(json.dumps(results, indent=2))
    print(text)
    if any(outcomes) or any(row['status'] != 'complete' for row in results):
        raise SystemExit('Batch has failed, incomplete or invalid games; see run and bridge logs.')


if __name__ == '__main__':
    main()
