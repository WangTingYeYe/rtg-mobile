package org.rhythmtengoku.mobile;

public final class RendererOptionsTest {
    private static int checks;
    private static void check(boolean ok){checks++;if(!ok)throw new AssertionError("check "+checks);}
    private static void invalid(String text){try{RendererOptions.parse(text);throw new AssertionError("accepted "+text);}catch(IllegalArgumentException expected){checks++;}}
    public static void main(String[] args){
        RendererOptions defaults=new RendererOptions(0,3);check(defaults.enabled(1)&&defaults.enabled(2));
        RendererOptions clear=defaults.changed(2,false,100);check(clear.flags==1&&clear.sequence==100);
        RendererOptions off=clear.changed(1,false,99);check(off.flags==0&&off.sequence==101);
        RendererOptions store=off.changed(2,true,200);check(store.flags==2&&store.sequence==200);
        RendererOptions both=store.changed(1,true,201);check(both.flags==3);
        RendererOptions restored=RendererOptions.parse(both.encode());check(restored.sequence==201&&restored.flags==3);
        RendererOptions windows=RendererOptions.parse("10 3\r\n");check(windows.sequence==10&&windows.flags==3);
        RendererOptions windowsChanged=windows.changed(1,false,202);check(windowsChanged.flags==2&&windowsChanged.sequence==202);
        check(RendererOptions.parse(windowsChanged.encode()).flags==2);
        check(RendererOptions.parse("11 0").flags==0);
        for(int flags=0;flags<4;flags++){RendererOptions r=RendererOptions.parse("517000000000000 "+flags+"\n");check(r.flags==flags&&r.sequence==517000000000000L);}
        for(String bad:new String[]{"","-1 0","1 4","1 -1","1 0 extra","1 0\n2 3","18446744073709551615 3","1\t3","1 03","1 3\n\n","1 3\r","1 3\r\n2 0"})invalid(bad);
        try{new RendererOptions(Long.MAX_VALUE,3).changed(1,false,1);throw new AssertionError("wrapped sequence");}catch(IllegalArgumentException expected){checks++;}
        System.out.println("RENDERER_OPTIONS checks="+checks+" independent_switches=1 persistence_format=1 stale_clock=1 invalid_input=1 passed=1");
    }
}
