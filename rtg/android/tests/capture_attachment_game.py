"""Capture a same-process game A/B using logs, display timestamps and scene data.

No input, screenshots, APK install, restart or global device settings changes.
Requires the diagnostic build, attachment-ab marker and scene_91738_probe.
Hardware memory bandwidth is explicitly unavailable when the driver denies it.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import threading
import time

PKG = 'org.d2r.binaryprobe'
DIAG = 'files/d2r-mobile/diagnostics/'


def fields(line):
    return {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)(?=\s|$)', line)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--level', type=int, default=1)
    parser.add_argument('--warmup', type=int, default=180)
    parser.add_argument('--max-warmup', type=int, default=600)
    parser.add_argument('--seconds', type=int, default=90)
    parser.add_argument('--tail', type=int, default=30)
    parser.add_argument('--modes', default='0,3,0,1,2,3')
    args = parser.parse_args()
    requested_modes=[int(x) for x in args.modes.split(',')]
    if not requested_modes or any(x<0 or x>3 for x in requested_modes):parser.error('Modes must be 0..3')
    if args.seconds < args.tail or args.tail < 30 or args.warmup < 120:
        parser.error('Require >=120 seconds warmup and >=30 seconds measured tail')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    adb_path = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk/platform-tools/adb.exe'
    adb = [str(adb_path), '-s', args.serial]
    stop = threading.Event()
    lock = threading.Lock()
    frames, modes, states, display, errors = [], [], [], {}, []
    refresh = [0]
    report = {'completed': False, 'serial': args.serial, 'started_utc': datetime.now(timezone.utc).isoformat(),
              'hardware_bandwidth_measured': False, 'hardware_bandwidth_reason': 'KGSL READ returns EPERM; VK_KHR_performance_query absent',
              'screenshot_taken': False, 'input_sent': False, 'scene_level_expected': args.level,
              'drop_definition': 'Adjacent actual-display timestamps: gap >= 2 * reported display refresh period; strict threshold, no rounding tolerance.',
              'thermal_criterion': 'At least 110 seconds of observations in preceding 120 seconds; battery range <=0.5 C; skin ranges <=1 C; thermal status unchanged.'}
    receipt = out / 'result.json'

    def save():
        receipt.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

    def run(command, timeout=20):
        p = subprocess.run(adb + command, capture_output=True, timeout=timeout)
        if p.returncode:
            raise RuntimeError(str(command) + ': ' + (p.stdout + p.stderr).decode(errors='replace')[-300:])
        return p.stdout

    def private(*command):
        return run(['exec-out', 'run-as', PKG, *command])

    pid = run(['shell', 'pidof', PKG]).decode().strip()
    if not pid.isdecimal():
        raise RuntimeError('Expected one running game PID')
    initial = private('cat', DIAG + 'game-startup.log')
    (out / 'initial-startup.log').write_bytes(initial)
    text = initial.decode(errors='replace')
    base = re.search(r'main: base (0x[0-9a-f]+)', text)[1]
    if 'ATTACHMENT_FRAME ' not in text or 'ATTACHMENT_AB_CAPS ' not in text:
        raise RuntimeError('Diagnostic build/marker not active')
    scene = json.loads(private('files/attachment-scene-probe', pid, base))
    players = scene.get('players', [])
    if not scene.get('version_verified') or len(players) != 1 or players[0]['level'] != args.level:
        raise RuntimeError('Game data does not identify one player in expected level: ' + json.dumps(scene))
    identity = {k: players[0][k] for k in ('slot', 'id', 'unit', 'path', 'x_fixed', 'y_fixed', 'level', 'level_ptr')}
    sequence = max(int(x) for x in re.findall(r'ATTACHMENT_MODE [^\n]*sequence=(\d+)', text))
    report.update(pid=pid, main_base=base, scene_identity=identity)
    listing = run(['shell', 'dumpsys', 'SurfaceFlinger', '--list']).decode()
    (out / 'layers.txt').write_text(listing)
    layers = [line.removeprefix('RequestedLayerState{').split(' parentId=')[0].rstrip('}')
              for line in listing.splitlines() if 'SurfaceView[' + PKG in line and '](BLAST)#' in line]
    if len(layers) != 1:
        raise RuntimeError('Expected one live game BLAST layer: ' + repr(layers))
    layer = layers[0]
    report['surface_layer'] = layer
    save()

    def fail(error):
        with lock:
            errors.append(str(error))
        stop.set()

    def logs_loop():
        offset = 0
        pending = b''
        with (out / 'interval.log').open('wb') as log:
            while not stop.is_set():
                try:
                    size = int(private('stat', '-c', '%s', DIAG + 'game-startup.log'))
                    if size < offset:
                        raise RuntimeError('Log truncated/process restarted')
                    length = size - offset
                    if length:
                        cmd = f'tail -c +{offset + 1} {DIAG}game-startup.log | head -c {length}'
                        raw = private('sh', '-c', cmd)
                        if len(raw) != length:
                            raise RuntimeError('Short log interval')
                        log.write(raw)
                        log.flush()
                        offset = size
                        lines = (pending + raw).split(b'\n')
                        pending = lines.pop()
                        with lock:
                            for raw_line in lines:
                                line = raw_line.decode(errors='replace')
                                if 'ATTACHMENT_FRAME ' in line:
                                    frames.append(fields(line))
                                elif 'ATTACHMENT_MODE ' in line:
                                    modes.append(fields(line))
                                if any(s in line for s in ('[nvn-vulkan] STOP', 'ANDROID_FATAL', 'VK_ERROR_DEVICE_LOST')):
                                    raise RuntimeError('Fatal game log: ' + line)
                except Exception as e:
                    fail(e)
                    return
                stop.wait(2)

    def display_loop():
        nonlocal layer
        with (out / 'display-samples.jsonl').open('w') as log:
            while not stop.is_set():
                try:
                    raw = run(['shell', 'dumpsys', 'SurfaceFlinger', '--latency', '"' + layer + '"']).decode()
                    rows = []
                    for line in raw.splitlines():
                        parts = line.split()
                        if len(parts) == 1 and parts[0].isdecimal():
                            refresh[0] = int(parts[0])
                        if len(parts) == 3 and all(x.isdecimal() for x in parts):
                            values = tuple(map(int, parts))
                            if min(values) > 0 and max(values) < (1 << 63) - 1:
                                rows.append(values)
                    if not refresh[0] or not rows:
                        listing=run(['shell','dumpsys','SurfaceFlinger','--list']).decode()
                        candidates=[line.removeprefix('RequestedLayerState{').split(' parentId=')[0].rstrip('}')
                                    for line in listing.splitlines() if 'SurfaceView['+PKG in line and '](BLAST)#' in line]
                        if len(candidates)!=1 or candidates[0]==layer:raise RuntimeError('No actual display timestamps')
                        log.write(json.dumps({'layer_recreated':True,'old_layer':layer,'new_layer':candidates[0],'rows':[]})+'\n');log.flush()
                        layer=candidates[0];continue
                    with lock:
                        for row in rows:
                            display[row[1]] = row
                    log.write(json.dumps({'host_utc': datetime.now(timezone.utc).isoformat(), 'refresh_ns': refresh[0], 'rows': rows}) + '\n')
                    log.flush()
                except Exception as e:
                    fail(e)
                    return
                stop.wait(.8)

    def state_loop():
        with (out / 'state-samples.jsonl').open('w') as log, ThreadPoolExecutor(max_workers=4) as pool:
            while not stop.is_set():
                try:
                    commands = [lambda: private('files/attachment-scene-probe', pid, base),
                                lambda: run(['shell', 'dumpsys', 'battery']),
                                lambda: run(['shell', 'dumpsys', 'thermalservice']),
                                lambda: run(['shell', 'pidof', PKG]),
                                lambda: run(['shell','dumpsys','display'])]
                    results = list(pool.map(lambda fn: fn(), commands))
                    s = json.loads(results[0])
                    if results[3].decode().strip() != pid:
                        raise RuntimeError('Game process changed')
                    battery = results[1].decode()
                    thermal = results[2].decode()
                    device=next(l.strip() for l in results[4].decode(errors='replace').splitlines() if 'DisplayDeviceInfo{' in l)
                    mode_id=int(re.search(r'modeId (\d+)',device)[1])
                    active=re.search(r'\{id='+str(mode_id)+r',.*?fps=([\d.]+), vsync=([\d.]+)',device)
                    temperatures = {name: float(value) for value, name in re.findall(r'Temperature\{mValue=([-\d.]+), mType=\d+, mName=([^,]+), mStatus=\d+\}', thermal)}
                    item = {'scene': s, 'mono_ns': s['mono_ns'],
                            'battery_c': int(re.search(r'temperature:\s*(\d+)', battery)[1]) / 10,
                            'battery_level': int(re.search(r'level:\s*(\d+)', battery)[1]),
                            'usb_powered': 'USB powered: true' in battery,
                            'thermal_status': int(re.search(r'Thermal Status:\s*(\d+)', thermal)[1]),
                            'active_display_hz':float(active[2]),'display_mode_id':mode_id,'display_raw':device,
                            'temperatures_c': temperatures, 'battery_raw': battery, 'thermal_raw': thermal}
                    with lock:
                        states.append(item)
                    log.write(json.dumps(item) + '\n')
                    log.flush()
                except Exception as e:
                    fail(e)
                    return
                stop.wait(5)

    def thermal_stable(now):
        with lock:
            recent = [s for s in states if s['mono_ns'] >= now - 120_000_000_000]
        if len(recent) < 20 or recent[-1]['mono_ns'] - recent[0]['mono_ns'] < 110_000_000_000:
            return {'passed': False, 'reason': 'insufficient duration'}
        battery_range = max(s['battery_c'] for s in recent) - min(s['battery_c'] for s in recent)
        skin_names = [k for k in recent[-1]['temperatures_c'] if 'skin' in k.lower()]
        skin_ranges = {k: max(s['temperatures_c'].get(k, float('inf')) for s in recent) -
                          min(s['temperatures_c'].get(k, float('-inf')) for s in recent) for k in skin_names}
        status = sorted(set(s['thermal_status'] for s in recent))
        return {'passed': battery_range <= .5 and all(v <= 1 for v in skin_ranges.values()) and len(status) == 1,
                'battery_range_c': battery_range, 'skin_ranges_c': skin_ranges, 'thermal_statuses': status,
                'battery_last_c': recent[-1]['battery_c'], 'observations': len(recent)}

    def wait(seconds, label):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if stop.wait(min(10, max(0, end - time.monotonic()))):
                raise RuntimeError('; '.join(errors))
        with lock:
            latest = dict(frames[-1]) if frames else None
        if not latest or latest['pid'] != int(pid):
            raise RuntimeError('No frames in expected process')
        print(json.dumps({'stage': label, 'last_frame': latest, 'thermal': thermal_stable(latest['mono_ns'])}), flush=True)
        return latest

    def set_mode(flags):
        nonlocal sequence
        sequence += 1
        request = out / f'mode-{sequence}.txt'
        request.write_text(f'{sequence} {flags}\n')
        remote = '/data/local/tmp/d2r-attachment-20261016/attachment-mode.txt'
        run(['push', str(request), remote])
        private('cp', remote, DIAG + 'attachment-mode.new')
        private('mv', DIAG + 'attachment-mode.new', DIAG + 'attachment-mode.txt')
        for _ in range(10):
            if stop.wait(1):
                raise RuntimeError('; '.join(errors))
            with lock:
                acknowledged = [m for m in modes if m['sequence'] == sequence and m['flags'] == flags]
            if acknowledged:
                print(json.dumps({'mode_acknowledged': acknowledged[-1]}), flush=True)
                return acknowledged[-1]
        raise RuntimeError('Mode request not acknowledged')

    threads = [threading.Thread(target=fn, daemon=True) for fn in (logs_loop, display_loop, state_loop)]
    for thread in threads:
        thread.start()
    try:
        set_mode(requested_modes[0])
        elapsed = 0
        while elapsed < args.max_warmup:
            latest = wait(30, 'warmup')
            elapsed += 30
            if elapsed >= args.warmup and thermal_stable(latest['mono_ns'])['passed']:
                break
        else:
            raise RuntimeError('No thermal steady state within bounded warmup')
        report['warmup_seconds'] = elapsed
        report['phases'] = []
        for index, flags in enumerate(requested_modes):
            ack = set_mode(flags)
            latest = None
            remaining = args.seconds
            while remaining:
                block = min(30, remaining)
                latest = wait(block, f'phase-{index}-mode-{flags}')
                remaining -= block
            finish = latest['mono_ns']
            begin = finish - args.tail * 1_000_000_000
            with lock:
                ff = [f for f in frames if begin <= f['mono_ns'] <= finish]
                ss = [s for s in states if begin <= s['mono_ns'] <= finish]
                tt = sorted(t for t in display if begin <= t <= finish)
            if len(tt) < 2 or len(ff) < 2 or not ss:
                raise RuntimeError('Insufficient phase observations')
            gaps = [b - a for a, b in zip(tt, tt[1:])]
            valid_scene = all(len(s['scene'].get('players', [])) == 1 and
                              all(s['scene']['players'][0][k] == v for k, v in identity.items()) for s in ss)
            dropped = sum(g >= 2 * refresh[0] for g in gaps)
            logical = {k: ff[-1][k] - ff[0][k] for k in ('draws', 'clear_loads', 'inline_clears', 'transfer_clears', 'discard_stores', 'logical_load_bytes', 'logical_store_bytes')}
            stable = thermal_stable(finish)
            phase = {'index': index, 'flags': flags, 'sequence': sequence, 'ack': ack,
                     'measurement_begin_mono_ns': begin, 'measurement_end_mono_ns': finish,
                     'same_scene_and_position': valid_scene, 'same_mode': all(f['flags'] == flags and f['sequence'] == sequence for f in ff),
                     'thermal': stable, 'display_frames': len(tt), 'display_intervals': len(gaps),
                     'refresh_ns': refresh[0], 'display_fps': (len(tt) - 1) * 1e9 / (tt[-1] - tt[0]),
                     'display_gap_p50_ms': statistics.median(gaps) / 1e6,
                     'display_gap_p95_ms': sorted(gaps)[int(.95 * (len(gaps) - 1))] / 1e6,
                     'display_gap_max_ms': max(gaps) / 1e6, 'gaps_at_least_two_refreshes': dropped,
                     'drop_percent': dropped * 100 / len(gaps), 'cpu_present_frames': len(ff),
                     'logical_counters_delta': logical, 'hardware_bandwidth_bytes': None,
                     'battery_begin_c': ss[0]['battery_c'], 'battery_end_c': ss[-1]['battery_c']}
            phase['valid'] = valid_scene and phase['same_mode'] and stable['passed']
            report['phases'].append(phase)
            save()
            print(json.dumps({'phase_complete': phase}), flush=True)
        report['completed'] = True
        report['all_phases_valid'] = all(p['valid'] for p in report['phases'])
    except Exception as e:
        report['error'] = str(e)
        raise
    finally:
        stop.set()
        for thread in threads:
            thread.join(timeout=25)
        report['errors'] = errors
        report['ended_utc'] = datetime.now(timezone.utc).isoformat()
        report['raw_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir() if p.is_file() and p != receipt}
        save()


if __name__ == '__main__':
    main()
