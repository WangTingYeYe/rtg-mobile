"""Reprocess captured actual-display intervals using recorded active display modes.

Retains the capture's strict nominal threshold alongside a refresh-slot count.
Rounding to the nearest refresh slot avoids misclassifying a 66.59ms double
refresh as one refresh when Android reports a nominal 33.333333ms period.
"""
import argparse
import hashlib
import json
import statistics
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--supplement',type=Path)
    args = parser.parse_args()
    root = args.directory
    capture = json.loads((root / 'result.json').read_text())
    if not capture.get('completed') and not args.supplement:
        raise RuntimeError('Capture not complete')
    captures=[(root,capture)]
    if args.supplement:
        extra=json.loads((args.supplement/'result.json').read_text())
        if not extra.get('completed') or extra['pid']!=capture['pid'] or extra['scene_identity']!=capture['scene_identity']:
            raise RuntimeError('Supplement must complete in the same process and same scene')
        captures.append((args.supplement,extra))
    phases = []
    for directory,part in captures:
      display={}
      for line in (directory/'display-samples.jsonl').read_text().splitlines():
        for times in json.loads(line)['rows']:display[times[1]]=times
      if (directory/'display-modes.jsonl').exists():
        modes=[json.loads(line) for line in (directory/'display-modes.jsonl').read_text().splitlines()]
      else:
        modes=[{'mono_ns':s['mono_ns'],'vsync_hz':s['active_display_hz']} for s in
               (json.loads(line) for line in (directory/'state-samples.jsonl').read_text().splitlines())]
      for original in part['phases']:
        p = dict(original)
        p['index']=len(phases);p['source_run']=directory.name
        start, finish = p['measurement_begin_mono_ns'], p['measurement_end_mono_ns']
        mode_rows = [m for m in modes if start - 6_000_000_000 <= m['mono_ns'] <= finish + 6_000_000_000]
        if not mode_rows:
            raise RuntimeError('Missing active display mode for phase ' + str(p['index']))
        rates = sorted({m['vsync_hz'] for m in mode_rows})
        times = sorted(t for t in display if start <= t <= finish)
        if len(rates) != 1:
            p['valid'] = False
            p['display_mode_changed'] = True
        hz = rates[-1]
        gaps = [b - a for a, b in zip(times, times[1:])]
        # Quantize observed presentation intervals to complete refresh slots.
        # All groups use the same rule; keep original strict counts visible.
        slots = [max(1, int(g * hz / 1e9 + .5)) for g in gaps]
        missed = sum(n - 1 for n in slots)
        p.update(display_frames=len(times),display_intervals=len(gaps),
                 display_fps=(len(times)-1)*1e9/(times[-1]-times[0]),
                 display_gap_p50_ms=statistics.median(gaps)/1e6,
                 display_gap_p95_ms=sorted(gaps)[int(.95*(len(gaps)-1))]/1e6,
                 display_gap_max_ms=max(gaps)/1e6)
        p.update(active_display_hz=rates, strict_nominal_drop_percent=p['drop_percent'],
                 refresh_slot_drop_intervals=sum(n >= 2 for n in slots),
                 refresh_slot_drop_percent=100 * sum(n >= 2 for n in slots) / len(gaps),
                 missed_refresh_slots=missed, missed_refresh_slot_percent=100 * missed / sum(slots),
                 clear_loads_per_cpu_frame=p['logical_counters_delta']['clear_loads'] / (p['cpu_present_frames'] - 1),
                 logical_load_bytes_per_cpu_frame=p['logical_counters_delta']['logical_load_bytes'] / (p['cpu_present_frames'] - 1))
        phases.append(p)
    result = {'completed': True, 'hardware_bandwidth_measured': False,
              'same_process': capture['pid'], 'warmup_seconds': capture['warmup_seconds'],
              'scene_identity': capture['scene_identity'], 'phases': phases,
              'interrupted_capture_error':capture.get('error'),
              'drop_rule': 'Round each actual-display gap to nearest number of active-display refresh slots; count gaps spanning >=2 slots. Preserve strict nominal >=2T metric separately.',
              'source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'raw_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for d,_ in captures for p in d.iterdir() if p.is_file() and p.name != 'analysis.json'}}
    (root / 'analysis.json').write_text(json.dumps(result, indent=2) + '\n')
    for p in phases:
        print(json.dumps({k: p[k] for k in ('index', 'flags', 'display_fps', 'active_display_hz', 'refresh_slot_drop_intervals', 'display_intervals', 'refresh_slot_drop_percent', 'clear_loads_per_cpu_frame', 'logical_load_bytes_per_cpu_frame', 'valid')}))


if __name__ == '__main__':
    main()
