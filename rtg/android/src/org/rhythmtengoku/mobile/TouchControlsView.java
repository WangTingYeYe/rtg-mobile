package org.rhythmtengoku.mobile;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Insets;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import java.util.function.BooleanSupplier;

/** Minimal rhythm controls; touch regions are independent of their visual opacity. */
public final class TouchControlsView extends View {
    interface InputSink {void publish(long buttons,int lx,int ly,int rx,int ry);}
    static final int INK=0x80161d2b, RIM=0xffd5dee9, TEXT=0xfff7f9fc, PRESSED=0xe650397e;
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path chevron=new Path();
    private final NintendoControllerState state=new NintendoControllerState();
    private final BooleanSupplier ready;
    private final InputSink sink;
    private final Runnable cancelSink;
    private final float density;
    private final int[] pulseGeneration=new int[32];
    private int accessibilityFocus=View.NO_ID;
    private final android.content.SharedPreferences preferences;
    private int opacity;
    private boolean extras;

    public int opacity(){return opacity;}
    public boolean extraButtons(){return extras;}
    public void setOpacity(int value){
        opacity=Math.max(0,Math.min(100,value));
        preferences.edit().putInt("opacity",opacity).apply();invalidate();
    }
    public void setExtraButtons(boolean show){
        extras=show;preferences.edit().putBoolean("extra-buttons",show).apply();
        state.setExtraButtons(show);resize();
    }

    public TouchControlsView(Context context,BooleanSupplier ready,InputSink sink,Runnable cancelSink){
        super(context);this.ready=ready;this.sink=sink;this.cancelSink=cancelSink;density=getResources().getDisplayMetrics().density;
        preferences=context.getSharedPreferences("rhythm-touch",Context.MODE_PRIVATE);
        opacity=Math.max(0,Math.min(100,preferences.getInt("opacity",35)));
        extras=preferences.getBoolean("extra-buttons",false);state.setExtraButtons(extras);
        setContentDescription("节奏操作：左侧方向盘，右半屏 A；左上独立 B、Y、减号和加号");
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);setFocusable(true);
    }
    public void cancel(){
        state.clear();for(int i=0;i<pulseGeneration.length;i++)pulseGeneration[i]++;
        cancelSink.run();invalidate();
    }
    private void publish(){sink.publish(state.buttons(),0,0,0,0);}
    static Insets safeInsets(WindowInsets insets){
        if(insets==null)return Insets.NONE;
        Insets gestures=insets.getSystemGestureInsets();
        if(insets.getDisplayCutout()==null)return gestures;
        return Insets.of(Math.max(gestures.left,insets.getDisplayCutout().getSafeInsetLeft()),
                Math.max(gestures.top,insets.getDisplayCutout().getSafeInsetTop()),
                Math.max(gestures.right,insets.getDisplayCutout().getSafeInsetRight()),
                Math.max(gestures.bottom,insets.getDisplayCutout().getSafeInsetBottom()));
    }
    private void resize(){
        if(getWidth()==0||getHeight()==0)return;
        Insets safe=safeInsets(getRootWindowInsets());
        cancel();state.layout(getWidth()/density,getHeight()/density,safe.left/density,safe.top/density,safe.right/density,safe.bottom/density);invalidate();
        sendAccessibilityEvent(AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
    }
    @Override protected void onSizeChanged(int w,int h,int oldw,int oldh){super.onSizeChanged(w,h,oldw,oldh);resize();}
    @Override public WindowInsets onApplyWindowInsets(WindowInsets insets){post(this::resize);return super.onApplyWindowInsets(insets);}
    @Override public void onWindowFocusChanged(boolean hasFocus){super.onWindowFocusChanged(hasFocus);if(!hasFocus)cancel();invalidate();}
    @Override protected void onDetachedFromWindow(){cancel();super.onDetachedFromWindow();}

    private void shape(Canvas canvas,float x,float y,float hw,float hh,boolean round){
        if(round)canvas.drawCircle(x,y,hw,paint);
        else canvas.drawRoundRect(x-hw,y-hh,x+hw,y+hh,8,8,paint);
    }
    private void label(Canvas canvas,String text,float x,float y,float size,float maxWidth,int alpha){
        paint.setTextAlign(Paint.Align.CENTER);paint.setTextSize(size*getResources().getConfiguration().fontScale);
        float measured=paint.measureText(text);if(measured>maxWidth)paint.setTextSize(paint.getTextSize()*maxWidth/measured);
        float baseline=y-(paint.ascent()+paint.descent())/2;
        // A two-tone edge stays visible on both the white tutorials and dark stages.
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(2.5f);paint.setColor(0xff10141c);paint.setAlpha(alpha);
        canvas.drawText(text,x,baseline,paint);
        paint.setStyle(Paint.Style.FILL);paint.setColor(TEXT);paint.setAlpha(alpha);canvas.drawText(text,x,baseline,paint);
    }
    private void directionIcon(Canvas canvas,NintendoControllerState.Control c,int alpha){
        canvas.save();canvas.translate(c.x,c.y);
        float angle=c.mask==NintendoControllerState.RIGHT?90:c.mask==NintendoControllerState.DOWN?180:c.mask==NintendoControllerState.LEFT?270:0;
        canvas.rotate(angle);chevron.reset();chevron.moveTo(-7,3);chevron.lineTo(0,-4);chevron.lineTo(7,3);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeCap(Paint.Cap.ROUND);paint.setStrokeJoin(Paint.Join.ROUND);
        paint.setColor(0xff10141c);paint.setAlpha(alpha/2);paint.setStrokeWidth(5);canvas.drawPath(chevron,paint);
        paint.setColor(TEXT);paint.setAlpha(alpha);paint.setStrokeWidth(2.5f);canvas.drawPath(chevron,paint);
        paint.setStrokeCap(Paint.Cap.BUTT);paint.setStrokeJoin(Paint.Join.MITER);canvas.restore();
    }
    private void controlSurfaces(Canvas canvas,int alpha){
        if(alpha==0||state.controls().isEmpty())return;
        NintendoControllerState.Control b=state.button(NintendoControllerState.B),plus=state.button(NintendoControllerState.PLUS);
        paint.setStyle(Paint.Style.FILL);paint.setColor(INK);paint.setAlpha(Math.round(alpha*.16f));
        canvas.drawRoundRect(b.x-28,b.y-24,plus.x+28,plus.y+24,24,24,paint);
        canvas.drawCircle(state.padX(),state.padY(),80,paint);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1);paint.setColor(TEXT);paint.setAlpha(Math.round(alpha*.5f));
        canvas.drawRoundRect(b.x-28,b.y-24,plus.x+28,plus.y+24,24,24,paint);
        canvas.drawCircle(state.padX(),state.padY(),80,paint);
        // The centre is deliberately neutral; its ring shows where a thumb can rest.
        paint.setAlpha(Math.round(alpha*.35f));canvas.drawCircle(state.padX(),state.padY(),20,paint);
    }
    @Override protected void onDraw(Canvas canvas){
        super.onDraw(canvas);canvas.save();canvas.scale(density,density);
        boolean enabled=ready.getAsBoolean();
        controlSurfaces(canvas,Math.round(255*opacity/100f*(enabled?1f:.4f)));
        for(NintendoControllerState.Control c:state.controls()){
            boolean pressed=state.held(c),focus=accessibilityFocus==c.id;
            int alpha=Math.round(255*(focus?1f:pressed?Math.max(.8f,opacity/100f):opacity/100f)*(enabled?1f:.4f));
            if(alpha==0)continue;
            boolean face=c.kind==NintendoControllerState.A_REGION;
            float x=face?state.faceX(c):c.x,y=face?state.faceY(c):c.y;
            float hw=face?26:c.kind==NintendoControllerState.BUTTON?19:22;
            float hh=hw;
            boolean round=true;
            if(face){
                // The large frame explains the hit area without covering the
                // game with a filled panel or a flashing screen-sized overlay.
                paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(focus?2:1);
                paint.setColor(TEXT);paint.setAlpha(Math.round(alpha*.3f));
                canvas.drawRoundRect(c.x-c.halfWidth+4,c.y-c.halfHeight+4,c.x+c.halfWidth-4,c.y+c.halfHeight-4,12,12,paint);
            }
            if(pressed||focus){
                paint.setStyle(Paint.Style.FILL);paint.setColor(PRESSED);paint.setAlpha(Math.round(alpha*.65f));shape(canvas,x,y,hw,hh,round);
            }
            if(c.kind==NintendoControllerState.BUTTON){
                paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(focus?4:3);paint.setColor(0xff10141c);paint.setAlpha(alpha/2);shape(canvas,x,y,hw,hh,round);
                paint.setStrokeWidth(focus?2:1);paint.setColor(TEXT);paint.setAlpha(alpha);shape(canvas,x,y,hw,hh,round);
            }
            if(c.kind==NintendoControllerState.DIRECTION)directionIcon(canvas,c,alpha);
            else label(canvas,c.label,x,y,face?24:17,hw*1.8f,alpha);
        }
        paint.setAlpha(255);canvas.restore();
    }
    @Override public boolean onTouchEvent(MotionEvent event){
        if(!ready.getAsBoolean()){cancel();return true;}
        int action=event.getActionMasked(),index=event.getActionIndex();
        switch(action){
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                state.down(event.getPointerId(index),event.getX(index)/density,event.getY(index)/density);break;
            case MotionEvent.ACTION_MOVE:
                for(int i=0;i<event.getPointerCount();i++)state.move(event.getPointerId(i),event.getX(i)/density,event.getY(i)/density);
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                state.up(event.getPointerId(index));break;
            case MotionEvent.ACTION_CANCEL:
                cancel();return true;
            default:return true;
        }
        // Do not debounce or wait for a drawing frame: rhythm timing follows the
        // actual DOWN/UP event, and every pointer keeps its independent owner.
        publish();invalidate();return true;
    }
    private void accessibilityEvent(int id,int type){
        AccessibilityEvent event=AccessibilityEvent.obtain(type);event.setSource(this,id);
        event.setPackageName(getContext().getPackageName());event.setClassName("android.widget.Button");
        NintendoControllerState.Control c=state.control(id);if(c!=null)event.setContentDescription(c.description);
        if(getParent()!=null)getParent().requestSendAccessibilityEvent(this,event);
    }
    @Override public AccessibilityNodeProvider getAccessibilityNodeProvider(){return new AccessibilityNodeProvider(){
        @Override public AccessibilityNodeInfo createAccessibilityNodeInfo(int id){
            if(id==View.NO_ID){
                AccessibilityNodeInfo node=AccessibilityNodeInfo.obtain(TouchControlsView.this);onInitializeAccessibilityNodeInfo(node);
                for(NintendoControllerState.Control c:state.controls())node.addChild(TouchControlsView.this,c.id);
                return node;
            }
            NintendoControllerState.Control c=state.control(id);if(c==null)return null;
            AccessibilityNodeInfo node=AccessibilityNodeInfo.obtain();node.setPackageName(getContext().getPackageName());
            node.setSource(TouchControlsView.this,id);node.setParent(TouchControlsView.this);
            node.setClassName("android.widget.Button");
            node.setContentDescription(c.description);node.setClickable(true);
            node.setFocusable(true);node.setAccessibilityFocused(accessibilityFocus==id);node.setSelected(state.held(c));
            node.setEnabled(ready.getAsBoolean());node.setVisibleToUser(isShown());
            boolean face=c.kind==NintendoControllerState.A_REGION;
            float ax=face?state.faceX(c):c.x,ay=face?state.faceY(c):c.y;
            float ahw=face?24:c.halfWidth,ahh=face?24:c.halfHeight;
            Rect bounds=new Rect((int)((ax-ahw)*density),(int)((ay-ahh)*density),
                    (int)((ax+ahw)*density),(int)((ay+ahh)*density));
            node.setBoundsInParent(bounds);int[] location=new int[2];getLocationOnScreen(location);
            bounds.offset(location[0],location[1]);node.setBoundsInScreen(bounds);
            node.addAction(AccessibilityNodeInfo.ACTION_CLICK);
            if(c.mask==NintendoControllerState.A||c.mask==NintendoControllerState.B||c.mask==NintendoControllerState.Y)
                node.addAction(new AccessibilityNodeInfo.AccessibilityAction(AccessibilityNodeInfo.ACTION_LONG_CLICK,"切换按住或松开"));
            node.addAction(accessibilityFocus==id?AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS:AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS);
            return node;
        }
        @Override public boolean performAction(int id,int action,Bundle args){
            NintendoControllerState.Control c=state.control(id);if(c==null)return false;
            if(action==AccessibilityNodeInfo.ACTION_CLICK&&ready.getAsBoolean()){
                int generation=++pulseGeneration[id];state.accessibilityButton(id,true);publish();invalidate();
                accessibilityEvent(id,AccessibilityEvent.TYPE_VIEW_CLICKED);
                postDelayed(()->{if(pulseGeneration[id]==generation){state.accessibilityButton(id,false);publish();invalidate();}},120);
                return true;
            }
            if(action==AccessibilityNodeInfo.ACTION_LONG_CLICK&&ready.getAsBoolean()&&(c.mask==NintendoControllerState.A||c.mask==NintendoControllerState.B||c.mask==NintendoControllerState.Y)){
                ++pulseGeneration[id];state.accessibilityButton(id,!state.held(c));publish();invalidate();return true;
            }
            if(action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS||action==AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS){
                accessibilityFocus=action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS?id:View.NO_ID;
                accessibilityEvent(id,action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS?AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED:AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
                invalidate();return true;
            }
            return false;
        }
    };}
}
