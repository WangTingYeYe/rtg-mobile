package org.rhythmtengoku.mobile;

/** JVM checks for the Touchable rhythm layout, adaptive scene hints, and pointer ownership. */
public final class NintendoControllerStateTest {
    private static int checks;
    private static void check(boolean value,String what){checks++;if(!value)throw new AssertionError(what);}
    private static NintendoControllerState fresh(int layout,int first,int second,int extra){
        NintendoControllerState s=new NintendoControllerState();
        s.layout(837,388,0,0,0,0);s.setHint(layout,first,second,extra);return s;
    }
    private static void center(NintendoControllerState s,NintendoControllerState.Control c){s.down(1,c.x,c.y);}
    private static NintendoControllerState.Control find(NintendoControllerState s,long mask){NintendoControllerState.Control c=s.button(mask);check(c!=null,"control "+mask);return c;}

    private static void geometry(){
        float[][] screens={{837,388},{750,333},{568,320},{960,540},{1024,768},{667,375},{1280,800}};
        for(float[] screen:screens)for(int layout=0;layout<4;layout++){
            NintendoControllerState s=new NintendoControllerState();s.layout(screen[0],screen[1],0,0,0,0);s.setHint(layout,0,0,0);
            for(NintendoControllerState.Control c:s.controls()){
                check(c.x-c.size/2>=-0.01f&&c.x+c.size/2<=screen[0]+0.01f,"horizontal bounds "+c.label+" "+screen[0]);
                check(c.y-c.size/2>=-0.01f&&c.y+c.size/2<=screen[1]+0.01f,"vertical bounds "+c.label+" "+screen[1]);
                check(c.size>=44,"touch target at least 44dp for "+c.label);
            }
            NintendoControllerState.Control b=find(s,NintendoControllerState.B),y=find(s,NintendoControllerState.Y),x=find(s,NintendoControllerState.X);
            check(b.visible&&find(s,NintendoControllerState.MINUS).visible&&find(s,NintendoControllerState.PLUS).visible,"B - + always visible");
            check(!y.visible&&!x.visible,"Y and X wait for the scene hint");
            for(NintendoControllerState.Control c:s.controls())
                if(c.kind==NintendoControllerState.KIND_DIRECTION)check(c.visible==(layout==NintendoControllerState.LAYOUT_MENU||layout==NintendoControllerState.LAYOUT_DPAD),"small D-pad follows layout");
            check(s.aLeft()<s.aRight()&&s.aTop()<s.aBottom(),"A area exists");
            check(s.leftX()<s.leftRight()&&s.leftY()<s.leftBottom(),"left area exists");
            check(s.aLeft()>=s.contentLeft(),"A starts in the picture");
        }
        // A wide phone puts B, -, +, Y and X in the side letterbox bars. A 4:3
        // screen has top/bottom bars, so the side controls move to the top edge.
        NintendoControllerState wide=new NintendoControllerState();wide.layout(837,388,0,0,0,0);wide.setHint(NintendoControllerState.LAYOUT_DPAD,0,0,0);
        check(!wide.verticalBars(),"20:9 landscape keeps the picture full height");
        check(wide.button(NintendoControllerState.B).x<wide.contentLeft(),"B sits in the left letterbox bar");
        NintendoControllerState tall=new NintendoControllerState();tall.layout(1024,768,0,0,0,0);tall.setHint(NintendoControllerState.LAYOUT_DPAD,0,0,0);
        check(tall.verticalBars(),"4:3 has top and bottom bars");
        check(tall.button(NintendoControllerState.B).y<tall.contentTop(),"B moves to the top bar");
        NintendoControllerState exact=new NintendoControllerState();exact.layout(1920,1080,0,0,0,0);
        check(!exact.verticalBars(),"16:9 has no bars");
    }
    private static void adaptive(){
        NintendoControllerState one=fresh(NintendoControllerState.LAYOUT_ONE,0,0,0);
        one.down(3,(one.leftX()+one.leftRight())/2,one.leftY()+30);
        check(one.buttons()==NintendoControllerState.A,"ONE presses A across the left half");
        check(one.leftHeld(),"ONE reports the adaptive zone held");
        one.move(3,one.leftRight()-2,one.leftBottom()-2);check(one.buttons()==NintendoControllerState.A,"ONE keeps A");
        one.up(3);check(one.buttons()==0,"ONE releases");
        check(!one.leftHeld(),"ONE clears the adaptive hold");

        NintendoControllerState two=fresh(NintendoControllerState.LAYOUT_TWO,1,12,0);
        two.down(4,one.leftX()+20,one.leftY()+40);check(two.buttons()==NintendoControllerState.B,"TWO left is B");
        check(two.leftHeld(),"TWO adaptive zone is held");
        two.move(4,one.leftRight()-20,one.leftY()+40);check(two.buttons()==NintendoControllerState.LEFT,"TWO right is left");
        two.up(4);
        two.down(40,two.button(NintendoControllerState.B).x,two.button(NintendoControllerState.B).y);
        check(!two.leftHeld(),"a side button is not an adaptive hold");two.up(40);

        NintendoControllerState dpad=fresh(NintendoControllerState.LAYOUT_DPAD,NintendoControllerState.DIR_LEFT|NintendoControllerState.DIR_RIGHT,0,0);
        float cx=(dpad.leftX()+dpad.leftRight())/2,cy=(dpad.leftY()+dpad.leftBottom())/2;
        dpad.down(5,cx,cy);check(dpad.buttons()==0,"adaptive D-pad centre is neutral");
        dpad.move(5,cx-80,cy);check(dpad.buttons()==NintendoControllerState.LEFT,"adaptive D-pad left");
        dpad.move(5,cx+80,cy);check(dpad.buttons()==NintendoControllerState.RIGHT,"adaptive D-pad right");
        dpad.move(5,cx,cy-80);check(dpad.buttons()==NintendoControllerState.UP,"adaptive D-pad up");
        dpad.move(5,cx,cy+80);check(dpad.buttons()==NintendoControllerState.DOWN,"adaptive D-pad down");
        dpad.up(5);

        NintendoControllerState menu=fresh(NintendoControllerState.LAYOUT_MENU,0,0,0);
        check(!menu.leftVisible(),"menu hides the adaptive left area");
        menu.down(6,cx,cy);check(menu.buttons()==0,"menu left half is neutral");
        check(dpad.controls().get(0).visible,"menu shows the small D-pad");

        NintendoControllerState spell=fresh(NintendoControllerState.LAYOUT_ONE,1,0,
                NintendoControllerState.SHOW_DPAD|NintendoControllerState.SHOW_X|NintendoControllerState.SHOW_Y);
        check(spell.button(NintendoControllerState.Y).visible&&spell.button(NintendoControllerState.X).visible,"BeatSpell shows Y and X");
        check(spell.button(NintendoControllerState.UP).visible,"BeatSpell keeps the small D-pad");
    }
    private static void ownership(){
        NintendoControllerState s=fresh(NintendoControllerState.LAYOUT_DPAD,0,0,0);
        NintendoControllerState.Control b=s.button(NintendoControllerState.B);
        center(s,b);check(s.buttons()==NintendoControllerState.B,"B press");
        s.move(1,b.x+b.size,b.y);check(s.buttons()==0,"B releases outside");
        s.move(1,b.x,b.y);check(s.buttons()==NintendoControllerState.B,"B presses again on return");
        s.up(1);check(s.buttons()==0,"B up");

        NintendoControllerState.Control a=s.button(NintendoControllerState.DOWN);
        s.down(2,a.x,a.y);check(s.buttons()==NintendoControllerState.DOWN,"small D-pad down");
        s.move(2,a.x+1,a.y+1);check(s.buttons()==NintendoControllerState.DOWN,"small D-pad holds");
        s.up(2);

        float ax=(s.aLeft()+s.aRight())/2,ay=(s.aTop()+s.aBottom())/2;
        s.down(10,ax,ay);check(s.buttons()==NintendoControllerState.A,"right half presses A");
        s.move(10,s.leftX()+4,ay);check(s.buttons()==NintendoControllerState.A,"A keeps ownership across the split");
        s.down(11,s.aRight()-8,ay+30);check(s.buttons()==NintendoControllerState.A,"second A finger");
        s.up(10);check(s.buttons()==NintendoControllerState.A,"one A finger remains");
        s.up(11);check(s.buttons()==0,"last A finger releases");

        NintendoControllerState t=fresh(NintendoControllerState.LAYOUT_TWO,1,12,0);
        t.down(20,t.leftX()+30,t.leftY()+30);check(t.buttons()==NintendoControllerState.B,"held adaptive B");
        t.setHint(NintendoControllerState.LAYOUT_MENU,0,0,0);check(t.buttons()==0,"scene change releases the old left button");

        NintendoControllerState y=fresh(NintendoControllerState.LAYOUT_ONE,0,0,NintendoControllerState.SHOW_Y);
        NintendoControllerState.Control yc=y.button(NintendoControllerState.Y);
        center(y,yc);check(y.buttons()==NintendoControllerState.Y,"Y press");
        y.setHint(NintendoControllerState.LAYOUT_ONE,0,0,0);check(y.buttons()==0,"hidden Y releases");

        NintendoControllerState acc=fresh(NintendoControllerState.LAYOUT_ONE,0,0,0);
        NintendoControllerState.Control ac=acc.button(NintendoControllerState.B);
        acc.accessibilityButton(ac.id,true);check(acc.buttons()==NintendoControllerState.B,"focus hold");
        acc.accessibilityButton(ac.id,false);check(acc.buttons()==0,"focus release");
        acc.down(30,ax,ay);acc.clear();check(acc.buttons()==0,"clear releases every owner");
        acc.down(31,ax,ay);acc.layout(667,375,0,0,0,0);check(acc.buttons()==0,"resize releases every owner");
    }
    public static void main(String[] args){
        geometry();adaptive();ownership();
        check(NintendoControllerState.A==1&&NintendoControllerState.B==2&&NintendoControllerState.Y==8,"Npad action bits");
        check(NintendoControllerState.LEFT==4096&&NintendoControllerState.UP==8192&&NintendoControllerState.RIGHT==16384&&NintendoControllerState.DOWN==32768,"Npad direction bits");
        System.out.println("RHYTHM_TOUCH checks="+checks+" adaptive=1 glass_layout=1 zones=1 multitouch=1 scene_change=1 passed=1");
    }
}