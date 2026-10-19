#!/usr/bin/env python3
"""Cold-start this project's installed Android app and retain crash evidence.

Requires an explicit device serial. Does not clear data, install, or touch saves.
A pass means guest startup survived the observation window without reported
memory faults. --require-visible also requires nonblack GPU pixels; inspect the
device screenshot to verify orientation/content. Neither proves full playability.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--seconds', type=float, default=15)
    parser.add_argument('--min-frames', type=int, default=3)
    parser.add_argument('--screenshot', action='store_true')
    parser.add_argument('--require-visible', action='store_true', help='Require nonblack pixels in GPU readback, excluding Android UI overlays')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    package = 'org.rhythmtengoku.mobile'
    adb = ['adb', '-s', args.serial]

    def run(*command):
        return subprocess.run(adb + list(command), capture_output=True,
                              text=True, errors='replace', timeout=30)

    state = run('get-state')
    if state.returncode or state.stdout.strip() != 'device':
        raise SystemExit('Selected device is not connected/authorized')
    run('shell', 'am', 'force-stop', package).check_returncode()
    marker = 'files/rhythm-tengoku/diagnostics/capture-frames'
    had_marker = run('shell', 'run-as', package, 'test', '-e', marker).returncode == 0
    if args.require_visible:
        run('shell', 'run-as', package, 'touch', marker).check_returncode()
    (args.output / 'package.txt').write_text(run('shell', 'dumpsys', 'package', package).stdout)
    with (args.output / 'logcat.txt').open('w') as log:
        capture = subprocess.Popen(adb + ['logcat', '-v', 'threadtime', '-T', '1',
                                         '-s', 'RhythmTengokuMobile', 'D2RMobile', 'AndroidRuntime', 'DEBUG'],
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            time.sleep(0.3)
            launch = run('shell', 'am', 'start', '-W', '-n', package + '/.GameActivity')
            (args.output / 'launch.txt').write_text(launch.stdout + launch.stderr)
            deadline = time.monotonic() + args.seconds
            pids = []
            while time.monotonic() < deadline:
                pid = run('shell', 'pidof', package).stdout.strip()
                if pid:
                    pids.append(pid)
                elif pids:
                    break
                time.sleep(0.5)
            alive = run('shell', 'pidof', package).stdout.strip()
        finally:
            capture.terminate()
            capture.wait(timeout=5)
    # Native stderr also contains faults absent from the Android logging tag.
    diagnostic = run('shell', 'run-as', package, 'cat', 'files/rhythm-tengoku/diagnostics/game-startup.log')
    (args.output / 'native.txt').write_text(diagnostic.stdout + diagnostic.stderr)
    log = (args.output / 'logcat.txt').read_text(errors='replace') + diagnostic.stdout
    status_raw = run('shell', 'run-as', package, 'cat', 'files/rhythm-tengoku/diagnostics/status.json').stdout
    (args.output / 'status.json').write_text(status_raw)
    try:
        status = json.loads(status_raw)
    except ValueError:
        status = {}
    frame_counts = [int(x) for x in re.findall(r'RENDER_PROFILE frame=(\d+)', diagnostic.stdout)]
    frames = max(frame_counts + [status.get('presented_frames', 0)])
    drew = 'DRAW_TRACE draw=' in diagnostic.stdout
    readbacks = [dict(frame=int(f), width=int(w), height=int(h), nonblack=int(n))
                 for f,w,h,n in re.findall(r'PRESENT_READBACK frame=(\d+) extent=(\d+),(\d+) nonblack=(\d+)', diagnostic.stdout)]
    visible = any(r['nonblack'] > r['width'] * r['height'] * 0.01 for r in readbacks)
    if args.require_visible:
        for r in readbacks:
            name = 'frame-%04d.ppm' % r['frame']
            image = subprocess.run(adb + ['exec-out', 'run-as', package, 'cat',
                                   'files/rhythm-tengoku/diagnostics/' + name], capture_output=True, timeout=30)
            if image.returncode == 0:
                (args.output / name).write_bytes(image.stdout)
        if not had_marker:
            run('shell', 'run-as', package, 'rm', marker).check_returncode()
    faults = [line for line in log.splitlines() if re.search(
        r'ANDROID_FATAL|FATAL EXCEPTION|\[softgarbage\]|Fatal signal|\[nvn-vulkan\] STOP', line)]
    started = 'ANDROID_GAME_STAGE 正在启动游戏' in log
    report = {'serial': args.serial, 'seconds_requested': args.seconds,
              'observed_pids': sorted(set(pids)), 'alive': alive,
              'guest_start_seen': started, 'fault_lines': len(faults),
              'presented_frames': frames, 'real_draw_seen': drew,
              'gpu_readbacks': readbacks, 'visible_game_pixels': visible,
              'first_faults': faults[:5],
              'passed': bool(alive and started and drew and frames >= args.min_frames and not faults
                             and (visible or not args.require_visible))}
    if args.screenshot and alive:
        capture = subprocess.run(adb + ['exec-out', 'screencap', '-p'], capture_output=True, timeout=15)
        if capture.returncode == 0:
            (args.output / 'screen.png').write_bytes(capture.stdout)
    (args.output / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
