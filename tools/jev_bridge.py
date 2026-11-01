#!/usr/bin/env python3
"""Bridge between M.A.X. Port's Jev computer player and the Jev decision model.

The game connects to 127.0.0.1:PORT and sends one JSON line per question:
    {"id": 1, "team": "red", "state": {...}, "questions": {"action": {...}}, "mock_scores": {...}}
The bridge adds the model, calls https://api.typesafe.ai/v1/systemone with the TYPESAFE_API_KEY from the
environment or from .env in the repository root, and answers with one line:
    {"id": 1, "choice": "m3", "model": "jev-1.14.0", "confidence": 0.9, "probabilities": {...}}
With --mock it answers locally with the option of the highest mock score, without calling Jev.

When the game sends its options as data ("options"), the instructions and option texts Jev sees come from the
templates in tools/jev_prompts.py; the game's own texts are kept in the log as "game_questions". --game-wording sends
the game's texts instead.

Every question and answer is appended to the log file as one JSON line.

Usage: python3 tools/jev_bridge.py [--mock] [--port 47811] [--log jev-log.jsonl]
"""

import argparse
import json
import os
import socket
import ssl
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

import jev_prompts

API_URL = 'https://api.typesafe.ai/v1/systemone'
ROOT = Path(__file__).resolve().parent.parent


def load_key():
    key = os.environ.get('TYPESAFE_API_KEY')
    env_file = ROOT / '.env'
    if not key and env_file.exists():
        for line in env_file.read_text().splitlines():
            if line.startswith('TYPESAFE_API_KEY='):
                key = line.split('=', 1)[1].strip().strip('"')
    return key


def tls_context():
    """Python from python.org has no root certificates until its Install Certificates step has run, so fall back to
    certifi or the system bundle. Certificates are always verified."""
    context = ssl.create_default_context()
    if not context.cert_store_stats()['x509_ca']:
        try:
            import certifi
            context.load_verify_locations(certifi.where())
        except ImportError:
            for bundle in ('/etc/ssl/cert.pem', '/etc/ssl/certs/ca-certificates.crt'):
                if os.path.exists(bundle):
                    context.load_verify_locations(bundle)
                    break
    return context


TLS_CONTEXT = tls_context()


def ask_jev(key, model, request, timeout):
    body = json.dumps({'model': model, 'state': request['state'], 'questions': request['questions']}).encode()
    http_request = urllib.request.Request(API_URL, data=body, method='POST', headers={
        'Authorization': f'Bearer {key}', 'Content-Type': 'application/json'})
    with urllib.request.urlopen(http_request, timeout=timeout, context=TLS_CONTEXT) as response:
        result = json.loads(response.read())
    answer = next(iter(result['answers'].values()))
    return {'choice': answer['choice'], 'model': result.get('model', model), 'confidence': answer.get('confidence'),
            'probabilities': answer.get('probabilities'), 'usage': result.get('usage')}


def ask_mock(request):
    scores = request.get('mock_scores', {})
    choice = max(scores, key=scores.get) if scores else 'stay'
    return {'choice': choice, 'model': 'mock', 'confidence': 1.0}


class Bridge:
    def __init__(self, args):
        self.args = args
        self.key = None if args.mock else load_key()
        if not args.mock and not self.key:
            raise SystemExit('No TYPESAFE_API_KEY in the environment or in .env. Use --mock to run without Jev.')
        self.log_lock = threading.Lock()
        self.log = open(args.log, 'a', encoding='utf-8')

    def answer(self, request):
        start = time.monotonic()

        if 'options' in request and not self.args.game_wording:
            request['game_questions'] = request['questions']
            try:
                request['questions'] = jev_prompts.render_question(request)
            except (KeyError, TypeError) as error:  # a template bug must not stop the game
                request['questions'] = request['game_questions']
                request['template_error'] = f'{type(error).__name__}: {error}'

        try:
            reply = ask_mock(request) if self.args.mock else ask_jev(self.key, self.args.model, request,
                                                                       self.args.timeout)
        except urllib.error.HTTPError as error:
            reply = {'error': f'HTTP {error.code}: {error.read().decode(errors="replace")[:500]}'}
        except Exception as error:  # the game must always get an answer
            reply = {'error': f'{type(error).__name__}: {error}'}
        reply['id'] = request.get('id')
        reply['milliseconds'] = round((time.monotonic() - start) * 1000)
        with self.log_lock:
            self.log.write(json.dumps({'time': time.time(), 'request': request, 'reply': reply}) + '\n')
            self.log.flush()
        question = next(iter(request.get('questions', {}).values()), {})
        options = question.get('criteria', {})
        summary = reply.get('error') or f"{reply['choice']}: {options.get(reply['choice'], '?')}"
        print(f"{request.get('team')} q{request.get('id')} {reply['milliseconds']} ms "
              f"{reply.get('model', '')} -> {summary}", flush=True)
        return reply

    def serve_client(self, connection):
        with connection, connection.makefile('rwb') as stream:
            for line in stream:
                if not line.strip():
                    continue
                try:
                    request = json.loads(line)
                except json.JSONDecodeError as error:
                    reply = {'id': None, 'error': f'bad request: {error}'}
                else:
                    reply = self.answer(request)
                stream.write((json.dumps(reply) + '\n').encode())
                stream.flush()

    def run(self):
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(('127.0.0.1', self.args.port))
        server.listen()
        mode = 'mock answers' if self.args.mock else f'Jev model {self.args.model}'
        print(f'Jev bridge on 127.0.0.1:{self.args.port}, {mode}, logging to {self.args.log}', flush=True)
        while True:
            connection, _ = server.accept()
            threading.Thread(target=self.serve_client, args=(connection,), daemon=True).start()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--mock', action='store_true', help='answer locally instead of calling Jev')
    parser.add_argument('--port', type=int, default=int(os.environ.get('MAX_JEV_BRIDGE_PORT', 47811)))
    parser.add_argument('--model', default='jev-latest')
    parser.add_argument('--timeout', type=float, default=10.0, help='seconds to wait for Jev')
    parser.add_argument('--log', default='jev-log.jsonl', help='JSON lines file of questions and answers')
    parser.add_argument('--game-wording', action='store_true', help="send the game's texts, not the templates")
    Bridge(parser.parse_args()).run()


if __name__ == '__main__':
    main()
