import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from check_public_tree import issues


class PublicationGuardTest(unittest.TestCase):
    def test_artifact_and_binary_rejected_even_with_source_extension(self):
        self.assertTrue(issues('game/main.c', b'text'))
        self.assertTrue(issues('rtg/disguised.c', b'NSO0\0binary'))
        self.assertTrue(issues('docs/font.ttf', b'text'))
        self.assertTrue(issues('alias.py', b'', '120000'))
        self.assertFalse(issues('rtg/android/input_backend.c', b'int value;'))

    def test_staged_secret_is_checked_even_if_worktree_is_clean(self):
        script = Path(__file__).with_name('check_public_tree.py')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            env = os.environ.copy()
            env.pop('GIT_INDEX_FILE', None)
            subprocess.run(['git', 'init', '-q', temporary], check=True, env=env)
            secret = 'ghp_' + 'x' * 36
            source = root / 'example.py'
            source.write_text('token = ' + repr(secret))
            subprocess.run(['git', '-C', temporary, 'add', 'example.py'], check=True, env=env)
            source.write_text('value = 1\n')
            result = subprocess.run([sys.executable, str(script), '--root', temporary, '--staged'],
                                    capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 1)
            self.assertIn('GitHub token', result.stderr)
            self.assertNotIn(secret, result.stderr + result.stdout)


if __name__ == '__main__':
    unittest.main()
