#include "rtg_audio_renderer.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* These layouts are from this game's SDK serializers, not the older libnx
 * VoiceInfo layout. In particular, VoiceInfo is 0x188 and Mix is a delta. */
enum { MAX_VOICES = RTG_AUDIO_VOICE_SLOTS, MAX_CHANNELS = 256, MAX_MIXES = 32,
       MIX_CHANNELS = 24, MAX_SINKS = 8, WAVE_COUNT = 4,
       MAX_EFFECTS = 64, BLOCK = 64, HISTORY = 24001, COMB_MAX = 4096,
       ALLPASS_MAX = 1024, COMBS = 8, ALLPASSES = 2 };
enum { CHANNEL_BYTES = 0x70, VOICE_BYTES = 0x188, WAVE_BYTES = 0x38,
       MIX_BYTES = 0x930, SINK_BYTES = 0x140 };

typedef struct {
    const int16_t *pcm;
    uint64_t bytes;
    uint32_t start, end, loop_start, loop_end;
    int32_t loop_count;
    uint32_t loops;
    bool valid, looping, end_of_stream;
} Wave;

typedef struct {
    bool used, unsupported;
    uint32_t node, channels, rate, state, mix, channel_ids[2];
    uint16_t flags;
    float pitch, volume, mix_gain[2][MIX_CHANNELS];
    Wave waves[WAVE_COUNT];
    unsigned head;
    double position, played;
    uint32_t consumed;
    uint64_t generation;
    bool positioned;
} Voice;

typedef struct { bool used; float mix[MIX_CHANNELS]; } Channel;
typedef struct {
    bool used;
    uint32_t channels, destination;
    float volume, matrix[MIX_CHANNELS][MIX_CHANNELS];
} Mix;

/* Host approximation of I3dl2: delayed early reflections plus a stereo
 * Schroeder network with frequency-dependent RT60 and diffusion. This is
 * deliberately not described as the original DSP's bit-exact reverberator.
 * Storage is allocated in effects(), never in render(). */
typedef struct {
    float history[2][HISTORY];
    float comb[2][COMBS][COMB_MAX], low[2][COMBS];
    float allpass[2][ALLPASSES][ALLPASS_MAX], input_low[2];
    unsigned history_pos, comb_pos[2][COMBS], ap_pos[2][ALLPASSES];
} ReverbStorage;
typedef struct {
    bool used, enabled;
    unsigned mix, order, count, input[2], output[2], early, late;
    unsigned comb_len[2][COMBS], ap_len[2][ALLPASSES];
    float dry, room, room_hf, early_gain, late_gain, alpha, diffusion;
    float feedback[2][COMBS], feedback_hf[2][COMBS];
    uint8_t parameters[0x30];
    ReverbStorage *dsp;
} Effect;

struct RtgAudioRenderer {
    Voice voices[MAX_VOICES];
    Channel channels[MAX_CHANNELS];
    Mix mixes[MAX_MIXES];
    float sink_gain[MIX_CHANNELS][2];
    uint8_t route_state[MAX_MIXES];
    unsigned mix_order[MAX_MIXES], mix_count;
    unsigned effect_order[MAX_EFFECTS], effect_count;
    Effect effects[MAX_EFFECTS];
    float bus[MAX_MIXES][MIX_CHANNELS][BLOCK];
    uint64_t generation_serial;
    RtgAudioRendererMetrics metrics;
};

static uint32_t u32(const void *p) { uint32_t x; memcpy(&x,p,4); return x; }
static uint64_t u64(const void *p) { uint64_t x; memcpy(&x,p,8); return x; }
static uint16_t u16(const void *p) { uint16_t x; memcpy(&x,p,2); return x; }
static float f32(const void *p) { float x; memcpy(&x,p,4); return x; }
static void put32(void *p, uint32_t v) { memcpy(p,&v,4); }
static void put64(void *p, uint64_t v) { memcpy(p,&v,8); }

RtgAudioRenderer *rtg_audio_renderer_create(void) {
    return calloc(1,sizeof(RtgAudioRenderer));
}
void rtg_audio_renderer_destroy(RtgAudioRenderer *r) {
    if(r) for(unsigned i=0;i<MAX_EFFECTS;i++) free(r->effects[i].dsp);
    free(r);
}
void rtg_audio_renderer_reset(RtgAudioRenderer *r) {
    if (r) {
        uint64_t generation_serial=r->generation_serial;
        for(unsigned i=0;i<MAX_EFFECTS;i++) free(r->effects[i].dsp);
        memset(r,0,sizeof(*r));
        r->generation_serial=generation_serial;
    }
}

static int failure(RtgAudioRenderer *r, int code) {
    if (code == RTG_AUDIO_UNSUPPORTED) r->metrics.unsupported_parameters++;
    else r->metrics.invalid_parameters++;
    return code;
}

static int mix_route(RtgAudioRenderer *r, unsigned id) {
    if (id >= MAX_MIXES) return RTG_AUDIO_INVALID;
    if (r->route_state[id] == 2) return RTG_AUDIO_OK;
    if (r->route_state[id] == 1) return RTG_AUDIO_INVALID; /* cycle */
    r->route_state[id] = 1;
    const Mix *m = &r->mixes[id];
    if (m->used) {
        if (id && m->destination != 0x7fffffffU) {
            int rc = mix_route(r,m->destination);
            if (rc) return rc;
        }
        r->mix_order[r->mix_count++]=id; /* reverse this for source-first order */
    }
    r->route_state[id] = 2;
    return RTG_AUDIO_OK;
}

static int rebuild_routes(RtgAudioRenderer *r) {
    r->mix_count=0;
    memset(r->route_state,0,sizeof(r->route_state));
    for (unsigned m=0;m<MAX_MIXES;m++) {
        int rc=mix_route(r,m);
        if (rc) return rc;
    }
    for (unsigned i=0;i<MAX_VOICES;i++) {
        Voice *v=&r->voices[i];
        memset(v->mix_gain,0,sizeof(v->mix_gain));
        if (!v->used || v->unsupported || v->mix==0x7fffffffU) continue;
        if (v->mix>=MAX_MIXES) return RTG_AUDIO_INVALID;
        for (unsigned c=0;c<v->channels;c++) {
            const Channel *channel=&r->channels[v->channel_ids[c]];
            if (!channel->used) continue;
            for (unsigned d=0;d<r->mixes[v->mix].channels;d++)
                v->mix_gain[c][d]=v->volume*channel->mix[d];
        }
    }
    for(unsigned i=0;i<r->effect_count;i++) {
        Effect *e=&r->effects[r->effect_order[i]];
        if(!r->mixes[e->mix].used) return RTG_AUDIO_INVALID;
        for(unsigned c=0;c<e->count;c++)
            if(e->input[c]>=r->mixes[e->mix].channels ||
               e->output[c]>=r->mixes[e->mix].channels) return RTG_AUDIO_INVALID;
    }
    return RTG_AUDIO_OK;
}

static float mb_gain(float mb) { return powf(10.0f,mb/2000.0f); }
static int reverb_parameters(Effect *e,const uint8_t *p) {
    float hf=f32(p+0x38),reference=f32(p+0x3c),decay=f32(p+0x40);
    float ratio=f32(p+0x44),room=f32(p+0x48),early=f32(p+0x4c);
    float late=f32(p+0x50),diffusion=f32(p+0x54),early_s=f32(p+0x58);
    float late_s=f32(p+0x5c),density=f32(p+0x60),dry=f32(p+0x64);
    for(unsigned i=0;i<12;i++) if(!isfinite(f32(p+0x38+i*4))) return RTG_AUDIO_INVALID;
    if(hf< -10000||hf>0||reference<20||reference>24000||decay<.1f||decay>20||
       ratio<.1f||ratio>2||room< -10000||room>0||early< -10000||early>2000||
       late< -10000||late>2000||diffusion<0||diffusion>100||early_s<0||early_s>.3f||
       late_s<0||late_s>.1f||density<0||density>100||dry<0||dry>2)
        return RTG_AUDIO_INVALID;
    e->dry=dry;e->room=mb_gain(room);e->room_hf=mb_gain(hf);
    e->early_gain=mb_gain(early);e->late_gain=mb_gain(late);
    e->early=(unsigned)lroundf(early_s*48000);
    e->late=(unsigned)lroundf((early_s+late_s)*48000);
    e->alpha=1-expf(-6.283185307179586f*reference/48000);
    e->diffusion=.15f+.55f*diffusion/100;
    static const unsigned base[COMBS]={1151,1301,1451,1601,1753,1901,2053,2203};
    for(unsigned c=0;c<2;c++) {
        for(unsigned k=0;k<COMBS;k++) {
            unsigned n=(unsigned)lroundf((base[k]+23*c)*(.6f+.4f*density/100));
            e->comb_len[c][k]=n;
            e->feedback[c][k]=powf(10,-3.0f*n/(48000*decay));
            e->feedback_hf[c][k]=powf(10,-3.0f*n/(48000*decay*ratio));
            e->dsp->comb_pos[c][k]%=n;
        }
        e->ap_len[c][0]=149+23*c;e->ap_len[c][1]=337+23*c;
    }
    memcpy(e->parameters,p+0x38,sizeof(e->parameters));
    return RTG_AUDIO_OK;
}

int rtg_audio_renderer_effects(RtgAudioRenderer *r,const uint8_t *data,size_t bytes) {
    if(!r) return RTG_AUDIO_INVALID;
    if((bytes&&!data)||bytes%0xc0||bytes/0xc0>MAX_EFFECTS)
        return failure(r,RTG_AUDIO_INVALID);
    r->effect_count=0;
    for(unsigned i=0;i<MAX_EFFECTS;i++) {
        Effect *e=&r->effects[i];
        const uint8_t *p=i<bytes/0xc0 ? data+i*0xc0 : NULL;
        if(!p||!p[0]) { free(e->dsp);memset(e,0,sizeof(*e));continue; }
        if(p[0]!=5) return failure(r,RTG_AUDIO_UNSUPPORTED);
        unsigned count=u16(p+0x2e),mix=u32(p+4),rate=u32(p+0x34);
        if(count<1||count>2) return failure(r,RTG_AUDIO_UNSUPPORTED);
        if(mix>=MAX_MIXES||!rate||rate>192000||p[0x68]>2)
            return failure(r,RTG_AUDIO_INVALID);
        bool reset=!e->used||p[1]||p[0x68]==0;
        if(!e->dsp) { e->dsp=calloc(1,sizeof(*e->dsp));if(!e->dsp)return failure(r,RTG_AUDIO_INVALID); }
        else if(reset) memset(e->dsp,0,sizeof(*e->dsp));
        e->used=true;e->enabled=p[2]!=0;e->count=count;e->mix=mix;e->order=u32(p+0x18);
        for(unsigned c=0;c<count;c++) {
            e->input[c]=p[0x20+c];e->output[c]=p[0x26+c];
            if(e->input[c]>=MIX_CHANNELS||e->output[c]>=MIX_CHANNELS)
                return failure(r,RTG_AUDIO_INVALID);
        }
        if(reset||p[0x68]==1||memcmp(e->parameters,p+0x38,sizeof(e->parameters))) {
            int rc=reverb_parameters(e,p);if(rc)return failure(r,rc);
        }
        unsigned j=r->effect_count++;
        while(j && r->effects[r->effect_order[j-1]].order>e->order) {
            r->effect_order[j]=r->effect_order[j-1];j--;
        }
        r->effect_order[j]=i;
    }
    return RTG_AUDIO_OK;
}

static float clean(float x) { return fabsf(x)<1e-20f?0:x; }
static void reverb_process(Effect *e,const float in[2],float out[2]) {
    if(!e->enabled) { out[0]=in[0];out[1]=in[1];return; }
    ReverbStorage *s=e->dsp;
    for(unsigned c=0;c<e->count;c++) {
        float x=in[c];s->input_low[c]+=e->alpha*(x-s->input_low[c]);
        float filtered=(s->input_low[c]+e->room_hf*(x-s->input_low[c]))*e->room;
        s->history[c][s->history_pos]=clean(filtered);
        float early=s->history[c][(s->history_pos+HISTORY-e->early)%HISTORY];
        float excitation=s->history[c][(s->history_pos+HISTORY-e->late)%HISTORY];
        float late=0;
        for(unsigned k=0;k<COMBS;k++) {
            unsigned j=s->comb_pos[c][k];float y=s->comb[c][k][j];
            s->low[c][k]+=e->alpha*(y-s->low[c][k]);
            float feedback=e->feedback[c][k]*s->low[c][k]
                +e->feedback_hf[c][k]*(y-s->low[c][k]);
            s->comb[c][k][j]=clean(excitation+feedback);
            /* Positions are normalized when parameters change. Only the next
             * slot can wrap here; avoid a variable integer divide per sample. */
            s->comb_pos[c][k]=j+1==e->comb_len[c][k]?0:j+1;late+=y/COMBS;
        }
        for(unsigned k=0;k<ALLPASSES;k++) {
            unsigned j=s->ap_pos[c][k];float y=s->allpass[c][k][j];
            float t=late+e->diffusion*y;
            s->allpass[c][k][j]=clean(t);late=y-e->diffusion*t;
            s->ap_pos[c][k]=j+1==e->ap_len[c][k]?0:j+1;
        }
        out[c]=e->dry*x+e->early_gain*early+e->late_gain*late;
    }
    s->history_pos=(s->history_pos+1)%HISTORY;
}

static int read_wave(Wave *w, const uint8_t *p, unsigned channels) {
    const uint64_t address=u64(p), bytes=u64(p+8);
    uint32_t start=u32(p+0x10),end=u32(p+0x14);
    if (!address || !bytes) return RTG_AUDIO_OK; /* unused slot */
    if ((address&1) || start>=end || end>bytes/(channels*2U))
        return RTG_AUDIO_INVALID;
    uint32_t loop_start=u32(p+0x30),loop_end=u32(p+0x34);
    int32_t loop_count=(int32_t)u32(p+0x1c);
    if (p[0x18] && (loop_start>=loop_end || loop_end>bytes/(channels*2U)
                    || loop_count < -1)) return RTG_AUDIO_INVALID;
    *w=(Wave){ .pcm=(const int16_t *)(uintptr_t)address,.bytes=bytes,
        .start=start,.end=end,.loop_start=loop_start,.loop_end=loop_end,
        .loop_count=loop_count,.valid=true,.looping=p[0x18]!=0,
        .end_of_stream=p[0x19]!=0 };
    return RTG_AUDIO_OK;
}

int rtg_audio_renderer_update(RtgAudioRenderer *r,
    const uint8_t *channels,size_t channel_bytes,
    const uint8_t *voices,size_t voice_bytes,
    const uint8_t *mixes,size_t mix_bytes,
    const uint8_t *sinks,size_t sink_bytes) {
    if (!r) return RTG_AUDIO_INVALID;
    if ((channel_bytes&&!channels)||(voice_bytes&&!voices)||
        (mix_bytes&&!mixes)||(sink_bytes&&!sinks)||
        channel_bytes%CHANNEL_BYTES || voice_bytes%VOICE_BYTES ||
        sink_bytes%SINK_BYTES || channel_bytes/CHANNEL_BYTES>MAX_CHANNELS ||
        voice_bytes/VOICE_BYTES>MAX_VOICES || sink_bytes/SINK_BYTES>MAX_SINKS)
        return failure(r,RTG_AUDIO_INVALID);
    if (mix_bytes && (mix_bytes<0x20 || u32(mixes)!=0xcafebabeU ||
        u32(mixes+4)>MAX_MIXES ||
        mix_bytes!=0x20+(size_t)u32(mixes+4)*MIX_BYTES))
        return failure(r,RTG_AUDIO_INVALID);

    r->metrics.updates++;
    int result=RTG_AUDIO_OK;
    memset(r->channels,0,sizeof(r->channels));
    for (size_t off=0;off<channel_bytes;off+=CHANNEL_BYTES) {
        const uint8_t *p=channels+off;
        if (!p[0x64]) continue;
        unsigned id=u32(p);
        if (id>=MAX_CHANNELS) return failure(r,RTG_AUDIO_INVALID);
        Channel *c=&r->channels[id];
        c->used=true;
        for (unsigned j=0;j<MIX_CHANNELS;j++) {
            c->mix[j]=f32(p+4+j*4);
            if (!isfinite(c->mix[j])) return failure(r,RTG_AUDIO_INVALID);
        }
    }
    for (size_t off=0x20;mix_bytes && off<mix_bytes;off+=MIX_BYTES) {
        const uint8_t *p=mixes+off;
        unsigned id=u32(p+0x10);
        if (id>=MAX_MIXES) return failure(r,RTG_AUDIO_INVALID);
        Mix *m=&r->mixes[id];
        memset(m,0,sizeof(*m));
        m->used=p[0xc]!=0;
        if (!m->used) continue;
        m->channels=u32(p+8);
        m->destination=u32(p+0x924);
        m->volume=f32(p);
        if (!m->channels || m->channels>MIX_CHANNELS ||
            !isfinite(m->volume) || (id && m->destination>=MAX_MIXES &&
            m->destination!=0x7fffffffU)) return failure(r,RTG_AUDIO_INVALID);
        if (u32(p+0x928)!=0xffffffffU) return failure(r,RTG_AUDIO_UNSUPPORTED);
        for (unsigned c=0;c<MIX_CHANNELS;c++)
            for (unsigned d=0;d<MIX_CHANNELS;d++) {
                m->matrix[c][d]=f32(p+0x24+(c*MIX_CHANNELS+d)*4);
                if (!isfinite(m->matrix[c][d])) return failure(r,RTG_AUDIO_INVALID);
            }
    }
    memset(r->sink_gain,0,sizeof(r->sink_gain));
    for (size_t off=0;off<sink_bytes;off+=SINK_BYTES) {
        const uint8_t *p=sinks+off;
        if (!p[1]) continue;
        if (p[0]!=1 || p[0x12b]) return failure(r,RTG_AUDIO_UNSUPPORTED);
        unsigned count=u32(p+0x120);
        if (!count || count>2) return failure(r,RTG_AUDIO_UNSUPPORTED);
        for (unsigned s=0;s<count;s++) {
            unsigned id=p[0x124+s];
            if (id>=MIX_CHANNELS) return failure(r,RTG_AUDIO_INVALID);
            if (count==1) r->sink_gain[id][0]+=1, r->sink_gain[id][1]+=1;
            else r->sink_gain[id][s]+=1;
        }
    }
    bool seen[MAX_VOICES]={0};
    for (size_t off=0;off<voice_bytes;off+=VOICE_BYTES) {
        const uint8_t *p=voices+off;
        if (!p[9]) continue;
        unsigned id=u32(p);
        if (id>=MAX_VOICES || seen[id]) return failure(r,RTG_AUDIO_INVALID);
        seen[id]=true;
        Voice *v=&r->voices[id];
        if (!v->used || v->node!=u32(p+4) || p[8]) {
            memset(v,0,sizeof(*v));
            v->generation=++r->generation_serial;
            if(!v->generation)v->generation=++r->generation_serial;
            v->head=u16(p+0x58);
            if (v->head>=WAVE_COUNT) return failure(r,RTG_AUDIO_INVALID);
        }
        v->used=true; v->node=u32(p+4); v->state=p[0xa];
        v->rate=u32(p+0xc); v->channels=u32(p+0x18);
        v->pitch=f32(p+0x1c); v->volume=f32(p+0x20);
        v->mix=u32(p+0x70); v->flags=u16(p+0x174);
        if (!v->rate || v->rate>192000 || !isfinite(v->pitch) ||
            v->pitch<0 || v->pitch>16 || !isfinite(v->volume) ||
            v->state>2 || u32(p+0x54)>WAVE_COUNT)
            return failure(r,RTG_AUDIO_INVALID);
        v->unsupported=p[0xb]!=2 || !v->channels || v->channels>2 ||
            u32(p+0x74)!=0xffffffffU || (v->flags&~1U)!=0 || p[0x171]!=0 ||
            p[0x24]!=0 || p[0x3c]!=0;
        if (v->unsupported) { result=RTG_AUDIO_UNSUPPORTED; continue; }
        for (unsigned c=0;c<v->channels;c++) {
            v->channel_ids[c]=u32(p+0x158+c*4);
            if (v->channel_ids[c]>=MAX_CHANNELS) return failure(r,RTG_AUDIO_INVALID);
        }
        for (unsigned w=0;w<WAVE_COUNT;w++) {
            const uint8_t *wp=p+0x78+w*WAVE_BYTES;
            if (wp[0x1a]) continue; /* only accept SDK's unsent buffers */
            int rc=read_wave(&v->waves[w],wp,v->channels);
            if (rc) return failure(r,rc);
            if (w==v->head) v->positioned=false;
        }
        if (v->state==1) {
            for (unsigned n=0;n<WAVE_COUNT;n++) {
                if (v->waves[v->head].valid) {
                    v->waves[v->head].valid=false;
                    v->consumed++;
                    v->head=(v->head+1)%WAVE_COUNT;
                } else break;
            }
            v->positioned=false;
            v->played=0;
        }
        /* +170 resets the dropped flag, not the played-sample counter. */
    }
    for (unsigned i=0;i<MAX_VOICES;i++) if (!seen[i])
        memset(&r->voices[i],0,sizeof(r->voices[i]));
    int rc=rebuild_routes(r);
    if (rc) return failure(r,rc);
    return result ? failure(r,result) : RTG_AUDIO_OK;
}

static void consume_wave(Voice *v) {
    Wave *w=&v->waves[v->head];
    if (w->end_of_stream) v->played=0;
    w->valid=false;
    v->consumed++;
    v->head=(v->head+1)%WAVE_COUNT;
    v->positioned=false;
}

static bool position_voice(Voice *v) {
    Wave *w=&v->waves[v->head];
    if (!w->valid) return false;
    if (!v->positioned) { v->position=w->start; v->positioned=true; }
    return true;
}

static void advance_voice(RtgAudioRenderer *r, Voice *v, double step) {
    while (step>0 && position_voice(v)) {
        Wave *w=&v->waves[v->head];
        double end=w->loops ? w->loop_end : w->end;
        double remaining=end-v->position;
        double take=step<remaining ? step : remaining;
        double previous=v->played;
        v->position+=take;
        v->played+=take;
        r->metrics.source_frames+=(uint64_t)v->played-(uint64_t)previous;
        step-=take;
        if (v->position<end) break;
        if (w->looping && (w->loop_count<0 || w->loops<(uint32_t)w->loop_count)) {
            w->loops++;
            v->position=w->loop_start;
            if (v->flags&1) v->played=0;
        } else consume_wave(v);
    }
}

void rtg_audio_renderer_render(RtgAudioRenderer *r, float *out, size_t frames) {
    if (!out || frames>SIZE_MAX/(2*sizeof(*out))) return;
    memset(out,0,frames*2*sizeof(*out));
    if (!r) return;
    for(size_t first=0;first<frames;first+=BLOCK) {
        size_t count=frames-first<BLOCK?frames-first:BLOCK;
        for(unsigned m=0;m<r->mix_count;m++)
            memset(r->bus[r->mix_order[m]],0,sizeof(r->bus[0]));
        for (unsigned i=0;i<MAX_VOICES;i++) {
            Voice *v=&r->voices[i];
            if (!v->used || v->unsupported || v->state!=0) continue;
            const double step=(double)v->rate*v->pitch/48000.0;
            for (size_t f=0;f<count;f++) {
                if (!position_voice(v)) break;
                Wave *w=&v->waves[v->head];
                uint32_t index=(uint32_t)v->position;
                uint32_t end=w->loops ? w->loop_end : w->end;
                uint32_t next=index+1<end ? index+1 : index;
                float fraction=(float)(v->position-index);
                if(v->mix<MAX_MIXES && r->mixes[v->mix].used)
                    for (unsigned c=0;c<v->channels;c++) {
                        float a=w->pcm[(size_t)index*v->channels+c]/32768.0f;
                        float b=w->pcm[(size_t)next*v->channels+c]/32768.0f;
                        float sample=a+(b-a)*fraction;
                        for(unsigned d=0;d<r->mixes[v->mix].channels;d++)
                            r->bus[v->mix][d][f]+=sample*v->mix_gain[c][d];
                    }
                advance_voice(r,v,step);
            }
        }
        for(unsigned order=r->mix_count;order>0;order--) {
            unsigned id=r->mix_order[order-1];Mix *m=&r->mixes[id];
            for(unsigned n=0;n<r->effect_count;n++) {
                Effect *e=&r->effects[r->effect_order[n]];
                if(e->mix!=id)continue;
                for(size_t f=0;f<count;f++) {
                    float input[2]={0},output[2]={0};
                    for(unsigned c=0;c<e->count;c++)input[c]=r->bus[id][e->input[c]][f];
                    reverb_process(e,input,output);
                    for(unsigned c=0;c<e->count;c++)r->bus[id][e->output[c]][f]=output[c];
                }
            }
            if(id==0) {
                for(unsigned c=0;c<m->channels;c++)
                    for(unsigned s=0;s<2;s++) {
                        float gain=m->volume*r->sink_gain[c][s];if(gain==0)continue;
                        for(size_t f=0;f<count;f++)out[(first+f)*2+s]+=r->bus[id][c][f]*gain;
                    }
            } else if(m->destination<MAX_MIXES) {
                unsigned dst=m->destination;
                for(unsigned c=0;c<m->channels;c++)
                    for(unsigned d=0;d<r->mixes[dst].channels;d++) {
                        float gain=m->volume*m->matrix[c][d];if(gain==0)continue;
                        for(size_t f=0;f<count;f++)r->bus[dst][d][f]+=r->bus[id][c][f]*gain;
                    }
            }
        }
    }
    r->metrics.rendered_frames+=frames;
    for (size_t f=0;f<frames;f++) {
        bool nonzero=false;
        for (unsigned c=0;c<2;c++) {
            float *x=&out[f*2+c];
            if (*x>1) *x=1;
            if (*x< -1) *x=-1;
            float a=fabsf(*x);
            if (a>r->metrics.peak) r->metrics.peak=a;
            if (a>0.000001f) nonzero=true;
        }
        if (nonzero) r->metrics.nonzero_frames++;
    }
}

void rtg_audio_renderer_voice_status(const RtgAudioRenderer *r,
    uint8_t *out,size_t bytes) {
    if (!out) return;
    memset(out,0,bytes);
    if (!r) return;
    size_t count=bytes/16;
    if (count>MAX_VOICES) count=MAX_VOICES;
    for (size_t i=0;i<count;i++) {
        const Voice *v=&r->voices[i];
        put64(out+i*16,(uint64_t)v->played);
        put32(out+i*16+8,v->consumed);
        put32(out+i*16+12,v->unsupported?1:0);
    }
}

void rtg_audio_renderer_voice_checkpoint(const RtgAudioRenderer *r,
    RtgAudioVoiceCheckpoint *out,size_t count) {
    if(!out || count>SIZE_MAX/sizeof(*out))return;
    memset(out,0,count*sizeof(*out));
    if(!r)return;
    if(count>MAX_VOICES)count=MAX_VOICES;
    for(size_t i=0;i<count;i++) {
        const Voice *v=&r->voices[i];
        out[i].played=(uint64_t)v->played;
        out[i].consumed=v->consumed;
        out[i].dropped=v->unsupported?1:0;
        out[i].node=v->node;
        out[i].used=v->used?1:0;
        out[i].generation=v->generation;
    }
}

RtgAudioRendererMetrics rtg_audio_renderer_metrics(const RtgAudioRenderer *r) {
    RtgAudioRendererMetrics m={0};
    if (!r) return m;
    m=r->metrics;
    for(unsigned i=0;i<r->effect_count;i++) if(r->effects[r->effect_order[i]].enabled)
        m.active_effects++;
    for (unsigned i=0;i<MAX_VOICES;i++) {
        const Voice *v=&r->voices[i];
        if (!v->used || v->unsupported) continue;
        bool any=false;
        for (unsigned w=0;w<WAVE_COUNT;w++) if (v->waves[w].valid)
            m.queued_wave_buffers++,any=true;
        if (any && v->state==0) m.active_voices++;
    }
    return m;
}

bool rtg_audio_renderer_uses_memory(const RtgAudioRenderer *r,uint64_t address,uint64_t bytes) {
    if(!r||!bytes)return false;
    if(address>UINT64_MAX-bytes)return true;
    uint64_t end=address+bytes;
    for(unsigned i=0;i<MAX_VOICES;i++)for(unsigned j=0;j<WAVE_COUNT;j++) {
        const Wave *w=&r->voices[i].waves[j];if(!w->valid)continue;
        uint64_t start=(uint64_t)(uintptr_t)w->pcm;
        if(start>UINT64_MAX-w->bytes)return true;
        if(start<end && address<start+w->bytes)return true;
    }
    return false;
}
