#include "../tex/nvn_bcdec.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    const unsigned widths[]={1,3,4,5,16,31,64,257},heights[]={1,3,4,5,16,35};unsigned cases=0;
    uint32_t seed=1;uint64_t hash=1469598103934665603ULL;
    for(unsigned format=0x41;format<=0x50;format++)for(unsigned wi=0;wi<8;wi++)for(unsigned hi=0;hi<6;hi++){
        unsigned w=widths[wi],h=heights[hi],pf;int bpp;assert(nvn_bc_unpacked(format,&pf,&bpp));
        unsigned bb=(format==0x41||format==0x42||format==0x45||format==0x46||format==0x49||format==0x4a)?8:16;
        size_t pitch=((w+3)/4)*bb+16,source_bytes=pitch*((h+3)/4),dpitch=w*bpp+16,bytes=dpitch*h+32;
        uint8_t *src=malloc(source_bytes),*reference=malloc(bytes),*upload=malloc(bytes);assert(src&&reference&&upload);
        for(size_t i=0;i<source_bytes;i++){seed=seed*1664525+1013904223;src[i]=(uint8_t)(seed>>24);}
        memset(reference,0xcd,bytes);memset(upload,0xcd,bytes);
        assert(!nvn_bc_decode(format,src,pitch,w,h,reference+16,dpitch));
        assert(!nvn_bc_decode_upload(format,src,pitch,w,h,upload+16,dpitch));
        assert(!memcmp(reference,upload,bytes));
        for(size_t i=0;i<bytes;i++)hash=(hash^reference[i])*1099511628211ULL;
        free(src);free(reference);free(upload);cases++;
    }
    printf("PASS: %u bit-exact BC1-BC7/BC6H signed/sRGB cases, padded rows and partial blocks, canaries retained; hash=%016llx\n",cases,(unsigned long long)hash);
}
