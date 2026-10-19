package org.rhythmtengoku.mobile;
public final class DisplayRefreshPolicyTest {
    static DisplayRefreshPolicy.Mode mode(int id,int w,int h,float hz){return new DisplayRefreshPolicy.Mode(id,w,h,hz);}
    static void expect(int id,DisplayRefreshPolicy.Mode current,DisplayRefreshPolicy.Mode... available){
        if(DisplayRefreshPolicy.select(current,available).id!=id)throw new AssertionError("expected display mode "+id);
    }
    public static void main(String[] args){
        DisplayRefreshPolicy.Mode current=mode(3,1260,2720,60),high=mode(1,1260,2720,120.00001f),mid=mode(2,1260,2720,90);
        expect(1,current,current,high,mid);
        expect(3,current,current,mid);
        expect(2,mid,mid);
        expect(3,current,current,mode(4,1080,2400,120));
        expect(3,current,current,mode(4,1260,2720,144));
        expect(3,current,mode(4,1260,2720,Float.NaN));
        expect(3,current);
        expect(1,high,current,mid,high);
        System.out.println("PASS: 120/60 selection, no uneven 60-in-90 pacing, current fallback, resolution and invalid modes");
    }
}
