package org.rhythmtengoku.mobile;

import android.content.Context;
import android.media.AudioAttributes;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;

/** Game-driven rumble. No polling/Binder work on the render or audio thread. */
final class GameHaptics implements AutoCloseable {
    private static native long nativeBind(boolean available);
    private static native long nativeActive(long session,boolean active);
    private static native long nativeWait(long session);
    private static native void nativeClose(long session);
    private final Vibrator vibrator;
    private final boolean amplitudeControl;
    private final long session;
    private long epoch;
    private boolean active,closed;
    private int applied;
    private final Handler stopTimer=new Handler(Looper.getMainLooper());
    private final Runnable stopPulse=this::finishPulse;
    private final RumblePulseTiming timing;
    private final AudioAttributes attributes=new AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build();
    GameHaptics(Context context){
        vibrator=(Vibrator)context.getApplicationContext().getSystemService(Context.VIBRATOR_SERVICE);
        boolean available=vibrator!=null&&vibrator.hasVibrator();
        amplitudeControl=available&&vibrator.hasAmplitudeControl();
        // Mate 60 Pro's basic motor route often loses a one-frame (~17 ms)
        // game effect. Start immediately, but hold its motor output for 40 ms.
        // Native amplitude-control devices keep the game's original timing.
        timing=new RumblePulseTiming(amplitudeControl?0:40);
        session=nativeBind(available);
        Thread worker=new Thread(()->{
            for(;;){long event=nativeWait(session);if(event<0)return;apply(event);}
        },"game-vibration");
        worker.setDaemon(true);worker.start();
    }
    synchronized void setActive(boolean value){
        if(closed||active==value)return;
        active=value;epoch=nativeActive(session,value);applied=0;
        timing.reset();stopTimer.removeCallbacks(stopPulse);
        cancel();
    }
    private synchronized void apply(long event){
        if(closed||!active||(event>>>8)!=epoch||vibrator==null)return;
        long now=SystemClock.uptimeMillis();
        render(timing.update((int)(event&255),now));
        stopTimer.removeCallbacks(stopPulse);
        long delay=timing.delay(now);if(delay>=0)stopTimer.postDelayed(stopPulse,delay);
    }
    private synchronized void finishPulse(){
        if(closed||!active)return;
        long now=SystemClock.uptimeMillis();render(timing.advance(now));
        long delay=timing.delay(now);if(delay>=0)stopTimer.postDelayed(stopPulse,delay);
    }
    private void render(int strength){
        int effective=strength==0?0:amplitudeControl?strength:VibrationEffect.DEFAULT_AMPLITUDE;
        if(effective==applied)return;
        try{
            if(effective==0)vibrator.cancel();
            else vibrator.vibrate(VibrationEffect.createWaveform(new long[]{1000},new int[]{effective},0),attributes);
            applied=effective;
        }catch(RuntimeException e){
            android.util.Log.w("RhythmTengokuMobile","Game vibration unavailable",e);
            active=false;epoch=nativeActive(session,false);applied=0;timing.reset();stopTimer.removeCallbacks(stopPulse);cancel();
        }
    }
    private void cancel(){
        if(vibrator==null)return;
        try{vibrator.cancel();}
        catch(RuntimeException e){android.util.Log.w("RhythmTengokuMobile","Could not stop game vibration",e);}
    }
    @Override public synchronized void close(){
        if(closed)return;
        closed=true;active=false;timing.reset();stopTimer.removeCallbacks(stopPulse);nativeClose(session);
        cancel();
    }
}
