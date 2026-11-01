#!/usr/bin/env python3
"""Run one headless game with Jev computer players and keep its logs in runs/NAME/.

    python3 tools/jev_match.py --name m13 -- --controllers jev,classic --turns 100 --seed 42

Everything after -- goes to the game (see max-port --help); --hidden is added. The game runs under wine from a game
folder runs/slots/gameN, one per game running at the same time. A slot holds links to the shared game files in
runs/base, a copy of the exe and the files the game writes. runs/base is a game folder with the original assets and
the port's resources, made once by hand.

runs/NAME/ gets game.log (the game's stdout.txt), questions.jsonl (every Jev question and answer), bridge.log and
run.json (arguments, exe hash, git commit, start and end, the game over line). runs/ is not in git.
"""

import argparse
import datetime
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNS = ROOT / 'runs'
BASE = RUNS / 'base'
SLOTS = RUNS / 'slots'
DEFAULT_EXE = ROOT / 'build' / 'windows-hotkeys' / 'max-port.exe'

# files the game writes or that each slot needs its own copy of
OWN_FILES = {'max-port.exe', 'settings.json', 'stdout.txt', 'console.log'}


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def process_alive(pid):
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def take_slot():
    """The first slot no live process holds, created from runs/base when missing."""
    if not BASE.is_dir():
        raise SystemExit(f'{BASE} is missing: copy a working game folder there (assets, resources, settings.json).')

    for number in range(1, 17):
        slot = SLOTS / f'game{number}'
        lock = slot / '.busy'

        if lock.exists():
            try:
                if process_alive(int(lock.read_text())):
                    continue
            except ValueError:
                pass

        if not slot.exists():
            slot.mkdir(parents=True)
            for entry in BASE.iterdir():
                if entry.name not in OWN_FILES:
                    (slot / entry.name).symlink_to(entry)
            if (BASE / 'settings.json').exists():
                shutil.copy2(BASE / 'settings.json', slot / 'settings.json')

        lock.write_text(str(os.getpid()))
        return slot

    raise SystemExit('No free game slot.')


def file_hash(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()[:16]


def git_commit():
    def git(*args):
        return subprocess.run(['git', *args], cwd=ROOT, capture_output=True, text=True, check=False).stdout.strip()

    return git('rev-parse', '--short', 'HEAD') + ('-dirty' if git('status', '--porcelain', '--untracked-files=no') else '')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', help='run name, the folder in runs/ (default: the date and time)')
    parser.add_argument('--exe', default=str(DEFAULT_EXE), help='the game to run')
    parser.add_argument('--mock', action='store_true', help='answer with the bridge mock instead of Jev')
    parser.add_argument('--force', action='store_true', help='replace an existing run folder')
    parser.add_argument('game_args', nargs=argparse.REMAINDER, help='-- then the game arguments')
    args = parser.parse_args()

    game_args = args.game_args[1:] if args.game_args[:1] == ['--'] else args.game_args
    name = args.name or datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    run = RUNS / name

    if run.exists():
        if not args.force:
            raise SystemExit(f'{run} exists, use --force to replace it.')
        shutil.rmtree(run)

    run.mkdir(parents=True)
    slot = take_slot()
    port = free_port()

    try:
        shutil.copy2(args.exe, slot / 'max-port.exe')
        (slot / 'stdout.txt').unlink(missing_ok=True)

        info = {'name': name, 'game_args': game_args, 'exe': args.exe, 'exe_sha256': file_hash(args.exe),
                'git_commit': git_commit(), 'slot': slot.name, 'port': port, 'mock': args.mock,
                'start': time.time()}
        (run / 'run.json').write_text(json.dumps(info, indent=2))

        bridge_command = [sys.executable, str(ROOT / 'tools' / 'jev_bridge.py'), '--port', str(port), '--log',
                          str(run / 'questions.jsonl')] + (['--mock'] if args.mock else [])

        with open(run / 'bridge.log', 'w') as bridge_log:
            bridge = subprocess.Popen(bridge_command, stdout=bridge_log, stderr=subprocess.STDOUT)
            time.sleep(1)

            print(f'{name}: {" ".join(game_args)} in {slot.name}, bridge port {port}', flush=True)

            environment = dict(os.environ, MAX_JEV_BRIDGE_PORT=str(port))
            game = subprocess.run(['wine', 'max-port.exe', *game_args, '--hidden'], cwd=slot, env=environment,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)

            bridge.terminate()
            bridge.wait(timeout=10)

        if (slot / 'stdout.txt').exists():
            shutil.copy2(slot / 'stdout.txt', run / 'game.log')

        result = ''
        if (run / 'game.log').exists():
            for line in (run / 'game.log').read_text(errors='replace').splitlines():
                if 'Game over' in line:
                    result = line.split('Command line: ', 1)[-1]

        info.update({'end': time.time(), 'exit_code': game.returncode, 'result': result})
        (run / 'run.json').write_text(json.dumps(info, indent=2))

        minutes = (info['end'] - info['start']) / 60
        print(f'{name}: {minutes:.1f} min, {result or "no game over line, exit code " + str(game.returncode)}')

    finally:
        (slot / '.busy').unlink(missing_ok=True)


if __name__ == '__main__':
    main()
