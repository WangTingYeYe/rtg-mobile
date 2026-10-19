package org.rhythmtengoku.mobile;

public final class RumblePulseTimingTest {
    private static int checks;
    private static void eq(long actual,long expected){
        checks++;if(actual!=expected)throw new AssertionError("expected "+expected+", got "+actual);
    }
    public static void main(String[] args){
        // A captured first-stage jump sends amplitude 1 for a single 60 Hz frame.
        // "legacy" replays the previous immediate-stop behavior as a negative control.
        RumblePulseTiming p=new RumblePulseTiming(args.length==0?40:0);
        eq(p.update(255,0),255);eq(p.update(0,17),255);
        eq(p.delay(17),23);eq(p.advance(39),255);eq(p.advance(40),0);eq(p.delay(40),-1);

        p=new RumblePulseTiming(40);
        eq(p.update(255,0),255);eq(p.update(0,17),255);
        eq(p.update(128,30),128);eq(p.delay(30),-1);
        eq(p.advance(40),128); // An old stop callback cannot cut a resumed effect.
        eq(p.update(0,60),0); // A long game effect keeps its original end time.
        eq(p.update(255,100),255);eq(p.update(0,117),255);
        p.reset();eq(p.advance(120),0);eq(p.delay(120),-1); // Pause cancels immediately.
        eq(p.update(64,130),64);eq(p.advance(140),64); // Old callback after resume.
        eq(p.update(0,147),64);eq(p.advance(170),0);

        for(int duration=1;duration<=250;duration++){
            long start=duration*1000L,end=start+Math.max(40,duration);
            eq(p.update(128,start),128);
            eq(p.update(0,start+duration),duration<40?128:0);
            if(duration<40)eq(p.advance(end-1),128);
            eq(p.advance(end),0);eq(p.delay(end),-1);
        }
        // Devices with native amplitude control retain the original 17 ms timing.
        p=new RumblePulseTiming(0);eq(p.update(255,0),255);eq(p.update(0,17),0);eq(p.delay(17),-1);
        System.out.println("PASS "+checks+" assertions: one-frame game rumble, sustained effects, overlaps, pause/resume and stale timers");
    }
}
