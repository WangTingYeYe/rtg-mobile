#include "nvn_bc_preload.h"
#include "nvn_bcdec.h"
#include "nvn_deswizzle.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifndef NVN_BC_PRELOAD_LIMIT
#define NVN_BC_PRELOAD_LIMIT (192u*1024*1024)
#endif
enum { SLOTS=16,QUEUED,RUNNING,READY,FAILED };
struct NvnBcPrepared {
    nvn_texdesc desc;
    size_t bytes,pixel_bytes,reserved;
    uint8_t *source,*pixels;
    unsigned refs,state;
    uint64_t age;
};
static struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thread;
    int started,stopping;
    size_t reserved;
    uint64_t age;
    NvnBcPrepared *slots[SLOTS];
} cache={.lock=PTHREAD_MUTEX_INITIALIZER,.wake=PTHREAD_COND_INITIALIZER};

static int same_desc(const nvn_texdesc *a,const nvn_texdesc *b) {
    return a->target==b->target&&a->format==b->format&&a->flags==b->flags&&a->samples==b->samples&&
        a->width==b->width&&a->height==b->height&&a->depth==b->depth&&a->levels==b->levels&&
        a->stride==b->stride&&a->pkg_block_h==b->pkg_block_h;
}
typedef struct {
    const NvnBcPrepared *job;
    uint8_t *pixels;
    uint32_t row_bytes,rows,first,last;
    uint64_t base;
    unsigned tile_h,tile_d;
} DecodePart;
static void *decode_part(void *data) {
    const DecodePart *part=data;const NvnBcPrepared *job=part->job;
    size_t pitch=(size_t)job->desc.width*4;uint64_t block=512ull<<(part->tile_h+part->tile_d);
    for(unsigned by=part->first;by<part->last;by++){
        const uint8_t *origin=job->source+part->base+nvn_bl_offset(0,by,0,part->row_bytes,part->rows,part->tile_h,part->tile_d);
        unsigned height=job->desc.height-by*4;if(height>4)height=4;
        for(unsigned bx=0;bx<(job->desc.width+3)/4;bx++){
            const uint8_t *source=origin+(bx/4)*block+(bx&2)*128+(bx&1)*32;
            uint8_t *target=part->pixels+(size_t)by*4*pitch+(size_t)bx*16;
            unsigned width=job->desc.width-bx*4;if(width>4)width=4;
            if(width==4&&height==4)nvn_bc7_decode_block(source,target,pitch);
            else{uint32_t tail[16];nvn_bc7_decode_block(source,(uint8_t *)tail,16);
                for(unsigned y=0;y<height;y++)memcpy(target+y*pitch,tail+y*4,width*4);}
        }
    }
    return NULL;
}
static int decode_prepared(const NvnBcPrepared *job,uint8_t *pixels) {
    DecodePart whole={.job=job,.pixels=pixels};uint32_t slices,bpb;
    if(nvn_deswizzle_geom(&job->desc,0,0,&whole.row_bytes,&whole.rows,&slices,&whole.base,&whole.tile_h,&whole.tile_d,&bpb))return -1;
    whole.last=whole.rows;
    if(job->pixel_bytes>=16u*1024*1024&&whole.rows>1){
        /* Decode directly from Tegra blocks, avoiding a full linear compressed
         * copy. The two workers own disjoint block rows, including edge pixels. */
        DecodePart second=whole;second.first=whole.rows/2;
        pthread_t helper;
        if(!pthread_create(&helper,NULL,decode_part,&second)){
            whole.last=second.first;decode_part(&whole);
            if(pthread_join(helper,NULL))abort();
            return 0;
        }
    }
    decode_part(&whole);return 0;
}
/* All reference/registry changes are under lock; decoding only accesses the
 * worker's owned snapshot and unpublished pixel storage. */
static void unref(NvnBcPrepared *job) {
    if(--job->refs)return;
    cache.reserved-=job->reserved;free(job->source);free(job->pixels);free(job);
}
static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&cache.lock);
    for(;;){
        NvnBcPrepared *job=NULL;
        for(unsigned i=0;i<SLOTS;i++)if(cache.slots[i]&&cache.slots[i]->state==QUEUED&&
            (!job||cache.slots[i]->age<job->age))job=cache.slots[i];
        if(cache.stopping)break;
        if(!job){pthread_cond_wait(&cache.wake,&cache.lock);continue;}
        job->state=RUNNING;job->refs++;
        pthread_mutex_unlock(&cache.lock);
        uint8_t *pixels=malloc(job->pixel_bytes);
        int ok=pixels&&!decode_prepared(job,pixels);
        pthread_mutex_lock(&cache.lock);
        if(ok){job->pixels=pixels;job->state=READY;}else{free(pixels);job->state=FAILED;}
        unref(job);
    }
    pthread_mutex_unlock(&cache.lock);return NULL;
}
static int geometry(const nvn_texdesc *d,size_t bytes,size_t *pixels) {
    if(!d||d->target!=1||(d->format!=0x4d&&d->format!=0x4e)||d->levels!=1||d->depth>1||
        d->samples>1||(d->flags&0x10)||!d->width||!d->height)return 0;
    uint64_t area=(uint64_t)d->width*d->height;
    if(area<262144||area>16777216)return 0;
    nvn_texlayout layout;nvn_tex_layout(d,&layout);if(layout.size!=bytes)return 0;
    *pixels=(size_t)area*4;
    return bytes<=NVN_BC_PRELOAD_LIMIT&&*pixels<=NVN_BC_PRELOAD_LIMIT-bytes;
}
void nvn_bc_preload_offer(const nvn_texdesc *d,const uint8_t *source,size_t bytes) {
    size_t pixels;if(!source||!geometry(d,bytes,&pixels))return;
    pthread_mutex_lock(&cache.lock);
    if(cache.stopping){pthread_mutex_unlock(&cache.lock);return;}
    for(unsigned i=0;i<SLOTS;i++){
        NvnBcPrepared *job=cache.slots[i];
        if(job&&job->state!=FAILED&&job->bytes==bytes&&same_desc(&job->desc,d)&&!memcmp(job->source,source,bytes)){
            job->age=++cache.age;pthread_mutex_unlock(&cache.lock);return;
        }
    }
    size_t reserved=bytes+pixels;unsigned slot=SLOTS;
    for(;;){
        for(unsigned i=0;i<SLOTS;i++)if(!cache.slots[i]){slot=i;break;}
        if(slot<SLOTS&&cache.reserved<=NVN_BC_PRELOAD_LIMIT-reserved)break;
        unsigned oldest=SLOTS;
        for(unsigned i=0;i<SLOTS;i++)if(cache.slots[i]&&cache.slots[i]->refs==1&&
            (oldest==SLOTS||cache.slots[i]->age<cache.slots[oldest]->age))oldest=i;
        if(oldest==SLOTS){pthread_mutex_unlock(&cache.lock);return;}
        unref(cache.slots[oldest]);cache.slots[oldest]=NULL;
    }
    NvnBcPrepared *job=calloc(1,sizeof(*job));uint8_t *snapshot=malloc(bytes);
    if(!job||!snapshot){free(job);free(snapshot);pthread_mutex_unlock(&cache.lock);return;}
    memcpy(snapshot,source,bytes);
    *job=(NvnBcPrepared){.desc=*d,.bytes=bytes,.pixel_bytes=pixels,
        .reserved=reserved,.source=snapshot,.refs=1,.state=QUEUED,.age=++cache.age};
    cache.slots[slot]=job;cache.reserved+=reserved;
    if(!cache.started){
        if(pthread_create(&cache.thread,NULL,worker,NULL)){cache.slots[slot]=NULL;unref(job);pthread_mutex_unlock(&cache.lock);return;}
        cache.started=1;
    }
    pthread_cond_signal(&cache.wake);pthread_mutex_unlock(&cache.lock);
}
NvnBcPrepared *nvn_bc_preload_acquire(const nvn_texdesc *d,const uint8_t *source,size_t bytes) {
    if(!d||!source)return NULL;
    /* Never stall frame rendering behind an offer's snapshot allocation/copy. */
    if(pthread_mutex_trylock(&cache.lock))return NULL;
    NvnBcPrepared *found=NULL;
    for(unsigned i=0;i<SLOTS;i++){
        NvnBcPrepared *job=cache.slots[i];
        if(job&&job->state==READY&&job->bytes==bytes&&same_desc(&job->desc,d)&&!memcmp(job->source,source,bytes)){
            found=job;job->refs++;job->age=++cache.age;break;
        }
    }
    pthread_mutex_unlock(&cache.lock);return found;
}
const uint8_t *nvn_bc_preload_pixels(const NvnBcPrepared *job) {return job->pixels;}
void nvn_bc_preload_release(NvnBcPrepared *job) {
    if(!job)return;
    pthread_mutex_lock(&cache.lock);unref(job);pthread_mutex_unlock(&cache.lock);
}
void nvn_bc_preload_shutdown(void) {
    pthread_mutex_lock(&cache.lock);cache.stopping=1;
    for(unsigned i=0;i<SLOTS;i++)if(cache.slots[i]){unref(cache.slots[i]);cache.slots[i]=NULL;}
    pthread_cond_signal(&cache.wake);int started=cache.started;pthread_mutex_unlock(&cache.lock);
    if(started)pthread_join(cache.thread,NULL);
}
