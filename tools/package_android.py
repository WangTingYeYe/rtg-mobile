#!/usr/bin/env python3
"""Package a local development APK from source and explicitly supplied assets.

The APK contains game-derived modules, shaders and a local font. Keep it private.
This tool never installs the APK or modifies device data.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import zipfile
from rtg_message_symbols import private_symbols


def sha(data):
    return hashlib.sha256(data).hexdigest()


def module_assets(directory):
    """Only package the two modules loaded by app_jni; never copy a directory wholesale."""
    assets, entries = {}, []
    for module in ('main', 'sdk'):
        for suffix in ('patched.bin', 'fixups.tsv', 'gates.tsv'):
            name = module + '.' + suffix
            data = (directory / name).read_bytes()
            if not data:
                raise ValueError(f'Empty module artifact: {name}')
            if suffix == 'fixups.tsv' and b'# ANDROID_APP_SLOT\t0x2' not in data.splitlines():
                raise ValueError(f'{name}: rebuild with DM_TARGET=android')
            assets['assets/modules/' + name] = data
            entries.append(dict(name=name, bytes=len(data), sha256=sha(data)))
    assets['assets/module-manifest.json'] = json.dumps({'files': entries}, indent=2).encode()
    return assets


def shader_assets(directory):
    entries = json.loads((directory / 'shader-manifest.json').read_text())['files']
    assets, names = {}, set()
    for entry in entries:
        name = entry['name']
        if not re.fullmatch(r'shader_index\.bin|[0-9a-f]{16}\.(spv|vkmeta)', name) or name in names:
            raise ValueError(f'Invalid or duplicate shader artifact: {name}')
        names.add(name)
        data = (directory / name).read_bytes()
        if not 0 < len(data) <= 64 * 1024 * 1024 or len(data) != entry['bytes'] or sha(data) != entry['sha256']:
            raise ValueError(f'Shader artifact changed: {name}')
        assets['assets/shaders/' + name] = data
    if 'shader_index.bin' not in names:
        raise ValueError('Shader cache is missing shader_index.bin')
    assets['assets/shader-manifest.json'] = json.dumps({'files': entries}, indent=2).encode()
    return assets, entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--modules', type=Path, required=True, help='Directory with Android-patched main and sdk modules')
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--shaders', type=Path, required=True)
    parser.add_argument('--shared-font', type=Path, required=True, help='Locally built shared TTF, including RTG controller aliases')
    parser.add_argument('--jdk', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--keystore', type=Path, required=True)
    parser.add_argument('--ks-pass-env', default='RTG_KEYSTORE_PASSWORD', help='Environment variable holding the keystore/key password')
    parser.add_argument('--messages', type=Path, required=True, help='Local RomFS/mesg directory for font coverage validation')
    parser.add_argument('--build-tools', default='35.0.0')
    parser.add_argument('--compile-sdk', default='34')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if not os.environ.get(args.ks_pass_env):
        parser.error(f'Set {args.ks_pass_env} to the local keystore/key password')
    replacements = module_assets(args.modules)
    shader_files, entries = shader_assets(args.shaders)
    replacements.update(shader_files)
    # Refuse an A/B-only or stale font artifact before producing another APK.
    # The builder audits every language; the packager rechecks the current game
    # text inventory and binds its report to the exact bytes being shipped.
    font = args.shared_font.read_bytes()
    font_report = json.loads(args.shared_font.with_suffix('.json').read_text())
    if font_report.get('output_sha256') != sha(font):
        raise ValueError('Shared font hash does not match its coverage report')
    symbols, message_files, messages = private_symbols(args.messages)
    covered = {int(c[2:], 16) for c in font_report.get('controller_symbols', {})}
    if symbols - covered:
        raise ValueError('Shared font misses game symbols: ' + ', '.join(f'U+{c:04X}' for c in sorted(symbols-covered)))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    classes, dex = output / 'classes', output / 'dex'
    classes.mkdir(); dex.mkdir()
    env = os.environ.copy()
    env['JAVA_HOME'] = str(args.jdk.resolve())
    env['PATH'] = str(args.jdk.resolve() / 'bin') + os.pathsep + env.get('PATH', '')
    buildtools = args.sdk / 'build-tools' / args.build_tools
    android = args.sdk / 'platforms' / ('android-' + args.compile_sdk) / 'android.jar'

    def run(command, name):
        result = subprocess.run([str(x) for x in command], env=env, capture_output=True)
        (output / (name + '.log')).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError((result.stdout + result.stderr).decode(errors='replace')[-4000:])

    # Compile the project's manifest instead of inheriting D2R branding and
    # package identity from the historical bootstrap APK.
    resources = root / 'rtg/android/res'
    compiled = []
    if resources.is_dir() and any(p.is_file() for p in resources.rglob('*')):
        resource_zip = output / 'resources.zip'
        run([buildtools / 'aapt2', 'compile', '--dir', resources, '-o', resource_zip], 'resources')
        compiled = [resource_zip]
    manifest_apk = output / 'manifest.apk'
    run([buildtools / 'aapt2', 'link', '--manifest', root / 'rtg/android/AndroidManifest.xml',
         '-I', android, '-o', manifest_apk, *compiled], 'manifest')

    sources = sorted((root / 'rtg/android/src').rglob('*.java'))
    run([args.jdk / 'bin/javac', '--release', '8', '-encoding', 'UTF-8', '-cp',
         android, '-d', classes, *sources], 'javac')
    run([buildtools / 'd8', '--min-api', '29', '--lib', android, '--output', dex,
         *sorted(classes.rglob('*.class'))], 'dex')
    library = 'lib/arm64-v8a/librtg_mobile_android.so'
    replacements.update({library: args.native.read_bytes(), 'classes.dex': (dex / 'classes.dex').read_bytes()})
    with zipfile.ZipFile(manifest_apk) as manifest_source:
        replacements.update({name: manifest_source.read(name) for name in manifest_source.namelist()})
    with zipfile.ZipFile(output / 'unsigned.apk', 'w') as target:
        replacements['assets/fonts/ArialUnicode.ttf'] = font
        replacements['assets/font-manifest.json'] = json.dumps({'bytes': len(font), 'sha256': sha(font)}).encode()
        for name, data in replacements.items():
            target.writestr(name, data, compress_type=zipfile.ZIP_STORED if name.endswith('.so') or name=='resources.arsc' else zipfile.ZIP_DEFLATED)
    apk = output / 'rhythm-tengoku-mobile.apk'
    run([buildtools / 'zipalign', '-f', '-P', '16', '4', output / 'unsigned.apk', output / 'aligned.apk'], 'align')
    run([buildtools / 'apksigner', 'sign', '--ks', args.keystore, '--ks-pass', 'env:' + args.ks_pass_env,
         '--key-pass', 'env:' + args.ks_pass_env, '--out', apk, output / 'aligned.apk'], 'sign')
    run([buildtools / 'apksigner', 'verify', '--verbose', apk], 'verify')
    run([buildtools / 'zipalign', '-c', '-P', '16', '4', apk], 'verify-align')
    receipt = dict(apk=str(apk), apk_sha256=sha(apk.read_bytes()), native_sha256=sha(replacements[library]),
                   module_manifest_sha256=sha(replacements['assets/module-manifest.json']), shader_files=len(entries), shared_font_sha256=sha(font),
                   java_sources={str(p.relative_to(root)): sha(p.read_bytes()) for p in sources},
                   controller_symbols=font_report['controller_symbols'], message_files=message_files, messages=messages)
    (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: v for k, v in receipt.items() if k != 'java_sources'}, indent=2))


if __name__ == '__main__':
    main()
