package org.rhythmtengoku.mobile;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.Shader;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import java.util.LinkedHashMap;
import java.util.function.BooleanSupplier;

/** Touchable's rhythm layout with an Android rendition of its clear-glass buttons.
 *  Touch areas are independent of visual opacity; the right half is A. */
public final class TouchControlsView extends View {
    interface InputSink {void publish(long buttons,int lx,int ly,int rx,int ry);}
    static final int INK=0xff161d2b, RIM=0xffd5dee9, TEXT=0xfff7f9fc, PRESSED=0xe650397e;
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path triangle=new Path();
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
    private long hint=NintendoControllerState.LAYOUT_MENU;
    private float fontScale;
    /** The game's pause menu is opened by +. While it is up the level's
     *  adaptive left key cannot move the menu cursor, so show the small D-pad. */
    private boolean pauseMenu;
    private long lastButtons;
    private float[] pressScale=new float[0];
    private long lastFrameNanos;
    private static final class Feedback {
        final String label;
        float x,y;
        long released=-1;
        Feedback(String label,float x,float y){this.label=label;this.x=x;this.y=y;}
    }
    private final LinkedHashMap<Integer,Feedback> feedback=new LinkedHashMap<>();

    public int opacity(){return opacity;}
    public boolean extraButtons(){return extras;}
    public void setOpacity(int value){
        opacity=Math.max(0,Math.min(100,value));
        preferences.edit().putInt("opacity",opacity).apply();invalidate();
    }
    public void setExtraButtons(boolean show){
        extras=show;preferences.edit().putBoolean("extra-buttons",show).apply();
        reapplyHint();invalidate();
    }
    private void reapplyHint(){
        applyCurrentHint();publish();invalidate();
    }
    private void applyCurrentHint(){
        int layout=(int)(hint&255),first=(int)(hint>>8&255),second=(int)(hint>>16&255),extra=(int)(hint>>24&255);
        if(pauseMenu)layout=NintendoControllerState.LAYOUT_MENU;
        if(extras)extra|=NintendoControllerState.SHOW_DPAD;
        state.setHint(layout,first,second,extra);
    }
    public void applyHint(long packed){
        if(packed==hint)return;
        int oldLayout=state.hintLayout();
        hint=packed;applyCurrentHint();
        // A scene change can release a held adaptive key; tell the game now.
        feedback.keySet().removeIf(id->!state.owns(id));
        if(state.hintLayout()!=oldLayout)feedback.clear();
        publish();invalidate();
    }
    public TouchControlsView(Context context,BooleanSupplier ready,InputSink sink,Runnable cancelSink){
        super(context);this.ready=ready;this.sink=sink;this.cancelSink=cancelSink;density=getResources().getDisplayMetrics().density;
        fontScale=getResources().getConfiguration().fontScale;
        preferences=context.getSharedPreferences("rhythm-touch",Context.MODE_PRIVATE);
        opacity=Math.max(0,Math.min(100,preferences.getInt("opacity",45)));
        extras=preferences.getBoolean("extra-buttons",false);
        setContentDescription("节奏操作：右半屏 A，左侧随关卡变化的按键，小十字键与侧边 B、Y、X、减号、加号");
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);setFocusable(true);
    }
    public void cancel(){
        pauseMenu=false;lastButtons=0;state.clear();feedback.clear();
        for(int i=0;i<pulseGeneration.length;i++)pulseGeneration[i]++;
        cancelSink.run();invalidate();
    }
    private void publish(){
        long held=state.buttons();
        long pressed=held&~lastButtons;
        boolean wasPause=pauseMenu;
        // + opens the game's pause menu; A/B leave it. While it is up the small
        // D-pad is the only way to move the cursor.
        if((pressed&NintendoControllerState.PLUS)!=0)pauseMenu=!pauseMenu;
        if((pressed&(NintendoControllerState.A|NintendoControllerState.B))!=0)pauseMenu=false;
        if(pauseMenu!=wasPause){applyCurrentHint();held=state.buttons();}
        lastButtons=held;
        sink.publish(held,0,0,0,0);
        if(pauseMenu!=wasPause)invalidate();
    }
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
        fontScale=getResources().getConfiguration().fontScale;
        cancel();state.layout(getWidth()/density,getHeight()/density,safe.left/density,safe.top/density,safe.right/density,safe.bottom/density);
        pressScale=new float[state.controls().size()];java.util.Arrays.fill(pressScale,1f);
        invalidate();
        sendAccessibilityEvent(AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
    }
    @Override protected void onSizeChanged(int w,int h,int oldw,int oldh){super.onSizeChanged(w,h,oldw,oldh);resize();}
    @Override public WindowInsets onApplyWindowInsets(WindowInsets insets){post(this::resize);return super.onApplyWindowInsets(insets);}
    @Override public void onWindowFocusChanged(boolean hasFocus){super.onWindowFocusChanged(hasFocus);if(!hasFocus)cancel();invalidate();}
    @Override protected void onDetachedFromWindow(){cancel();super.onDetachedFromWindow();}

    private static int white(float alpha){return Color.argb(Math.round(Math.max(0,Math.min(1,alpha))*255),255,255,255);}
    /** A clear-glass circle: cool translucent body, bright rim, top highlight. */
    private void glassCircle(Canvas canvas,float x,float y,float radius,float alpha,boolean pressed){
        float r=radius*(pressed?1.06f:1f);
        paint.setStyle(Paint.Style.FILL);
        paint.setShader(new RadialGradient(x-r*.36f,y-r*.44f,r*1.55f,
                new int[]{white(.30f*alpha),white(.13f*alpha),white(.045f*alpha)},
                new float[]{0f,.55f,1f},Shader.TileMode.CLAMP));
        canvas.drawCircle(x,y,r,paint);
        paint.setShader(null);
        if(pressed){
            paint.setColor(white(.13f*alpha));canvas.drawCircle(x,y,r,paint);
        }
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1.2f);
        paint.setShader(new LinearGradient(x-r,y-r,x+r,y+r,
                new int[]{white(.80f*alpha),white(.18f*alpha),white(.38f*alpha)},
                new float[]{0f,.55f,1f},Shader.TileMode.CLAMP));
        canvas.drawCircle(x,y,r-.7f,paint);
        paint.setShader(null);
        paint.setStrokeWidth(1f);paint.setColor(white(.30f*alpha));
        canvas.drawArc(x-r*.62f,y-r*.76f,x+r*.62f,y+r*.48f,204,132,false,paint);
        paint.setStyle(Paint.Style.FILL);
    }
    private void label(Canvas canvas,String text,float x,float y,float size,float alpha){
        paint.setStyle(Paint.Style.FILL);paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(size*fontScale);
        paint.setColor(white(alpha));
        float baseline=y-(paint.ascent()+paint.descent())/2;
        canvas.drawText(text,x,baseline,paint);
    }
    private void arrow(Canvas canvas,float x,float y,float size,long mask,float alpha){
        float tip=size*.22f;
        canvas.save();canvas.translate(x,y);
        canvas.rotate(mask==NintendoControllerState.RIGHT?90:mask==NintendoControllerState.DOWN?180:mask==NintendoControllerState.LEFT?270:0);
        triangle.reset();triangle.moveTo(0,-tip);triangle.lineTo(tip*.9f,tip*.55f);triangle.lineTo(-tip*.9f,tip*.55f);triangle.close();
        paint.setStyle(Paint.Style.FILL);paint.setColor(white(alpha));canvas.drawPath(triangle,paint);
        canvas.restore();
    }
    private void zones(Canvas canvas,float alpha){
        if(state.leftVisible()&&state.hintLayout()!=NintendoControllerState.LAYOUT_ONE){
            boolean pressed=state.leftHeld();
            if(pressed){paint.setStyle(Paint.Style.FILL);paint.setColor(white(.10f*alpha));
                canvas.drawRoundRect(state.leftX(),state.leftY(),state.leftRight(),state.leftBottom(),18,18,paint);}
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1);paint.setColor(white(.10f*alpha));
            canvas.drawRoundRect(state.leftX(),state.leftY(),state.leftRight(),state.leftBottom(),18,18,paint);
            adaptiveLabels(canvas,alpha);
        }
        if((state.buttons()&NintendoControllerState.A)!=0){
            paint.setStyle(Paint.Style.FILL);paint.setColor(white(.10f*alpha));
            canvas.drawRoundRect(state.aLeft(),state.aTop(),state.aRight(),state.aBottom(),18,18,paint);
        }
        if(state.hintLayout()==NintendoControllerState.LAYOUT_ONE&&state.leftHeld()){
            // The single left key is transparent; the finger glow below shows it.
            paint.setStyle(Paint.Style.FILL);paint.setColor(white(.10f*alpha));
            canvas.drawRoundRect(state.leftX(),state.leftY(),state.leftRight(),state.leftBottom(),18,18,paint);
        }
    }
    private void adaptiveLabels(Canvas canvas,float alpha){
        int layout=state.hintLayout();float cx=(state.leftX()+state.leftRight())/2,cy=(state.leftY()+state.leftBottom())/2;
        if(layout==NintendoControllerState.LAYOUT_TWO){
            label(canvas,NintendoControllerState.labelFor(state.hintFirst()),(state.leftX()+cx)/2,cy,36,alpha*.28f);
            label(canvas,NintendoControllerState.labelFor(state.hintSecond()),(cx+state.leftRight())/2,cy,36,alpha*.28f);
            paint.setStyle(Paint.Style.FILL);paint.setColor(white(.10f*alpha));
            canvas.drawRect(cx-.5f,state.leftY()+16,cx+.5f,state.leftBottom()-16,paint);
        }else if(layout==NintendoControllerState.LAYOUT_DPAD){
            float w=state.leftRight()-state.leftX(),h=state.leftBottom()-state.leftY();
            label(canvas,"▲",state.leftX()+w/2,state.leftY()+h*.2f,30,alpha*.28f);
            label(canvas,"▼",state.leftX()+w/2,state.leftY()+h*.8f,30,alpha*.28f);
            label(canvas,"◀",state.leftX()+w*.2f,cy,30,alpha*.28f);
            label(canvas,"▶",state.leftX()+w*.8f,cy,30,alpha*.28f);
        }
    }
    /** The 35% dark disc Touchable puts behind every visible clear-glass control. */
    private void dimming(Canvas canvas,float alpha){
        paint.setStyle(Paint.Style.FILL);paint.setColor(Color.argb(Math.round(89*alpha),0,0,0));
        for(NintendoControllerState.Control c:state.controls())
            if(c.visible&&!c.faded)canvas.drawCircle(c.x,c.y,c.size/2,paint);
    }
    private boolean held(NintendoControllerState.Control c){return state.held(c);}
    private void animatePresses(){
        long now=System.nanoTime();
        if(lastFrameNanos==0){lastFrameNanos=now;return;}
        float dt=Math.min(.05f,(now-lastFrameNanos)/1e9f);lastFrameNanos=now;
        boolean moving=false;
        float step=Math.min(1f,dt*24f);
        for(NintendoControllerState.Control c:state.controls()){
            float target=held(c)?1.06f:1f,v=pressScale[c.id];
            if(Math.abs(target-v)>0.002f){v+=(target-v)*step;if(Math.abs(target-v)<0.002f)v=target;pressScale[c.id]=v;moving=true;}
        }
        if(moving)postInvalidateOnAnimation();
    }
    @Override protected void onDraw(Canvas canvas){
        super.onDraw(canvas);canvas.save();canvas.scale(density,density);
        boolean enabled=ready.getAsBoolean();
        float opacityScale=opacity/100f;
        float alpha=Math.min(1,opacityScale*(enabled?1f:.4f));
        animatePresses();
        if(alpha>0){
            zones(canvas,alpha);
            dimming(canvas,alpha);
            for(NintendoControllerState.Control c:state.controls()){
                if(!c.visible||c.kind==NintendoControllerState.KIND_A_REGION||c.kind==NintendoControllerState.KIND_LEFT_REGION)continue;
                float scale=pressScale[c.id];boolean pressed=held(c);
                float x=c.x,y=c.y,r=c.size/2*scale;
                float a=Math.min(1,alpha*(pressed?1f:Math.max(.85f,opacityScale)));
                if(c.faded){
                    paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1);paint.setColor(white(.25f*a));
                    canvas.drawCircle(x,y,r,paint);
                    if(c.kind==NintendoControllerState.KIND_DIRECTION)arrow(canvas,x,y,c.size*scale,c.mask,.30f*a);
                    else label(canvas,c.label,x,y,18,.30f*a);
                    continue;
                }
                glassCircle(canvas,x,y,c.size/2,a,pressed);
                if(c.kind==NintendoControllerState.KIND_DIRECTION)arrow(canvas,x,y,c.size*scale,c.mask,pressed?.72f*a:a);
                else label(canvas,c.label,x,y,c.kind==NintendoControllerState.KIND_BUTTON?18:20,pressed?.62f*a:a);
            }
        }
        drawFeedback(canvas,enabled?1f:.4f);
        paint.setAlpha(255);canvas.restore();
    }
    private void drawFeedback(Canvas canvas,float enabled){
        long now=SystemClock.uptimeMillis();boolean animating=false;
        for(java.util.Iterator<Feedback> it=feedback.values().iterator();it.hasNext();){
            Feedback f=it.next();float a=enabled;
            if(f.released>=0){long t=now-f.released;if(t>=160){it.remove();continue;}a*=1-t/160f;animating=true;}
            float radius=80;
            paint.setStyle(Paint.Style.FILL);
            paint.setShader(new RadialGradient(f.x,f.y,radius,
                    new int[]{white(.24f*a),white(.10f*a),white(0f)},
                    new float[]{0f,.42f,1f},Shader.TileMode.CLAMP));
            canvas.drawCircle(f.x,f.y,radius,paint);paint.setShader(null);
            float cx=Math.max(20,Math.min(f.x,getWidth()/density-20));
            float cy=Math.max(40,f.y-60);
            paint.setTextAlign(Paint.Align.CENTER);paint.setStyle(Paint.Style.FILL);
            paint.setTextSize(26*fontScale);
            paint.setColor(Color.argb(Math.round(.6f*a*255),0,0,0));
            float baseline=cy-(paint.ascent()+paint.descent())/2;
            canvas.drawText(f.label,cx+1,baseline+1,paint);
            paint.setColor(white(.9f*a));canvas.drawText(f.label,cx,baseline,paint);
        }
        if(animating)postInvalidateOnAnimation();
    }
    private String feedbackLabel(){
        int region=state.lastDownRegion();
        if(region==NintendoControllerState.KIND_A_REGION)return "A";
        if(region==NintendoControllerState.KIND_LEFT_REGION&&state.hintLayout()==NintendoControllerState.LAYOUT_ONE)
            return NintendoControllerState.labelFor(state.hintFirst());
        return null;
    }
    @Override public boolean onTouchEvent(MotionEvent event){
        if(!ready.getAsBoolean()){cancel();return true;}
        int action=event.getActionMasked(),index=event.getActionIndex();
        switch(action){
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:{
                int id=event.getPointerId(index);float x=event.getX(index)/density,y=event.getY(index)/density;
                state.down(id,x,y);
                String fb=state.owns(id)?feedbackLabel():null;
                if(fb!=null)feedback.put(id,new Feedback(fb,x,y));
                break;}
            case MotionEvent.ACTION_MOVE:
                for(int i=0;i<event.getPointerCount();i++){
                    int id=event.getPointerId(i);float x=event.getX(i)/density,y=event.getY(i)/density;
                    state.move(id,x,y);Feedback f=feedback.get(id);if(f!=null){f.x=x;f.y=y;}
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:{
                int id=event.getPointerId(index);state.up(id);
                Feedback f=feedback.get(id);if(f!=null)f.released=SystemClock.uptimeMillis();
                break;}
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
                for(NintendoControllerState.Control c:state.controls())if(c.visible)node.addChild(TouchControlsView.this,c.id);
                return node;
            }
            NintendoControllerState.Control c=state.control(id);if(c==null||!c.visible)return null;
            AccessibilityNodeInfo node=AccessibilityNodeInfo.obtain();node.setPackageName(getContext().getPackageName());
            node.setSource(TouchControlsView.this,id);node.setParent(TouchControlsView.this);
            node.setClassName("android.widget.Button");
            node.setContentDescription(c.description);node.setClickable(true);
            node.setFocusable(true);node.setAccessibilityFocused(accessibilityFocus==id);node.setSelected(state.held(c));
            node.setEnabled(ready.getAsBoolean());node.setVisibleToUser(isShown());
            float hw=c.size/2;
            Rect bounds=new Rect((int)((c.x-hw)*density),(int)((c.y-hw)*density),
                    (int)((c.x+hw)*density),(int)((c.y+hw)*density));
            node.setBoundsInParent(bounds);int[] location=new int[2];getLocationOnScreen(location);
            bounds.offset(location[0],location[1]);node.setBoundsInScreen(bounds);
            node.addAction(AccessibilityNodeInfo.ACTION_CLICK);
            if(c.mask==NintendoControllerState.A||c.mask==NintendoControllerState.B||c.mask==NintendoControllerState.Y)
                node.addAction(new AccessibilityNodeInfo.AccessibilityAction(AccessibilityNodeInfo.ACTION_LONG_CLICK,"切换按住或松开"));
            node.addAction(accessibilityFocus==id?AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS:AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS);
            return node;
        }
        @Override public boolean performAction(int id,int action,Bundle args){
            NintendoControllerState.Control c=state.control(id);if(c==null||!c.visible)return false;
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