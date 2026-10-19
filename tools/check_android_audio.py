#!/usr/bin/env python3
"""Check actual REV output on the explicitly selected, already running device.

Use a scene with continuous music. This checks device-clock progress and audio
energy, not perceptual fidelity or completion of every game/audio feature.
--capture requests one bounded ten-second callback capture in app-private data.
"""
import argparse
import array
import json
import math
from pathlib import Path
import re
import shlex
import subprocess
import time
import wave


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial',required=True)
    p.add_argument('--seconds',type=float,default=12)
    p.add_argument('--capture',action='store_true')
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    adb=['adb','-s',a.serial];package='org.rhythmtengoku.mobile'
    root='files/rhythm-tengoku/diagnostics/'
    def shell(*args,required=True):
        r=subprocess.run(adb+['shell',shlex.join(args)],capture_output=True,text=True,errors='replace',timeout=15)
        if required:r.check_returncode()
        return r.stdout.strip()
    def metrics(log):
        lines=[x for x in log.splitlines() if '[audio-renderer] OUTPUT ' in x]
        if not lines:return {}
        return {k:float(v) for k,v in re.findall(r'(\w+)=(-?[0-9]+(?:\.[0-9]+)?)',lines[-1])}
    def foreground():
        return any('mCurrentFocus' in line and package+'/' in line
                   for line in shell('dumpsys','window','displays').splitlines())
    if not foreground():raise SystemExit('Game must be foreground')
    pid=shell('pidof',package)
    if not pid:raise SystemExit('Game is not running')
    before=shell('run-as',package,'cat',root+'game-startup.log');first=metrics(before)
    initial_stat=shell('run-as',package,'stat','-c','%Y %s',root+'rendered-audio.f32',required=False)
    if a.capture:shell('run-as',package,'touch',root+'capture-audio-now')
    start=time.monotonic()
    while time.monotonic()-start<a.seconds:time.sleep(max(0,min(1,a.seconds-(time.monotonic()-start))))
    capture_ready=False
    if a.capture:
        deadline=start+max(25,a.seconds)
        while time.monotonic()<deadline:
            stat=shell('run-as',package,'stat','-c','%Y %s',root+'rendered-audio.f32',required=False)
            if stat and stat!=initial_stat and stat.endswith(' 3840000'):
                capture_ready=True;break
            time.sleep(1)
    after=shell('run-as',package,'cat',root+'game-startup.log');last=metrics(after)
    (a.output/'native.txt').write_text(after+'\n')
    (a.output/'audioflinger.txt').write_text(shell('dumpsys','media.audio_flinger'))
    if foreground():
        (a.output/'screen.png').write_bytes(subprocess.check_output(adb+['exec-out','screencap','-p'],timeout=15))
    final_pid=shell('pidof',package,required=False)
    appended=after[len(before):] if after.startswith(before) else after
    errors=[line for line in appended.splitlines() if re.search(r'PARAMETER_ERROR|DEVICE_RECOVERY_FAILED|ANDROID_FATAL|FATAL EXCEPTION',line)]
    frames=last.get('frames',0)-first.get('frames',0)
    nonzero=last.get('nonzero',0)-first.get('nonzero',0)
    report={'serial':a.serial,'pid':pid,'final_pid':final_pid,'before':first,'after':last,
            'frames_advanced':frames,'nonzero_samples_added':nonzero,'errors':errors[:8]}
    no_underruns=True
    if 'underrun_frames' in first and 'underrun_frames' in last:
        report['underrun_frames_added']=last['underrun_frames']-first['underrun_frames']
        no_underruns=report['underrun_frames_added']==0
    causal=True
    if all(k in last for k in ('mixed_frames','consumed_frames','status_frames')):
        causal=0<=last['status_frames']<=last['consumed_frames']<=last['mixed_frames']
        report['consumed_checkpoint_order_valid']=causal
    pcm_ok=not a.capture
    if capture_ready:
        raw=subprocess.check_output(adb+['exec-out','run-as',package,'cat',root+'rendered-audio.f32'],timeout=30)
        (a.output/'audio.f32').write_bytes(raw);samples=array.array('f');samples.frombytes(raw)
        finite=all(math.isfinite(x) for x in samples)
        if finite and samples:
            rms=math.sqrt(sum(x*x for x in samples)/len(samples));peak=max(abs(x) for x in samples)
            report['pcm']={'frames':len(samples)//2,'rms':rms,'peak':peak,
                'nonzero_fraction':sum(abs(x)>1e-6 for x in samples)/len(samples),
                'clipped_fraction':sum(abs(x)>=1 for x in samples)/len(samples)}
            pcm=array.array('h',(round(max(-1,min(1,x))*32767) for x in samples))
            with wave.open(str(a.output/'audio.wav'),'wb') as out:
                out.setparams((2,2,48000,0,'NONE','not compressed'));out.writeframes(pcm.tobytes())
            pcm_ok=len(samples)==960000 and rms>0.0001 and peak<=1
        else:report['pcm']={'finite':False}
    report['passed']=bool(pid==final_pid and frames>0 and nonzero>0 and last.get('timestamp')==0 and not errors and pcm_ok and no_underruns and causal)
    (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
