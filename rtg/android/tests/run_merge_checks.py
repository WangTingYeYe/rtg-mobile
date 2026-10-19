"""Run Android host regressions with GCC on Linux/WSL.

Usage: python3 rtg/android/tests/run_merge_checks.py --output build/host-checks
Uses real pthread/mmap/mprotect; it does not contact a phone or run a GPU.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    include = out / 'include/android'
    include.mkdir(parents=True, exist_ok=True)
    (include / 'api-level.h').write_text('int android_get_device_api_level(void);\n')
    common = ['gcc', '-std=c11', '-D_GNU_SOURCE', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread']
    hooks = 'rtg/gfx/tests/t_dirty_host_hooks.c'
    core = 'rtg/gfx/nvn/nvn_dirty.c'
    tests = [
        ('pace', ['rtg/gfx/tests/t_pace.c'], [], [[]]),
        ('present', ['rtg/android/tests/t_present_policy.c'], [], [[]]),
        ('dirty-race', ['rtg/gfx/tests/t_dirty_race.c', hooks], ['-D__ANDROID__'], [[]]),
        ('dirty-disabled', ['rtg/gfx/tests/t_dirty_disabled.c', core, hooks], ['-D__ANDROID__'], [[]]),
        ('dirty-slots', ['rtg/gfx/tests/t_dirty_slots.c', core, hooks], ['-D__ANDROID__'], [[]]),
        ('dirty-summary', ['rtg/gfx/tests/t_dirty_summary.c', core, hooks], ['-D__ANDROID__'],
         [['tracker', '0'], ['tracker', '1'], ['properties', '1']]),
        ('thread-lifetime', ['rtg/android/tests/t_thread_lifetime.c'],
         ['-D__ANDROID__', '-I' + str(include.parent)], [[]]),
        ('save-recovery', ['rtg/android/tests/t_fs_host.c', 'rtg/host/hle_fs.c',
                           'rtg/android/services_fs_probe.c', 'rtg/android/services_fs.c', 'rtg/android/romfs_image.c'],
         ['-D__ANDROID__', '-Wno-format', '-Wno-unused-function', '-Wno-format-truncation'],
         [[str(out / 'save-recovery.json')]]),
    ]
    results = []
    report = out / 'result.json'
    report.write_text(json.dumps({'passed': False, 'status': 'running', 'device_contacted': False}) + '\n')
    def run(command, log):
        process = subprocess.run(command, cwd=repo, capture_output=True, text=True, timeout=90)
        (out / log).write_text(process.stdout + process.stderr, encoding='utf-8')
        process.check_returncode()
        return process.stdout
    for name, sources, flags, runs in tests:
        run(common + sources + flags + ['-o', str(out / name)], name + '-build.log')
        for i, arguments in enumerate(runs):
            stdout = run([str(out / name)] + arguments, name + '-' + str(i) + '.log')
            last_line = stdout.splitlines()[-1] if stdout.strip() else 'passed (see log)'
            if name == 'save-recovery':
                saved = json.loads(Path(arguments[0]).read_text())
                if not saved['passed'] or saved['failures']:
                    raise RuntimeError('Save recovery probe failed')
                last_line = 'SAVE_RECOVERY checks=%d failures=0 host_linux=1' % saved['checks']
            result = {'test': name, 'arguments': arguments, 'passed': True, 'last_line': last_line}
            results.append(result)
            print(result['last_line'], flush=True)
    # Hash all project C/headers so included production code is part of the receipt.
    hashes = {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in sorted((repo / 'rtg').rglob('*'))
              if p.suffix in ('.c', '.h', '.inc') and 'third_party' not in p.parts}
    report.write_text(json.dumps({'passed': True, 'device_contacted': False, 'gpu_executed': False,
                                'tests': results, 'source_sha256': hashes}, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    main()
