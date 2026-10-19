package org.rhythmtengoku.mobile;

import android.content.Context;
import android.os.SystemClock;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/** One atomic private file owns persisted settings and runtime requests. */
final class RendererOptionsStore {
    private final AtomicFile file;
    RendererOptionsStore(Context context){file=new AtomicFile(new File(context.getFilesDir(),"rhythm-tengoku/diagnostics/attachment-mode.txt"));}
    RendererOptions read()throws IOException {
        if(!file.getBaseFile().exists()&&!new File(file.getBaseFile()+".bak").exists())return new RendererOptions(0,RendererOptions.DEFAULT_FLAGS);
        try(FileInputStream in=file.openRead()){
            byte[] data=new byte[96];int size=in.read(data);
            if(size<=0||in.read()!=-1)throw new IOException("设置文件大小无效");
            try{return RendererOptions.parse(new String(data,0,size,StandardCharsets.US_ASCII));}
            catch(IllegalArgumentException e){throw new IOException("设置文件无效",e);}
        }
    }
    RendererOptions change(int bit,boolean enabled)throws IOException {
        RendererOptions next;
        try{next=read().changed(bit,enabled,SystemClock.elapsedRealtimeNanos());}
        catch(IllegalArgumentException e){throw new IOException("无法更新设置版本",e);}
        FileOutputStream out=null;
        try{out=file.startWrite();out.write(next.encode().getBytes(StandardCharsets.US_ASCII));file.finishWrite(out);return next;}
        catch(IOException e){if(out!=null)file.failWrite(out);throw e;}
    }
}
