#!/usr/bin/env python3
"""The prompt test suite: ask Jev logged questions rendered with the current prompt templates and score the answers.

    python3 tools/jev_suite.py                         run every scenario 3 times with tools/jev_prompts.py
    python3 tools/jev_suite.py --prompts candidate.py  the same with other templates, to compare
    python3 tools/jev_suite.py --only opening          scenarios whose id contains this text
    python3 tools/jev_suite.py --add runs/b1-w0-s42/questions.jsonl 5 --id opening-heavy-plant \\
        --accept '{"action": "build", "type": "Heavy Vehicle Plant"}' --why "no factory yet: build one first"

A scenario (tools/jev_scenarios/ID.json) is one logged request (state and options as data) with the answers that are
acceptable and those that are forbidden, as option patterns: every key of a pattern must equal the option's value.
A key ending in > or < compares numbers ("danger>": 0), a dotted key looks inside ("purpose.key": "LANDPLT").
"label" says who decided what is right: "agent", "user", or "unsure" (to ask the user). Scenarios with "holdout":
true are scored apart; do not tune the wording on them.

Per ask, the top choice is acceptable, forbidden, or neither; the margin is the best acceptable probability minus
the best other one. The totals are compared with the previous run of the same templates file
(runs/suite/last-<prompts>.json). A wording change is kept when acceptable picks rise and forbidden picks do not.
"""

import argparse
import importlib.util
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import jev_bridge

ROOT = Path(__file__).resolve().parent.parent
SCENARIOS = ROOT / 'tools' / 'jev_scenarios'
RESULTS = ROOT / 'runs' / 'suite'


def load_prompts(path):
    spec = importlib.util.spec_from_file_location('candidate_prompts', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def value(option, key):
    for part in key.split('.'):
        if not isinstance(option, dict) or part not in option:
            return None
        option = option[part]
    return option


def matches(option, pattern):
    for key, expected in pattern.items():
        if key.endswith('>'):
            actual = value(option, key[:-1])
            if not isinstance(actual, (int, float)) or not actual > expected:
                return False
        elif key.endswith('<'):
            actual = value(option, key[:-1])
            if not isinstance(actual, (int, float)) or not actual < expected:
                return False
        elif value(option, key) != expected:
            return False
    return True


def classify(scenario, key):
    option = scenario['request']['options'].get(key, {})
    if any(matches(option, pattern) for pattern in scenario['expect'].get('forbidden', [])):
        return 'forbidden'
    if any(matches(option, pattern) for pattern in scenario['expect'].get('acceptable', [])):
        return 'acceptable'
    return 'other'


def run_scenario(scenario, prompts, key, asks, model):
    request = json.loads(json.dumps(scenario['request']))
    request['questions'] = prompts.render_question(request)
    results = []
    for _ in range(asks):
        reply = jev_bridge.ask_jev(key, model, request, 30)
        probabilities = reply['probabilities']
        kinds = {option: classify(scenario, option) for option in probabilities}
        best_acceptable = max((p for o, p in probabilities.items() if kinds[o] == 'acceptable'), default=0.0)
        best_other = max((p for o, p in probabilities.items() if kinds[o] != 'acceptable'), default=0.0)
        results.append({'choice': reply['choice'], 'kind': kinds.get(reply['choice'], 'other'),
                        'margin': round(best_acceptable - best_other, 3),
                        'tokens': (reply.get('usage') or {}).get('input_tokens', 0)})
    return results


def add_scenario(args):
    entry = json.loads(Path(args.add[0]).read_text().splitlines()[int(args.add[1])])
    request = entry['request']
    if 'options' not in request:
        raise SystemExit('This logged question has no options as data (logged before the prompt templates).')
    name = next(iter(request.get('game_questions', request['questions'])))
    scenario = {
        'id': args.id,
        'source': f'{args.add[0]}#{args.add[1]}',
        'why': args.why or '',
        'label': args.label,
        'holdout': args.holdout,
        'expect': {'acceptable': [json.loads(p) for p in args.accept or []],
                   'forbidden': [json.loads(p) for p in args.forbid or []]},
        'request': {'id': request.get('id'), 'team': request.get('team'), 'state': request['state'],
                    'questions': {name: {'type': 'choice', 'instructions': '', 'criteria': {}}},
                    'options': request['options']},
    }
    acceptable = [k for k in request['options'] if classify(scenario, k) == 'acceptable']
    forbidden = [k for k in request['options'] if classify(scenario, k) == 'forbidden']
    if not acceptable:
        raise SystemExit('No option of this question matches an acceptable pattern.')
    SCENARIOS.mkdir(parents=True, exist_ok=True)
    (SCENARIOS / f'{args.id}.json').write_text(json.dumps(scenario, indent=1) + '\n')
    print(f'{args.id}: {len(request["options"])} options, acceptable {acceptable}, forbidden {forbidden}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--prompts', default=str(ROOT / 'tools' / 'jev_prompts.py'), help='prompt templates file')
    parser.add_argument('--asks', type=int, default=3, help='asks per scenario')
    parser.add_argument('--only', help='scenarios whose id contains this text')
    parser.add_argument('--model', default='jev-latest')
    parser.add_argument('--add', nargs=2, metavar=('LOG', 'INDEX'), help='make a scenario from a logged question')
    parser.add_argument('--id', help='--add: scenario id')
    parser.add_argument('--accept', action='append', help='--add: an acceptable option pattern, JSON')
    parser.add_argument('--forbid', action='append', help='--add: a forbidden option pattern, JSON')
    parser.add_argument('--why', help='--add: why these answers are right')
    parser.add_argument('--label', default='agent', choices=['agent', 'user', 'unsure'])
    parser.add_argument('--holdout', action='store_true', help='--add: kept out of tuning')
    args = parser.parse_args()

    if args.add:
        if not args.id:
            parser.error('--add needs --id')
        add_scenario(args)
        return

    prompts = load_prompts(args.prompts)
    key = jev_bridge.load_key()
    if not key:
        raise SystemExit('No TYPESAFE_API_KEY in the environment or in .env.')

    scenarios = [json.loads(path.read_text()) for path in sorted(SCENARIOS.glob('*.json'))]
    scenarios = [s for s in scenarios if not args.only or args.only in s['id']]
    totals = {False: {'asks': 0, 'acceptable': 0, 'forbidden': 0, 'margin': 0.0},
              True: {'asks': 0, 'acceptable': 0, 'forbidden': 0, 'margin': 0.0}}
    report = {'prompts': args.prompts, 'time': time.time(), 'scenarios': {}}
    tokens = 0

    for scenario in scenarios:
        results = run_scenario(scenario, prompts, key, args.asks, args.model)
        report['scenarios'][scenario['id']] = results
        total = totals[bool(scenario.get('holdout'))]
        for result in results:
            total['asks'] += 1
            total['acceptable'] += result['kind'] == 'acceptable'
            total['forbidden'] += result['kind'] == 'forbidden'
            total['margin'] += result['margin']
            tokens += result['tokens']
        picks = ' '.join(f"{r['choice']}:{r['kind'][0]}" for r in results)
        mean_margin = sum(r['margin'] for r in results) / len(results)
        flag = ' (holdout)' if scenario.get('holdout') else ''
        print(f"{scenario['id']:40} {picks:30} margin {mean_margin:+.2f}{flag}")

    RESULTS.mkdir(parents=True, exist_ok=True)
    last = RESULTS / f'last-{Path(args.prompts).stem}.json'
    previous = json.loads(last.read_text()) if last.exists() else None

    for holdout, total in totals.items():
        if total['asks']:
            report['holdout' if holdout else 'tuning'] = total
            line = (f"{'holdout' if holdout else 'tuning':8} {total['acceptable']}/{total['asks']} acceptable, "
                    f"{total['forbidden']} forbidden, mean margin {total['margin'] / total['asks']:+.2f}")
            before = previous and previous.get('holdout' if holdout else 'tuning')
            if before:
                line += (f"   (last run: {before['acceptable']}/{before['asks']}, {before['forbidden']} forbidden, "
                         f"{before['margin'] / before['asks']:+.2f})")
            print(line)
    print(f'{tokens} input tokens, ${tokens * 0.042 / 1e6:.4f}')

    stamp = time.strftime('%Y%m%d-%H%M%S')
    (RESULTS / f'{stamp}-{Path(args.prompts).stem}.json').write_text(json.dumps(report, indent=1))
    last.write_text(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
