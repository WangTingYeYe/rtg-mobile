#include "../nvn_deswizzle.h"
#include <stdio.h>
#include <string.h>

static int check(unsigned format,unsigned width,unsigned bytes,unsigned flags,unsigned stride) {
    nvn_texdesc desc={.target=1,.format=format,.width=width,.height=3,.depth=1,.levels=1,.flags=flags,.stride=stride};
    nvn_texlayout layout;nvn_tex_layout(&desc,&layout);
    unsigned row=width*bytes,pitch=stride?stride:(row+layout.align-1)&~(layout.align-1);
    unsigned char storage[1024],linear[1024],roundtrip[1024];
    memset(storage,0xa5,sizeof storage);memset(linear,0xcc,sizeof linear);memset(roundtrip,0x5a,sizeof roundtrip);
    for(unsigned y=0;y<3;y++)for(unsigned x=0;x<row;x++)storage[y*pitch+x]=(unsigned char)(y*40+x);
    if(nvn_deswizzle(&desc,0,0,storage,linear,row+7)) { puts("FAIL: valid linear texture was rejected");return 1; }
    for(unsigned y=0;y<3;y++)for(unsigned x=0;x<row+7;x++) {
        unsigned char expected=x<row?(unsigned char)(y*40+x):0xcc;
        if(linear[y*(row+7)+x]!=expected) { puts("FAIL: pixel or destination padding differs");return 1; }
    }
    if(nvn_swizzle(&desc,0,0,linear,row+7,roundtrip))return 1;
    for(unsigned y=0;y<3;y++)for(unsigned x=0;x<pitch;x++)
        if(roundtrip[y*pitch+x]!=(x<row?(unsigned char)(y*40+x):0x5a))return 1;
    if(nvn_deswizzle(&desc,1,0,storage,linear,row+7)!=-1)return 1;
    if(nvn_deswizzle(&desc,0,1,storage,linear,row+7)!=-1)return 1;
    desc.stride=row-1;
    if(nvn_deswizzle(&desc,0,0,storage,linear,row+7)!=-1)return 1;
    return 0;
}
int main(void) {
    if(check(1,7,1,0x14,0)||check(1,7,1,0x14,37)||check(1,7,1,0x54,0)||check(0x25,5,4,0x14,32))return 1;
    puts("PASS: linear R8/RGBA rows, explicit/default pitches, padding, inverse copy, invalid mip/layer/stride");return 0;
}
