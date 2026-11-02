"""Regression checks for independent parallel game folders (no Wine or API calls)."""
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import jev_match


WORKER = '''
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import jev_match
jev_match.BASE = Path(sys.argv[2]) / 'base'
jev_match.SLOTS = Path(sys.argv[2]) / 'slots'
with jev_match.take_slot() as slot:
    print(slot.name, flush=True)
    sys.stdin.readline()
'''


class SlotTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'base').mkdir()
        (self.root / 'base' / 'asset.dat').write_text('shared asset')
        (self.root / 'base' / 'settings.json').write_text('{}')
        self.previous = jev_match.BASE, jev_match.SLOTS
        jev_match.BASE, jev_match.SLOTS = self.root / 'base', self.root / 'slots'
        self.addCleanup(self.restore)

    def restore(self):
        jev_match.BASE, jev_match.SLOTS = self.previous

    def worker(self):
        child = subprocess.Popen([sys.executable, '-c', WORKER, str(Path(jev_match.__file__).parent),
                                  str(self.root)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        def cleanup():
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=10)
        self.addCleanup(cleanup)
        return child

    def slot_name(self, child):
        self.assertTrue(select.select([child.stdout], [], [], 10)[0], 'worker did not acquire a slot')
        name = child.stdout.readline().strip()
        self.assertTrue(name, 'worker exited without acquiring a slot')
        return name

    def test_concurrent_claims_are_unique_and_release_is_reusable(self):
        children = [self.worker() for _ in range(8)]
        names = [self.slot_name(child) for child in children]
        self.assertEqual(len(set(names)), 8)
        for name in names:
            slot = jev_match.SLOTS / name
            self.assertTrue((slot / 'asset.dat').is_symlink())
            self.assertFalse((slot / 'settings.json').is_symlink())
        for child in children:
            _, errors = child.communicate('\n', timeout=10)
            self.assertEqual(child.returncode, 0, errors)
        with jev_match.take_slot() as slot:
            self.assertEqual(slot.name, 'game1')
        self.assertFalse((slot / '.busy').exists())
        self.assertTrue((slot / '.lock').exists())

    def test_killed_holder_releases_lock_and_stale_pid_is_reclaimed(self):
        child = self.worker()
        name = self.slot_name(child)
        child.kill()
        child.communicate(timeout=10)
        with jev_match.take_slot() as slot:
            self.assertEqual(slot.name, name)

    def test_live_legacy_pid_is_respected(self):
        legacy = jev_match.SLOTS / 'game1'
        legacy.mkdir(parents=True)
        (legacy / '.busy').write_text(str(os.getpid()))
        with jev_match.take_slot() as slot:
            self.assertEqual(slot.name, 'game2')
        self.assertTrue((legacy / '.busy').exists())

    def test_release_preserves_another_owners_metadata(self):
        with jev_match.take_slot() as slot:
            (slot / '.busy').write_text('another owner')
        self.assertEqual((slot / '.busy').read_text(), 'another owner')


if __name__ == '__main__':
    unittest.main()
