#!/usr/bin/env python3
"""Find likely mistakes of the Jev computer player in a game's logs.

    python3 tools/jev_review.py runs/NAME [--team red]

Reads runs/NAME/game.log and questions.jsonl and prints one line per finding, most important kind first. Each
finding says which side is at fault:
- controller: Jev's choice was reasonable but the game did not carry it out, or the options or facts were wrong.
  Fix it in src/jevai.cpp.
- choice: Jev chose badly from honest options. A candidate scenario for tools/jev_suite.py.
- info: Jev could not have known, for example fire from out of sight. A fact or state field is missing.
"""

import argparse
import collections
import json
import re
from pathlib import Path

POINT = r'\[(\d+),(\d+)\]'


class Review:
    def __init__(self, run, team):
        self.run = Path(run)
        self.team = team
        self.findings = []
        self.lines = (self.run / 'game.log').read_text(errors='replace').splitlines()
        self.questions = []
        if (self.run / 'questions.jsonl').exists():
            for line in (self.run / 'questions.jsonl').read_text().splitlines():
                entry = json.loads(line)
                if entry['request'].get('team') == team:
                    self.questions.append(entry)

    def add(self, side, kind, turn, text):
        self.findings.append((side, kind, turn, text))

    def jev_lines(self):
        """(turn, text) of the team's Jev lines, the turn taken from the last turn header."""
        turn = 0
        prefix = f'Jev {self.team}: '
        for line in self.lines:
            match = re.search(rf'{prefix}turn (\d+), ', line)
            if match:
                turn = int(match.group(1))
            if prefix in line:
                yield turn, line.split(prefix, 1)[1]

    def lost_units(self):
        """A unit lost on the turn of, or the turn after, a move into danger is a bad choice; lost without any
        danger shown, an unseen attacker."""
        moves = collections.defaultdict(list)
        seen_losses = set()
        for turn, text in self.jev_lines():
            match = re.match(rf'(.+?) at {POINT} chose m\d+ \(move(?: away from danger)? \w+ \d+ cells to {POINT}: '
                             r'.*?danger (\d+)', text)
            if match:
                moves[match.group(1)].append((turn, (int(match.group(4)), int(match.group(5))), int(match.group(6))))
            match = re.match(rf'lost (.+?) at {POINT}', text)
            if match and (turn, text) not in seen_losses:
                seen_losses.add((turn, text))
                unit, where = match.group(1), (int(match.group(2)), int(match.group(3)))
                recent = [m for m in moves[unit] if turn - m[0] <= 1 and max(abs(m[1][0] - where[0]),
                                                                          abs(m[1][1] - where[1])) <= 2]
                if recent and recent[-1][2] > 0:
                    self.add('choice', 'moved into danger and was lost', turn,
                             f'{unit} moved to {list(recent[-1][1])} at danger {recent[-1][2]}, lost at {list(where)}')
                else:
                    self.add('info', 'lost without danger shown', turn, f'{unit} lost at {list(where)}')

    def builds_not_done(self):
        for turn, text in self.jev_lines():
            match = re.match(r'(.+?) (gives up the .+ site at .+|did not reach the .+ site)', text)
            if match:
                self.add('controller', 'chosen build not carried out', turn, f'{match.group(1)} {match.group(2)}')

    def repeated_choices(self):
        """The same unit at the same place making the same choice three or more times in one turn: the choice does
        not take effect. Reported once per kind of choice and turn."""
        counts = collections.Counter()
        for turn, text in self.jev_lines():
            match = re.match(rf'(.+? at {POINT}) chose (\w+) \((.*?)\), model', text)
            if match and match.group(4) not in ('stay', 'nothing'):
                counts[(turn, match.group(1), match.group(5))] += 1
        grouped = collections.defaultdict(list)
        for (turn, who, what), count in counts.items():
            if count >= 3:
                grouped[(turn, what.split(' ')[0], what.split(':')[0])].append(who.split(' at ')[0])
        for (turn, _, what), units in sorted(grouped.items()):
            self.add('controller', 'same choice repeated without effect', turn,
                     f'{len(units)} units ({", ".join(sorted(set(units)))}) chose "{what}" 3 or more times')

    def idle_buildings(self):
        """Buildings that need to work but stay switched off or waiting for five turns or more."""
        waiting = collections.defaultdict(list)
        for turn, text in self.jev_lines():
            if not re.match(r'turn \d+, \d+ units, buildings:', text):
                continue
            for match in re.finditer(rf'(Eco-sphere|Mining Station|Habitat|Research Centre|Power Generator|'
                                     rf'Heavy Vehicle Plant|Light Vehicle Plant) \((Awaiting|Power Off|Building Halted|'
                                     rf'Halt Building)[^)]*at {POINT}', text):
                waiting[(match.group(1), match.group(3), match.group(4))].append((turn, match.group(2)))
        for (building, x, y), turns in waiting.items():
            if len(turns) >= 5:
                side = 'choice' if building.endswith('Plant') and turns[0][1] == 'Awaiting' else 'controller'
                self.add(side, 'factory idle' if side == 'choice' else 'building not working', turns[0][0],
                         f'{building} at [{x},{y}] {turns[0][1]} on {len(turns)} turns, from turn {turns[0][0]}')

    def idle_builders(self):
        """A builder that stays while it could build with what it carries."""
        streak = collections.Counter()
        for entry in self.questions:
            request, reply = entry['request'], entry['reply']
            you = request['state'].get('you', {})
            if you.get('type') not in ('Constructor', 'Engineer'):
                continue
            options = request.get('options') or {}
            can_build = [o for o in options.values() if o.get('action') == 'build' and
                         not o.get('not_enough_materials')]
            key = (you['type'], tuple(you.get('position', [])))
            if reply.get('choice') == 'stay' and can_build:
                streak[key] += 1
                if streak[key] == 3:
                    self.add('choice', 'builder stays while it could build', request['state'].get('turn'),
                             f"{you['type']} at {list(key[1])} stayed 3 times with {len(can_build)} affordable builds")
            else:
                streak[key] = 0

    def stock_capped(self):
        """Base raw materials at capacity: mining is wasted."""
        capped = []
        for entry in self.questions:
            base = entry['request']['state'].get('base')
            turn = entry['request']['state'].get('turn')
            if base and base.get('raw_capacity') and base['raw'] >= base['raw_capacity'] and turn not in capped:
                capped.append(turn)
        if len(capped) >= 5:
            self.add('choice', 'raw materials at capacity', capped[0],
                     f'the base was full on {len(capped)} turns, from turn {capped[0]}')

    def no_points(self):
        """An Eco-sphere stood for five turns or more while the team's points did not change."""
        eco_turns = []
        points = {}
        for line in self.lines:
            match = re.search(rf'Command line: Turn (\d+):.*?{self.team} units .*? points (\d+)', line)
            if match:
                points[int(match.group(1))] = int(match.group(2))
        for turn, text in self.jev_lines():
            if re.match(r'turn \d+, \d+ units, buildings:.*Eco-sphere \(Power On', text):
                eco_turns.append(turn)
        if len(eco_turns) >= 5 and points.get(eco_turns[-1], 0) == points.get(eco_turns[0], 0):
            self.add('controller', 'Eco-sphere on but no points', eco_turns[0],
                     f'Eco-sphere on from turn {eco_turns[0]}, points stayed {points.get(eco_turns[0], 0)}')

    def report(self):
        for check in (self.builds_not_done, self.repeated_choices, self.idle_buildings, self.no_points,
                      self.lost_units, self.idle_builders, self.stock_capped):
            check()
        order = {'controller': 0, 'info': 1, 'choice': 2}
        self.findings.sort(key=lambda f: (order[f[0]], f[2] or 0))
        for side, kind, turn, text in self.findings:
            print(f'{side:10} turn {turn or "?":>3}  {kind}: {text}')
        counts = collections.Counter(side for side, *_ in self.findings)
        print(f"{len(self.findings)} findings: {counts['controller']} controller, {counts['info']} info, "
              f"{counts['choice']} choice")
        return self.findings


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('run', help='a run folder, runs/NAME')
    parser.add_argument('--team', default='red', help='the Jev team')
    args = parser.parse_args()
    Review(args.run, args.team).report()


if __name__ == '__main__':
    main()
