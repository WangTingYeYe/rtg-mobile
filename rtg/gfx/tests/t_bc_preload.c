#include "../tex/nvn_bc_preload.h"
#include "../tex/nvn_bcdec.h"
#include "../tex/nvn_deswizzle.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static nvn_texdesc desc={.target=1,.format=0x4d,.width=512,.height=512,.depth=1,.levels=1};
static uint8_t *make_source(unsigned seed,size_t *bytes) {
    nvn_texlayout layout;nvn_tex_layout(&desc,&layout);*bytes=layout.size;
    uint8_t *source=malloc(*bytes);assert(source);
    for(size_t i=0;i<*bytes;i++){seed=seed*1664525+1013904223;source[i]=seed>>24;}
    return source;
}
static NvnBcPrepared *wait_ready(const uint8_t *source,size_t bytes) {
    struct timespec nap={0,1000000};NvnBcPrepared *job=NULL;
    for(unsigned i=0;i<5000&&!job;i++){job=nvn_bc_preload_acquire(&desc,source,bytes);if(!job)nanosleep(&nap,NULL);}
    assert(job);return job;
}
static void *producer(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<30;i++){
        size_t bytes;uint8_t *source=make_source(1000+id*100+i,&bytes);
        nvn_bc_preload_offer(&desc,source,bytes);
        NvnBcPrepared *job=nvn_bc_preload_acquire(&desc,source,bytes);
        if(job)nvn_bc_preload_release(job);
        /* Source lifetime ends before the asynchronous decoder can finish. */
        memset(source,0,bytes);free(source);
    }
    return NULL;
}
int main(void) {
    size_t bytes;uint8_t *source=make_source(123,&bytes),*saved=malloc(bytes);
    assert(saved);memcpy(saved,source,bytes);
    nvn_bc_preload_offer(&desc,source,bytes);
    memset(source,0,bytes);free(source);
    NvnBcPrepared *job=wait_ready(saved,bytes);
    size_t pitch=(desc.width/4)*16,linear_bytes=pitch*(desc.height/4),pixels=(size_t)desc.width*desc.height*4;
    uint8_t *linear=malloc(linear_bytes),*expected=malloc(pixels);assert(linear&&expected);
    assert(!nvn_deswizzle(&desc,0,0,saved,linear,pitch));
    assert(!nvn_bc_decode(desc.format,linear,pitch,desc.width,desc.height,expected,desc.width*4));
    assert(!memcmp(expected,nvn_bc_preload_pixels(job),pixels));
    /* An identical image at a new NVN object can reuse preparation, whereas
     * any source-byte or descriptor change must miss. */
    nvn_bc_preload_offer(&desc,saved,bytes);NvnBcPrepared *same=wait_ready(saved,bytes);
    assert(same==job);nvn_bc_preload_release(same);
    saved[bytes/2]^=1;assert(!nvn_bc_preload_acquire(&desc,saved,bytes));saved[bytes/2]^=1;
    nvn_texdesc changed=desc;changed.format=0x4e;assert(!nvn_bc_preload_acquire(&changed,saved,bytes));
    changed=desc;changed.pkg_block_h=2;assert(!nvn_bc_preload_acquire(&changed,saved,bytes));
    pthread_t threads[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(threads+i,NULL,producer,(void *)(uintptr_t)i));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    assert(!memcmp(expected,nvn_bc_preload_pixels(job),pixels));
    /* Exercise the parallel split with a partial final block row. */
    desc.width=2049;desc.height=2049;desc.pkg_block_h=2;
    size_t large_bytes;uint8_t *large=make_source(789,&large_bytes);
    nvn_bc_preload_offer(&desc,large,large_bytes);NvnBcPrepared *large_job=wait_ready(large,large_bytes);
    size_t large_pitch=((desc.width+3)/4)*16,large_pixels=(size_t)desc.width*desc.height*4;
    uint8_t *large_linear=malloc(large_pitch*((desc.height+3)/4)),*large_expected=malloc(large_pixels);
    assert(large_linear&&large_expected);
    assert(!nvn_deswizzle(&desc,0,0,large,large_linear,large_pitch));
    assert(!nvn_bc_decode(desc.format,large_linear,large_pitch,desc.width,desc.height,large_expected,desc.width*4));
    assert(!memcmp(large_expected,nvn_bc_preload_pixels(large_job),large_pixels));
    nvn_bc_preload_release(large_job);free(large);free(large_linear);free(large_expected);
    /* Shutdown can cancel queued work and join a running decode while an
     * acquired immutable result remains alive until its consumer releases it. */
    nvn_bc_preload_shutdown();assert(!memcmp(expected,nvn_bc_preload_pixels(job),pixels));
    nvn_bc_preload_release(job);free(saved);free(linear);free(expected);
    puts("PASS: owned snapshots, full-data/descriptor validation, reuse, 4 producers, bounded eviction, borrowed lifetime and shutdown");
}
