package org.rhythmtengoku.mobile;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.Choreographer;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import org.json.JSONArray;
import org.json.JSONObject;

public final class GameActivity extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("rtg_mobile_android"); }
    private static native int nativeConfigure(String files, String locale,boolean touchRoute);
    private static native int nativeActions(long buttons,int lx,int ly,int rx,int ry);
    private static native void nativeCancelInput();
    private static native int nativeControllerButtons(long buttons);
    private static native void nativeControllerCancel(long remainingButtons);
    private static native int nativeSurface(Surface surface,int width,int height,float density,float hz,int focus);
    private static native int nativeStart();
    private static native void nativeVsync(long frameTimeNanos);
    private static native String nativeStatus();
    private static native long nativeTouchHint();
    private static native String nativeKeyboardRequest();
    private static native void nativeKeyboardResult(long id,boolean accepted,String text);
    private static native void nativeKeyboardReady(boolean ready);
    private android.app.AlertDialog keyboardDialog;
    private long keyboardId;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final HandlerThread surfaceThread=new HandlerThread("game-surface");
    private Handler surfaceWorker;
    private boolean nativeLaunched, destroyed;
    private volatile boolean inputReady;
    private volatile int surfaceGeneration;
    private SurfaceView game;
    private TextView status,fps;
    private final GameDisplayMetrics.FpsSampler fpsSampler=new GameDisplayMetrics.FpsSampler();
    private double displayedFps=Double.NaN;
    private boolean showFps;
    private final Choreographer.FrameCallback vsyncCallback=new Choreographer.FrameCallback(){
        @Override public void doFrame(long frameTimeNanos){
            if(!resumed||destroyed||failed)return;
            nativeVsync(frameTimeNanos);
            if(touchControls!=null)touchControls.applyHint(nativeTouchHint());
            Choreographer.getInstance().postFrameCallback(this);
        }
    };
    private RendererSettingsDialog rendererSettings;
    private int appliedRendererFlags=RendererOptions.DEFAULT_FLAGS;
    private TouchControlsView touchControls;
    private GameHaptics haptics;
    private boolean prepared, configured, surfaceReady, launched, resumed, failed;
    private int width,height;
    private final BackButtonState backButtons=new BackButtonState();
    private android.hardware.input.InputManager inputManager;
    private final android.hardware.input.InputManager.InputDeviceListener deviceListener=new android.hardware.input.InputManager.InputDeviceListener(){
        @Override public void onInputDeviceAdded(int id){}
        @Override public void onInputDeviceChanged(int id){}
        @Override public void onInputDeviceRemoved(int id){if(backButtons.removeDevice(id)&&configured)nativeControllerCancel(backButtons.buttons());}
    };
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        haptics=new GameHaptics(this);
        inputManager=(android.hardware.input.InputManager)getSystemService(INPUT_SERVICE);
        if(inputManager!=null)inputManager.registerInputDeviceListener(deviceListener,ui);
        surfaceThread.start();surfaceWorker=new Handler(surfaceThread.getLooper());
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        WindowManager.LayoutParams windowParams=getWindow().getAttributes();
        android.view.Display display=getWindowManager().getDefaultDisplay();
        android.view.Display.Mode currentMode=display.getMode();
        android.view.Display.Mode[] supported=display.getSupportedModes();
        DisplayRefreshPolicy.Mode[] candidates=new DisplayRefreshPolicy.Mode[supported.length];
        for(int i=0;i<supported.length;i++){
            android.view.Display.Mode mode=supported[i];
            candidates[i]=new DisplayRefreshPolicy.Mode(mode.getModeId(),mode.getPhysicalWidth(),mode.getPhysicalHeight(),mode.getRefreshRate());
        }
        DisplayRefreshPolicy.Mode chosen=DisplayRefreshPolicy.select(new DisplayRefreshPolicy.Mode(currentMode.getModeId(),
            currentMode.getPhysicalWidth(),currentMode.getPhysicalHeight(),currentMode.getRefreshRate()),candidates);
        windowParams.preferredRefreshRate=chosen.hz;windowParams.preferredDisplayModeId=chosen.id;
        windowParams.layoutInDisplayCutoutMode=WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(windowParams);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY|View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        FrameLayout root=new FrameLayout(this);root.setBackgroundColor(0xff0d0d0f);
        GameViewport viewport=new GameViewport(this);
        root.addView(viewport,new FrameLayout.LayoutParams(-1,-1,Gravity.CENTER));
        game=new SurfaceView(this);game.setContentDescription("游戏画面");game.getHolder().addCallback(this);
        viewport.addView(game,new FrameLayout.LayoutParams(-1,-1));
        touchControls=new TouchControlsView(this,
            () -> inputReady&&resumed&&!failed&&rendererSettings==null&&keyboardDialog==null,
            (buttons,lx,ly,rx,ry) -> {
                if(!configured)return;
                int rc=nativeActions(buttons,lx,ly,rx,ry);
                if(rc!=0&&inputReady&&resumed&&!failed)fail("手柄输入不可用",new Exception("错误 "+rc));
            },() -> {backButtons.clear();if(configured)nativeCancelInput();});
        // Use the full screen for rhythm input, including the side gutters;
        // the original game image stays in its unchanged 16:9 viewport.
        root.addView(touchControls,new FrameLayout.LayoutParams(-1,-1));
        showFps=new File(getFilesDir(),"rhythm-tengoku/diagnostics/show-fps").exists();
        fps=new TextView(this);fps.setText("FPS —");fps.setTextColor(TouchControlsView.TEXT);
        fps.setVisibility(showFps?View.VISIBLE:View.GONE);
        fps.setTextSize(12);fps.setTypeface(Typeface.MONOSPACE);fps.setGravity(Gravity.CENTER);
        fps.setMinWidth(dp(84));fps.setPadding(dp(8),dp(4),dp(8),dp(4));fps.setBackgroundColor(TouchControlsView.INK);
        fps.setClickable(false);fps.setFocusable(false);
        FrameLayout.LayoutParams fpsParams=new FrameLayout.LayoutParams(-2,-2,Gravity.BOTTOM|Gravity.CENTER_HORIZONTAL);
        fpsParams.bottomMargin=dp(8);root.addView(fps,fpsParams);
        root.setOnApplyWindowInsetsListener((v,insets)->{
            android.graphics.Insets safe=TouchControlsView.safeInsets(insets);
            fpsParams.bottomMargin=dp(8)+safe.bottom;fps.setLayoutParams(fpsParams);
            fps.setTranslationX((safe.left-safe.right)/2f);
            return insets;
        });
        status=new TextView(this);status.setTextColor(0xffe9d9ba);status.setTextSize(18);
        status.setGravity(Gravity.CENTER);status.setBackgroundColor(0xe60d0d0f);
        status.setPadding(dp(28),dp(18),dp(28),dp(18));status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        status.setText("正在准备游戏文件");root.addView(status,new FrameLayout.LayoutParams(-1,-1));setContentView(root);
        new Thread(() -> {
            try { prepareModules();ui.post(() -> {prepared=true;configureAndStart();}); }
            catch(Exception e){ui.post(() -> fail("游戏文件准备失败",e));}
        },"game-files").start();
    }
    private int dp(int value){return Math.round(value*getResources().getDisplayMetrics().density);}
    private static String digest(File file)throws Exception {
        MessageDigest md=MessageDigest.getInstance("SHA-256");byte[] bytes=new byte[65536];
        try(InputStream in=new FileInputStream(file)){int n;while((n=in.read(bytes))!=-1)md.update(bytes,0,n);}
        StringBuilder text=new StringBuilder();for(byte b:md.digest())text.append(String.format("%02x",b&255));return text.toString();
    }
    private void prepareModules()throws Exception {
        String json;
        try(InputStream in=getAssets().open("module-manifest.json")){
            java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream();byte[] b=new byte[4096];int n;
            while((n=in.read(b))!=-1)out.write(b,0,n);json=new String(out.toByteArray(),StandardCharsets.UTF_8);
        }
        JSONArray list=new JSONObject(json).getJSONArray("files");File directory=new File(getFilesDir(),"rhythm-tengoku/modules");
        if(!directory.isDirectory()&&!directory.mkdirs())throw new java.io.IOException("不能创建游戏目录");
        for(int i=0;i<list.length();i++){
            JSONObject entry=list.getJSONObject(i);String name=entry.getString("name"),hash=entry.getString("sha256");
            if(!name.matches("(main|sdk|subsdk0)\\.(patched\\.bin|fixups\\.tsv|gates\\.tsv)"))throw new java.io.IOException("模块清单无效");
            File target=new File(directory,name);
            if(target.isFile()&&target.length()==entry.getLong("bytes")&&digest(target).equals(hash))continue;
            File temporary=new File(directory,name+".new");
            try(InputStream in=getAssets().open("modules/"+name);FileOutputStream out=new FileOutputStream(temporary)){
                byte[] b=new byte[65536];int n;while((n=in.read(b))!=-1)out.write(b,0,n);out.getFD().sync();
            }
            if(temporary.length()!=entry.getLong("bytes")||!digest(temporary).equals(hash))throw new java.io.IOException("游戏模块校验失败");
            if(!temporary.renameTo(target))throw new java.io.IOException("不能保存游戏模块");
        }
        /* v63：共享字体资产——设备无 TrueType 轮廓 CJK 字体（NotoSansCJK 是 OTTO/CFF，SDK
         * 解析器不认；DroidSansFallback 不存在）。随包带 Mac 验证过的 Arial Unicode.ttf。 */
        {
            File fontDir=new File(getFilesDir(),"rhythm-tengoku/fonts");
            if(!fontDir.isDirectory()&&!fontDir.mkdirs())throw new java.io.IOException("不能创建字体目录");
            String fontJson;
            try(InputStream in=getAssets().open("font-manifest.json")){
                java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream();
                byte[] bytes=new byte[4096];int n;while((n=in.read(bytes))!=-1)out.write(bytes,0,n);
                fontJson=new String(out.toByteArray(),StandardCharsets.UTF_8);
            }
            JSONObject fontInfo=new JSONObject(fontJson);
            long fontBytes=fontInfo.getLong("bytes");String fontHash=fontInfo.getString("sha256");
            File target=new File(fontDir,"ArialUnicode.ttf");
            if(!target.isFile()||target.length()!=fontBytes||!digest(target).equals(fontHash)){
                File temporary=new File(fontDir,"ArialUnicode.ttf.new");
                try(InputStream in=getAssets().open("fonts/ArialUnicode.ttf");FileOutputStream out=new FileOutputStream(temporary)){
                    byte[] b=new byte[65536];int n;while((n=in.read(b))!=-1)out.write(b,0,n);out.getFD().sync();
                }
                if(temporary.length()!=fontBytes||!digest(temporary).equals(fontHash))throw new java.io.IOException("共享字体校验失败");
                if(!temporary.renameTo(target))throw new java.io.IOException("不能保存共享字体");
            }
        }
        prepareShaders();
    }
    private void prepareShaders()throws Exception {
        String json;
        try(InputStream in=getAssets().open("shader-manifest.json")){
            java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream();
            byte[] bytes=new byte[4096];int n;
            while((n=in.read(bytes))!=-1)out.write(bytes,0,n);
            json=new String(out.toByteArray(),StandardCharsets.UTF_8);
        }
        JSONArray entries=new JSONObject(json).getJSONArray("files");
        File directory=new File(getFilesDir(),"rhythm-tengoku/shaders");
        if(!directory.isDirectory()&&!directory.mkdirs())throw new java.io.IOException("不能创建图形资源目录");
        boolean hasIndex=false;
        java.util.HashSet<String> names=new java.util.HashSet<>();
        for(int i=0;i<entries.length();i++){
            JSONObject entry=entries.getJSONObject(i);
            String name=entry.getString("name"),hash=entry.getString("sha256");
            if(!name.matches("shader_index\\.bin|[0-9a-f]{16}\\.(spv|vkmeta)")||!hash.matches("[0-9a-f]{64}")||!names.add(name))
                throw new java.io.IOException("图形资源清单无效");
            hasIndex|=name.equals("shader_index.bin");
            long size=entry.getLong("bytes");
            if(size<=0||size>64L*1024*1024)throw new java.io.IOException("图形资源大小无效");
            File target=new File(directory,name);
            if(target.isFile()&&target.length()==size&&digest(target).equals(hash))continue;
            File temporary=new File(directory,name+".new");
            try(InputStream in=getAssets().open("shaders/"+name);FileOutputStream out=new FileOutputStream(temporary)){
                byte[] bytes=new byte[65536];int n;long copied=0;
                while((n=in.read(bytes))!=-1){copied+=n;if(copied>size)throw new java.io.IOException("图形资源长度不符");out.write(bytes,0,n);}
                out.getFD().sync();
            }
            if(temporary.length()!=size||!digest(temporary).equals(hash))throw new java.io.IOException("图形资源校验失败");
            if(!temporary.renameTo(target))throw new java.io.IOException("不能保存图形资源");
        }
        if(!hasIndex)throw new java.io.IOException("缺少图形资源索引");
        android.util.Log.i("RhythmTengoku","ANDROID_SHADER_CACHE_READY files="+entries.length());
    }
    private void configureAndStart(){
        if(!prepared||failed||destroyed)return;
        if(!configured){int rc=nativeConfigure(getFilesDir().getAbsolutePath(),getResources().getConfiguration().getLocales().get(0).toLanguageTag(),
            getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_TOUCHSCREEN));
            if(rc!=0){fail("游戏初始化失败",new Exception("错误 "+rc));return;}configured=true;}
        if(surfaceReady)publishSurface();
    }
    private void publishSurface(){
        if(!configured)return;
        final Surface surface=surfaceReady?game.getHolder().getSurface():null;
        // Content remains 60 FPS; the window may use a faster panel mode for
        // Android input/UI. Reporting a display request is not a game FPS gain.
        if(surface!=null&&surface.isValid()&&android.os.Build.VERSION.SDK_INT>=30)surface.setFrameRate(60f,Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
        final int w=width,h=height,focus=resumed?1:3;
        final float density=getResources().getDisplayMetrics().density,hz=getWindowManager().getDefaultDisplay().getRefreshRate();
        final boolean start=surfaceReady&&resumed&&!destroyed;
        final int generation=++surfaceGeneration;
        if(touchControls!=null)touchControls.cancel();
        inputReady=false;
        // Draining an in-flight GPU frame can take seconds. Serialize Surface
        // replacement and startup off the Android lifecycle/UI thread.
        surfaceWorker.post(() -> {
            if(generation!=surfaceGeneration)return;
            int rc=nativeSurface(surface,w,h,density,hz,focus);
            if(generation!=surfaceGeneration)return;
            if(rc!=0){final int error=rc;ui.post(() -> {if(!destroyed&&generation==surfaceGeneration)fail("游戏窗口不可用",new Exception("错误 "+error));});return;}
            inputReady=start;
            ui.post(() -> {if(!destroyed)touchControls.postInvalidateOnAnimation();});
            if(start&&!nativeLaunched){rc=nativeStart();
                if(rc!=0){final int error=rc;ui.post(() -> {if(!destroyed)fail("游戏启动失败",new Exception("错误 "+error));});return;}
                nativeLaunched=true;ui.post(() -> {if(!destroyed&&!launched){launched=true;ui.post(poll);}});
            }
        });
    }
    private void closeRendererSettings(){if(rendererSettings!=null){rendererSettings.dismiss();rendererSettings=null;}}
    private boolean canSendBack(){return configured&&resumed&&inputReady&&!failed&&!destroyed;}
    private void clearBackButtons(){backButtons.clear();if(configured)nativeControllerCancel(0);}
    private void publishBackButtons(){
        if(configured){int rc=nativeControllerButtons(backButtons.buttons());
            if(rc!=0)android.util.Log.w("RhythmInput","Back button publication unavailable: "+rc);}
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event){
        int key=event.getKeyCode();
        if(!BackButtonState.handles(key))return super.dispatchKeyEvent(event);
        if(rendererSettings!=null||keyboardDialog!=null){
            if(event.getAction()==KeyEvent.ACTION_UP&&!event.isCanceled()){
                if(rendererSettings!=null){closeRendererSettings();return true;}
                if(keyboardDialog!=null){keyboardDialog.dismiss();return true;}
            }
            return true;
        }
        if(event.getAction()==KeyEvent.ACTION_DOWN||event.getAction()==KeyEvent.ACTION_UP){
            if(event.isCanceled()){
                if(backButtons.cancel(event.getDeviceId(),key)&&configured)nativeControllerCancel(backButtons.buttons());
                return true;
            }
            boolean down=event.getAction()==KeyEvent.ACTION_DOWN&&canSendBack();
            backButtons.update(event.getDeviceId(),key,down);
            publishBackButtons();
        }
        return true;
    }
    private void fail(String title,Exception error){
        failed=true;if(touchControls!=null)touchControls.cancel();clearBackButtons();
        closeRendererSettings();nativeKeyboardReady(false);
        if(keyboardDialog!=null){keyboardDialog.dismiss();keyboardDialog=null;}
        status.setVisibility(View.VISIBLE);status.setText(title+"\n"+error.getMessage());
    }
    private final Runnable poll=new Runnable(){public void run(){
        if(!launched||failed)return;
        try{String raw=nativeStatus();JSONObject s=new JSONObject(raw);
            appliedRendererFlags=s.optInt("renderer_flags",RendererOptions.DEFAULT_FLAGS);
            if(rendererSettings!=null)rendererSettings.updateApplied(appliedRendererFlags);
            long frame=s.getLong("presented_frames");
            if(frame>0)status.setVisibility(View.GONE);else status.setText(s.getString("phase"));
            if(resumed&&showFps){double current=fpsSampler.sample(frame,SystemClock.elapsedRealtime());
                if(!Double.isNaN(current)&&Double.compare(current,displayedFps)!=0){
                    fps.setText(String.format(getResources().getConfiguration().getLocales().get(0),"FPS %.1f",current));
                    displayedFps=current;
                }}
            String request=nativeKeyboardRequest();if(request!=null&&resumed&&!destroyed){JSONObject k=new JSONObject(request);long id=k.getLong("id");
                if(keyboardDialog==null||keyboardId!=id){closeRendererSettings();if(keyboardDialog!=null)keyboardDialog.dismiss();keyboardId=id;touchControls.cancel();clearBackButtons();
                    keyboardDialog=ShellTextInputDialog.show(GameActivity.this,k.getString("title"),k.getString("subtitle"),k.getString("hint"),k.getString("initial"),k.getInt("min"),k.getInt("max"),(ok,text)->{nativeKeyboardResult(id,ok,text);keyboardDialog=null;});}}
        }catch(Exception e){fail("无法读取游戏状态",e);}ui.postDelayed(this,500);
    }};
    @Override public void onResume(){super.onResume();resumed=true;if(haptics!=null)haptics.setActive(true);fpsSampler.reset();nativeKeyboardReady(true);
        Choreographer.getInstance().removeFrameCallback(vsyncCallback);Choreographer.getInstance().postFrameCallback(vsyncCallback);configureAndStart();}
    @Override public void onPause(){
        if(haptics!=null)haptics.setActive(false);
        Choreographer.getInstance().removeFrameCallback(vsyncCallback);nativeVsync(0);
        closeRendererSettings();nativeKeyboardReady(false);
        if(keyboardDialog!=null){keyboardDialog.dismiss();keyboardDialog=null;}
        resumed=false;fpsSampler.reset();displayedFps=Double.NaN;if(fps!=null)fps.setText("FPS —");
        if(touchControls!=null)touchControls.cancel();clearBackButtons();inputReady=false;
        if(configured&&surfaceReady)publishSurface();super.onPause();
    }
    @Override public void onWindowFocusChanged(boolean hasFocus){
        if(haptics!=null)haptics.setActive(hasFocus&&resumed&&!destroyed);
        super.onWindowFocusChanged(hasFocus);if(!hasFocus){if(touchControls!=null)touchControls.cancel();clearBackButtons();}
    }
    @Override public void surfaceCreated(SurfaceHolder holder){if(!destroyed)surfaceReady=true;}
    @Override public void surfaceChanged(SurfaceHolder holder,int format,int w,int h){if(destroyed)return;width=w;height=h;surfaceReady=true;configureAndStart();}
    @Override public void surfaceDestroyed(SurfaceHolder holder){if(destroyed)return;surfaceReady=false;if(configured)publishSurface();}
    @Override public void onDestroy(){if(haptics!=null)haptics.close();Choreographer.getInstance().removeFrameCallback(vsyncCallback);nativeVsync(0);closeRendererSettings();nativeKeyboardReady(false);if(keyboardDialog!=null){keyboardDialog.dismiss();keyboardDialog=null;}destroyed=true;if(touchControls!=null)touchControls.cancel();clearBackButtons();if(inputManager!=null)inputManager.unregisterInputDeviceListener(deviceListener);inputReady=false;surfaceReady=false;if(configured)publishSurface();ui.removeCallbacks(poll);surfaceThread.quitSafely();super.onDestroy();}
}
