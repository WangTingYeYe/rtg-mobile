/* Uses the captured game's effect fixture. Compare exact output hashes and
 * CPU time across builds, including live delay-length changes. */
#define main audio_correctness_main
#include "t_rtg_audio_renderer.c"
#undef main
#include <time.h>
static uint64_t cpu_ns(void) {struct timespec t;assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(void) {
    uint8_t e[0xc0];game_reverb(e);int16_t pcm[480];
    for(unsigned i=0;i<480;i++)pcm[i]=(int16_t)((i*313u)%16384-8192);
    Fixture *f=reverb_fixture(pcm,240,e,1);float out[480];
    uint64_t hash=1469598103934665603ULL,start=cpu_ns();
    for(unsigned n=0;n<4000;n++) {
        if(n%500==0){e[1]=0;e[0x68]=1;wf(e+0x60,(float)((n/500*37)%101));
            assert(rtg_audio_renderer_effects(f->r,e,sizeof e)==RTG_AUDIO_OK);}
        render(f,out,240);
        for(unsigned i=0;i<480;i++){uint32_t bits;memcpy(&bits,&out[i],4);hash=(hash^bits)*1099511628211ULL;}
    }
    double ms=(cpu_ns()-start)/1e6;uint64_t played;uint32_t consumed;status(f,0,&played,&consumed);
    printf("{\"rendered_frames\":960000,\"cpu_ms\":%.3f,\"pcm_hash\":\"%016llx\",\"played\":%llu,\"consumed\":%u}\n",ms,(unsigned long long)hash,(unsigned long long)played,consumed);
    dispose(f);
}
