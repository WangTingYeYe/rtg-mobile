package org.rhythmtengoku.mobile;

public final class NintendoControllerStateTest {
    private static int checks;
    private static void check(boolean value,String what){checks++;if(!value)throw new AssertionError(what);}
    private static boolean face(NintendoControllerState.Control c){return c.kind==NintendoControllerState.A_REGION;}
    private static void down(NintendoControllerState s,int id,long mask){NintendoControllerState.Control c=s.button(mask);s.down(id,face(c)?s.faceX(c):c.x,face(c)?s.faceY(c):c.y);}
    private static NintendoControllerState fresh(){NintendoControllerState s=new NintendoControllerState();s.layout(906,420,24,0,24,8);return s;}
    private static void geometry(){
        float[][] screens={{375,667,0,24,0,24},{568,320,0,0,0,0},{568,320,48,0,48,8},{667,375,24,0,24,8},{640,360,32,0,0,16},
                {837,388,40,40,0,0},{906,420,36,0,24,8},{1280,800,24,24,24,24}};
        for(float[] screen:screens)for(boolean extras:new boolean[]{false,true}){
            NintendoControllerState s=new NintendoControllerState();s.setExtraButtons(extras);
            s.layout(screen[0],screen[1],screen[2],screen[3],screen[4],screen[5]);
            check(s.controls().size()==(extras?14:9),"only needed controls shown");
            for(NintendoControllerState.Control c:s.controls()){
                check(c.halfWidth>=24&&c.halfHeight>=24,"48dp target "+c.description);
                check(c.x-c.halfWidth>=0&&c.x+c.halfWidth<=screen[0],"horizontal screen bounds "+c.description);
                check(c.y-c.halfHeight>=0&&c.y+c.halfHeight<=screen[1],"vertical screen bounds "+c.description);
                check(s.hit(face(c)?s.faceX(c):c.x,face(c)?s.faceY(c):c.y)==c,"reachable target "+c.description+" width="+screen[0]);
                if(face(c))continue;
                for(NintendoControllerState.Control other:s.controls())if(other.id>c.id&&!face(other))
                    check(!(Math.abs(c.x-other.x)<c.halfWidth+other.halfWidth&&Math.abs(c.y-other.y)<c.halfHeight+other.halfHeight),"no overlapping visible controls");
            }
            // The right play area, not merely the A artwork, accepts direct presses.
            NintendoControllerState.Control a=s.button(NintendoControllerState.A);
            for(float x=a.x-a.halfWidth+2;x<a.x+a.halfWidth;x+=31)for(float y=s.contentTop()+2;y<s.contentBottom();y+=29){
                NintendoControllerState.Control hit=s.hit(x,y);
                if(hit!=null&&hit.kind==NintendoControllerState.BUTTON)continue; // explicitly visible extra keys
                s.down(11,x,y);check(s.buttons()==NintendoControllerState.A,"full right region A");s.up(11);check(s.buttons()==0,"release A");
            }
            NintendoControllerState.Control b=s.button(NintendoControllerState.B);
            check(b.kind==NintendoControllerState.BUTTON&&b.halfWidth==24&&b.halfHeight==24,"B is a dedicated 48dp button");
            check(a.x+a.halfWidth>=screen[0]-4,"right A region reaches edge");
            check(b.y-b.halfHeight==4,"immersive toolbar stays at physical top despite swipe inset");
            long[] utility={NintendoControllerState.B,NintendoControllerState.Y,NintendoControllerState.MINUS,NintendoControllerState.PLUS};
            for(int i=1;i<utility.length;i++){
                NintendoControllerState.Control prev=s.button(utility[i-1]),next=s.button(utility[i]);
                check(next.y==prev.y&&next.x-prev.x==56,"one aligned utility group with 8dp target gaps");
            }
            if(screen[0]>=568){
                check(s.padX()-84>=screen[2]+12,"direction pad inset from side gesture/cutout area");
                check(screen[1]-(s.padY()+84)>=28,"direction pad raised from bottom edge");
            }
            for(float x=6;x<s.split();x+=17)for(float y=s.contentTop()+2;y<s.contentBottom();y+=19){
                NintendoControllerState.Control hit=s.hit(x,y);
                if(hit!=null){check(hit.mask!=NintendoControllerState.B,"left gameplay area never emits B");continue;}
                s.down(12,x,y);check(s.buttons()==0,"left blank area is neutral");
                s.move(12,a.x,a.y);check(s.buttons()==0,"blank-area drift never starts A");s.up(12);
            }
            s.down(14,screen[0]-32,s.contentTop()-28);s.move(14,a.x,a.y);
            check(s.buttons()==0,"blank toolbar never starts A when dragged");s.clear();
        }
    }
    public static void main(String[] args){
        geometry();NintendoControllerState s=fresh();
        check(NintendoControllerState.A==1&&NintendoControllerState.B==2&&NintendoControllerState.Y==8,"Npad action bits");
        check(NintendoControllerState.LEFT==4096&&NintendoControllerState.UP==8192&&NintendoControllerState.RIGHT==16384&&NintendoControllerState.DOWN==32768,"Npad direction bits");
        NintendoControllerState.Control a=s.button(NintendoControllerState.A),up=s.button(NintendoControllerState.UP),b=s.button(NintendoControllerState.B),y=s.button(NintendoControllerState.Y);
        down(s,17,NintendoControllerState.UP);down(s,3,NintendoControllerState.A);
        check(s.buttons()==(NintendoControllerState.UP|NintendoControllerState.A),"direction + A chord");
        s.up(3);check(s.buttons()==NintendoControllerState.UP,"A up leaves direction held");s.up(17);check(s.buttons()==0,"both released");
        down(s,27,NintendoControllerState.B);down(s,2,NintendoControllerState.A);
        check(s.buttons()==3,"B plus A for Beatspell");s.up(27);check(s.buttons()==1,"B up leaves A held");s.clear();
        down(s,7,NintendoControllerState.A);down(s,8,NintendoControllerState.A);s.up(7);
        check(s.buttons()==1,"independent finger owners for A");s.up(8);check(s.buttons()==0,"last finger releases A");
        down(s,5,NintendoControllerState.A);s.move(5,up.x,up.y);check(s.buttons()==1,"held A survives drift across split");
        s.move(5,-10,-10);check(s.buttons()==1,"A only releases at UP/CANCEL");s.up(5);check(s.buttons()==0,"true UP ends hold");
        s.down(41,s.padX(),s.padY());check(s.buttons()==0,"neutral D-pad centre");
        s.move(41,up.x,up.y);check(s.buttons()==NintendoControllerState.UP,"neutral to up");
        NintendoControllerState.Control right=s.button(NintendoControllerState.RIGHT);
        s.move(41,right.x,right.y);check(s.buttons()==NintendoControllerState.RIGHT,"slide changes direction");
        s.move(41,a.x,a.y);check(s.buttons()==0,"left-side capture cannot turn into A");s.clear();
        down(s,42,NintendoControllerState.UP);s.move(42,s.faceX(b),s.faceY(b));
        check(s.buttons()==0,"leaving direction pad never triggers B");s.move(42,up.x,up.y);
        check(s.buttons()==NintendoControllerState.UP,"returning to pad restores direction");s.clear();
        down(s,43,NintendoControllerState.B);s.move(43,up.x,up.y);
        check(s.buttons()==NintendoControllerState.B,"B hold does not turn into direction");s.move(43,a.x,a.y);
        check(s.buttons()==NintendoControllerState.B,"B hold crossing split does not become A");s.up(43);check(s.buttons()==0,"B releases on UP");
        down(s,44,NintendoControllerState.B);down(s,45,NintendoControllerState.B);s.up(44);
        check(s.buttons()==NintendoControllerState.B,"two B fingers retain independent ownership");s.clear();
        down(s,9,NintendoControllerState.Y);check(s.buttons()==8,"Y held demonstration");
        s.move(9,a.x,a.y);check(s.buttons()==0,"leaving Y releases, never triggers A");s.clear();
        down(s,4,NintendoControllerState.A);s.accessibilityButton(a.id,true);s.up(4);
        check(s.buttons()==1,"accessibility hold independent");s.accessibilityButton(a.id,false);check(s.buttons()==0,"accessibility release");
        down(s,4,NintendoControllerState.A);s.accessibilityButton(y.id,true);s.clear();check(s.buttons()==0,"cancel clears physical and accessibility");
        down(s,5,NintendoControllerState.A);s.layout(667,375,24,0,24,8);check(s.buttons()==0,"resize releases inputs");
        System.out.println("RHYTHM_TOUCH checks="+checks+" full_half_A=1 dedicated_B=1 neutral_left=1 independent_pad=1 hold_release=1 multi_touch=1 cancel=1 passed=1");
    }
}
