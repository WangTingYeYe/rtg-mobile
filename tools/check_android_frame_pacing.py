#!/usr/bin/env python3
"""Measure actual SurfaceFlinger presentation cadence for the running game.

Does not restart the app or change device settings. Use an unchanged scene for
A/B comparisons; retain temperature and raw timestamps with every measurement.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import time


def snapshot_gap(previous, current):
    """A disjoint nonempty ring-buffer snapshot cannot prove continuous pacing."""
    return bool(previous and current and not set(previous).intersection(current))


def layer_name(line):
    # Android 15 may wrap --list entries in frontend state. --latency still
    # requires the surface name inside that wrapper, including its view hash.
    prefix = 'RequestedLayerState{'
    if line.startswith(prefix):
        return line[len(prefix):].split(' parentId=', 1)[0].rstrip('}')
    return line


def frame_statistics(timestamps, requested_seconds, initial_pid, final_pid,
                     min_fps=59, max_missed_percent=0.1, max_frame_ms=35, target_fps=60):
    if target_fps<=0:raise ValueError("target_fps must be positive")
    ordered=sorted(set(timestamps))
    intervals=[(b-a)/1e6 for a,b in zip(ordered,ordered[1:])]
    if not intervals:
        return {'passed': False, 'reason': 'no_new_frames'}, intervals
    sorted_ms=sorted(intervals)
    seconds=sum(intervals)/1000
    fps=len(intervals)/seconds
    budget_ms=1500/target_fps
    missed=sum(ms>budget_ms for ms in intervals)*100/len(intervals)
    stable=bool(initial_pid) and initial_pid==final_pid
    covered=seconds>=max(requested_seconds*.9,requested_seconds-2.5)
    return dict(seconds=seconds, intervals=len(intervals), actual_fps=fps,
                p50_ms=sorted_ms[len(sorted_ms)//2],
                p95_ms=sorted_ms[min(len(sorted_ms)-1,int(len(sorted_ms)*0.95))],
                max_ms=max(intervals),over_25ms_percent=sum(ms>25 for ms in intervals)*100/len(intervals),
                target_fps=target_fps,long_frame_threshold_ms=budget_ms,over_budget_percent=missed,
                process_stable=stable,duration_covered=covered,
                passed=stable and covered and fps>=min_fps and missed<=max_missed_percent
                       and max(intervals)<=max_frame_ms),intervals


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', required=True)
    p.add_argument('--seconds', type=float, default=15)
    p.add_argument('--target-fps', type=float, default=60)
    p.add_argument('--min-fps', type=float)
    p.add_argument('--max-missed-percent', type=float, default=0.1)
    p.add_argument('--max-frame-ms', type=float)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if a.target_fps<=0:p.error("target FPS must be positive")
    if a.min_fps is None:a.min_fps=a.target_fps*59/60
    if a.max_frame_ms is None:a.max_frame_ms=2100/a.target_fps
    a.output.mkdir(parents=True, exist_ok=False)
    adb = ['adb', '-s', a.serial]

    def shell(*args):
        return subprocess.check_output(adb + ['shell', shlex.join(args)], text=True, errors='replace', timeout=15)

    package = 'org.rhythmtengoku.mobile'
    def foreground():
        return any('mCurrentFocus' in line and package+'/' in line for line in shell('dumpsys','window','displays').splitlines())
    if not foreground():raise SystemExit('Game must be foreground')
    layers = [layer_name(s) for s in shell('dumpsys', 'SurfaceFlinger', '--list').splitlines()]
    layers = [s for s in layers if 'SurfaceView[' + package + '/' in s and '(BLAST)' in s]
    if len(layers) != 1:
        raise SystemExit('Expected one visible game SurfaceView, found: ' + repr(layers))
    layer = layers[0]
    pid = shell('pidof', package).strip()
    (a.output / 'thermal-before.txt').write_text(shell('dumpsys', 'thermalservice'))
    timestamps = set()
    baseline = 0
    refresh_ns = 0
    refresh_periods = set()
    previous = []
    disjoint_snapshots = 0
    # SurfaceFlinger retains only about 128 frames. At 120 FPS, polling once
    # per second leaves too little room for ADB latency and loses real frames.
    poll_seconds = min(1.0, 32 / a.target_fps)
    deadline = time.monotonic() + a.seconds
    sample = 0
    while time.monotonic() < deadline:
        raw = shell('dumpsys', 'SurfaceFlinger', '--latency', layer)
        (a.output / ('latency-%03d.txt' % sample)).write_text(raw)
        lines = raw.splitlines()
        if lines:
            refresh_ns = int(lines[0])
            refresh_periods.add(refresh_ns)
        current = []
        for line in lines[1:]:
            row = line.split()
            if len(row) == 3:
                present = int(row[1])
                if 0 < present < 2**63 - 1:
                    current.append(present)
        if sample == 0:
            baseline = max(current, default=0)
        if snapshot_gap(previous, current):
            disjoint_snapshots += 1
        if current:
            previous = current
        timestamps.update(t for t in current if t >= baseline)
        sample += 1
        time.sleep(min(poll_seconds, max(0, deadline - time.monotonic())))
    (a.output / 'thermal-after.txt').write_text(shell('dumpsys', 'thermalservice'))
    (a.output / 'native.txt').write_text(shell('run-as', package, 'cat', 'files/rhythm-tengoku/diagnostics/game-startup.log'))
    if foreground():
        screen = subprocess.check_output(adb + ['exec-out', 'screencap', '-p'], timeout=15)
        (a.output / 'screen.png').write_bytes(screen)
    final_pid=subprocess.run(adb+['shell','pidof',package],capture_output=True,
                             text=True,timeout=15).stdout.strip()
    stats,intervals=frame_statistics(timestamps,a.seconds,pid,final_pid,a.min_fps,a.max_missed_percent,a.max_frame_ms,a.target_fps)
    stats['capture_complete'] = disjoint_snapshots == 0
    stats['passed'] = stats['passed'] and stats['capture_complete']
    if not intervals or not baseline:
        raise SystemExit('No new presentation timestamps; capture cannot establish frame pacing')
    report = dict(serial=a.serial, pid=pid, layer=layer, refresh_ns=refresh_ns,
                  refresh_periods_ns=sorted(refresh_periods),poll_seconds=poll_seconds,
                  disjoint_snapshots=disjoint_snapshots,
                  requested_seconds=a.seconds,final_pid=final_pid,
                  min_fps=a.min_fps,max_missed_percent=a.max_missed_percent,max_frame_ms=a.max_frame_ms,**stats)
    (a.output / 'intervals-ms.json').write_text(json.dumps(intervals)+'\n')
    (a.output / 'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
