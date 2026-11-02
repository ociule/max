#!/usr/bin/env python3
"""Score the static defense of the Jev team in finished (or running) games.

    python3 tools/jev_defense.py runs/b9-exits-* runs/b10-defense-*

Reads each run's game.log (the team's building list every turn, with positions) and questions.jsonl (enemies the
team's military units saw near the base) and prints one row per game:

- turrets: most Gun Turrets standing at once; first turret: the turn the first one stood.
- contact: the first turn an enemy was seen within 12 cells of the base, or a unit was lost within 12 cells of it,
  or a base building disappeared; ready: a turret stood by then. With no contact at all, ready is unknown (-).
- pressure: turns with an enemy seen within 8 cells of the base. A quiet game says little about defense.
- coverage: from the first factory on, the average share of the base's value within range of a turret, valued
  Eco-sphere 4, Habitat and factories 3, Mining Station 2, Power Generator 1.
- idle turrets: turrets at the end that cover none of those buildings.
- core lost: base buildings that disappeared from one turn to the next (destroyed, as Jev never removes buildings),
  of which Eco-spheres.
- eco kept: Eco-spheres standing at the end out of the most there were.
- points lost: every drop of the team's points added up. A destroyed Eco-sphere takes the points it holds with it.

score (0 to 100) = 40 x coverage + 30 x eco kept + 20 if ready + 10 x (1 - core lost / most core buildings), with
eco kept 0 when there never was an Eco-sphere and ready counting 10 when there was no contact. An eliminated team
(rank 0) scores 0. The parts matter more than the sum; the weights only make one number to compare batches with.
Runs whose run.json says "invalid" are skipped.
"""

import argparse
import json
import re
from pathlib import Path

WEIGHTS = {'Eco-sphere': 4, 'Habitat': 3, 'Heavy Vehicle Plant': 3, 'Light Vehicle Plant': 3, 'Mining Station': 2,
           'Power Generator': 1}
DEFENSES = {'Gun Turret': 6}
CONTACT_RADIUS = 12
PRESSURE_RADIUS = 8


def distance(a, b):
    return int(((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5)


def game_log(run):
    if (run / 'game.log').exists():
        return (run / 'game.log').read_text(errors='replace')
    info = json.loads((run / 'run.json').read_text()) if (run / 'run.json').exists() else {}
    slot = run.parent / 'slots' / info.get('slot', '') / 'stdout.txt'
    return slot.read_text(errors='replace') if info.get('slot') and slot.exists() else ''


def buildings_by_turn(text, team):
    turns = {}
    for match in re.finditer(rf'Jev {team}: turn (\d+), \d+ units, buildings:(.*)', text):
        found = re.findall(r'([A-Z][A-Za-z -]+?) \(([A-Za-z ]+?) -?\d+ at \[(\d+),(\d+)\]', match.group(2))
        turns[int(match.group(1))] = [(name.strip(), order, (int(x), int(y))) for name, order, x, y in found]
    return turns


def enemy_contacts(run, team):
    """Turn -> smallest distance from the base of an enemy the team's units saw."""
    nearest = {}
    path = run / 'questions.jsonl'
    if not path.exists():
        return nearest
    for line in path.read_text().splitlines():
        request = json.loads(line)['request']
        state = request['state']
        if request.get('team') != team or 'base_position' not in state:
            continue
        for enemy in state.get('nearest_enemies', []):
            gap = distance(enemy['position'], state['base_position'])
            turn = state['turn']
            nearest[turn] = min(nearest.get(turn, gap), gap)
    return nearest


def coverage(buildings):
    turrets = [(at, DEFENSES[name]) for name, _, at in buildings if name in DEFENSES]
    total = covered = 0
    for name, _, at in buildings:
        weight = WEIGHTS.get(name, 0)
        total += weight
        if weight and any(distance(at, spot) <= reach for spot, reach in turrets):
            covered += weight
    return covered / total if total else 0.0


def losses_near_base(text, team, turns):
    """Turn -> 0 for each turn a unit of the team was lost within 12 cells of a base building."""
    found = {}
    turn = 0
    for line in text.splitlines():
        match = re.search(rf'Jev {team}: turn (\d+), ', line)
        if match:
            turn = int(match.group(1))
        match = re.search(rf'Jev {team}: lost .+? at \[(\d+),(\d+)\]', line)
        if match and turn in turns:
            at = (int(match.group(1)), int(match.group(2)))
            if any(WEIGHTS.get(n) and distance(at, spot) <= CONTACT_RADIUS for n, _, spot in turns[turn]):
                found[turn] = 0
    return found


def score_game(run, team):
    text = game_log(run)
    turns = buildings_by_turn(text, team)
    contacts = enemy_contacts(run, team)
    result = {'name': run.name, 'turns': max(turns) if turns else 0}
    if not turns:
        return result
    for turn, gap in losses_near_base(text, team, turns).items():
        contacts[turn] = min(contacts.get(turn, gap), gap)

    ordered = sorted(turns)
    first_factory = next((t for t in ordered if any('Plant' in n for n, _, _ in turns[t])), None)
    first_turret = next((t for t in ordered if any(n in DEFENSES for n, _, _ in turns[t])), None)

    shares = [coverage(turns[t]) for t in ordered if first_factory is not None and t >= first_factory]
    lost = eco_lost = 0
    for before, after in zip(ordered, ordered[1:]):
        for name in WEIGHTS:
            drop = sum(n == name for n, _, _ in turns[before]) - sum(n == name for n, _, _ in turns[after])
            if drop > 0:
                lost += drop
                eco_lost += drop if name == 'Eco-sphere' else 0
                contacts[after] = 0
    contact = next((t for t in sorted(contacts) if contacts[t] <= CONTACT_RADIUS), None)

    final = turns[ordered[-1]]
    final_turrets = [(at, DEFENSES[n]) for n, _, at in final if n in DEFENSES]
    idle = sum(1 for at, reach in final_turrets
               if not any(WEIGHTS.get(n) and distance(at, spot) <= reach for n, _, spot in final))
    eco_max = max(sum(n == 'Eco-sphere' for n, _, _ in turns[t]) for t in ordered)
    eco_end = sum(n == 'Eco-sphere' for n, _, _ in final)
    core_max = max(sum(1 for n, _, _ in turns[t] if n in WEIGHTS) for t in ordered)

    result.update({
        'turrets': max(sum(n in DEFENSES for n, _, _ in turns[t]) for t in ordered),
        'first_turret': first_turret,
        'contact': contact,
        'ready': None if contact is None else (first_turret is not None and first_turret <= contact),
        'pressure': sum(1 for t in contacts if contacts[t] <= PRESSURE_RADIUS),
        'coverage': round(sum(shares) / len(shares), 2) if shares else 0.0,
        'idle_turrets': idle,
        'core_lost': lost,
        'eco_lost': eco_lost,
        'eco_kept': f'{eco_end}/{eco_max}',
    })
    eco_share = eco_end / eco_max if eco_max else 0.0
    ready_points = 10 if result['ready'] is None else 20 if result['ready'] else 0
    result['score'] = round(40 * result['coverage'] + 30 * eco_share + ready_points +
                            10 * (1 - min(1.0, lost / core_max if core_max else 0.0)))
    points = [int(m.group(1)) for m in re.finditer(rf'{team} units \d+ .*? points (\d+)', text)]
    result['points_lost'] = sum(max(0, a - b) for a, b in zip(points, points[1:])) if points else None
    match = re.search(rf'Game over on turn \d+:.*?{team} rank (\d+)', text)
    if match:
        result['rank'] = int(match.group(1))
        if result['rank'] == 0:
            result['score'] = 0
    return result


def table(results):
    columns = ['name', 'turns', 'rank', 'score', 'turrets', 'first_turret', 'contact', 'ready', 'pressure', 'coverage',
               'idle_turrets', 'core_lost', 'eco_lost', 'eco_kept', 'points_lost']
    lines = ['| ' + ' | '.join(columns) + ' |', '|' + '---|' * len(columns)]
    for row in results:
        lines.append('| ' + ' | '.join('-' if row.get(c) is None else str(row.get(c)) for c in columns) + ' |')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('runs', nargs='+', help='run folders, runs/NAME')
    parser.add_argument('--team', default='red')
    parser.add_argument('--json', action='store_true', help='print the numbers as JSON')
    args = parser.parse_args()
    runs = [Path(run) for run in args.runs if Path(run).is_dir()]
    runs = [run for run in runs
            if not (run / 'run.json').exists() or 'invalid' not in json.loads((run / 'run.json').read_text())]
    results = [score_game(run, args.team) for run in runs]
    print(json.dumps(results, indent=1) if args.json else table(results))


if __name__ == '__main__':
    main()
