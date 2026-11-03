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
import fcntl
import hashlib
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
from contextlib import contextmanager
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNS = ROOT / 'runs'
BASE = RUNS / 'base'
SLOTS = RUNS / 'slots'
DEFAULT_EXE = ROOT / 'build' / 'windows-hotkeys' / 'max-port.exe'

# files the game writes or that each slot needs its own copy of
OWN_FILES = {'max-port.exe', 'settings.json', 'stdout.txt', 'console.log', '.busy', '.lock'}


# the game plays sound through miniaudio from its first frame, before a hidden game turns sound off: switch it off
# in the settings the game reads at startup
SILENT_SETTINGS = {'disable_music': 1, 'disable_fx': 1, 'disable_voice': 1, 'disable_movie': 1, 'intro_movie': 0,
                   'music_level': 0, 'movie_level': 0, 'fx_sound_level': 0, 'voice_level': 0}


def silence(settings_file):
    if settings_file.exists():
        settings = json.loads(settings_file.read_text())
        settings.setdefault('settings', {}).setdefault('SETUP', {}).update(SILENT_SETTINGS)
        settings_file.write_text(json.dumps(settings, indent=4))


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def process_alive(pid):
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except PermissionError:
        return True
    except OSError:
        return False
    return True


@contextmanager
def take_slot():
    """Hold an OS lock for the whole match, including setup and log collection.

    Keep the lock file permanently: unlinking it would let another process lock
    a different inode. .busy remains PID metadata for compatibility with older runners.
    """
    if not BASE.is_dir():
        raise SystemExit(f'{BASE} is missing: copy a working game folder there (assets, resources, settings.json).')

    for number in range(1, 17):
        slot = SLOTS / f'game{number}'
        slot.mkdir(parents=True, exist_ok=True)
        lease = (slot / '.lock').open('a')
        try:
            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            lease.close()
            continue

        busy = slot / '.busy'
        try:
            # Respect matches started by the old runner, which did not use flock.
            try:
                if process_alive(int(busy.read_text())):
                    continue
            except (FileNotFoundError, ValueError):
                pass

            for entry in BASE.iterdir():
                target = slot / entry.name
                if entry.name not in OWN_FILES and not target.exists() and not target.is_symlink():
                    (slot / entry.name).symlink_to(entry)
            if (BASE / 'settings.json').exists() and not (slot / 'settings.json').exists():
                shutil.copy2(BASE / 'settings.json', slot / 'settings.json')

            busy.write_text(str(os.getpid()))
            try:
                yield slot
            finally:
                if busy.exists() and busy.read_text() == str(os.getpid()):
                    busy.unlink()
            return
        finally:
            lease.close()

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


def stop_process(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', help='run name, the folder in runs/ (default: the date and time)')
    parser.add_argument('--exe', default=str(DEFAULT_EXE), help='the game to run')
    parser.add_argument('--mock', action='store_true', help='answer with the bridge mock instead of Jev')
    parser.add_argument('--force', action='store_true', help='replace an existing run folder')
    parser.add_argument('--timeout', type=float, default=3600, help='maximum game duration in seconds')
    parser.add_argument('--model', default='jev-latest', help='Jev model version (pin for comparisons)')
    parser.add_argument('--max-calls', type=int, default=5000, help='maximum paid requests')
    parser.add_argument('--max-input-tokens', type=int, default=5_000_000,
                        help='stop new requests after this many billed input tokens')
    parser.add_argument('game_args', nargs=argparse.REMAINDER, help='-- then the game arguments')
    args = parser.parse_args()

    # a stopped match (kill, pkill) cleans up like an interrupted one: game and bridge stopped, logs kept
    def stop_requested(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')

    signal.signal(signal.SIGTERM, stop_requested)

    game_args = args.game_args[1:] if args.game_args[:1] == ['--'] else args.game_args
    name = args.name or datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    run = RUNS / name

    if run.exists():
        if not args.force:
            raise SystemExit(f'{run} exists, use --force to replace it.')
        shutil.rmtree(run)

    run.mkdir(parents=True)
    with take_slot() as slot:
        port = free_port()
        shutil.copy2(args.exe, slot / 'max-port.exe')
        silence(slot / 'settings.json')
        shutil.copy2(ROOT / 'tools' / 'jev_prompts.py', run / 'prompts.py')
        (slot / 'stdout.txt').unlink(missing_ok=True)

        info = {'name': name, 'game_args': game_args, 'exe': args.exe, 'exe_sha256': file_hash(args.exe),
                'git_commit': git_commit(), 'slot': slot.name, 'port': port, 'mock': args.mock,
                'start': time.time(), 'prompts_sha256': file_hash(run / 'prompts.py'), 'model': args.model,
                'max_calls': args.max_calls, 'max_input_tokens': args.max_input_tokens}
        (run / 'run.json').write_text(json.dumps(info, indent=2))

        bridge_command = [sys.executable, str(ROOT / 'tools' / 'jev_bridge.py'), '--port', str(port), '--log',
                          str(run / 'questions.jsonl'), '--prompts', str(run / 'prompts.py'), '--model', args.model,
                          '--max-calls', str(args.max_calls), '--max-input-tokens', str(args.max_input_tokens)] + \
                         (['--mock'] if args.mock else [])

        game = bridge = None
        failure = None
        with open(run / 'bridge.log', 'w') as bridge_log:
            try:
                bridge = subprocess.Popen(bridge_command, stdout=bridge_log, stderr=subprocess.STDOUT)
                time.sleep(1)
                if bridge.poll() is not None:
                    raise RuntimeError('Bridge did not start; see bridge.log')
                print(f'{name}: {" ".join(game_args)} in {slot.name}, bridge port {port}', flush=True)
                environment = dict(os.environ, MAX_JEV_BRIDGE_PORT=str(port))
                # the game's stderr says why it stopped when it crashes (wine and C++ runtime messages)
                with open(run / 'stderr.log', 'w') as game_errors:
                    game = subprocess.Popen(['wine', 'max-port.exe', *game_args, '--hidden'], cwd=slot,
                                            env=environment, stdout=subprocess.DEVNULL, stderr=game_errors)
                    game.wait(timeout=args.timeout)
            except (Exception, KeyboardInterrupt) as error:
                failure = f'{type(error).__name__}: {error}'
            finally:
                stop_process(game)
                stop_process(bridge)

        if (slot / 'stdout.txt').exists():
            shutil.copy2(slot / 'stdout.txt', run / 'game.log')

        result = ''
        if (run / 'game.log').exists():
            for line in (run / 'game.log').read_text(errors='replace').splitlines():
                if 'Game over' in line:
                    result = line.split('Command line: ', 1)[-1]

        exit_code = game.returncode if game else None
        info.update({'end': time.time(), 'exit_code': exit_code, 'result': result})
        if failure:
            info['error'] = failure
        (run / 'run.json').write_text(json.dumps(info, indent=2))

        minutes = (info['end'] - info['start']) / 60
        print(f'{name}: {minutes:.1f} min, {result or failure or "no game over line, exit code " + str(exit_code)}')
        if failure or exit_code != 0 or not result:
            raise SystemExit(1)


if __name__ == '__main__':
    main()
