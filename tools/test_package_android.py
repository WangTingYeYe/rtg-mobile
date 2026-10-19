import json
from pathlib import Path
import tempfile
import unittest

from package_android import module_assets, shader_assets, sha


class PackageInputsTest(unittest.TestCase):
    def test_modules_are_explicit_and_android_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for module in ('main', 'sdk'):
                (root / (module + '.patched.bin')).write_bytes(b'synthetic test module')
                (root / (module + '.gates.tsv')).write_text('0x10\tsvc\t1\t0x0\n')
                (root / (module + '.fixups.tsv')).write_text('# ANDROID_APP_SLOT\t0x2\n')
            (root / 'unrelated.key').write_text('must not be packaged')
            assets = module_assets(root)
            self.assertEqual(len(assets), 7)
            manifest = json.loads(assets['assets/module-manifest.json'])
            for entry in manifest['files']:
                data = assets['assets/modules/' + entry['name']]
                self.assertEqual(entry['sha256'], sha(data))
                self.assertEqual(entry['bytes'], len(data))
            (root / 'main.fixups.tsv').write_text('# Apple-only module\n')
            with self.assertRaises(ValueError):
                module_assets(root)

    def test_shader_manifest_rejects_traversal_and_changed_content(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entry = dict(name='shader_index.bin', bytes=5, sha256=sha(b'index'))
            manifest = root / 'shader-manifest.json'
            (root / 'shader_index.bin').write_bytes(b'index')
            manifest.write_text(json.dumps({'files': [entry]}))
            self.assertEqual(len(shader_assets(root)[1]), 1)
            (root / 'shader_index.bin').write_bytes(b'other')
            with self.assertRaises(ValueError):
                shader_assets(root)
            entry['name'] = '../private.key'
            manifest.write_text(json.dumps({'files': [entry]}))
            with self.assertRaises(ValueError):
                shader_assets(root)


if __name__ == '__main__':
    unittest.main()
