#!/usr/bin/env python3
"""Check RTG's start-menu UI resolution using an actual Vulkan frame trace.

Leave the game on the start menu (the list containing single-player / multiplayer).
This requests one diagnostic frame without restarting or navigating the game.
A pass proves 1000x900 menu rendering and an unscaled 1080p output copy, not FPS,
audio, visual quality or correctness of other scenes. Inspect the saved screenshot.
Use --trace to replay a retained trace, including a pre-fix regression capture.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time


def inspect_trace(text):
    start = text.rfind('[nvn-vulkan] TRACE_NEXT_FRAME')
    if start < 0:
        raise ValueError('No requested frame trace')
    text = text[start:]
    requested = re.search(r'TRACE_NEXT_FRAME frame=(\d+)', text)
    present = re.search(r'PRESENT_FORMAT frame=(\d+).*source_extent=(\d+),(\d+) '
                        r'swap_extent=(\d+),(\d+)', text)
    if not present or present[1] != requested[1]:
        raise ValueError('Requested frame has not completed')
    frame, width, height, display_width, display_height = map(int, present.groups())
    copies = [list(map(int, c)) for c in re.findall(
        r'COPY_T2T .*region=(\d+),(\d+),1 -> (\d+),(\d+),1', text[:present.start()])]
    targets = sorted({tuple(map(int, size)) for size in re.findall(
        r'RT_ATTACHMENT index=\d+ depth=0 .*?extent=(\d+),(\d+) ', text)})
    # Use the final copy, not unrelated offscreen/mipmap copies earlier in the frame.
    output_copy = copies[-1] if copies else []
    native_output = output_copy == [width, height, width, height] and height >= 1080
    menu_native = (1000, 900) in targets
    return {'frame': frame, 'render_targets': targets, 'output_copy': output_copy,
            'present_extent': [width, height], 'display_extent': [display_width, display_height],
            'menu_native_resolution': menu_native, 'unscaled_output_copy': native_output,
            'passed': menu_native and native_output}, text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--serial')
    source.add_argument('--trace', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    if args.trace:
        report, trace = inspect_trace(args.trace.read_text())
    else:
        adb = ['adb', '-s', args.serial]
        package = 'org.rhythmtengoku.mobile'
        root = 'files/rhythm-tengoku/diagnostics/'

        def call(*command):
            return subprocess.check_output(adb + list(command), timeout=20)

        focus = call('shell', 'dumpsys', 'window').decode(errors='replace')
        if not any(package + '/' in line and 'mCurrentFocus' in line for line in focus.splitlines()):
            raise SystemExit('Game must be foreground on the start menu')
        pid = call('shell', 'pidof', package).strip()
        entries = call('shell', 'run-as', package, 'ls', root).decode().splitlines()
        if any(n in entries for n in ('trace-request', 'trace-ui-request')):
            raise SystemExit('Another frame capture is already pending')
        before = call('exec-out', 'run-as', package, 'cat', root + 'game-startup.log')
        marker = root + 'trace-ui-request'
        call('shell', 'run-as', package, 'touch', marker)
        deadline = time.monotonic() + 8
        try:
            while True:
                time.sleep(0.2)
                raw = call('exec-out', 'run-as', package, 'cat', root + 'game-startup.log')
                if not raw.startswith(before):
                    raise RuntimeError('Game log restarted during capture')
                try:
                    report, trace = inspect_trace(raw[len(before):].decode(errors='replace'))
                    break
                except ValueError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError('Timed out waiting for the requested frame')
            if pid != call('shell', 'pidof', package).strip():
                raise RuntimeError('Game process changed during capture')
            (args.output / 'screen.png').write_bytes(call('exec-out', 'screencap', '-p'))
        finally:
            call('shell', 'run-as', package, 'rm', '-f', marker)
    (args.output / 'trace.txt').write_text(trace)
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
