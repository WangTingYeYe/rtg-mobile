package org.rhythmtengoku.mobile;

/** Prefer 120/60 Hz at the current resolution for evenly paced 60 FPS content. */
final class DisplayRefreshPolicy {
    static final class Mode {
        final int id,width,height;final float hz;
        Mode(int id,int width,int height,float hz){this.id=id;this.width=width;this.height=height;this.hz=hz;}
    }
    static Mode select(Mode current,Mode[] available){
        Mode best=null;
        for(Mode mode:available){
            if(mode.width!=current.width||mode.height!=current.height||!Float.isFinite(mode.hz))continue;
            if(Math.abs(mode.hz-60f)>.5f&&Math.abs(mode.hz-120f)>.5f)continue;
            if(best==null||mode.hz>best.hz+.5f)best=mode;
        }
        return best==null?current:best;
    }
}
