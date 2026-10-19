/* NVN auto-layout boundary regression. 496x336 BC7 is the captured lotus
 * atlas: 84 block rows, 126 weighted rows -> 8 GOBs, not 16.
 * Reference driver thresholds: tegra_swizzle/src/blockheight.rs.
 * Write a known explicit layout, then exercise the actual automatic reader;
 * a same-descriptor swizzle/deswizzle round trip cannot catch this bug. */
#include "../tex/nvn_deswizzle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned cases;
static void check(unsigned rows,unsigned tile,int format,unsigned width,unsigned levels,unsigned layers) {
    int bw,bh;unsigned bpb=nvn_fmt_bpb(format,&bw,&bh);
    nvn_texdesc automatic={.target=layers>1?4:1,.format=format,.width=width,
        .height=rows*bh,.depth=layers,.levels=levels};
    nvn_texdesc packaged=automatic;packaged.pkg_block_h=tile+1;
    nvn_texlayout actual,expected;nvn_tex_layout(&automatic,&actual);nvn_tex_layout(&packaged,&expected);
    if(actual.tileH!=tile){fprintf(stderr,"FAIL: %ux%u format=%#x block_rows=%u expected tileH=%u got=%u\n",width,automatic.height,format,rows,tile,actual.tileH);exit(1);}
    assert(actual.size==expected.size && actual.layer_size==expected.layer_size);
    unsigned char *storage=calloc(1,expected.size);assert(storage);
    for(unsigned layer=0;layer<layers;layer++)for(unsigned level=0;level<levels;level++){
        unsigned w=width>>level,h=automatic.height>>level;if(!w)w=1;if(!h)h=1;
        size_t pitch=((w+bw-1)/bw)*bpb,bytes=pitch*((h+bh-1)/bh);
        unsigned char *source=malloc(bytes),*decoded=malloc(bytes);assert(source&&decoded);
        for(size_t i=0;i<bytes;i++)source[i]=(unsigned char)((i*41+(i/67)*19+level*23+layer*31)%251);
        assert(!nvn_swizzle(&packaged,level,layer,source,pitch,storage));
        assert(!nvn_deswizzle(&automatic,level,layer,storage,decoded,pitch));
        assert(!memcmp(source,decoded,bytes));
        assert(nvn_tex_level_offset(&automatic,level)==nvn_tex_level_offset(&packaged,level));
        free(source);free(decoded);cases++;
    }
    /* Explicit package layout must remain authoritative at every threshold. */
    packaged.pkg_block_h=tile==4?2:5;nvn_tex_layout(&packaged,&actual);
    assert(actual.tileH==(unsigned)packaged.pkg_block_h-1);
    free(storage);
}
int main(void) {
    check(84,3,0x4d,496,1,1);
    static const unsigned bounds[][2]={{1,0},{6,0},{7,0},{8,0},{10,0},{11,1},
        {16,1},{20,1},{21,1},{22,2},{40,2},{41,2},{42,2},{43,3},
        {64,3},{80,3},{81,3},{84,3},{85,3},{86,4},{128,4}};
    static const int formats[]={1,0x25,0x41,0x4d,0x7e};
    for(unsigned i=0;i<sizeof(bounds)/sizeof(*bounds);i++)
        for(unsigned f=0;f<sizeof(formats)/sizeof(*formats);f++){
            check(bounds[i][0],bounds[i][1],formats[f],67,1,1);
            check(bounds[i][0],bounds[i][1],formats[f],65,5,3);
        }
    printf("PASS: %u texture subresources across NVN auto-layout thresholds, mips, arrays and explicit overrides\n",cases);
}
