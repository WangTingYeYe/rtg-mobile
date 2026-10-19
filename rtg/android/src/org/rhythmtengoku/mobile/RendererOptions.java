package org.rhythmtengoku.mobile;

/** Shared file format for the system menu and diagnostic A/B runner. */
final class RendererOptions {
    static final int CLEAR_MERGE=1, STORE_ELIDE=2, DEFAULT_FLAGS=3;
    final long sequence;
    final int flags;
    RendererOptions(long sequence,int flags){
        if(sequence<0||flags<0||flags>3)throw new IllegalArgumentException("设置格式无效");
        this.sequence=sequence;this.flags=flags;
    }
    static RendererOptions parse(String text){
        // Diagnostic files may be written by Windows; accept LF and CRLF.
        if(!text.matches("[0-9]{1,19} [0-3](?:\\r?\\n)?"))throw new IllegalArgumentException("设置格式无效");
        String[] parts=text.trim().split(" ");return new RendererOptions(Long.parseLong(parts[0]),Integer.parseInt(parts[1]));
    }
    RendererOptions changed(int bit,boolean enabled,long clock){
        if((bit!=CLEAR_MERGE&&bit!=STORE_ELIDE)||sequence==Long.MAX_VALUE)throw new IllegalArgumentException("设置版本无效");
        return new RendererOptions(Math.max(sequence+1,clock),enabled?flags|bit:flags&~bit);
    }
    String encode(){return sequence+" "+flags+"\n";}
    boolean enabled(int bit){return (flags&bit)!=0;}
    static String description(int flags){return "清屏合并"+((flags&1)!=0?"开":"关")+" · 省略写回"+((flags&2)!=0?"开":"关");}
}
