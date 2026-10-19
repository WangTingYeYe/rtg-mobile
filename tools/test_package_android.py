import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

from package_android import GAME_LANGUAGES, game_identity, module_assets, shader_assets, sha, write_game_resources


class PackageInputsTest(unittest.TestCase):
    def control_fixture(self, root):
        # Synthetic metadata/marker bytes only; no original game assets in tests.
        control = bytearray(0x4000)
        titles = {'AmericanEnglish': 'Sample Game', 'SimplifiedChinese': '示例游戏 & "名字"'}
        for language, title in titles.items():
            data = title.encode('utf-8')
            offset = GAME_LANGUAGES.index(language) * 0x300
            control[offset:offset + len(data)] = data
            (root / ('icon_' + language + '.dat')).write_bytes(b'\xff\xd8\xff' + language.encode() + b'\xff\xd9')
        (root / 'control.nacp').write_bytes(control)
        return titles

    def test_original_title_and_selected_icon_become_local_resources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            titles = self.control_fixture(root)
            identity, icon = game_identity(root, 'SimplifiedChinese')
            self.assertEqual(identity['app_name'], titles['SimplifiedChinese'])
            resources = root / 'res'
            write_game_resources(resources, identity, icon)
            self.assertEqual((resources / 'mipmap-nodpi/ic_launcher.jpg').read_bytes(),
                             (root / 'icon_SimplifiedChinese.dat').read_bytes())
            self.assertEqual(identity['icon_sha256'], sha(icon))
            strings = ET.parse(resources / 'values/game_identity.xml')
            self.assertEqual(strings.find('string').text, '"示例游戏 & \\"名字\\""')
            self.assertEqual({p.name for p in resources.rglob('*') if p.is_file()},
                             {'ic_launcher.jpg', 'game_identity.xml'})
            english, english_icon = game_identity(root, 'AmericanEnglish')
            self.assertEqual(english['app_name'], titles['AmericanEnglish'])
            self.assertNotEqual(english_icon, icon)

    def test_missing_local_title_or_icon_does_not_fall_back_to_custom_branding(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.control_fixture(root)
            with self.assertRaisesRegex(ValueError, 'No valid game title'):
                game_identity(root, 'Japanese')
            (root / 'icon_SimplifiedChinese.dat').unlink()
            with self.assertRaises(FileNotFoundError):
                game_identity(root, 'SimplifiedChinese')
            with self.assertRaisesRegex(ValueError, 'Unknown game language'):
                game_identity(root, '../AmericanEnglish')

    def test_invalid_control_and_icon_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.control_fixture(root)
            nacp = root / 'control.nacp'
            data = bytearray(nacp.read_bytes()); data[0x3215] = 1
            nacp.write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'Unsupported control.nacp'):
                game_identity(root, 'SimplifiedChinese')
            nacp.write_bytes(data[:128])
            with self.assertRaisesRegex(ValueError, 'Unsupported control.nacp'):
                game_identity(root, 'SimplifiedChinese')
            self.control_fixture(root)
            (root / 'icon_SimplifiedChinese.dat').write_bytes(b'not a JPEG')
            with self.assertRaisesRegex(ValueError, 'original JPEG'):
                game_identity(root, 'SimplifiedChinese')

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
