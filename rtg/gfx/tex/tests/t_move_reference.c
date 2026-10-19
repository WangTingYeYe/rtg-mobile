#include "../nvn_deswizzle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compare every byte with the original address formula, independently of the
 * optimized move loop. Round trips alone could conceal a shared layout error. */
int main(void) {
    const unsigned widths[]={1,3,17,63,64,65,131};
    const unsigned formats[]={1,0x0d,0x25,0x29,0x2e,0x42,0x4d};
    const unsigned targets[]={1,2,4};unsigned cases=0;uint32_t seed=3;
    for(unsigned ti=0;ti<3;ti++)for(unsigned fi=0;fi<7;fi++)for(unsigned wi=0;wi<7;wi++){
        nvn_texdesc d={.target=targets[ti],.format=formats[fi],.width=widths[wi],.height=37,.depth=ti?5:1,.levels=4};
        nvn_texlayout layout;nvn_tex_layout(&d,&layout);
        uint8_t *storage=malloc(layout.size+32),*expected=malloc(layout.size+32);assert(storage&&expected);
        for(unsigned layer=0;layer<layout.layers;layer++)for(unsigned level=0;level<4;level++){
            uint32_t rb,rows,slices,bpb;uint64_t base;unsigned th,td;
            assert(!nvn_deswizzle_geom(&d,level,layer,&rb,&rows,&slices,&base,&th,&td,&bpb));
            size_t pitch=rb+19,bytes=pitch*rows*slices+32;
            uint8_t *linear=malloc(bytes),*out=malloc(bytes);assert(linear&&out);
            memset(linear,0xcd,bytes);memset(out,0xcd,bytes);
            memset(storage,0xa5,layout.size+32);memset(expected,0xa5,layout.size+32);
            for(unsigned z=0;z<slices;z++)for(unsigned y=0;y<rows;y++)for(unsigned x=0;x<rb;x++){
                seed=seed*1664525+1013904223;
                uint8_t value=seed>>24;linear[16+((size_t)z*rows+y)*pitch+x]=value;
                expected[16+base+nvn_bl_offset(x,y,z,rb,rows,th,td)]=value;
            }
            assert(!nvn_swizzle(&d,level,layer,linear+16,pitch,storage+16));
            assert(!memcmp(storage,expected,layout.size+32));
            assert(!nvn_deswizzle(&d,level,layer,expected+16,out+16,pitch));
            assert(!memcmp(linear,out,bytes));cases++;free(linear);free(out);
        }
        free(storage);free(expected);
    }
    printf("PASS: %u mip/layer/volume/partial-row cases match byte-address reference; padding and canaries preserved\n",cases);
}
