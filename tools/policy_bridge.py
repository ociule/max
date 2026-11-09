#!/usr/bin/env python3
"""Serve MAX's experimental full-observation policy protocol on localhost.

No remote service or model is called. Supply --policy path.py:choose, whose
callable receives the complete request and returns one command dictionary, or
use --end-turn for an explicit transport/observation smoke test.
"""

import argparse
import importlib.util
import json
from pathlib import Path
import socketserver
import threading
import time

MAX_MESSAGE_BYTES = 16 * 1024 * 1024


def reject_constant(value):
    raise ValueError(f'non-finite JSON number: {value}')


def load_policy(specification):
    path, separator, name = specification.rpartition(':')
    if not separator or not path or not name:
        raise ValueError('policy must be path.py:callable')
    spec = importlib.util.spec_from_file_location('max_external_policy', Path(path).resolve())
    if spec is None or spec.loader is None:
        raise ValueError('policy must name a Python module file')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    policy = getattr(module, name)
    if not callable(policy):
        raise ValueError('policy entry point must be callable')
    return policy


def answer(request, policy):
    if not isinstance(request, dict) or request.get('protocol') != 'max-policy-v1':
        raise ValueError('unsupported request protocol')
    request_id = request.get('id')
    if type(request_id) is not int or request_id <= 0:
        raise ValueError('request ID must be a positive integer')
    state = request.get('state')
    if not isinstance(state, dict) or state.get('schema') != 'max-policy-observation-v1':
        raise ValueError('unsupported observation schema')
    command = policy(request)
    if not isinstance(command, dict) or not isinstance(command.get('action'), str):
        raise ValueError('policy must return a command object with an action')
    if command['action'] not in state.get('commands', []):
        raise ValueError('command is not advertised by the engine')
    return {'id': request_id, 'command': command}


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, port, policy, log):
        self.policy = policy
        self.log = log
        self.policy_lock = threading.Lock()
        self.log_lock = threading.Lock()
        super().__init__(('127.0.0.1', port), Handler)


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(60)
        while True:
            try:
                line = self.rfile.readline(MAX_MESSAGE_BYTES + 1)
                if not line:
                    return
                if len(line) > MAX_MESSAGE_BYTES or not line.endswith(b'\n'):
                    return
                start = time.monotonic()
                request = None
                try:
                    request = json.loads(line, parse_constant=reject_constant)
                    # One local policy may own state or a non-thread-safe runtime.
                    # Its own game memory must be keyed by connection/team/turn.
                    with self.server.policy_lock:
                        reply = answer(request, self.server.policy)
                    encoded = json.dumps(reply, allow_nan=False).encode() + b'\n'
                    if len(encoded) > MAX_MESSAGE_BYTES:
                        raise ValueError('policy reply exceeds the transport limit')
                except Exception as error:
                    request_id = request.get('id') if isinstance(request, dict) else None
                    reply = {'id': request_id, 'error': f'{type(error).__name__}: {error}'}
                    encoded = json.dumps(reply).encode() + b'\n'
                record = {'time': time.time(), 'request': request, 'reply': reply,
                          'milliseconds': round((time.monotonic() - start) * 1000, 3),
                          'request_bytes': len(line)}
                with self.server.log_lock:
                    self.server.log.write(json.dumps(record, allow_nan=False) + '\n')
                    self.server.log.flush()
                self.wfile.write(encoded)
                self.wfile.flush()
            except (OSError, TimeoutError):
                return


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--policy', help='Python file and callable: path.py:choose')
    mode.add_argument('--end-turn', action='store_true', help='end every turn; observation/transport test only')
    parser.add_argument('--port', type=int, default=47812)
    parser.add_argument('--log', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error('port must be between 1 and 65535')
    policy = load_policy(args.policy) if args.policy else lambda request: {'action': 'end_turn'}
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('a', encoding='utf-8') as log, Server(args.port, policy, log) as server:
        print(f'Policy bridge on 127.0.0.1:{args.port}', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
