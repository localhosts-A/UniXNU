import difflib
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'scripts/apply-xnu-patches.sh'


def make_patch(before, after):
    body = ''.join(difflib.unified_diff(
        before.splitlines(keepends=True), after.splitlines(keepends=True),
        fromfile='a/KernelConfigTables.cpp', tofile='b/KernelConfigTables.cpp',
    ))
    return 'diff --git a/KernelConfigTables.cpp b/KernelConfigTables.cpp\n' + body


class PatchReplay(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.worktree = self.root / 'xnu'
        self.worktree.mkdir()
        subprocess.run(['git', 'init', '-q', str(self.worktree)], check=True)
        self.target = self.worktree / 'KernelConfigTables.cpp'
        self.original = 'platform=base\n'
        self.platform = 'platform=armdt\n'
        self.device = 'platform=dali\n'
        self.target.write_text(self.original)
        subprocess.run(['git', '-C', str(self.worktree), 'add', 'KernelConfigTables.cpp'], check=True)
        subprocess.run(['git', '-C', str(self.worktree), '-c', 'user.name=Test', '-c',
                        'user.email=test@example.invalid', 'commit', '-qm', 'base'], check=True)
        self.patches = self.root / 'patches'
        self.patches.mkdir()
        (self.patches / '01-platform.patch').write_text(make_patch(self.original, self.platform))
        (self.patches / '02-device.patch').write_text(make_patch(self.platform, self.device))

    def tearDown(self):
        self.temporary.cleanup()

    def apply_stack(self):
        return subprocess.run(
            ['bash', str(SCRIPT), str(self.worktree), str(self.patches)],
            check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )

    def test_overlapping_patch_stack_can_be_replayed(self):
        first = self.apply_stack()
        self.assertEqual(first.returncode, 0, first.stdout)
        self.assertEqual(self.target.read_text(), self.device)

        second = self.apply_stack()
        self.assertEqual(second.returncode, 0, second.stdout)
        self.assertEqual(self.target.read_text(), self.device)

    def test_conflicting_local_edit_fails_without_overwriting_it(self):
        first = self.apply_stack()
        self.assertEqual(first.returncode, 0, first.stdout)
        local_edit = 'platform=local-change\n'
        self.target.write_text(local_edit)

        replay = self.apply_stack()
        self.assertNotEqual(replay.returncode, 0)
        self.assertEqual(self.target.read_text(), local_edit)


if __name__ == '__main__':
    unittest.main()
