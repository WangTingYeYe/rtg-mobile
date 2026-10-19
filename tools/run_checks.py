#!/usr/bin/env python3
"""Run portable CPU, JVM and packaging regressions without any game assets."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/checks')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--java-home', type=Path)
    parser.add_argument('--sanitize', action='store_true', help='Enable AddressSanitizer and UBSan in C tests')
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)

    def run(command):
        subprocess.run([str(x) for x in command], cwd=ROOT, check=True, timeout=180)

    run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tools', '-p', 'test_android_frame_pacing.py'])
    run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tools', '-p', 'test_public_tree.py'])
    run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tools', '-p', 'test_package_android.py'])
    common = [args.cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra',
              '-Werror', '-Wno-unused-function', '-Irtg/gfx/nvn/include']
    if args.sanitize:
        common += ['-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    texture = ['rtg/gfx/tex/nvn_deswizzle.c', 'rtg/gfx/nvn/nvn_tex.c']
    bc = ['rtg/gfx/tex/nvn_bcdec.c']
    tests = [(f'rtg/gfx/tests/t_{name}.c', [], []) for name in
             ('pace', 'shader_code_identity', 'writeback_plan', 'bc7_fast')]
    tests += [(f'rtg/android/tests/t_{name}.c', [], []) for name in
              ('shared_font_data', 'vsync_period', 'present_policy')]
    tests += [
        ('rtg/gfx/tests/t_attachment_plan.c', [], ['-D__ANDROID__']),
        ('rtg/gfx/tests/t_bc_upload.c', bc, []),
        ('rtg/gfx/tests/t_bc_preload.c', bc + texture + ['rtg/gfx/tex/nvn_bc_preload.c'], []),
        ('rtg/gfx/tests/t_tex_auto_layout.c', texture, []),
        ('rtg/gfx/tex/tests/t_linear.c', texture, []),
        ('rtg/gfx/tex/tests/t_move_reference.c', texture, []),
        ('rtg/android/tests/t_rtg_audio_renderer.c', ['rtg/android/rtg_audio_renderer.c'], []),
        ('rtg/android/tests/t_audio_renderer_lifecycle.c', [], []),
        ('rtg/android/tests/t_vibration_backend.c', ['rtg/android/vibration_backend.c'], []),
    ]
    for source, production, flags in tests:
        binary = out / Path(source).stem
        print('CHECK ' + source, flush=True)
        run(common + flags + [source] + production + ['-pthread', '-lm', '-o', binary])
        run([binary])
    java_dir = ROOT / 'rtg/android/src/org/rhythmtengoku/mobile'
    java_tests = sorted((ROOT / 'rtg/android/tests').glob('*Test.java'))
    classes = out / 'classes'; classes.mkdir(exist_ok=True)
    sources = [java_dir / (name + '.java') for name in
               ('BackButtonState', 'DisplayRefreshPolicy', 'GameDisplayMetrics',
                'NintendoControllerState', 'RendererOptions', 'RumblePulseTiming')]
    javac = args.java_home / 'bin/javac' if args.java_home else 'javac'
    java = args.java_home / 'bin/java' if args.java_home else 'java'
    run([javac, '--release', '8', '-encoding', 'UTF-8', '-d', classes, *sources, *java_tests])
    for test in java_tests:
        run([java, '-cp', classes, 'org.rhythmtengoku.mobile.' + test.stem])
    print(f'PASS: Python regressions, {len(tests)} C programs and {len(java_tests)} JVM programs')


if __name__ == '__main__':
    main()
