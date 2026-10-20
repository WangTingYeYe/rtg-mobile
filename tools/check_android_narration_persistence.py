#!/usr/bin/env python3
"""Verify a confirmed narration choice reached the committed game save.

Select a different narration mode, confirm with A, and wait for the start menu
before running. Modes: 0=off, 1=text, 2=text and situations. --restart also
cold-starts the installed game and checks the committed choice survived.
This checks persistence, not audible output or the selection drawn on screen;
inspect the narration menu after restarting. Saves/captures remain local.
--save replays a retained save, including a pre-fix failure, without a device.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time


def narration_mode(data):
    # Supported main build 03ff4a3bec96bde3a2e37ce3aa4caa0c: its loader checks
    # version/length, and its narration setter writes save payload offset 0xb4c.
    if len(data) != 0x1188 or struct.unpack_from('<II', data, 0x10) != (0x34, 0x1188):
        raise ValueError('Unsupported game save layout; refusing to interpret it')
    mode = data[0xb4c]
    if mode not in (0, 1, 2):
        raise ValueError('Invalid narration mode in committed save')
    return mode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--serial')
    source.add_argument('--save', type=Path)
    parser.add_argument('--expected-mode', type=int, choices=(0, 1, 2), required=True)
    parser.add_argument('--restart', action='store_true')
    parser.add_argument('--seconds', type=float, default=15)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.restart and not args.serial:
        parser.error('--restart requires --serial')
    if not 1 <= args.seconds <= 60:
        parser.error('--seconds must be between 1 and 60')
    args.output.mkdir(parents=True, exist_ok=False)
    report = {'expected_mode': args.expected_mode, 'passed': False,
              'restarted': False, 'audio_verified': False, 'ui_verified': False}
    package = 'org.rhythmtengoku.mobile'

    def call(*command):
        return subprocess.check_output(['adb', '-s', args.serial, *command], timeout=30)

    def inspect(data, name):
        (args.output / (name + '.bin')).write_bytes(data)
        mode = narration_mode(data)
        report[name] = {'mode': mode, 'sha256': hashlib.sha256(data).hexdigest()}
        if mode != args.expected_mode:
            raise ValueError(f'{name}: expected narration mode {args.expected_mode}, saved {mode}')

    def read_save():
        return call('exec-out', 'run-as', package, 'cat', 'files/rhythm-tengoku/save/game_save.bin')

    try:
        inspect(args.save.read_bytes() if args.save else read_save(), 'confirmed')
        if args.restart:
            call('shell', 'am', 'force-stop', package)
            launch = call('shell', 'am', 'start', '-W', '-n', package + '/.GameActivity')
            (args.output / 'launch.txt').write_bytes(launch)
            time.sleep(args.seconds)
            if not call('shell', 'pidof', package).strip():
                raise RuntimeError('Game exited after restart')
            report['restarted'] = True
            inspect(read_save(), 'after_restart')
            (args.output / 'screen.png').write_bytes(call('exec-out', 'screencap', '-p'))
        report['passed'] = True
    except (ValueError, RuntimeError, OSError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
