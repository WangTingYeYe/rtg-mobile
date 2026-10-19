"""Build this worktree's real NVN/Vulkan backend and compare GPU pixels on Android.

Uses the existing local original-shader fixture assets; does not install an APK
or modify game data. Device execution requires --run-device and --serial.
"""
import argparse
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime, timezone


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture-tools', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--serial')
    parser.add_argument('--run-device', action='store_true')
    args = parser.parse_args()
    if args.run_device and not args.serial:
        parser.error('--serial is required with --run-device')
    repo = Path(__file__).resolve().parents[3]
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'device_executed': False, 'original_game_frame': False,
              'recorded_at_utc': datetime.now(timezone.utc).isoformat()}
    receipt = out / 'result.json'
    receipt.write_text(json.dumps(report) + '\n')

    def run(command, log, timeout=90):
        process = subprocess.run([str(x) for x in command], cwd=repo, capture_output=True, timeout=timeout)
        (out / log).write_bytes(process.stdout + process.stderr)
        if process.returncode:
            print((process.stdout + process.stderr).decode('utf-8', errors='replace')[-5000:])
        process.check_returncode()
        return process

    sys.path.insert(0, str(args.fixture_tools.resolve()))
    api = importlib.import_module('shader_nvn_api_check')
    source = api.HARNESS.replace('    int compute_ok=', '    p_SyncWait(&sync,UINT64_MAX);\n    int compute_ok=')
    source = source.replace('#include "nvn_obj.h"', '#include "nvn_obj.h"\n#include "nvn_vk_attachment_plan.h"\n'
                            'void dm_vk_attachment_config(int);\nvoid dm_vk_attachment_stats(NvnAttachmentStats *);')
    cases = Path(__file__).with_name('attachment_device_cases.inc')
    anchor = 'p_TextureFinalize(&target);'
    assert source.count(anchor) == 1
    source = source.replace(anchor, cases.read_text(encoding='utf-8') + '\n' + anchor)
    functions = sorted(set(re.findall(r'p_([A-Z][A-Za-z0-9]+)', source)))
    source = source.replace('@DECLARATIONS@', '\n'.join(f'static PFNNVN{x.upper()}PROC p_{x};' for x in functions))
    source = source.replace('@INITIALIZERS@', '\n'.join(f'p_{x}=(PFNNVN{x.upper()}PROC)boot("nvn{x}");' for x in functions))
    harness = out / 'harness.c'; harness.write_text(source, encoding='utf-8')
    sdk = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk'
    ndk = sdk / 'ndk/27.2.12479018'
    clang = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang.cmd'
    source_root = repo / 'rtg'
    objects = sorted(p for p in (args.build / 'CMakeFiles/dm_android_core.dir').rglob('*.o') if p.name != 'diagnostics.c.o')
    if not objects:
        raise RuntimeError('Build this worktree with CMake first')
    sources = [harness, source_root / 'android/diagnostics.c', source_root / 'gfx/hle_nv.c',
               *(source_root / 'gfx/nvn' / p for p in ('hle_nvn.c', 'nvn_entry.S', 'nvn_thunks.S', 'nvn_vk.c', 'nvn_dirty.c', 'nvn_tex.c')),
               source_root / 'android/nvn_platform_clock.c', source_root / 'android/nvn_translation_queue.c',
               source_root / 'gfx/tex/nvn_deswizzle.c', source_root / 'gfx/tex/nvn_bcdec.c']
    binary = out / 'attachment_probe'
    flags = ['-std=c11', '-O2', '-g', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
             '-Wno-format', '-Wno-unused-function', '-ffixed-x18']
    for include in ('gfx/nvn/include', 'gfx/nvn', 'android', 'host'):
        flags += ['-I', source_root / include]
    flags += sources + objects + ['-lvulkan', '-landroid', '-llog', '-ldl', '-lz', '-lm', '-laaudio',
                                  '-Wl,-z,nostart-stop-gc', '-o', binary]
    response = out / 'build.rsp'
    response.write_text('\n'.join('"' + str(x).replace('\\', '/').replace('"', '\\"') + '"' for x in flags), encoding='utf-8')
    run([clang, '@' + str(response)], 'build.log', 180)
    report.update(build_passed=True, binary_sha256=sha(binary), harness_sha256=sha(harness),
                  template_sha256=sha(args.fixture_tools / 'shader_nvn_api_check.py'),
                  source_sha256={str(p.relative_to(repo)): sha(p) for p in sorted(source_root.rglob('*'))
                                 if p.suffix in ('.c', '.h', '.inc') and 'third_party' not in p.parts},
                  linked_core_objects_sha256={str(p.relative_to(args.build)): sha(p) for p in objects},
                  shader_cache_sha256={p.name: sha(p) for p in sorted(args.cache.iterdir()) if p.is_file()})
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    if not args.run_device:
        print('Build passed; device not contacted'); return
    adb = [sdk / 'platform-tools/adb.exe', '-s', args.serial]
    remote = '/data/local/tmp/d2r-attachment-20261016'
    run([*adb, 'shell', 'mkdir', '-p', remote + '/cache'], 'mkdir.log')
    for p in sorted(args.cache.iterdir()):
        if p.is_file():
            run([*adb, 'push', p, remote + '/cache/' + p.name], 'push-' + p.name + '.log')
    run([*adb, 'push', binary, remote + '/probe'], 'push-probe.log')
    run([*adb, 'shell', 'chmod', '700', remote + '/probe'], 'chmod.log')
    report.update(device_executed=True, serial=args.serial)
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    tested = run([*adb, 'shell', 'timeout', '90', remote + '/probe', remote + '/cache', remote + '/color.rgba', 'run'], 'device.log', 100)
    output = (tested.stdout + tested.stderr).decode('utf-8', errors='replace')
    phases = [dict(re.findall(r'(\w+)=([^\s]+)', line)) for line in output.splitlines() if 'ATTACHMENT_PHASE ' in line]
    summary = [dict(re.findall(r'(\w+)=([^\s]+)', line)) for line in output.splitlines() if 'ATTACHMENT_DEVICE ' in line]
    if (len(phases) != 6 or len(summary) != 1 or summary[0] !=
            dict(phases='6', cases='120', compared_bytes='1720320', mismatches='0', same_process='1', passed='1')):
        raise RuntimeError('Expected GPU comparison markers missing')
    runtime = json.loads(tested.stdout.decode('utf-8').strip().splitlines()[-1])
    if not all(runtime[k] for k in ('compute_passed', 'draw_passed', 'real_write_fault_hook_passed', 'compact_records_passed')):
        raise RuntimeError('Base NVN fixture failed')
    run([*adb, 'pull', remote + '/color.rgba', out / 'color.rgba'], 'pull-color.log')
    props = run([*adb, 'shell', 'getprop'], 'device-properties.log').stdout.decode('utf-8', errors='replace')
    report.update(passed=True, phases=phases, summary=summary[0], runtime=runtime, pixel_readback_sha256=sha(out / 'color.rgba'),
                  device_log_sha256=sha(out / 'device.log'),
                  gpu=re.search(r'\[nvn-vulkan\] initialized (.+)', output).group(1),
                  device={k: re.search(r'\[' + re.escape(k) + r'\]: \[(.*?)\]', props).group(1)
                          for k in ('ro.product.model', 'ro.build.version.release', 'ro.build.version.sdk', 'ro.hardware')})
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': True, 'serial': args.serial, 'phases': phases, 'receipt': str(receipt)}, indent=2))


if __name__ == '__main__':
    main()
