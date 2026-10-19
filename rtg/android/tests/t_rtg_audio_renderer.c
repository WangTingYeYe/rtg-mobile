#include "../rtg_audio_renderer.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CB=0x70, VB=0x188, MB=0x930, SB=0x140 };
typedef struct {
    uint8_t channels[CB*4],voices[VB*4],mixes[0x20+MB*3],sinks[SB];
    RtgAudioRenderer *r;
    unsigned voices_count,channels_count,mix_count;
} Fixture;
static void w32(void *p,uint32_t v) { memcpy(p,&v,4); }
static void w64(void *p,uint64_t v) { memcpy(p,&v,8); }
static void wf(void *p,float v) { memcpy(p,&v,4); }
static uint32_t r32(void *p) { uint32_t v;memcpy(&v,p,4);return v; }
static uint64_t r64(void *p) { uint64_t v;memcpy(&v,p,8);return v; }
static void near(float a,float b) { assert(fabsf(a-b)<0.0001f); }
static void mix(Fixture *f,unsigned id,unsigned channels,unsigned dest,float gain) {
    uint8_t *p=f->mixes+0x20+id*MB;
    wf(p,gain);w32(p+4,48000);w32(p+8,channels);p[0xc]=1;
    w32(p+0x10,id);w32(p+0x924,dest);w32(p+0x928,UINT32_MAX);
    for(unsigned c=0;c<channels;c++)wf(p+0x24+(c*24+c)*4,1);
    if(f->mix_count<=id)f->mix_count=id+1;
    w32(f->mixes+4,f->mix_count);
}
static Fixture *fixture(void) {
    Fixture *f=calloc(1,sizeof(*f));assert(f);
    f->r=rtg_audio_renderer_create();assert(f->r);
    w32(f->mixes,0xcafebabe);mix(f,0,2,0x7fffffffU,1);
    f->sinks[0]=1;f->sinks[1]=1;w32(f->sinks+0x120,2);f->sinks[0x125]=1;
    return f;
}
static void dispose(Fixture *f) { rtg_audio_renderer_destroy(f->r);free(f); }
static void voice(Fixture *f,unsigned id,unsigned rate,unsigned channels,unsigned dest) {
    uint8_t *p=f->voices+id*VB;memset(p,0,VB);
    w32(p,id);w32(p+4,id+10);p[8]=p[9]=1;p[0xb]=2;
    w32(p+0xc,rate);w32(p+0x18,channels);wf(p+0x1c,1);wf(p+0x20,1);
    w32(p+0x70,dest);w32(p+0x74,UINT32_MAX);
    for(unsigned c=0;c<channels;c++) {
        unsigned cid=f->channels_count++;
        uint8_t *ch=f->channels+cid*CB;w32(ch,cid);ch[0x64]=1;
        wf(ch+4+c*4,1);if(channels==1)wf(ch+8,1);
        w32(p+0x158+c*4,cid);
    }
    if(f->voices_count<=id)f->voices_count=id+1;
}
static void wave(Fixture *f,unsigned v,unsigned slot,const int16_t *pcm,
                 size_t bytes,unsigned start,unsigned end,int loop) {
    uint8_t *vp=f->voices+v*VB,*p=vp+0x78+slot*0x38;
    memset(p,0,0x38);w64(p,(uintptr_t)pcm);w64(p+8,bytes);
    w32(p+0x10,start);w32(p+0x14,end);p[0x18]=loop!=0;
    w32(p+0x1c,loop ? UINT32_MAX : 0);w32(p+0x30,start);w32(p+0x34,end);
    w32(vp+0x54,r32(vp+0x54)+1);
}
static int update(Fixture *f) {
    int r=rtg_audio_renderer_update(f->r,f->channels,f->channels_count*CB,
        f->voices,f->voices_count*VB,f->mixes,0x20+f->mix_count*MB,f->sinks,SB);
    /* Native UpdateVoiceOutStatus clears new and marks every slot sent. */
    for(unsigned v=0;v<f->voices_count;v++) {
        f->voices[v*VB+8]=0;
        for(unsigned s=0;s<4;s++) f->voices[v*VB+0x78+s*0x38+0x1a]=1;
    }
    f->mix_count=0;w32(f->mixes+4,0);
    return r;
}
static void status(Fixture *f,unsigned v,uint64_t *played,uint32_t *consumed) {
    uint8_t b[4*16];rtg_audio_renderer_voice_status(f->r,b,sizeof(b));
    *played=r64(b+v*16);*consumed=r32(b+v*16+8);
}
static void render(Fixture *f,float *out,size_t n) {
    rtg_audio_renderer_render(f->r,out,n);
#ifdef RTG_AUDIO_TEST_FORCE_SILENCE
    memset(out,0,n*2*sizeof(float));
#endif
}
static void test_pcm_and_end_exclusive(void) {
    Fixture *f=fixture();int16_t pcm[]={-32768,16384,8192,24576};float out[8];
    voice(f,0,48000,1,0);wave(f,0,0,pcm,sizeof(pcm),1,3,0);assert(update(f)==0);
    render(f,out,4);near(out[0],.5f);near(out[1],.5f);near(out[2],.25f);
    near(out[4],0);near(out[6],0);uint64_t p;uint32_t c;status(f,0,&p,&c);
    assert(p==2&&c==1);assert(rtg_audio_renderer_metrics(f->r).nonzero_frames==2);
    /* Repeated updates do not enqueue the already-consumed buffer again. */
    assert(update(f)==0);render(f,out,4);near(out[0],0);status(f,0,&p,&c);
    assert(p==2&&c==1);dispose(f);
}
static void test_loop_pause_stop_and_reuse(void) {
    Fixture *f=fixture();int16_t pcm[]={4096,8192,12288,16384};float out[32];
    voice(f,0,48000,1,0);wave(f,0,0,pcm,sizeof(pcm),0,2,0);
    wave(f,0,1,pcm,sizeof(pcm),2,4,1);assert(update(f)==0);render(f,out,7);
    const float expected[]={.125f,.25f,.375f,.5f,.375f,.5f,.375f};
    for(unsigned i=0;i<7;i++)near(out[2*i],expected[i]);
    uint64_t p;uint32_t c;status(f,0,&p,&c);assert(p==7&&c==1);
    f->voices[0xa]=2;assert(update(f)==0);render(f,out,8);near(out[0],0);
    status(f,0,&p,&c);assert(p==7&&c==1);
    f->voices[0xa]=0;assert(update(f)==0);render(f,out,1);near(out[0],.5f);
    f->voices[0xa]=1;assert(update(f)==0);render(f,out,2);near(out[0],0);
    status(f,0,&p,&c);assert(p==0&&c==2);
    /* SDK can append after the two slots that were released by stop. */
    f->voices[0xa]=0;wave(f,0,2,pcm,sizeof(pcm),0,1,0);
    assert(update(f)==0);render(f,out,2);near(out[0],.125f);near(out[2],0);
    status(f,0,&p,&c);assert(p==1&&c==3);dispose(f);
}
static void test_stereo_multivoice_mix_delta(void) {
    Fixture *f=fixture();int16_t stereo[]={16384,-8192,16384,-8192};
    int16_t mono[]={8192,8192};float out[8];
    mix(f,1,2,0,.5f);wf(f->mixes+0x20+MB+0x24,0);
    wf(f->mixes+0x20+MB+0x24+4,1);wf(f->mixes+0x20+MB+0x24+24*4,1);
    wf(f->mixes+0x20+MB+0x24+25*4,0); /* swap channels */
    voice(f,0,48000,2,1);wave(f,0,0,stereo,sizeof(stereo),0,2,1);
    voice(f,1,48000,1,0);wave(f,1,0,mono,sizeof(mono),0,2,1);
    assert(update(f)==0);render(f,out,1);near(out[0],.125f);near(out[1],.5f);
    assert(update(f)==0);render(f,out,2);near(out[0],.125f);near(out[1],.5f);
    /* Volume/pan update must take effect even when Mix delta is empty. */
    wf(f->channels+CB*2+4,.5f);assert(update(f)==0);render(f,out,1);
    near(out[0],0);near(out[1],.5f);dispose(f);
}
static void test_resampling_sine_and_pitch(void) {
    Fixture *f=fixture();int16_t pcm[3201];float out[4800*2];
    for(unsigned i=0;i<3201;i++)pcm[i]=(int16_t)lrint(sin(i*2*3.141592653589793*1000/32000)*12000);
    voice(f,0,32000,1,0);wave(f,0,0,pcm,sizeof(pcm),0,3200,0);assert(update(f)==0);
    render(f,out,4800);double error=0,energy=0;
    for(unsigned i=0;i<4798;i++) {
        double ideal=sin(i*2*3.141592653589793*1000/48000)*12000/32768;
        error+=(out[i*2]-ideal)*(out[i*2]-ideal);energy+=out[i*2]*out[i*2];
    }
    assert(sqrt(error/4798)<.002);assert(energy>250);
    uint64_t p;uint32_t c;status(f,0,&p,&c);assert(p>=3199&&p<=3200);
    render(f,out,2);status(f,0,&p,&c);assert(p==3200&&c==1);
    rtg_audio_renderer_reset(f->r);f->channels_count=0;f->mix_count=0;
    mix(f,0,2,0x7fffffffU,1);voice(f,0,32000,1,0);
    wave(f,0,0,pcm,sizeof(pcm),0,3200,0);wf(f->voices+0x1c,1.5f);
    assert(update(f)==0);render(f,out,1000);status(f,0,&p,&c);assert(p==1000&&c==0);
    dispose(f);
}
static void test_rejection_and_bounded_output(void) {
    Fixture *f=fixture();int16_t pcm[]={32767,32767};float out[8];
    voice(f,0,48000,1,0);wave(f,0,0,pcm,sizeof(pcm),0,2,0);
    f->voices[0xb]=6;assert(update(f)==RTG_AUDIO_UNSUPPORTED);
    render(f,out,2);near(out[0],0);assert(rtg_audio_renderer_metrics(f->r).unsupported_parameters==1);
    f->voices[8]=1;f->voices[0xb]=2;
    f->voices[0x78+0x1a]=0;w32(f->voices+0x78+0x14,3);
    assert(update(f)==RTG_AUDIO_INVALID);dispose(f);
    f=fixture();voice(f,0,48000,1,0);wave(f,0,0,pcm,sizeof(pcm),0,2,1);
    wf(f->voices+0x20,4);assert(update(f)==0);render(f,out,4);
    for(unsigned i=0;i<8;i++)near(out[i],1);dispose(f);
}
static void test_ring_generation_and_finite_loop(void) {
    Fixture *f=fixture();int16_t pcm[]={4096,8192,12288,16384};float out[20];
    voice(f,0,48000,1,0);
    for(unsigned s=0;s<4;s++)wave(f,0,s,pcm,sizeof(pcm),s,s+1,0);
    assert(update(f)==0);render(f,out,4);
    for(unsigned s=0;s<4;s++)near(out[s*2],(s+1)*.125f);
    uint64_t p;uint32_t c;status(f,0,&p,&c);assert(p==4&&c==4);
    /* Client has reaped all four slots, so slot zero can be reused. */
    w32(f->voices+0x54,0);wave(f,0,0,pcm,sizeof(pcm),0,2,1);
    w32(f->voices+0x78+0x1c,1); /* exactly one repeat */
    assert(update(f)==0);render(f,out,5);
    near(out[0],.125f);near(out[2],.25f);near(out[4],.125f);near(out[6],.25f);
    near(out[8],0);status(f,0,&p,&c);assert(p==8&&c==5);
    f->voices[0x170]=1;assert(update(f)==0);
    status(f,0,&p,&c);assert(p==8&&c==5); /* ResetVoiceDroppedFlag */
    /* Same id reacquired with a new generation resets counters and position. */
    f->channels_count=0;voice(f,0,48000,1,0);w32(f->voices+4,99);
    wave(f,0,0,pcm,sizeof(pcm),3,4,0);assert(update(f)==0);render(f,out,2);
    near(out[0],.5f);near(out[2],0);status(f,0,&p,&c);assert(p==1&&c==1);
    dispose(f);
}
static void game_reverb(uint8_t e[0xc0]) {
    memset(e,0,0xc0);e[0]=5;e[1]=e[2]=1;w32(e+4,1);
    e[0x20]=0;e[0x21]=1;e[0x26]=2;e[0x27]=3;
    e[0x2c]=e[0x2e]=2;w32(e+0x34,48000);
    /* Exact floats captured in initial-parameters-14.bin at effect+38. */
    const float p[]={-1000,5000,1,.2f,-1045,-310,-602,100,.0046f,.012f,61,.24f};
    memcpy(e+0x38,p,sizeof(p));
}
static Fixture *reverb_fixture(const int16_t *pcm,size_t frames,uint8_t e[0xc0],int loop) {
    Fixture *f=fixture();mix(f,1,4,0,1);
    memset(f->mixes+0x20+MB+0x24,0,24*24*4);
    wf(f->mixes+0x20+MB+0x24+(2*24)*4,1);
    wf(f->mixes+0x20+MB+0x24+(3*24+1)*4,1);
    voice(f,0,48000,2,1);wave(f,0,0,pcm,frames*4,0,(unsigned)frames,loop);
    assert(rtg_audio_renderer_effects(f->r,e,0xc0)==0);assert(update(f)==0);
    return f;
}
static double energy(const float *out,size_t start,size_t end) {
    double sum=0;for(size_t i=start;i<end;i++)sum+=out[i*2]*out[i*2];return sum;
}
static void test_game_reverb_and_tail(const char *capture) {
    uint8_t e[0xc0];game_reverb(e);
    if(capture) {
        FILE *f=fopen(capture,"rb");assert(f);
        assert(fseek(f,0x9e70,SEEK_SET)==0);assert(fread(e,1,sizeof(e),f)==sizeof(e));fclose(f);
        assert(e[0]==5&&e[2]==1&&r32(e+4)==1&&e[0x26]==2&&e[0x27]==3);
    }
    int16_t impulse[]={16384,0};float *out=calloc(96000,sizeof(float));assert(out);
    Fixture *f=reverb_fixture(impulse,1,e,0);
    render(f,out,128);near(out[0],.12f);near(out[1],0);
    /* Native publishes the same effect with state=2. Its tail must survive. */
    e[1]=0;e[0x68]=2;assert(rtg_audio_renderer_effects(f->r,e,sizeof(e))==0);
    assert(update(f)==0);render(f,out+256,48000-128);
    near(out[220*2],0);assert(fabsf(out[221*2])>.01f);
    assert(energy(out,1800,24000)>.00001);
    uint64_t played;uint32_t consumed;status(f,0,&played,&consumed);
    assert(played==1&&consumed==1);assert(rtg_audio_renderer_metrics(f->r).active_effects==1);
    double normal=energy(out,24000,48000);dispose(f);
    game_reverb(e);wf(e+0x40,.1f);f=reverb_fixture(impulse,1,e,0);
    render(f,out,48000);assert(normal>energy(out,24000,48000)*10);dispose(f);
    game_reverb(e);e[2]=0;f=reverb_fixture(impulse,1,e,0);
    render(f,out,512);near(out[0],.5f);assert(energy(out,1,512)==0);dispose(f);
    game_reverb(e);wf(e+0x64,0);f=reverb_fixture(impulse,1,e,0);
    render(f,out,512);near(out[0],0);assert(energy(out,221,512)>.001);dispose(f);
    free(out);
}
static void test_reverb_stability_and_memory(void) {
    uint8_t e[0xc0];game_reverb(e);wf(e+0x40,20);wf(e+0x44,2);
    int16_t pcm[]={8192,-8192,8192,-8192};float out[960];
    Fixture *f=reverb_fixture(pcm,2,e,1);
    for(unsigned n=0;n<500;n++) {
        render(f,out,480);
        for(unsigned i=0;i<960;i++)assert(isfinite(out[i])&&fabsf(out[i])<=1);
    }
    uint64_t addr=(uintptr_t)pcm;
    assert(rtg_audio_renderer_uses_memory(f->r,addr,sizeof(pcm)));
    assert(rtg_audio_renderer_uses_memory(f->r,addr-1,2));
    assert(!rtg_audio_renderer_uses_memory(f->r,addr+sizeof(pcm),1));
    assert(!rtg_audio_renderer_uses_memory(f->r,addr,0));
    assert(rtg_audio_renderer_uses_memory(f->r,UINT64_MAX-2,4));
    f->voices[0xa]=2;assert(update(f)==0);
    assert(rtg_audio_renderer_uses_memory(f->r,addr,sizeof(pcm)));
    f->voices[0xa]=1;assert(update(f)==0);
    assert(!rtg_audio_renderer_uses_memory(f->r,addr,sizeof(pcm)));
    e[0]=4;assert(rtg_audio_renderer_effects(f->r,e,sizeof(e))==RTG_AUDIO_UNSUPPORTED);
    game_reverb(e);wf(e+0x40,NAN);
    assert(rtg_audio_renderer_effects(f->r,e,sizeof(e))==RTG_AUDIO_INVALID);
    dispose(f);
}
static void test_output_checkpoint_generations(void) {
    _Static_assert(sizeof(RtgAudioVoiceCheckpoint)==32,"fixed checkpoint shape");
    Fixture *f=fixture();int16_t pcm[]={8192,8192};float out[480*2];
    RtgAudioVoiceCheckpoint queued[RTG_AUDIO_VOICE_SLOTS+1],current[RTG_AUDIO_VOICE_SLOTS+1];
    voice(f,0,48000,1,0);wave(f,0,0,pcm,sizeof(pcm),0,2,1);assert(update(f)==0);
    rtg_audio_renderer_voice_checkpoint(f->r,current,RTG_AUDIO_VOICE_SLOTS+1);
    assert(current[0].used&&current[0].generation&&current[0].played==0);
    uint64_t first_generation=current[0].generation;uint32_t first_node=current[0].node;
    render(f,out,480);
    rtg_audio_renderer_voice_checkpoint(f->r,queued,RTG_AUDIO_VOICE_SLOTS+1);
    assert(queued[0].played==480&&queued[0].consumed==0);
    assert(queued[0].generation==first_generation&&queued[0].node==first_node);
    assert(!queued[RTG_AUDIO_VOICE_SLOTS].used&&!queued[RTG_AUDIO_VOICE_SLOTS].generation);
    f->voices[0xa]=2;assert(update(f)==0);render(f,out,480);
    rtg_audio_renderer_voice_checkpoint(f->r,current,RTG_AUDIO_VOICE_SLOTS+1);
    assert(current[0].generation==first_generation&&current[0].played==480);
    /* A real SDK new flag can reuse even the same node id; queued counters
     * from the previous acquisition must be distinguishable and rejected. */
    f->channels_count=0;voice(f,0,48000,1,0);wave(f,0,0,pcm,sizeof(pcm),0,2,1);
    assert(update(f)==0);rtg_audio_renderer_voice_checkpoint(f->r,current,RTG_AUDIO_VOICE_SLOTS+1);
    assert(current[0].node==first_node&&current[0].generation>first_generation);
    assert(current[0].played==0&&current[0].consumed==0);
    assert(queued[0].generation!=current[0].generation);
    uint64_t second_generation=current[0].generation;
    f->voices[9]=0;assert(update(f)==0);
    rtg_audio_renderer_voice_checkpoint(f->r,current,RTG_AUDIO_VOICE_SLOTS+1);
    assert(!current[0].used&&!current[0].generation);
    rtg_audio_renderer_reset(f->r);f->channels_count=0;
    mix(f,0,2,0x7fffffffU,1);voice(f,0,48000,1,0);wave(f,0,0,pcm,sizeof(pcm),0,2,1);
    assert(update(f)==0);rtg_audio_renderer_voice_checkpoint(f->r,current,RTG_AUDIO_VOICE_SLOTS+1);
    assert(current[0].generation>second_generation&&current[0].played==0);
    uint64_t third_generation=current[0].generation;
    w32(f->voices+4,99);assert(update(f)==0);
    rtg_audio_renderer_voice_checkpoint(f->r,current,RTG_AUDIO_VOICE_SLOTS+1);
    assert(current[0].generation>third_generation&&current[0].node==99);
    rtg_audio_renderer_voice_checkpoint(NULL,current,RTG_AUDIO_VOICE_SLOTS+1);
    assert(!current[0].used&&!current[0].generation);dispose(f);
}
int main(int argc,char **argv) {
    test_pcm_and_end_exclusive();test_loop_pause_stop_and_reuse();
    test_stereo_multivoice_mix_delta();test_resampling_sine_and_pitch();
    test_rejection_and_bounded_output();test_ring_generation_and_finite_loop();
    test_game_reverb_and_tail(argc>1?argv[1]:NULL);test_reverb_stability_and_memory();
    test_output_checkpoint_generations();
    puts("PASS PCM, resampling/pitch, queue/loop/lifecycle, submix routing, captured I3dl2 routing/dry/early/tail/decay/bypass/stability, pool references, consumed-block checkpoints/generations");
    return 0;
}
