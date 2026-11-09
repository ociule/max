import importlib.util
import io
import json
from pathlib import Path
import socket
import tempfile
import threading
import unittest

SPEC = importlib.util.spec_from_file_location('policy_bridge', Path(__file__).resolve().parents[1] / 'tools/policy_bridge.py')
bridge = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bridge)


class PolicyProtocolTest(unittest.TestCase):
    def request(self):
        return {'protocol': 'max-policy-v1', 'episode': 1, 'id': 17,
                'state': {'schema': 'max-policy-observation-v1', 'commands': ['move', 'end_turn'],
                          'map': {'resources': [None, [0, 0]]}}}

    def test_policy_receives_complete_observation_and_preserves_request_id(self):
        request = self.request()
        def choose(value):
            self.assertIs(value, request)
            self.assertEqual(value['state']['map']['resources'], [None, [0, 0]])
            return {'action': 'move', 'unit': 3, 'at': [5, 9]}
        self.assertEqual(bridge.answer(request, choose), {'id': 17, 'command': {'action': 'move', 'unit': 3, 'at': [5, 9]}})

    def test_rejects_bad_envelopes_and_unsupported_actions(self):
        request = self.request()
        for key, value in [('protocol', 'other'), ('id', True), ('id', -1), ('state', {})]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                bridge.answer(dict(request, **{key: value}), lambda value: {'action': 'end_turn'})
        for value in [None, 'end_turn', {}, {'action': 'cheat'}]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                bridge.answer(request, lambda request: value)

    def test_loads_an_explicit_local_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'policy.py'
            path.write_text('def choose(request):\n    return {"action": "end_turn"}\n')
            reply = bridge.answer(self.request(), bridge.load_policy(str(path) + ':choose'))
            self.assertEqual(reply['command'], {'action': 'end_turn'})

    def test_real_socket_preserves_large_observation_and_records_errors(self):
        log = io.StringIO()
        request = self.request()
        request['state']['payload'] = list(range(30000))
        def choose(value):
            self.assertEqual(value, request)
            return {'action': 'end_turn'}
        with bridge.Server(0, choose, log) as server:
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                with socket.create_connection(server.server_address, timeout=3) as connection:
                    with connection.makefile('rwb') as stream:
                        stream.write(json.dumps(request).encode() + b'\n')
                        stream.flush()
                        reply = json.loads(stream.readline())
                        self.assertEqual(reply, {'id': 17, 'command': {'action': 'end_turn'}})
                        stream.write(b'{"broken":NaN}\n')
                        stream.flush()
                        self.assertIn('non-finite', json.loads(stream.readline())['error'])
            finally:
                server.shutdown()
                worker.join(timeout=3)
        records = [json.loads(line) for line in log.getvalue().splitlines()]
        self.assertEqual(records[0]['request'], request)
        self.assertGreater(records[0]['request_bytes'], 65536)
        self.assertIsNone(records[1]['request'])


if __name__ == '__main__':
    unittest.main()
