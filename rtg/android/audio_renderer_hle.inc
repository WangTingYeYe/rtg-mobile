/* REV service boundary for the RTG SDK. Voice/mix/pool objects and buffer
 * queues are owned by the original SDK; only the hardware service is replaced.
 * The AAudio callback consumes source samples, never a wall-clock simulation. */
#ifndef RTG_AUDIO_RENDERER_HLE_TEST
#include "../host/hle.h"
#include <aaudio/AAudio.h>
#endif
#include "rtg_audio_renderer.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void hle_bind_system_event(void *,int);
void hle_signal_system_event(void *);

enum { AR_BLOCK_FRAMES=240, AR_BLOCK_COUNT=3, AR_CAPTURE_FRAMES=48000*10 };
_Static_assert(ATOMIC_LLONG_LOCK_FREE==2&&ATOMIC_POINTER_LOCK_FREE==2,
    "AAudio callback requires lock-free counters and buffer publication");
typedef struct {
    float pcm[AR_BLOCK_FRAMES*2];
    RtgAudioVoiceCheckpoint voices[RTG_AUDIO_VOICE_SLOTS];
} ArPcmBlock;
typedef struct {
    float *save,*discard;
    int request;
    unsigned session;
    char path[1400];
} ArCaptureWork;
static struct {
    /* Only ordinary guest/producer threads take these locks. The AAudio
     * callback never locks, allocates, touches guest PCM, or calls the core. */
    pthread_mutex_t control,mutex;
    pthread_cond_t changed;
    RtgAudioRenderer *mixer;
    AAudioStream *stream;
    void *event;
    pthread_t notifier;
    sem_t notify;
    int opened,want_running,notifier_started,recovery_failed;
    _Atomic int running,closing,pending,muted,error,parameter_error,notify_ready;
    _Atomic uintptr_t generation;
    _Atomic float volume;
    _Atomic uint64_t callbacks,frames,nonzero,underrun_blocks,underrun_frames,callback_epoch;
    ArPcmBlock blocks[AR_BLOCK_COUNT];
    _Atomic uint64_t write_block,read_block,consumed_frames;
    unsigned read_offset; /* callback-owned; retained through stream recovery */
    uint64_t status_block,event_epoch,status_max_played;
    RtgAudioVoiceCheckpoint consumed[RTG_AUDIO_VOICE_SLOTS],current[RTG_AUDIO_VOICE_SLOTS];
    uint8_t sdk_handle[64];
    unsigned updates,recoveries,session;
    /* Pointer exchange transfers exclusive ownership. active/position belong
     * solely to the callback until AAudio has quiesced during Close. */
    _Atomic(float *) capture_pending,capture_ready,capture_retired;
    float *capture_active;
    size_t capture_position;
    _Atomic size_t capture_progress;
} ar={.control=PTHREAD_MUTEX_INITIALIZER,.mutex=PTHREAD_MUTEX_INITIALIZER,
      .changed=PTHREAD_COND_INITIALIZER,.volume=1.0f};

static void ar_notify(void) {
    if(atomic_load_explicit(&ar.notify_ready,memory_order_acquire)&&
       !atomic_exchange_explicit(&ar.pending,1,memory_order_acq_rel))sem_post(&ar.notify);
}
static int ar_can_play(void) {
    return atomic_load(&ar.running)&&!atomic_load(&ar.muted)&&!atomic_load(&ar.closing)&&
        !atomic_load(&ar.error)&&!atomic_load(&ar.parameter_error);
}
static void ar_capture_pcm(const float *pcm,size_t frames) {
    /* A full retire slot defers a replacement request; no callback-side free.
     * Main/producer threads atomically take retired and ready ownership. */
    if(!atomic_load_explicit(&ar.capture_retired,memory_order_acquire)){
        float *next=atomic_exchange_explicit(&ar.capture_pending,NULL,memory_order_acq_rel);
        if(next){
            if(ar.capture_active)atomic_store_explicit(&ar.capture_retired,ar.capture_active,memory_order_release);
            ar.capture_active=next;ar.capture_position=0;
        }
    }
    if(ar.capture_active){
        size_t n=frames,remaining=AR_CAPTURE_FRAMES-ar.capture_position;if(n>remaining)n=remaining;
        memcpy(ar.capture_active+ar.capture_position*2,pcm,n*2*sizeof(float));ar.capture_position+=n;
        atomic_store_explicit(&ar.capture_progress,ar.capture_position,memory_order_relaxed);
        if(ar.capture_position==AR_CAPTURE_FRAMES&&!atomic_load_explicit(&ar.capture_ready,memory_order_acquire)){
            atomic_store_explicit(&ar.capture_ready,ar.capture_active,memory_order_release);
            ar.capture_active=NULL;ar.capture_position=0;
        }
    }
}
static aaudio_data_callback_result_t ar_render(AAudioStream *stream,void *context,void *data,int32_t frames) {
    (void)stream;
    if(frames<=0)return AAUDIO_CALLBACK_RESULT_CONTINUE;
    float *out=data;memset(out,0,(size_t)frames*2*sizeof(float));
    if((uintptr_t)context!=atomic_load(&ar.generation))return AAUDIO_CALLBACK_RESULT_CONTINUE;
    atomic_fetch_add_explicit(&ar.callbacks,1,memory_order_relaxed);
    atomic_fetch_add_explicit(&ar.frames,(uint32_t)frames,memory_order_relaxed);
    int playing=ar_can_play();size_t copied=0;
    if(playing){
        uint64_t read=atomic_load_explicit(&ar.read_block,memory_order_relaxed);
        while(copied<(size_t)frames){
            uint64_t write=atomic_load_explicit(&ar.write_block,memory_order_acquire);
            if(read==write)break;
            size_t n=(size_t)frames-copied,available=AR_BLOCK_FRAMES-ar.read_offset;if(n>available)n=available;
            memcpy(out+copied*2,ar.blocks[read%AR_BLOCK_COUNT].pcm+ar.read_offset*2,n*2*sizeof(float));
            copied+=n;ar.read_offset+=(unsigned)n;
            atomic_fetch_add_explicit(&ar.consumed_frames,n,memory_order_relaxed);
            if(ar.read_offset==AR_BLOCK_FRAMES){
                ar.read_offset=0;read++;
                /* The producer commits this slot's checkpoint before reusing
                 * it. Partial consumption never publishes future counters. */
                atomic_store_explicit(&ar.read_block,read,memory_order_release);
            }
        }
        if(copied<(size_t)frames){
            atomic_fetch_add_explicit(&ar.underrun_blocks,1,memory_order_relaxed);
            atomic_fetch_add_explicit(&ar.underrun_frames,(size_t)frames-copied,memory_order_relaxed);
        }
    }
    float volume=atomic_load_explicit(&ar.volume,memory_order_relaxed);uint64_t nonzero=0;
    for(size_t i=0;i<(size_t)frames*2;i++){out[i]*=volume;nonzero+=fabsf(out[i])>0.000001f;}
    atomic_fetch_add_explicit(&ar.nonzero,nonzero,memory_order_relaxed);
    /* Capture the exact device buffer, including real underruns and mute. */
    ar_capture_pcm(out,(size_t)frames);
    if(playing){atomic_fetch_add_explicit(&ar.callback_epoch,1,memory_order_release);ar_notify();}
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/* All helpers below run under control on ordinary threads. A consumed PCM
 * slot remains immutable until its checkpoint has been copied here. */
static void ar_commit_consumed_locked(void) {
    uint64_t consumed=atomic_load_explicit(&ar.read_block,memory_order_acquire);
    while(ar.status_block<consumed){
        memcpy(ar.consumed,ar.blocks[ar.status_block%AR_BLOCK_COUNT].voices,sizeof ar.consumed);
        ar.status_block++;
    }
}
static void ar_fill_locked(void) {
    if(!ar.opened||!ar.want_running||atomic_load(&ar.closing)||atomic_load(&ar.error)||atomic_load(&ar.parameter_error))return;
    pthread_mutex_lock(&ar.mutex);
    for(unsigned budget=0;budget<AR_BLOCK_COUNT;budget++){
        ar_commit_consumed_locked();
        uint64_t write=atomic_load_explicit(&ar.write_block,memory_order_relaxed);
        /* status_block is the acquired read position whose checkpoint was
         * committed above; only those slots may be overwritten. */
        if(write-ar.status_block>=AR_BLOCK_COUNT)break;
        ArPcmBlock *block=&ar.blocks[write%AR_BLOCK_COUNT];
        rtg_audio_renderer_render(ar.mixer,block->pcm,AR_BLOCK_FRAMES);
        rtg_audio_renderer_voice_checkpoint(ar.mixer,block->voices,RTG_AUDIO_VOICE_SLOTS);
        atomic_store_explicit(&ar.write_block,write+1,memory_order_release);
    }
    pthread_mutex_unlock(&ar.mutex);pthread_cond_broadcast(&ar.changed);
}
static void ar_voice_status_locked(uint8_t *out,size_t bytes) {
    ar_commit_consumed_locked();
    rtg_audio_renderer_voice_checkpoint(ar.mixer,ar.current,RTG_AUDIO_VOICE_SLOTS);
    memset(out,0,bytes);size_t count=bytes/16;if(count>RTG_AUDIO_VOICE_SLOTS)count=RTG_AUDIO_VOICE_SLOTS;
    ar.status_max_played=0;
    for(size_t i=0;i<count;i++){
        const RtgAudioVoiceCheckpoint *current=&ar.current[i],*done=&ar.consumed[i];
        if(!current->used||!done->used||current->generation!=done->generation||current->node!=done->node)continue;
        memcpy(out+i*16,&done->played,8);memcpy(out+i*16+8,&done->consumed,4);memcpy(out+i*16+12,&done->dropped,4);
        if(done->played>ar.status_max_played)ar.status_max_played=done->played;
    }
}
static void ar_capture_clear_quiescent(void) {
    free(atomic_exchange(&ar.capture_pending,NULL));free(atomic_exchange(&ar.capture_ready,NULL));
    free(atomic_exchange(&ar.capture_retired,NULL));free(ar.capture_active);ar.capture_active=NULL;
    ar.capture_position=0;atomic_store(&ar.capture_progress,0);
}
static float *ar_capture_allocate(void) {
    size_t bytes=(size_t)AR_CAPTURE_FRAMES*2*sizeof(float);float *buffer=malloc(bytes);
    if(buffer){
        /* Fault in the backing pages before handing them to the callback. */
        volatile unsigned char *pages=(volatile unsigned char *)buffer;
        for(size_t offset=0;offset<bytes;offset+=4096)pages[offset]=0;
        pages[bytes-1]=0;
    }
    return buffer;
}
static void ar_error(AAudioStream *stream,void *context,aaudio_result_t result) {
    (void)stream;
    if((uintptr_t)context!=atomic_load(&ar.generation)||atomic_load(&ar.closing))return;
    atomic_store(&ar.error,result);ar_notify();
}

/* Caller owns control. No guest memory is touched by stream replacement. */
static void ar_stream_close_locked(void) {
    if(ar.stream){AAudioStream_requestStop(ar.stream);AAudioStream_close(ar.stream);ar.stream=NULL;}
}
static int ar_stream_open_locked(void) {
    AAudioStreamBuilder *builder=NULL;
    uintptr_t generation=atomic_fetch_add(&ar.generation,1)+1;
    int result=AAudio_createStreamBuilder(&builder);
    if(result==AAUDIO_OK){
        AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder,AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setSampleRate(builder,48000);
        AAudioStreamBuilder_setChannelCount(builder,2);
        AAudioStreamBuilder_setFormat(builder,AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setUsage(builder,AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setContentType(builder,AAUDIO_CONTENT_TYPE_MUSIC);
        AAudioStreamBuilder_setPerformanceMode(builder,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFramesPerDataCallback(builder,240);
        AAudioStreamBuilder_setDataCallback(builder,ar_render,(void *)generation);
        AAudioStreamBuilder_setErrorCallback(builder,ar_error,(void *)generation);
        result=AAudioStreamBuilder_openStream(builder,&ar.stream);AAudioStreamBuilder_delete(builder);
    }
    if(result==AAUDIO_OK&&(AAudioStream_getSampleRate(ar.stream)!=48000||AAudioStream_getChannelCount(ar.stream)!=2||AAudioStream_getFormat(ar.stream)!=AAUDIO_FORMAT_PCM_FLOAT))result=AAUDIO_ERROR_INVALID_FORMAT;
    if(result==AAUDIO_OK&&atomic_load(&ar.error))result=atomic_load(&ar.error);
    if(result!=AAUDIO_OK){ar_stream_close_locked();return result;}
    int burst=AAudioStream_getFramesPerBurst(ar.stream);if(burst>0)AAudioStream_setBufferSizeInFrames(ar.stream,burst*2);
    return AAUDIO_OK;
}
static void ar_recover_locked(void) {
    if(!ar.opened||atomic_load(&ar.closing)||ar.recovery_failed||!atomic_load(&ar.error))return;
    int result=atomic_load(&ar.error);atomic_store(&ar.running,0);
    ar_stream_close_locked();
    for(unsigned attempt=1;attempt<=3;attempt++){
        if(attempt>1){struct timespec delay={.tv_nsec=100000000};while(nanosleep(&delay,&delay)&&errno==EINTR){}}
        atomic_store(&ar.error,0);
        result=ar_stream_open_locked();
        if(result==AAUDIO_OK&&ar.want_running&&!atomic_load(&ar.parameter_error)){
            ar_fill_locked();atomic_store(&ar.running,1);result=AAudioStream_requestStart(ar.stream);
        }
        if(result==AAUDIO_OK&&atomic_load(&ar.error))result=atomic_load(&ar.error);
        if(result==AAUDIO_OK){
            ar.recoveries++;
            LOG("[audio-renderer] RECOVERED attempt=%u recoveries=%u running=%d",attempt,ar.recoveries,ar.want_running);
            pthread_cond_broadcast(&ar.changed);return;
        }
        atomic_store(&ar.running,0);ar_stream_close_locked();
        LOG("[audio-renderer] REOPEN_FAILED attempt=%u result=%d (%s)",attempt,result,AAudio_convertResultToText(result));
    }
    atomic_store(&ar.error,result);ar.recovery_failed=1;
    LOG("[audio-renderer] DEVICE_RECOVERY_FAILED result=%d; playback stopped",result);
    pthread_cond_broadcast(&ar.changed);
}
static void *ar_event_thread(void *unused) {
    (void)unused;pthread_setname_np(pthread_self(),"rtg-audio-event");
    for(;;){
        int wait_result;do{wait_result=sem_wait(&ar.notify);}while(wait_result&&errno==EINTR);
        atomic_store(&ar.pending,0);
        pthread_mutex_lock(&ar.control);
        if(atomic_load(&ar.closing)){pthread_mutex_unlock(&ar.control);break;}
        if(wait_result){
            atomic_store(&ar.error,AAUDIO_ERROR_INTERNAL);ar.recovery_failed=1;
            pthread_cond_broadcast(&ar.changed);pthread_mutex_unlock(&ar.control);break;
        }
        int had_error=atomic_load(&ar.error)!=0;
        ar_recover_locked();ar_fill_locked();
        uint64_t epoch=atomic_load_explicit(&ar.callback_epoch,memory_order_acquire);
        int event_due=epoch!=ar.event_epoch||had_error;ar.event_epoch=epoch;
        void *event=ar.event;
        pthread_mutex_unlock(&ar.control);
        if(event&&event_due&&!atomic_load(&ar.closing))hle_signal_system_event(event);
    }
    return NULL;
}
void dm_rtg_audio_suspend(int on) {atomic_store(&ar.muted,on!=0);if(!on)ar_notify();}

/* A guest update that races an error waits for the notifier's bounded recovery
 * instead of reporting an error before recovery has had a chance to run. */
static int ar_ready_locked(void) {
    unsigned session=ar.session;
    while(ar.session==session&&ar.opened&&!atomic_load(&ar.closing)&&atomic_load(&ar.error)&&!ar.recovery_failed){
        ar_notify();pthread_cond_wait(&ar.changed,&ar.control);
    }
    return ar.session==session&&ar.opened&&!atomic_load(&ar.closing)&&ar.stream&&!atomic_load(&ar.error);
}
static int ar_open(uint64_t *handle,void *event,const void *parameter) {
    if(!handle||!parameter)return 0x5299;
    pthread_mutex_lock(&ar.control);
    if(ar.opened||ar.mixer||atomic_load(&ar.closing)){pthread_mutex_unlock(&ar.control);return 0x5299;}
    ar.mixer=rtg_audio_renderer_create();
    if(!ar.mixer){pthread_mutex_unlock(&ar.control);return 0x5299;}
    atomic_store(&ar.running,0);atomic_store(&ar.closing,0);atomic_store(&ar.pending,0);
    atomic_store(&ar.error,0);atomic_store(&ar.parameter_error,0);
    atomic_store(&ar.callbacks,0);atomic_store(&ar.frames,0);atomic_store(&ar.nonzero,0);
    atomic_store(&ar.underrun_blocks,0);atomic_store(&ar.underrun_frames,0);atomic_store(&ar.callback_epoch,0);
    atomic_store(&ar.write_block,0);atomic_store(&ar.read_block,0);atomic_store(&ar.consumed_frames,0);
    ar.read_offset=0;ar.status_block=ar.event_epoch=ar.status_max_played=0;
    memset(ar.consumed,0,sizeof ar.consumed);memset(ar.current,0,sizeof ar.current);
    ar.updates=ar.recoveries=0;ar.want_running=ar.recovery_failed=0;ar.event=event;
    memset(ar.sdk_handle,0,sizeof ar.sdk_handle);
    *(uint32_t *)(ar.sdk_handle+8)=48000;*(uint32_t *)(ar.sdk_handle+0xc)=240;
    *(uint32_t *)(ar.sdk_handle+0x14)=1;
    if(sem_init(&ar.notify,0,0)){rtg_audio_renderer_destroy(ar.mixer);ar.mixer=NULL;pthread_mutex_unlock(&ar.control);return 0x5299;}
    atomic_store(&ar.notify_ready,1);
    int result=ar_stream_open_locked();
    if(result!=AAUDIO_OK){
        LOG("[audio-renderer] OPEN_FAILED result=%d (%s)",result,AAudio_convertResultToText(result));
        atomic_store(&ar.notify_ready,0);sem_destroy(&ar.notify);
        rtg_audio_renderer_destroy(ar.mixer);ar.mixer=NULL;ar.event=NULL;
        pthread_mutex_unlock(&ar.control);return 0x5299;
    }
    if(event)hle_bind_system_event(event,1);
    const char *root=getenv("DM_FRAME_DIR");char marker[1400];
    if(root){snprintf(marker,sizeof marker,"%s/capture-audio",root);if(!access(marker,F_OK))atomic_store(&ar.capture_pending,ar_capture_allocate());}
    ar.opened=1;ar.session++;
    if(pthread_create(&ar.notifier,NULL,ar_event_thread,NULL)){
        atomic_store(&ar.closing,1);ar_stream_close_locked();atomic_store(&ar.notify_ready,0);sem_destroy(&ar.notify);
        rtg_audio_renderer_destroy(ar.mixer);ar.mixer=NULL;ar_capture_clear_quiescent();
        ar.event=NULL;ar.opened=0;atomic_store(&ar.closing,0);pthread_mutex_unlock(&ar.control);return 0x5299;
    }
    ar.notifier_started=1;handle[0]=1;handle[1]=(uintptr_t)ar.sdk_handle;
    LOG("[audio-renderer] OPEN PCM_FLOAT stereo rate=48000 device=%d burst=%d callback=240 performance=%d",AAudioStream_getDeviceId(ar.stream),AAudioStream_getFramesPerBurst(ar.stream),AAudioStream_getPerformanceMode(ar.stream));
    pthread_mutex_unlock(&ar.control);return 0;
}
static void ar_set_running(int on) {
    pthread_mutex_lock(&ar.control);
    if(!ar_ready_locked()||atomic_load(&ar.parameter_error)){
        pthread_mutex_unlock(&ar.control);die("REV start/stop without a usable output stream");return;
    }
    int result=AAUDIO_OK;
    if(ar.want_running!=on){
        ar.want_running=on;
        if(on){
            unsigned session=ar.session;ar_notify();
            while(ar.session==session&&ar.opened&&!atomic_load(&ar.closing)&&!atomic_load(&ar.error)&&!atomic_load(&ar.parameter_error)&&
                  atomic_load(&ar.write_block)-atomic_load(&ar.read_block)<AR_BLOCK_COUNT)
                pthread_cond_wait(&ar.changed,&ar.control);
            if(ar.session!=session||!ar_ready_locked()||atomic_load(&ar.parameter_error)){
                pthread_mutex_unlock(&ar.control);die("REV producer prefill failed");return;
            }
        }
        /* Recovery may already have started a replacement while this call
         * waited for prefill; do not issue a second requestStart then. */
        int already_started=on&&atomic_load(&ar.running);
        atomic_store(&ar.running,on);
        result=already_started?AAUDIO_OK:on?AAudioStream_requestStart(ar.stream):AAudioStream_requestPause(ar.stream);
        if(result!=AAUDIO_OK){atomic_store(&ar.running,0);atomic_store(&ar.error,result);ar_notify();if(ar_ready_locked())result=AAUDIO_OK;}
    }
    if(result==AAUDIO_OK)*(uint32_t *)(ar.sdk_handle+0x14)=on?0:1;
    pthread_mutex_unlock(&ar.control);
    if(result!=AAUDIO_OK)die("REV output start/stop failed %d",result);
    LOG("[audio-renderer] %s",on?"START":"STOP");
}
static void ar_close(void) {
    pthread_mutex_lock(&ar.control);
    if(atomic_load(&ar.closing)){
        while(atomic_load(&ar.closing))pthread_cond_wait(&ar.changed,&ar.control);
        pthread_mutex_unlock(&ar.control);return;
    }
    if(!ar.opened){pthread_mutex_unlock(&ar.control);return;}
    atomic_store(&ar.closing,1);atomic_store(&ar.running,0);ar.want_running=0;
    *(uint32_t *)(ar.sdk_handle+0x14)=1;
    ar_stream_close_locked();
    pthread_cond_broadcast(&ar.changed);sem_post(&ar.notify);
    int join=ar.notifier_started;pthread_t notifier=ar.notifier;
    /* The notifier may be waiting for control. Never hold it across join. */
    pthread_mutex_unlock(&ar.control);
    if(join)pthread_join(notifier,NULL);
    pthread_mutex_lock(&ar.control);pthread_mutex_lock(&ar.mutex);
    rtg_audio_renderer_destroy(ar.mixer);ar.mixer=NULL;ar_capture_clear_quiescent();ar.event=NULL;
    pthread_mutex_unlock(&ar.mutex);
    atomic_store(&ar.notify_ready,0);sem_destroy(&ar.notify);
    ar.opened=ar.notifier_started=0;atomic_store(&ar.closing,0);
    pthread_cond_broadcast(&ar.changed);pthread_mutex_unlock(&ar.control);
}

HLE_FORCE("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", "host REV owns its own work storage; avoid audren hardware IPC",size_t,h_rtg_ar_Work,const void *parameter) {(void)parameter;return 0x100000;}
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm", "open actual Android REV output",uint32_t,h_rtg_ar_Open3,uint64_t *handle,const void *parameter,void *work,size_t bytes) {(void)work;(void)bytes;return ar_open(handle,NULL,parameter);}
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", "open actual Android REV output and device-driven update event",uint32_t,h_rtg_ar_Open5,uint64_t *handle,void *event,const void *parameter,void *work,size_t bytes) {(void)work;(void)bytes;return ar_open(handle,event,parameter);}
HLE_FORCE("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE", "start the AAudio render clock",void,h_rtg_ar_Start,uint64_t h0,uint64_t h1) {(void)h0;(void)h1;ar_set_running(1);}
HLE_FORCE("_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE", "pause the AAudio render clock",void,h_rtg_ar_Stop,uint64_t h0,uint64_t h1) {(void)h0;(void)h1;ar_set_running(0);}
HLE_FORCE("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", "close real output after callback quiescence",void,h_rtg_ar_Close,uint64_t h0,uint64_t h1) {(void)h0;(void)h1;ar_close();}
HLE_FORCE("_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi", "Android primary output device",int,h_rtg_ar_Devices,void *names,int count) {if(!names||count<1)return 0;memset(names,0,256);strcpy(names,"DeviceOut");return 1;}
HLE_FORCE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", "real REV master output gain",void,h_rtg_ar_Volume,const void *name,float volume) {(void)name;if(isfinite(volume))atomic_store(&ar.volume,fmaxf(0,fminf(1,volume)));}

typedef struct {
    uint32_t (*revision)(void *);
    uint32_t (*behavior)(void *,void *),(*pool)(void *,void *),(*voice)(void *,void *,uint64_t *,uint64_t *),
        (*effect)(void *,void *),(*splitter)(void *,void *),(*mix)(void *,void *),(*sink)(void *,void *),(*performance)(void *,void *);
    void (*pool_out)(void *,const void *),(*voice_out)(void *,const void *),(*effect_out)(void *,const void *),
        (*sink_out)(void *,const void *),(*performance_out)(void *,const void *),(*behavior_out)(void *,const void *);
} ArSdk;
static ArSdk ar_sdk;
static pthread_once_t ar_sdk_once=PTHREAD_ONCE_INIT;
static void *ar_symbol(const char *name){void *p=guest_sym(name);if(!p)die("REV SDK entry unavailable: %s",name);return p;}
static void ar_sdk_init(void) {
    ar_sdk.revision=ar_symbol("_ZNK2nn5audio15BehaviorManager11GetRevisionEv");
    ar_sdk.behavior=ar_symbol("_ZNK2nn5audio15BehaviorManager17UpdateInParameterEPNS0_8renderer3ipc8Behavior10UpdateDataILi0EEE");
    ar_sdk.pool=ar_symbol("_ZN2nn5audio17MemoryPoolManager17UpdateInParameterEPNS0_8renderer3ipc10MemoryPool10UpdateDataILi0EEE");
    ar_sdk.voice=ar_symbol("_ZN2nn5audio16VoiceInfoManager28UpdateVoiceRelatedParametersEPvPmS3_");
    ar_sdk.effect=ar_symbol("_ZN2nn5audio13EffectManager24UpdateEffectsInParameterEPNS0_8renderer3ipc6Effect10UpdateDataILi2EEE");
    ar_sdk.splitter=ar_symbol("_ZN2nn5audio19SplitterInfoManager17UpdateInParameterEPv");
    ar_sdk.mix=ar_symbol("_ZN2nn5audio10MixManager20UpdateMixInParameterEPv");
    ar_sdk.sink=ar_symbol("_ZN2nn5audio11SinkManager21UpdateSinkInParameterEPNS0_8renderer3ipc4Sink10UpdateDataILi0EEE");
    ar_sdk.performance=ar_symbol("_ZN2nn5audio24PerformanceBufferManager17UpdateInParameterEPNS0_8renderer3ipc18PerformanceMetrics10UpdateDataILi0EEE");
    ar_sdk.pool_out=ar_symbol("_ZN2nn5audio17MemoryPoolManager15UpdateOutStatusEPKNS0_8renderer3ipc10MemoryPool10StatusDataILi0EEE");
    ar_sdk.voice_out=ar_symbol("_ZN2nn5audio16VoiceInfoManager20UpdateVoiceOutStatusEPKNS0_8renderer3ipc5Voice10StatusDataILi0EEE");
    ar_sdk.effect_out=ar_symbol("_ZN2nn5audio13EffectManager21UpdateEffectOutStatusEPKNS0_8renderer3ipc6Effect10StatusDataILi1EEE");
    ar_sdk.sink_out=ar_symbol("_ZN2nn5audio11SinkManager19UpdateSinkOutStatusEPKNS0_8renderer3ipc4Sink10StatusDataILi0EEE");
    ar_sdk.performance_out=ar_symbol("_ZN2nn5audio24PerformanceBufferManager15UpdateOutStatusEPKNS0_8renderer3ipc18PerformanceMetrics10StatusDataILi0EEE");
    ar_sdk.behavior_out=ar_symbol("_ZN2nn5audio15BehaviorManager15UpdateOutStatusEPKNS0_8renderer3ipc8Behavior10StatusDataILi0EEE");
}

/* Only pointer ownership and the marker are handled under control. Allocation,
 * disk I/O and freeing detached buffers happen after releasing producer locks. */
static void ar_capture_collect_locked(ArCaptureWork *work,int check_request) {
    work->save=atomic_exchange_explicit(&ar.capture_ready,NULL,memory_order_acq_rel);
    work->discard=atomic_exchange_explicit(&ar.capture_retired,NULL,memory_order_acq_rel);
    work->session=ar.session;
    const char *root=getenv("DM_FRAME_DIR");
    if(root){
        snprintf(work->path,sizeof work->path,"%s/rendered-audio.f32",root);
        if(check_request){
            char marker[1400];snprintf(marker,sizeof marker,"%s/capture-audio-now",root);
            if(!unlink(marker))work->request=1;
            else if(errno!=ENOENT)LOG("[audio-renderer] CAPTURE_REQUEST_FAILED errno=%d",errno);
        }
    }
}
static void ar_capture_finish(ArCaptureWork *work) {
    if(work->save){
        FILE *out=work->path[0]?fopen(work->path,"wb"):NULL;
        if(out){
            size_t written=fwrite(work->save,2*sizeof(float),AR_CAPTURE_FRAMES,out);
            int closed=fclose(out);
            if(written==AR_CAPTURE_FRAMES&&!closed)LOG("[audio-renderer] CAPTURE frames=480000 rate=48000 channels=2 format=float32 includes_underrun=1");
            else LOG("[audio-renderer] CAPTURE_WRITE_FAILED frames=%zu errno=%d",written,errno);
        }else if(work->path[0])LOG("[audio-renderer] CAPTURE_OPEN_FAILED errno=%d",errno);
        free(work->save);
    }
    free(work->discard);
    if(work->request){
        float *buffer=ar_capture_allocate(),*previous=NULL;
        if(!buffer){LOG("[audio-renderer] CAPTURE_ALLOCATION_FAILED");return;}
        pthread_mutex_lock(&ar.control);
        int current=ar.opened&&!atomic_load(&ar.closing)&&ar.session==work->session;
        if(current)previous=atomic_exchange_explicit(&ar.capture_pending,buffer,memory_order_acq_rel);
        pthread_mutex_unlock(&ar.control);
        if(!current)free(buffer);free(previous);
        if(current)LOG("[audio-renderer] CAPTURE_STARTED frames=480000 rate=48000 channels=2");
    }
}
static void ar_dump_parameters(const uint8_t *request,size_t bytes,const char *name) {
    const char *root=getenv("DM_FRAME_DIR");if(!root||bytes>0x100000)return;
    char path[1400];snprintf(path,sizeof path,"%s/%s",root,name);
    FILE *out=fopen(path,"wb");if(out){fwrite(request,1,bytes,out);fclose(out);}
}
/* Caller owns control through parameter serialization, SDK status publication,
 * timestamp queries. Capture buffers transfer ownership before unlocking. */
static uint32_t ar_update_locked(const uint64_t *c,ArCaptureWork *capture_work) {
    if(!c||!c[6]||!c[9]||!c[11]||!ar_ready_locked()||!ar.mixer)return 0x5299;
    if(atomic_load(&ar.parameter_error))return 0x5299;
    pthread_once(&ar_sdk_once,ar_sdk_init);
    uint8_t *request=(uint8_t *)c[9],*p=request+0x40;
    uint32_t *header=(uint32_t *)request;memset(request,0,0x40);header[0]=ar_sdk.revision((void *)c[6]);
    header[1]=ar_sdk.behavior((void *)c[6],p);p+=header[1];
    uint8_t *pools=p;header[2]=c[5]?ar_sdk.pool((void *)c[5],p):0;p+=header[2];
    uint8_t *channels=p;uint64_t voice_bytes=0,channel_bytes=0;
    if(c[0])ar_sdk.voice((void *)c[0],p,&voice_bytes,&channel_bytes);
    header[3]=(uint32_t)voice_bytes;header[4]=(uint32_t)channel_bytes;uint8_t *voices=p+channel_bytes;p+=voice_bytes+channel_bytes;
    uint8_t *effects=p;header[5]=c[2]?ar_sdk.effect((void *)c[2],p):0;p+=header[5];
    header[9]=c[7]?ar_sdk.splitter((void *)c[7],p):0;p+=header[9];
    uint8_t *mixes=p;header[6]=c[1]?ar_sdk.mix((void *)c[1],p):0;p+=header[6];
    uint8_t *sinks=p;header[7]=c[3]?ar_sdk.sink((void *)c[3],p):0;p+=header[7];
    header[8]=c[4]?ar_sdk.performance((void *)c[4],p):0;p+=header[8];
    if((size_t)(p-request)>c[10]||voice_bytes%0x188||channel_bytes%0x70||header[2]%0x20||header[5]%0xc0||header[7]%0x140)die("REV serialized parameter sizes");
    uint8_t *response=(uint8_t *)c[11]+0x40;size_t pool_count=header[2]/0x20,voice_count=voice_bytes/0x188,effect_count=header[5]/0xc0,sink_count=header[7]/0x140;
    size_t pool_out_bytes=pool_count*0x10,voice_out_bytes=voice_count*0x10,effect_out_bytes=effect_count*0x90,sink_out_bytes=sink_count*0x20;
    size_t total=pool_out_bytes+voice_out_bytes+effect_out_bytes+sink_out_bytes+0x100;
    if(total+0x40>c[10])die("REV output status bounds");memset(response,0,total);
    uint8_t *voice_status=response+pool_out_bytes;
    RtgAudioRendererMetrics metrics={0};
    pthread_mutex_lock(&ar.mutex);
    int result=rtg_audio_renderer_effects(ar.mixer,effects,header[5]);
    if(!result)result=rtg_audio_renderer_update(ar.mixer,channels,channel_bytes,voices,voice_bytes,mixes,header[6],sinks,header[7]);
    if(result){
        /* Core validation can fail after some fields have changed. Do not let
         * a callback observe that partial state, even after another update. */
        atomic_store(&ar.parameter_error,result);atomic_store(&ar.running,0);
    }else{
        for(size_t i=0;i<pool_count;i++){
            uint32_t state;uint64_t address,bytes;
            memcpy(&address,pools+i*0x20,8);memcpy(&bytes,pools+i*0x20+8,8);
            memcpy(&state,pools+i*0x20+0x10,4);
            if(state==4)state=5;
            else if(state==2&&!rtg_audio_renderer_uses_memory(ar.mixer,address,bytes))state=3;
            /* Keep RequestDetach=2 while even a paused/queued wave refers to
             * the pool. SDK Store retains this state and still reports attached. */
            memcpy(response+i*0x10,&state,4);
        }
        ar_voice_status_locked(voice_status,voice_out_bytes);
        metrics=rtg_audio_renderer_metrics(ar.mixer);
    }
    pthread_mutex_unlock(&ar.mutex);
    if(result){ar_dump_parameters(request,(size_t)(p-request),"audio-parameter-error.bin");LOG("[audio-renderer] PARAMETER_ERROR rc=%d voices=%llu channels=%llu mixes=%u sinks=%u",result,(unsigned long long)voice_bytes,(unsigned long long)channel_bytes,header[6],header[7]);return 0x5299;}
    if(ar.updates==0&&getenv("DM_RUNTIME_DIAGNOSTICS"))ar_dump_parameters(request,(size_t)(p-request),"audio-initial-parameters.bin");
    uint8_t *effect_status=voice_status+voice_out_bytes;
    for(size_t i=0;i<effect_count;i++)effect_status[i*0x90]=effects[i*0xc0+2]?2:1;
    if(c[5])ar_sdk.pool_out((void *)c[5],response);
    if(c[0])ar_sdk.voice_out((void *)c[0],voice_status);
    if(c[2])ar_sdk.effect_out((void *)c[2],effect_status);
    if(c[3])ar_sdk.sink_out((void *)c[3],effect_status+effect_out_bytes);
    if(c[4])ar_sdk.performance_out((void *)c[4],effect_status+effect_out_bytes+sink_out_bytes);
    uint8_t behavior[0xb0]={0};if(c[6])ar_sdk.behavior_out((void *)c[6],behavior);if(c[8])*(uint64_t *)c[8]=0;
    int diagnostic=ar.updates++%600==0;
    ar_capture_collect_locked(capture_work,diagnostic);
    if(diagnostic&&getenv("DM_RUNTIME_DIAGNOSTICS")){
        int64_t position=0,stamp=0;int timestamp=AAudioStream_getTimestamp(ar.stream,CLOCK_MONOTONIC,&position,&stamp);
        LOG("[audio-renderer] OUTPUT updates=%u callbacks=%llu frames=%llu nonzero=%llu active=%u queued=%u effects=%u peak=%.5f underrun_blocks=%llu underrun_frames=%llu mixed_frames=%llu consumed_frames=%llu status_frames=%llu status_max_played=%llu timestamp=%d position=%lld",ar.updates,(unsigned long long)atomic_load(&ar.callbacks),(unsigned long long)atomic_load(&ar.frames),(unsigned long long)atomic_load(&ar.nonzero),metrics.active_voices,metrics.queued_wave_buffers,metrics.active_effects,metrics.peak,(unsigned long long)atomic_load(&ar.underrun_blocks),(unsigned long long)atomic_load(&ar.underrun_frames),(unsigned long long)atomic_load(&ar.write_block)*AR_BLOCK_FRAMES,(unsigned long long)atomic_load(&ar.consumed_frames),(unsigned long long)ar.status_block*AR_BLOCK_FRAMES,(unsigned long long)ar.status_max_played,timestamp,(long long)position);
    }
    ar_notify();return 0;
}

HLE_FORCE("_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE", "native REV parameter serialization with real sample consumption status",uint32_t,h_rtg_ar_Update,uint64_t h0,uint64_t h1,const uint64_t *c) {
    (void)h0;(void)h1;ArCaptureWork capture_work={0};pthread_mutex_lock(&ar.control);
    uint32_t result=ar_update_locked(c,&capture_work);
    pthread_mutex_unlock(&ar.control);ar_capture_finish(&capture_work);return result;
}
