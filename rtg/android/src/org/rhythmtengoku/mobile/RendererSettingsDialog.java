package org.rhythmtengoku.mobile;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.res.ColorStateList;
import android.graphics.drawable.ColorDrawable;
import android.view.View;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.SeekBar;
import android.widget.TextView;
import java.io.IOException;

/** Native system-menu surface; the platform owns focus, Back and switch semantics. */
final class RendererSettingsDialog {
    private final Activity activity;
    private final RendererOptionsStore store;
    private final Switch clear,writeback;
    private final TextView status;
    private final AlertDialog dialog;
    private RendererOptions desired;
    private boolean binding,readFailed;
    private String error;
    private int applied;
    private RendererSettingsDialog(Activity activity,int applied,TouchControlsView touch,View trigger,Runnable closed){
        this.activity=activity;this.applied=applied;store=new RendererOptionsStore(activity);
        LinearLayout body=new LinearLayout(activity);body.setOrientation(LinearLayout.VERTICAL);body.setPadding(dp(18),dp(8),dp(18),dp(12));
        text(body,"触控",18);
        text(body,"左侧方向盘，右半屏 A；按住持续输入，抬手松开。左上独立 B 键用于返回或辅助操作；按住 Y 看示范，− 跳过练习，+ 暂停。方向盘以外的左侧空白不触发按键。",14);
        TextView opacityLabel=text(body,"按键不透明度："+touch.opacity()+"%",16);
        SeekBar opacity=new SeekBar(activity);opacity.setMax(100);opacity.setProgress(touch.opacity());opacity.setMinHeight(dp(48));
        opacity.setContentDescription("按键不透明度，0 为隐藏，100 为完全显示");
        body.addView(opacity,new LinearLayout.LayoutParams(-1,dp(48)));
        text(body,"只改变按键显示，不改变触控范围。0% 隐藏轮廓，按下时仍有反馈。设置入口始终可见。",13);
        opacity.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
            @Override public void onProgressChanged(SeekBar bar,int value,boolean fromUser){if(fromUser){touch.setOpacity(value);opacityLabel.setText("按键不透明度："+value+"%");}}
            @Override public void onStartTrackingTouch(SeekBar bar){}
            @Override public void onStopTrackingTouch(SeekBar bar){}
        });
        Switch extras=option(body,"显示额外按键","展开 X、L、R、ZL、ZR，供打鼓课程、玩具和音乐等模式使用。");
        extras.setChecked(touch.extraButtons());extras.setOnCheckedChangeListener((v,on)->touch.setExtraButtons(on));
        text(body,"图形优化",18);
        clear=option(body,"清屏合并","将符合条件的整屏清除并入绘制，减少独立清除和显存读取。关闭后恢复原清除路径。");
        writeback=option(body,"省略无用写回","确认内容在读取前会被完整覆盖时，省掉这次显存保存。需要保留的内容仍正常保存。");
        text(body,"运行中生效并自动保存，可在同一场景往返对比。收益随场景变化，不保证提高帧率；若画面异常，可关闭对应项。",14);
        status=text(body,"正在读取设置…",14);status.setMinHeight(dp(48));status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        ScrollView scroll=new ScrollView(activity);scroll.setFillViewport(false);scroll.addView(body);
        dialog=new AlertDialog.Builder(activity).setTitle("系统菜单").setView(scroll).setPositiveButton("返回游戏",null).create();
        dialog.setOnDismissListener(d->{closed.run();if(!activity.isFinishing())trigger.requestFocus();});
        clear.setOnCheckedChangeListener((v,on)->change(RendererOptions.CLEAR_MERGE,on));
        writeback.setOnCheckedChangeListener((v,on)->change(RendererOptions.STORE_ELIDE,on));
        reload();
    }
    static RendererSettingsDialog show(Activity activity,int applied,TouchControlsView touch,View trigger,Runnable closed){
        RendererSettingsDialog panel=new RendererSettingsDialog(activity,applied,touch,trigger,closed);panel.dialog.show();
        panel.dialog.getWindow().setBackgroundDrawable(new ColorDrawable(0xff000000|TouchControlsView.INK));
        panel.dialog.getWindow().setLayout(Math.min(panel.dp(600),activity.getResources().getDisplayMetrics().widthPixels-panel.dp(36)),WindowManager.LayoutParams.WRAP_CONTENT);
        panel.dialog.getButton(AlertDialog.BUTTON_POSITIVE).setTextColor(TouchControlsView.TEXT);return panel;
    }
    private int dp(int n){return Math.round(n*activity.getResources().getDisplayMetrics().density);}
    private TextView text(LinearLayout body,String value,int size){
        TextView view=new TextView(activity);view.setText(value);view.setTextColor(TouchControlsView.TEXT);view.setTextSize(size);
        view.setPadding(0,dp(4),0,dp(8));body.addView(view,new LinearLayout.LayoutParams(-1,-2));return view;
    }
    private Switch option(LinearLayout body,String title,String detail){
        Switch control=new Switch(activity);control.setText(title);control.setTextColor(TouchControlsView.TEXT);control.setTextSize(17);
        control.setMinHeight(dp(48));control.setSwitchPadding(dp(18));control.setThumbTintList(ColorStateList.valueOf(TouchControlsView.TEXT));
        control.setTrackTintList(new ColorStateList(new int[][]{new int[]{android.R.attr.state_checked},new int[]{}},new int[]{TouchControlsView.PRESSED,TouchControlsView.RIM}));
        body.addView(control,new LinearLayout.LayoutParams(-1,-2));text(body,detail,14);return control;
    }
    private void reload(){
        try{desired=store.read();readFailed=false;error=null;bind();}
        catch(IOException e){readFailed=true;error="设置读取失败，返回游戏后重新打开菜单重试。";clear.setEnabled(false);writeback.setEnabled(false);feedback();}
    }
    private void bind(){
        binding=true;clear.setChecked(desired.enabled(RendererOptions.CLEAR_MERGE));writeback.setChecked(desired.enabled(RendererOptions.STORE_ELIDE));binding=false;feedback();
    }
    private void change(int bit,boolean enabled){
        if(binding||readFailed)return;
        try{desired=store.change(bit,enabled);error=null;bind();}
        catch(IOException e){error="保存失败，设置未改变。请重新切换重试。";bind();}
    }
    void updateApplied(int flags){
        applied=flags;
        if(!readFailed)try{RendererOptions latest=store.read();if(desired==null||latest.sequence!=desired.sequence){desired=latest;error=null;bind();}}
        catch(IOException e){error="设置暂时无法读取，请返回游戏后重试。";}
        feedback();
    }
    private void feedback(){
        String value=error!=null?error:applied==desired.flags?"已生效："+RendererOptions.description(applied):"正在应用…\n当前生效："+RendererOptions.description(applied);
        if(!value.contentEquals(status.getText()))status.setText(value);
    }
    void dismiss(){dialog.dismiss();}
}
