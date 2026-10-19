"""Compile/run the production attachment proof against a host pixel model."""
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
    report = out / 'result.json'
    report.write_text(json.dumps({'passed': False, 'gpu_executed': False}) + '\n')
    source = 'rtg/gfx/tests/t_attachment_plan.c'
    tests = []
    for mode, flags in [('normal', ['-O2']), ('sanitized', ['-O1', '-fsanitize=address,undefined',
                                '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'])]:
        binary = out / mode
        command = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                   '-g', '-D__ANDROID__', '-Irtg/gfx/nvn/include', *flags, source, '-o', str(binary)]
        for name, cmd in [('build', command), ('run', [str(binary)])]:
            result = subprocess.run(cmd, cwd=repo, capture_output=True, text=True, timeout=60)
            (out / f'{mode}-{name}.log').write_text(result.stdout + result.stderr)
            if result.returncode:
                print(result.stdout + result.stderr)
            result.check_returncode()
        print(result.stdout.strip())
        tests.append({'mode': mode, 'passed': True, 'stdout': result.stdout.strip(), 'command': command})
    sources = [source, 'rtg/gfx/nvn/nvn_vk_attachment_plan.h', 'rtg/gfx/nvn/nvn_obj.h',
               'rtg/android/tests/run_attachment_checks.py']
    report.write_text(json.dumps({'passed': True, 'gpu_executed': False, 'tests': tests,
        'source_sha256': {s: hashlib.sha256((repo / s).read_bytes()).hexdigest() for s in sources}}, indent=2) + '\n')


if __name__ == '__main__':
    main()
