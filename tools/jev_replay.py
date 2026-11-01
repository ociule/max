#!/usr/bin/env python3
"""Prototype Jev prompts without running a game: resend a logged question, changed or not, and show Jev's choice.

The Jev controller logs every question it asks (see tools/jev_bridge.py --log). Pick one, change its instructions,
state or option texts, and ask Jev again: the probabilities show at once whether new wording moves the decision. A
question costs about 0.2 s and a fraction of a cent, a test match takes minutes. Check new wording on a few logged
questions (an opening, a mid game one) before building and running a match.

    python3 tools/jev_replay.py --list jev-log.jsonl
    python3 tools/jev_replay.py jev-log.jsonl 129
    python3 tools/jev_replay.py jev-log.jsonl 129 --instructions JevAi_ConstructionInstructions --patch patch.json

--instructions NAME takes the instruction text from that string constant in src/jevai.cpp, so edited source text can
be tried before building. The patch file is JSON, every field optional:
    {"question": "construction",            the question name
     "instructions": "...",                 replaces the instructions
     "state": {"score": {...}},             merged into the state, top level keys replaced
     "criteria": {"b2": "new text", "m0": null, "b90": "a new option"}}   null removes an option
--repeat N asks N times, to see how stable a near tie is.
"""

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import jev_bridge  # noqa: E402

SOURCE = jev_bridge.ROOT / 'src' / 'jevai.cpp'


def source_string(name):
    """The concatenated literal of `static const char* const NAME = "..." "...";` in src/jevai.cpp."""
    match = re.search(r'static const char\* const ' + re.escape(name) + r' =\s*((?:"(?:[^"\\]|\\.)*"\s*)+);',
                      SOURCE.read_text())
    if not match:
        raise SystemExit(f'No string constant {name} in {SOURCE}')
    return ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(1))).encode().decode('unicode_escape')


def list_questions(log):
    for index, line in enumerate(Path(log).read_text().splitlines()):
        entry = json.loads(line)
        request = entry['request']
        name, question = next(iter(request['questions'].items()))
        state = request['state']
        choice = entry['reply'].get('choice', entry['reply'].get('error', '?'))
        print(f"{index:5} turn {state.get('turn', '?'):>3} {request.get('team', '?'):5} {name:12} "
              f"{state.get('you', {}).get('type', '?'):20} {len(question['criteria']):3} options -> {choice}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--list', metavar='LOG', help='list the questions of a log with their index')
    parser.add_argument('log', nargs='?', help='JSON lines log written by jev_bridge.py')
    parser.add_argument('index', nargs='?', type=int, help='question index, from --list')
    parser.add_argument('--patch', help='JSON file with changes, see above')
    parser.add_argument('--instructions', metavar='NAME', help='instructions from this constant in src/jevai.cpp')
    parser.add_argument('--repeat', type=int, default=1, help='ask this many times')
    parser.add_argument('--top', type=int, default=8, help='options to show')
    parser.add_argument('--model', default='jev-latest')
    args = parser.parse_args()

    if args.list:
        list_questions(args.list)
        return

    if args.log is None or args.index is None:
        parser.error('give LOG and INDEX, or --list LOG')

    entry = json.loads(Path(args.log).read_text().splitlines()[args.index])
    request = entry['request']
    name, question = next(iter(request['questions'].items()))

    if args.patch:
        patch = json.loads(Path(args.patch).read_text())
        name = patch.get('question', name)
        question['instructions'] = patch.get('instructions', question['instructions'])
        request['state'].update(patch.get('state', {}))
        for key, text in patch.get('criteria', {}).items():
            if text is None:
                question['criteria'].pop(key, None)
            else:
                question['criteria'][key] = text

    if args.instructions:
        question['instructions'] = source_string(args.instructions)

    request['questions'] = {name: question}
    key = jev_bridge.load_key()
    if not key:
        raise SystemExit('No TYPESAFE_API_KEY in the environment or in .env.')

    logged = entry['reply'].get('probabilities') or {}
    print(f"question {args.index}: turn {request['state'].get('turn')}, {name}, "
          f"{request['state'].get('you', {}).get('type')}, logged choice {entry['reply'].get('choice')}")

    for attempt in range(args.repeat):
        reply = jev_bridge.ask_jev(key, args.model, request, 30)
        ranked = sorted(reply['probabilities'].items(), key=lambda item: -item[1])
        print(f"-- {reply['model']}, {reply.get('usage', {}).get('input_tokens', '?')} input tokens"
              + (f', ask {attempt + 1}' if args.repeat > 1 else ''))
        for option, probability in ranked[:args.top]:
            before = f' (logged {logged[option]:.2f})' if option in logged and (args.patch or args.instructions) else ''
            print(f"{probability:.2f}{before} {option}: {question['criteria'][option][:120]}")


if __name__ == '__main__':
    main()
