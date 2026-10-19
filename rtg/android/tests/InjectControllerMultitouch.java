package org.rhythmtengoku.test;

import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;
import java.lang.reflect.Method;

/** Shell-only integration probe: exercises actual Android pointer IDs, not JNI.
 * Invoke with explicitly selected on-screen direction/A coordinates after checking
 * the game is foreground. Never bundled into the app. */
public final class InjectControllerMultitouch {
    static Object manager;
    static Method inject;
    static long down;
    static void send(int action,float[] xs,float[] ys)throws Exception {
        int count=xs.length;
        MotionEvent.PointerProperties[] properties=new MotionEvent.PointerProperties[count];
        MotionEvent.PointerCoords[] coords=new MotionEvent.PointerCoords[count];
        for(int i=0;i<count;i++){
            properties[i]=new MotionEvent.PointerProperties();properties[i].id=i;
            properties[i].toolType=MotionEvent.TOOL_TYPE_FINGER;
            coords[i]=new MotionEvent.PointerCoords();coords[i].x=xs[i];coords[i].y=ys[i];
            coords[i].pressure=1;coords[i].size=.1f;
        }
        MotionEvent event=MotionEvent.obtain(down,SystemClock.uptimeMillis(),action,count,properties,coords,
            0,0,1,1,0,0,InputDevice.SOURCE_TOUCHSCREEN,0);
        boolean accepted=(Boolean)inject.invoke(manager,event,2);event.recycle();
        if(!accepted)throw new IllegalStateException("Input injection rejected");
        SystemClock.sleep(180);
    }
    public static void main(String[] args)throws Exception {
        if(args.length!=5&&args.length!=7)throw new IllegalArgumentException("padX padY upY aX aY [bX bY]");
        float sx=Float.parseFloat(args[0]),sy=Float.parseFloat(args[1]),uy=Float.parseFloat(args[2]);
        float ax=Float.parseFloat(args[3]),ay=Float.parseFloat(args[4]);
        Class<?> type=Class.forName("android.hardware.input.InputManager");
        manager=type.getMethod("getInstance").invoke(null);
        inject=type.getMethod("injectInputEvent",InputEvent.class,int.class);
        down=SystemClock.uptimeMillis();
        boolean cancelled=false;
        try{
            send(MotionEvent.ACTION_DOWN,new float[]{sx},new float[]{sy});
            send(MotionEvent.ACTION_MOVE,new float[]{sx},new float[]{uy});
            send(MotionEvent.ACTION_POINTER_DOWN|(1<<MotionEvent.ACTION_POINTER_INDEX_SHIFT),new float[]{sx,ax},new float[]{uy,ay});
            send(MotionEvent.ACTION_POINTER_UP|(1<<MotionEvent.ACTION_POINTER_INDEX_SHIFT),new float[]{sx,ax},new float[]{uy,ay});
            send(MotionEvent.ACTION_CANCEL,new float[]{sx},new float[]{uy});
            cancelled=true;
            if(args.length==7){
                float bx=Float.parseFloat(args[5]),by=Float.parseFloat(args[6]);
                down=SystemClock.uptimeMillis();cancelled=false;
                send(MotionEvent.ACTION_DOWN,new float[]{bx},new float[]{by});
                send(MotionEvent.ACTION_POINTER_DOWN|(1<<MotionEvent.ACTION_POINTER_INDEX_SHIFT),new float[]{bx,ax},new float[]{by,ay});
                send(MotionEvent.ACTION_MOVE,new float[]{sx,ax},new float[]{uy,ay});
                send(MotionEvent.ACTION_POINTER_UP|(1<<MotionEvent.ACTION_POINTER_INDEX_SHIFT),new float[]{sx,ax},new float[]{uy,ay});
                send(MotionEvent.ACTION_CANCEL,new float[]{sx},new float[]{uy});cancelled=true;
            }
        }finally{if(!cancelled)send(MotionEvent.ACTION_CANCEL,new float[]{sx},new float[]{uy});}
        System.out.println("INJECTED direction+A, independent release/cancel; optional B+A and B drift over pad; inspect native Npad receipts");
        System.out.flush();
        System.exit(0);
    }
}
