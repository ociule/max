#!/usr/bin/env python3
"""Run an isolated MAX game with a local policy and preserve its observations.

Example: python3 tools/policy_match.py --name policy-smoke --end-turn -- \
    --controllers policy,none --turns 3 --seed 42

Uses the same local game assets and slot locks as jev_match.py. No remote model
is called. A local policy can be supplied as --policy path.py:choose instead.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

import jev_match

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--end-turn', action='store_true')
    mode.add_argument('--policy', help='local Python file and callable: path.py:choose')
    parser.add_argument('--exe', type=Path, default=jev_match.DEFAULT_EXE)
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('game_args', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', args.name):
        parser.error('name must be a single run-folder name')
    if args.timeout <= 0:
        parser.error('timeout must be positive')
    game_args = args.game_args[1:] if args.game_args[:1] == ['--'] else args.game_args
    if '--controllers' not in game_args:
        game_args = ['--controllers', 'policy,classic', *game_args]
    if '--turns' not in game_args:
        game_args += ['--turns', '3']
    run = jev_match.RUNS / args.name
    run.mkdir(parents=True, exist_ok=False)
    exe = args.exe.resolve()
    bridge_args = ['--end-turn']
    policy_hash = None
    if args.policy:
        path, separator, function = args.policy.rpartition(':')
        if not separator or not function:
            parser.error('policy must be path.py:callable')
        path = Path(path).resolve()
        shutil.copy2(path, run / 'policy.py')
        policy_hash = jev_match.file_hash(run / 'policy.py')
        bridge_args = ['--policy', f'{run / "policy.py"}:{function}']

    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGTERM, interrupted)
    info = {'name': args.name, 'game_args': game_args, 'git_commit': jev_match.git_commit(),
            'exe': str(exe), 'exe_sha256': jev_match.file_hash(exe), 'policy_sha256': policy_hash,
            'bridge_sha256': jev_match.file_hash(ROOT / 'tools/policy_bridge.py'),
            'mode': 'end-turn' if args.end_turn else 'local-policy', 'start': time.time()}
    (run / 'run.json').write_text(json.dumps(dict(info, status='running'), indent=2) + '\n')
    failure = None
    game = bridge = None
    try:
        with jev_match.take_slot() as slot:
            port = jev_match.free_port()
            info.update(slot=slot.name, port=port)
            shutil.copy2(exe, slot / 'max-port.exe')
            jev_match.silence(slot / 'settings.json')
            (slot / 'stdout.txt').unlink(missing_ok=True)
            with (run / 'bridge.log').open('w') as bridge_log, (run / 'stderr.log').open('w') as errors:
                try:
                    bridge = subprocess.Popen([sys.executable, str(ROOT / 'tools/policy_bridge.py'),
                                               '--port', str(port), '--log', str(run / 'observations.jsonl'),
                                               *bridge_args], stdout=bridge_log, stderr=subprocess.STDOUT)
                    start = time.monotonic()
                    while 'Policy bridge on ' not in (run / 'bridge.log').read_text():
                        if bridge.poll() is not None or time.monotonic() - start > 5:
                            raise RuntimeError('Policy bridge did not start; see bridge.log')
                        time.sleep(0.05)
                    environment = dict(os.environ, MAX_POLICY_BRIDGE_PORT=str(port))
                    game = subprocess.Popen(['wine', 'max-port.exe', *game_args, '--hidden'], cwd=slot,
                                            env=environment, stdout=subprocess.DEVNULL, stderr=errors)
                    game.wait(timeout=args.timeout)
                    info['exit_code'] = game.returncode
                finally:
                    jev_match.stop_process(game)
                    jev_match.stop_process(bridge)
                    if (slot / 'stdout.txt').exists():
                        shutil.copy2(slot / 'stdout.txt', run / 'game.log')
    except (Exception, KeyboardInterrupt) as error:
        failure = f'{type(error).__name__}: {error}'
    log = (run / 'game.log').read_text(errors='replace') if (run / 'game.log').exists() else ''
    results = [line for line in log.splitlines() if 'Game over on turn ' in line]
    info.update(end=time.time(), result=results[-1] if results else None)
    policy_failures = [line for line in log.splitlines() if 'ending turn without a substitute policy' in line]
    if '"status":"rejected"' in log:
        info['rejected_commands'] = sum('"status":"rejected"' in line for line in log.splitlines())
    observation_log = run / 'observations.jsonl'
    bridge_errors = []
    observations = 0
    if observation_log.exists():
        for line in observation_log.open():
            try:
                entry = json.loads(line)
                observations += 1
                if 'error' in entry.get('reply', {}):
                    bridge_errors.append(entry['reply']['error'])
            except (ValueError, TypeError):
                bridge_errors.append('malformed observation log')
    info['observations'] = observations
    info['bridge_errors'] = bridge_errors
    info['controller_failures'] = policy_failures
    info['failure'] = failure
    info['status'] = 'complete' if (info.get('exit_code') == 0 and results and observations and
                                   not failure and not policy_failures and not bridge_errors) else 'failed'
    (run / 'run.json').write_text(json.dumps(info, indent=2) + '\n')
    print(f'{args.name}: {info["status"]}; {info["result"] or failure or "no completed game"}')
    return 0 if info['status'] == 'complete' else 1


if __name__ == '__main__':
    raise SystemExit(main())
