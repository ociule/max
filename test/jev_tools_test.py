"""Outcome checks for reliable experiment reports and bounded paid requests."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import jev_bridge
import jev_batch
import jev_suite


class ReportTests(unittest.TestCase):
    def test_filtered_run_does_not_replace_full_suite_comparison(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            scenarios = root / 'scenarios'
            scenarios.mkdir()
            for name in ('first', 'second'):
                (scenarios / f'{name}.json').write_text(json.dumps({'id': name}))
            prompts = root / 'prompts.py'
            prompts.write_text('# Test prompt identity')

            def run(only=None):
                args = ['jev_suite.py', '--prompts', str(prompts)] + (['--only', only] if only else [])
                output = io.StringIO()
                with patch.object(sys, 'argv', args), contextlib.redirect_stdout(output):
                    jev_suite.main()
                return output.getvalue()

            result = [{'choice': 'safe', 'kind': 'acceptable', 'margin': 0.5, 'tokens': 100}] * 3
            with patch.object(jev_suite, 'SCENARIOS', scenarios), patch.object(jev_suite, 'RESULTS', root / 'results'), \
                    patch.object(jev_suite, 'load_prompts'), patch.object(jev_bridge, 'load_key', return_value='test'), \
                    patch.object(jev_suite, 'run_scenario', return_value=result):
                self.assertNotIn('last run:', run())
                self.assertNotIn('last run:', run('first'))
                self.assertIn('last run: 6/6', run())
            reports = list((root / 'results').glob('last-*.json'))
            self.assertEqual(len(reports), 2)
            self.assertTrue(all(json.loads(p.read_text())['prompts_sha256'] for p in reports))

    def test_api_errors_in_a_finished_game_make_the_result_invalid(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            run = root / 'example'
            run.mkdir()
            (run / 'run.json').write_text(json.dumps({'exit_code': 0, 'result': 'Game over on turn 100'}))
            (run / 'game.log').write_text('')
            (run / 'questions.jsonl').write_text(json.dumps({
                'request': {'team': 'red'}, 'reply': {'error': 'Jev run budget exhausted', 'budget_exhausted': True}
            }) + '\n')
            with patch.object(jev_batch, 'RUNS', root):
                result = jev_batch.summarize_game('example', 'red')
            self.assertEqual(result['status'], 'budget exhausted')
            self.assertEqual(result['errors'], 1)

    def test_batch_preserves_existing_run_evidence(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'saved-w0-s42').mkdir()
            with patch.object(jev_batch, 'RUNS', root), patch.object(jev_batch, 'run_game') as start, \
                    patch.object(sys, 'argv', ['jev_batch.py', '--label', 'saved', '--games', '0:42']), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                jev_batch.main()
            start.assert_not_called()
            self.assertTrue((root / 'saved-w0-s42').is_dir())


class BudgetTests(unittest.TestCase):
    def bridge(self, root, **limits):
        args = SimpleNamespace(mock=False, prompts=None, log=str(root / 'questions.jsonl'),
                               model='test', timeout=1, max_calls=limits.get('max_calls', 2),
                               max_input_tokens=limits.get('max_input_tokens', 1000))
        with patch.object(jev_bridge, 'load_key', return_value='test'):
            bridge = jev_bridge.Bridge(args)
        self.addCleanup(bridge.log.close)
        return bridge

    def test_call_limit_stops_network_requests(self):
        with tempfile.TemporaryDirectory() as folder:
            bridge = self.bridge(Path(folder))
            with patch.object(jev_bridge, 'ask_jev', return_value={'usage': {'input_tokens': 10}}) as ask:
                bridge.call_model({})
                bridge.call_model({})
                self.assertTrue(bridge.call_model({})['budget_exhausted'])
                self.assertEqual(ask.call_count, 2)

    def test_token_threshold_stops_the_next_network_request(self):
        with tempfile.TemporaryDirectory() as folder:
            bridge = self.bridge(Path(folder), max_input_tokens=10)
            with patch.object(jev_bridge, 'ask_jev', return_value={'usage': {'input_tokens': 12}}) as ask:
                bridge.call_model({})
                self.assertTrue(bridge.call_model({})['budget_exhausted'])
                self.assertEqual(ask.call_count, 1)


if __name__ == '__main__':
    unittest.main()
