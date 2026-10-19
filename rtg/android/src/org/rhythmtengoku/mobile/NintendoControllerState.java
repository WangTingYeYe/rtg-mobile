package org.rhythmtengoku.mobile;

import java.util.ArrayList;
import java.util.LinkedHashMap;

/** Game-specific touch regions publishing Nintendo Npad bits, without Android/JNI. */
final class NintendoControllerState {
    // Keep these bits identical to rtg/host/dm_input.h (nn::hid::NpadButton).
    static final long A=1L, B=1L<<1, X=1L<<2, Y=1L<<3;
    static final long PLUS=1L<<10, MINUS=1L<<11, L=1L<<6, R=1L<<7, ZL=1L<<8, ZR=1L<<9;
    static final long LEFT=1L<<12, UP=1L<<13, RIGHT=1L<<14, DOWN=1L<<15;
    static final int BUTTON=0, DIRECTION=1, A_REGION=2;
    static final class Control {
        final int id,kind;
        final String label,description;
        final long mask;
        final float x,y,halfWidth,halfHeight;
        Control(int id,int kind,String label,String description,long mask,float x,float y,float hw,float hh){
            this.id=id;this.kind=kind;this.label=label;this.description=description;this.mask=mask;
            this.x=x;this.y=y;halfWidth=hw;halfHeight=hh;
        }
        boolean contains(float px,float py){return Math.abs(px-x)<=halfWidth&&Math.abs(py-y)<=halfHeight;}
    }
    private static final class Pointer {
        final int kind;
        final Control origin;
        float x,y;
        Pointer(int kind,Control origin,float x,float y){this.kind=kind;this.origin=origin;this.x=x;this.y=y;}
    }
    private final LinkedHashMap<Integer,Pointer> pointers=new LinkedHashMap<>();
    private final ArrayList<Control> controls=new ArrayList<>();
    private long accessibilityButtons,buttons;
    private float left,right,top,bottom,split,padX,padY;
    private boolean extraButtons;

    /** dp geometry; the top utility strip never emits rhythm inputs. */
    void layout(float width,float height,float safeLeft,float safeTop,float safeRight,float safeBottom){
        clear();controls.clear();
        if(width<=0||height<=0)return;
        // Keep rhythm input edge-to-edge on the right, but place the direction
        // pad within the left thumb's reach, clear of the edge and gesture bar.
        // In immersive landscape Android still reports a top swipe inset;
        // it is not a visible status bar. Keep utilities aligned with Settings.
        left=4;right=width-4;top=4;bottom=height-4;
        split=(left+right)/2;
        float padMinX=Math.max(left+84,safeLeft+96),padMaxX=split-96;
        padX=Math.max(left+84,Math.min(padMaxX,Math.max(padMinX,width*.19f)));
        padY=Math.min(bottom-84-Math.max(24,safeBottom),Math.max(top+156,height*.67f));
        add(BUTTON,"B","B，返回或辅助操作；按下或按住，抬手松开",B,36,top+24,24,24);
        add(BUTTON,"Y","Y，按住查看示范，松开继续练习",Y,92,top+24,24,24);
        add(BUTTON,"−","减号，跳过练习",MINUS,148,top+24,24,24);
        add(BUTTON,"+","加号，游戏暂停菜单",PLUS,204,top+24,24,24);
        if(extraButtons){
            add(BUTTON,"L","L，切换页面",L,split-112,top+80,24,24);
            add(BUTTON,"ZL","ZL，玩具模式辅助操作",ZL,split-56,top+80,24,24);
            add(BUTTON,"X","X，模式辅助操作",X,split,top+80,24,24);
            add(BUTTON,"ZR","ZR，打鼓课程敲钹",ZR,split+56,top+80,24,24);
            add(BUTTON,"R","R，切换页面",R,split+112,top+80,24,24);
        }
        add(DIRECTION,"↑","方向键，上",UP,padX,padY-56,24,24);
        add(DIRECTION,"←","方向键，左",LEFT,padX-56,padY,24,24);
        add(DIRECTION,"→","方向键，右",RIGHT,padX+56,padY,24,24);
        add(DIRECTION,"↓","方向键，下",DOWN,padX,padY+56,24,24);
        add(A_REGION,"A","右半屏 A，按下或按住，抬手松开",A,(split+right)/2,(top+56+bottom)/2,(right-split)/2,(bottom-top-56)/2);
    }
    private void add(int kind,String label,String description,long mask,float x,float y,float hw,float hh){
        controls.add(new Control(controls.size(),kind,label,description,mask,x,y,hw,hh));
    }
    ArrayList<Control> controls(){return controls;}
    void setExtraButtons(boolean show){extraButtons=show;}
    Control control(int id){return id>=0&&id<controls.size()?controls.get(id):null;}
    Control button(long mask){for(Control c:controls)if(c.mask==mask)return c;return null;}
    float split(){return split;} float padX(){return padX;} float padY(){return padY;}
    float contentTop(){return top+56;} float contentBottom(){return bottom;}
    float faceX(Control c){return c.x;}
    float faceY(Control c){return c.kind==A_REGION?contentTop()+(bottom-contentTop())*.52f:c.y;}
    private boolean inPlayArea(float x,float y){return !controls.isEmpty()&&x>=left&&x<=right&&y>=contentTop()&&y<=bottom;}
    private boolean inDirectionArea(float x,float y){return inPlayArea(x,y)&&Math.abs(x-padX)<=84&&Math.abs(y-padY)<=84;}
    private Control direction(float x,float y){
        if(!inDirectionArea(x,y))return null;
        float dx=x-padX,dy=y-padY;
        // A small neutral centre lets a thumb rest without holding a direction.
        if(dx*dx+dy*dy<20*20)return null;
        return button(Math.abs(dx)>Math.abs(dy)?(dx<0?LEFT:RIGHT):(dy<0?UP:DOWN));
    }
    Control hit(float x,float y){
        for(Control c:controls)if(c.kind==BUTTON&&c.contains(x,y))return c;
        if(!inPlayArea(x,y))return null;
        if(inDirectionArea(x,y))return direction(x,y);
        return x>=split?button(A):null;
    }
    void down(int pointerId,float x,float y){
        if(pointers.containsKey(pointerId)||pointers.size()>=16)return;
        Control c=hit(x,y);
        // Capture the original action at DOWN. The direction pad owns its
        // whole square, including neutral/gaps; blank space never starts an action.
        int kind=c!=null?c.kind:inDirectionArea(x,y)?DIRECTION:-1;
        pointers.put(pointerId,new Pointer(kind,c,x,y));recompute();
    }
    void move(int pointerId,float x,float y){
        Pointer p=pointers.get(pointerId);if(p!=null){p.x=x;p.y=y;recompute();}
    }
    void up(int pointerId){pointers.remove(pointerId);recompute();}
    void accessibilityButton(int id,boolean down){
        Control c=control(id);if(c==null)return;
        if(down)accessibilityButtons|=c.mask;else accessibilityButtons&=~c.mask;
        recompute();
    }
    void clear(){pointers.clear();accessibilityButtons=buttons=0;}
    private void recompute(){
        buttons=accessibilityButtons;
        for(Pointer p:pointers.values()){
            // A and the dedicated B button keep their DOWN owner through drift.
            if(p.kind==A_REGION||(p.kind==BUTTON&&p.origin.mask==B))buttons|=p.origin.mask;
            else if(p.kind==DIRECTION){Control c=direction(p.x,p.y);if(c!=null)buttons|=c.mask;}
            else if(p.kind==BUTTON&&p.origin.contains(p.x,p.y))buttons|=p.origin.mask;
        }
    }
    long buttons(){return buttons;}
    boolean held(Control c){return (buttons&c.mask)!=0;}
}
