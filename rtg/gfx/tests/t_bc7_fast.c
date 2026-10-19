#define BCDEC_IMPLEMENTATION
#include "../../third_party/bcdec/bcdec.h"
#include "../tex/nvn_bc7_fast.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    uint32_t random=1234;unsigned cases=0;
    for(unsigned mode=0;mode<=8;mode++)for(unsigned n=0;n<50000;n++){
        uint64_t source[2];uint8_t reference[160],fast[160];
        for(unsigned i=0;i<16;i++){random=random*1664525+1013904223;((uint8_t *)source)[i]=random>>24;}
        ((uint8_t *)source)[0]=mode==8?0:(((uint8_t *)source)[0]&~((1u<<(mode+1))-1))|(1u<<mode);
        memset(reference,0xcd,sizeof(reference));memset(fast,0xcd,sizeof(fast));
        bcdec_bc7(source,reference+16,32);
        int supported=nvn_bc7_fast_block((const uint8_t *)source,fast+16,32);
        assert(supported==(mode==3||mode==5||mode==6));
        if(!supported)bcdec_bc7(source,fast+16,32);
        if(memcmp(reference,fast,sizeof(fast))){fprintf(stderr,"mismatch mode=%u case=%u\n",mode,n);return 1;}
        cases++;
    }
    printf("PASS: %u BC7 blocks, all modes/rotations/partitions, padded rows and canaries identical\n",cases);
}
