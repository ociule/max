#!/usr/bin/env python3
"""The words the Jev computer player sends to Jev: instructions per question and the text of each option.

The game sends each question's options as data ("options" in the request: action, unit type, turns, cost, danger,
...). render_question() turns them into Jev's instructions and criteria. This file is the prompt: the prompt
improvement loop changes the wording here, then tests it with tools/jev_suite.py on logged questions and with games.

    python3 tools/jev_prompts.py --check runs/NAME/questions.jsonl

renders every logged option and compares it with the text the game wrote for it. The two were the same when the
wording moved here (2026-11-01); since then the templates are what Jev reads and may differ. The game keeps its own
texts for its log, which tools/jev_review.py reads.
"""

import argparse
import json
import sys
from pathlib import Path

# Instructions per question name.
INSTRUCTIONS = {
    'construction': (
        'You manage construction for your team in M.A.X., a turn-based strategy game on a map of square '
        'cells. Military units are commanded separately; your job is the economy and production that feed '
        'them. The game ends at a turn limit: the team with the most points wins, a tie goes to the team with'
        ' the most valuable units and buildings, and a team with no Constructor and no factory is eliminated.'
        ' Each working Eco-sphere scores 1 point every turn, so Eco-spheres built early score the most. An '
        'Eco-sphere holds the points it scored and they are lost when it is destroyed, so Eco-spheres must be'
        ' defended. Choose what this unit does now. Rules, most important first: 1. If your team has no '
        'factory, building one comes first; the Heavy Vehicle Plant is the only factory that makes tanks and '
        'other strong combat units, and new Constructors. 2. A building that needs power does not work while '
        'the base has no spare power; Engineers build Power Generators quickly, so start the factory and let '
        'an Engineer add power. 3. Keep builders out of danger. If your team has only one Constructor and no '
        'factory, losing it loses the game. 4. Put mining stations on surveyed resources to raise income, raw'
        ' materials first: they pay for everything; surveyors find those resources near the base. Raw '
        'materials mined while the base storage is full are lost, so spend them, build faster or add storage.'
        ' 5. Builders use the raw materials they carry; refill at the base when they carry too little. 6. '
        'Once a factory is started, build a Habitat for workers and then Eco-spheres, with Power Generators '
        'for them. Buildings that touch your base join it and share its materials, power and workers.'),
    'production': (
        'You manage production for your team in M.A.X., a turn-based strategy game. Military units are '
        'commanded separately; you choose what this factory produces next, or nothing. A unit takes the given'
        ' number of turns and uses raw materials from your base every turn while it is being built. Rules, '
        'most important first: 1. If your team has only one Constructor, produce another one first when this '
        'factory can, so that losing a builder does not lose the game. 2. Then produce combat units that fight'
        ' ground units, a mix of strong and long range ones. 3. Produce nothing only when the base cannot '
        'afford anything useful.'),
    'military': (
        'You command one military or support unit of your team in M.A.X., a turn-based strategy game on a map'
        ' of square cells. Construction is managed separately; your job is to find and destroy the enemy '
        "while keeping your base and your builders safe. Choose this unit's action now. Rules, most important"
        ' first: 1. Attack enemies you can damage, above all ones that can be destroyed this turn or cannot '
        'shoot back. 2. If enemies are near your base, your Eco-spheres or your builders, move between them '
        'or attack them; the enemy goes for the team with the most points, and each Eco-sphere it destroys '
        "takes away the points it holds. 3. Do not move where the danger is higher than this unit's hits "
        'unless it can destroy the enemy there. 4. Otherwise explore to find the enemy: fast units go far, '
        'slow units stay within a few cells of the base until enemies are found.'),
}

# What a unit type is for, by engine name. Factories are described by what they make.
ROLES = {
    'MININGST': "mines raw materials, fuel and gold from the resources under it, the base's only income;",
    'POWGEN': 'small power plant, most buildings need power to work;',
    'POWERSTN': 'large power plant for a big base;',
    'CNCT_4W': 'connector, joins separate buildings into one base;',
    'ADUMP': 'extra storage for raw materials, only useful when storage is full;',
    'FDUMP': 'extra storage for fuel, only useful when storage is full;',
    'GOLDSM': 'extra storage for gold;',
    'HABITAT': 'dwelling, gives workers to training halls, research centres and eco-spheres;',
    'RESEARCH': 'research centre, slowly upgrades units, needs workers;',
    'GREENHSE': 'Eco-sphere, scores 1 point every turn while it works and holds the points it scored: if it is '
                'destroyed, they are lost; points decide the game;',
    'DEPOT': 'repairs and reloads land units next to it;',
    'HANGAR': 'repairs and reloads aircraft;',
    'DOCK': 'repairs and reloads ships;',
    'BARRACKS': 'repairs and reloads infantry;',
    'RADAR': 'radar, sees enemies far away;',
    'GUNTURRT': 'fixed defense turret;',
    'ARTYTRRT': 'fixed defense turret;',
    'ANTIAIR': 'fixed defense turret;',
    'ANTIMSSL': 'fixed defense turret;',
    'ROAD': 'road, units move faster on it;',
    'BRIDGE': 'bridge, lets land units cross water;',
    'WTRPLTFM': 'platform for building on water;',
    'LANDPAD': 'landing pad for aircraft;',
    'BLOCK': 'concrete wall that blocks movement;',
    'CONSTRCT': 'builds large buildings: factories, mining stations, power plants;',
    'ENGINEER': 'builds small buildings: power generators, connectors, turrets, roads;',
    'SURVEYOR': 'finds resources for mining stations;',
    'SCOUT': 'fast scout that sees far;',
    'TANK': 'main battle tank;',
    'ARTILLRY': 'artillery, long range against ground;',
    'ROCKTLCH': 'rocket launcher, long range against ground;',
    'MISSLLCH': 'missile launcher, very long range;',
    'SP_FLAK': 'mobile anti-aircraft, only shoots aircraft;',
    'MINELAYR': 'lays mines;',
    'SCANNER': 'mobile radar;',
    'SPLYTRCK': 'supply truck, reloads units;',
    'REPAIR': 'repair unit;',
    'FUELTRCK': 'fuel truck;',
    'GOLDTRCK': 'gold truck;',
    'BULLDOZR': 'clears rubble;',
    'INFANTRY': 'infantry;',
    'COMMANDO': 'infiltrator, steals or disables enemy units;',
}

# Types the game sends without an engine name key, by name.
ROLES_BY_NAME = {
    'Personnel Carrier': 'armored transport for infantry;',
}


def article(name):
    return 'an' if name[:1] in 'AEIOU' else 'a'


def cell(point):
    return f'[{point[0]},{point[1]}]'


def describe_purpose(purpose):
    factory = purpose.get('factory')
    if factory is None:
        return ROLES.get(purpose.get('key', '')) or ROLES_BY_NAME.get(purpose.get('type', ''), '')
    text = 'factory; '
    text += ('combat units against ground: ' + ', '.join(factory['combat']) if factory['combat']
             else 'it makes no combat units that fight ground units')
    if factory['other']:
        text += '; other units: ' + ', '.join(factory['other'])
    if 'Constructor' in factory['combat'] + factory['other']:
        text += '; the only factory that makes new Constructors'
    return text + ';'


def describe_stats(stats):
    text = f"attack {stats['attack']}, range {stats['range']}, " if 'attack' in stats else ''
    text += f"hits {stats['hits']}, armor {stats['armor']}"
    if 'speed' in stats:
        text += f", speed {stats['speed']}"
    return text


def render_move(o):
    text = ('move away from danger ' if o.get('away_from_danger') else 'move ') + \
        f"{o['direction']} {o['distance']} cells to {cell(o['to'])}: reveals {o['revealed']} unexplored cells, " \
        f"danger {o['danger']}"
    if 'from_base' in o:
        text += f", {o['from_base']} cells from your base"
    if 'from_builder' in o:
        text += f", {o['from_builder']} cells from your nearest builder"
    if 'nearest_unexplored' in o:
        text += f", nearest unexplored cell {o['nearest_unexplored']} cells away"
    if 'nearest_enemy' in o:
        text += f", nearest known enemy {o['nearest_enemy']} cells away"
    if 'resources' in o:
        r = o['resources']
        if 'unsurveyed_cells' in o:
            text += f", surveyed resources there raw {r['raw']} fuel {r['fuel']} gold {r['gold']}, " \
                f"{o['unsurveyed_cells']} of 9 cells not surveyed"
        else:
            text += f", resources there raw {r['raw']} fuel {r['fuel']} gold {r['gold']}"
    return text


def render_build(o):
    text = f"build {article(o['type'])} {o['type']} at {cell(o['at'])}: {describe_purpose(o['purpose'])} {o['turns']} turns, " \
        f"{o['cost']} raw materials, you carry {o['carried']}, you have {o['have']} already"
    if o.get('walks_there'):
        text += ', the builder walks there first'
    if 'resources' in o:
        r = o['resources']
        text += f", surveyed resources under it raw {r['raw']} fuel {r['fuel']} gold {r['gold']}"
        if o.get('joins_base'):
            text += ', joins your base'
        elif 'connectors_needed' in o:
            text += f", not joined to your base: it works once {o['connectors_needed']} connectors join it, which " \
                'Engineers lay'
        if 'base_mining' in o:
            m = o['base_mining']
            text += f", a station mines up to 16 per turn in all; your base mines raw {m['raw']} " \
                f"fuel {m['fuel']} gold {m['gold']} per turn from {m['stations']} stations"
    else:
        text += ', joins your base' if o.get('joins_base') else ', not connected to your base'
        if 'gives_workers' in o:
            text += f", gives {o['gives_workers']} workers, each Eco-sphere needs 1, you have {o['workers_spare']} spare"
        elif 'workers_spare' in o:
            text += f", {o['workers_spare']} workers spare"
            if 'turns_left' in o:
                text += f", {o['turns_left']} turns left in the game"
        if 'generator' in o:
            g = o['generator']
            text += f", burns {g['burns_fuel']} fuel per turn for {g['gives_power']} power; your buildings, with " \
                f"those being built, need {g['power_needed']} power and your generators give {g['power_generated']}, " \
                f"the base mines {g['fuel_mined_max']} fuel per turn at most"
        elif 'needs_power' in o:
            text += f", needs {o['needs_power']} power"
            if o['power_spare'] < o['needs_power']:
                text += ' and the base has none spare yet'
    if o.get('not_enough_materials'):
        text += ', not enough materials carried'
    if 'defense' in o:
        text += ', a defense: ' + describe_stats(o['defense'])
    if 'link' in o:
        text += f", one of {o['link']['left']} connectors still needed to join the Mining Station at " \
            f"{cell(o['link']['mine_at'])} to your base; it works once joined"
    if 'coverage' in o:
        text += describe_coverage(o['coverage'])
    if 'storage' in o:
        st = o['storage']
        text += f", stores raw materials: your base holds {st['base_raw']} of {st['capacity']} and loses what it mines " \
            f"while full; this adds {st['adds']}"
    if 'speed' in o:
        sp = o['speed']
        text += f", built at x{sp['rate']} speed: {sp['turns']} turn{'' if sp['turns'] == 1 else 's'} and " \
            f"{sp['cost']} raw materials instead"
    return text


def describe_coverage(c):
    covered = ', '.join(f'{count} {name}' for name, count in c['covers'].items())
    text = f', covers {covered}' if covered else ', covers none of your buildings'
    text += f", {c['defenses_covering_site']} of your defenses reach this spot"
    if 'approach' in c:
        a = c['approach']
        text += f", covers {a['covered']} of {a['cells']} cells of the way from the enemy seen near {cell(a['from'])}" \
            ' to your base'
    if 'nearest_known_enemy' in c:
        text += f", nearest known enemy {c['nearest_known_enemy']} cells away"
    return text


def render_produce(o):
    return f"produce {article(o['type'])} {o['type']}: {describe_purpose(o['purpose'])} {o['turns']} turns, {o['cost']} raw materials, " \
        f"{describe_stats(o['stats'])}, you have {o['have']}"


def render_attack(o):
    text = f"attack the enemy {o['target']} at {cell(o['at'])}: it has {o['hits']} of {o['max_hits']} hits, each shot " \
        f"does about {o['damage_per_shot']} damage, you have {o['shots']} shots this turn"
    text += ', in range now' if o['in_range'] else ', you move closer first'
    text += (f", it can shoot back with attack {o['shoots_back_attack']}" if 'shoots_back_attack' in o
             else ', it cannot shoot back')
    if o['destroyable']:
        text += ', it can be destroyed this turn'
    return text


def render_produce_nothing(o):
    text = f"produce nothing this turn; the base has {o['base_raw']} of {o['base_raw_capacity']} raw materials stored"
    if o['base_raw_capacity'] and o['base_raw'] >= o['base_raw_capacity']:
        text += ', storage is full, so the raw materials mined while nothing is built are lost'
    return text


def render_survey_base(o):
    return f"survey the ground around your base: go to {cell(o['to'])}, {o['distance']} cells away, " \
        f"{o['unsurveyed_near_base']} land cells within {o['mining_range']} cells of your base are not surveyed yet; " \
        'only deposits that close can be mined and joined to the base'


RENDERERS = {
    'survey_base': render_survey_base,
    'move': render_move,
    'build': render_build,
    'produce': render_produce,
    'attack': render_attack,
    'produce_nothing': render_produce_nothing,
    'refill': lambda o: f"take {o['amount']} raw materials from the {o['from']} next to you, you carry "
                        f"{o['carried']} of {o['capacity']}, the base stores {o['base_raw']}",
    'go_refill': lambda o: f"go next to the {o['to_building']} at {cell(o['to'])}, {o['distance']} cells away, to "
                           f"take raw materials there; you carry {o['carried']} of {o['capacity']}, the base stores "
                           f"{o['base_raw']}",
    'stay': lambda o: f"stay where it is for the rest of this turn, danger {o['danger']}",
}


def render_option(option):
    return RENDERERS[option['action']](option)


def render_question(request):
    """The request's questions with instructions and criteria from the templates. Needs request['options']."""
    name, question = next(iter(request['questions'].items()))
    criteria = {key: render_option(option) for key, option in request['options'].items()}
    return {name: dict(question, instructions=INSTRUCTIONS.get(name, question['instructions']), criteria=criteria)}


def check(log):
    """Compares rendered and logged text of every option and instructions in a log; returns the mismatch count."""
    total = mismatches = 0
    for number, line in enumerate(Path(log).read_text().splitlines()):
        request = json.loads(line)['request']
        if 'options' not in request:
            continue
        # the bridge keeps the game's own texts when it sends the templates' ones
        name, question = next(iter(request.get('game_questions', request['questions']).items()))
        if INSTRUCTIONS.get(name) != question['instructions']:
            mismatches += 1
            print(f'{number}: instructions for {name} differ')
        for key, option in request['options'].items():
            total += 1
            rendered = render_option(option)
            if rendered != question['criteria'][key]:
                mismatches += 1
                if mismatches <= 20:
                    print(f"{number} {key}:\n  game     {question['criteria'][key]}\n  template {rendered}")
    print(f'{total} options, {mismatches} differences')
    return mismatches


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', metavar='LOG', required=True, help='a questions.jsonl log to compare')
    sys.exit(1 if check(parser.parse_args().check) else 0)
