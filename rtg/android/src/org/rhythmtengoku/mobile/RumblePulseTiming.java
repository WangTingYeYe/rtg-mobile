package org.rhythmtengoku.mobile;

/** Preserve game onsets while allowing a basic phone motor to complete a short pulse. */
final class RumblePulseTiming {
    private final long minimumMillis;
    private int amplitude;
    private long began,stopAt=-1;
    RumblePulseTiming(long minimumMillis){this.minimumMillis=minimumMillis;}
    int update(int value,long now){
        if(value>0){
            if(amplitude==0)began=now;
            amplitude=value;stopAt=-1;
        }else if(amplitude!=0&&now<began+minimumMillis){
            stopAt=began+minimumMillis;
        }else reset();
        return amplitude;
    }
    int advance(long now){if(stopAt>=0&&now>=stopAt)reset();return amplitude;}
    long delay(long now){return stopAt<0?-1:Math.max(0,stopAt-now);}
    void reset(){amplitude=0;began=0;stopAt=-1;}
}
