/* Host regression harness for the actual REV HLE wrapper. AAudio is a controlled
 * device double; the PCM/DSP implementation has separate sample-level tests. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../rtg_audio_renderer.h"

/* macOS has no unnamed POSIX semaphores; this test-only equivalent preserves
 * wake/destroy semantics while running the unmodified production wrapper. */
typedef struct {pthread_mutex_t lock;pthread_cond_t ready;unsigned value;int live;} TestSemaphore;
static int test_sem_init(TestSemaphore *s,int shared,unsigned value){(void)shared;pthread_mutex_init(&s->lock,NULL);pthread_cond_init(&s->ready,NULL);s->value=value;s->live=1;return 0;}
static int test_sem_post(TestSemaphore *s){pthread_mutex_lock(&s->lock);assert(s->live);s->value++;pthread_cond_signal(&s->ready);pthread_mutex_unlock(&s->lock);return 0;}
static int test_sem_wait(TestSemaphore *s){pthread_mutex_lock(&s->lock);while(!s->value){assert(s->live);pthread_cond_wait(&s->ready,&s->lock);}s->value--;pthread_mutex_unlock(&s->lock);return 0;}
static int test_sem_destroy(TestSemaphore *s){assert(s->live);s->live=0;pthread_cond_destroy(&s->ready);pthread_mutex_destroy(&s->lock);return 0;}
#define sem_t TestSemaphore
#define sem_init test_sem_init
#define sem_post test_sem_post
#define sem_wait test_sem_wait
#define sem_destroy test_sem_destroy
static int test_thread_name(pthread_t thread,const char *name){(void)thread;(void)name;return 0;}
#define pthread_setname_np test_thread_name

#define AAUDIO_OK 0
#define AAUDIO_ERROR_DISCONNECTED -899
#define AAUDIO_ERROR_INVALID_FORMAT -883
#define AAUDIO_ERROR_INTERNAL -896
#define AAUDIO_DIRECTION_OUTPUT 0
#define AAUDIO_SHARING_MODE_SHARED 1
#define AAUDIO_FORMAT_PCM_FLOAT 2
#define AAUDIO_USAGE_GAME 14
#define AAUDIO_CONTENT_TYPE_MUSIC 2
#define AAUDIO_PERFORMANCE_MODE_LOW_LATENCY 12
#define AAUDIO_CALLBACK_RESULT_CONTINUE 0
typedef int aaudio_result_t;
typedef int aaudio_data_callback_result_t;
typedef struct AAudioStream AAudioStream;
typedef int (*DataCallback)(AAudioStream *,void *,void *,int32_t);
typedef void (*ErrorCallback)(AAudioStream *,void *,int);
typedef struct {DataCallback data;ErrorCallback error;void *data_ctx,*error_ctx;} AAudioStreamBuilder;
struct AAudioStream {AAudioStreamBuilder callbacks;int closed,started;pthread_mutex_t lock;pthread_cond_t idle;unsigned in_callback;};
static int fake_open_failures,fake_opens,fake_closes,fake_starts;
static AAudioStream *fake_streams[128];
static unsigned fake_stream_count;
static int AAudio_createStreamBuilder(AAudioStreamBuilder **b){*b=calloc(1,sizeof(**b));return 0;}
#define BUILDER_OPTION(name) static void AAudioStreamBuilder_##name(AAudioStreamBuilder *b,int value){(void)b;(void)value;}
BUILDER_OPTION(setDirection) BUILDER_OPTION(setSharingMode) BUILDER_OPTION(setSampleRate)
BUILDER_OPTION(setChannelCount) BUILDER_OPTION(setFormat) BUILDER_OPTION(setUsage)
BUILDER_OPTION(setContentType) BUILDER_OPTION(setPerformanceMode) BUILDER_OPTION(setFramesPerDataCallback)
static void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b,DataCallback fn,void *ctx){b->data=fn;b->data_ctx=ctx;}
static void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b,ErrorCallback fn,void *ctx){b->error=fn;b->error_ctx=ctx;}
static int AAudioStreamBuilder_openStream(AAudioStreamBuilder *b,AAudioStream **s){
    fake_opens++;if(fake_open_failures){fake_open_failures--;*s=NULL;return AAUDIO_ERROR_DISCONNECTED;}
    *s=calloc(1,sizeof(**s));(*s)->callbacks=*b;pthread_mutex_init(&(*s)->lock,NULL);pthread_cond_init(&(*s)->idle,NULL);
    assert(fake_stream_count<128);fake_streams[fake_stream_count++]=*s;return 0;
}
static void AAudioStreamBuilder_delete(AAudioStreamBuilder *b){free(b);}
#define STREAM_GETTER(name,value) static int AAudioStream_##name(AAudioStream *s){assert(s&&!s->closed);return value;}
STREAM_GETTER(getSampleRate,48000) STREAM_GETTER(getChannelCount,2) STREAM_GETTER(getFormat,AAUDIO_FORMAT_PCM_FLOAT)
STREAM_GETTER(getFramesPerBurst,192) STREAM_GETTER(getDeviceId,1) STREAM_GETTER(getPerformanceMode,12)
static int AAudioStream_setBufferSizeInFrames(AAudioStream *s,int value){assert(s&&!s->closed);return value;}
static const char *AAudio_convertResultToText(int value){(void)value;return "test device";}
static int AAudioStream_requestStart(AAudioStream *s){assert(s&&!s->closed);s->started=1;fake_starts++;return 0;}
static int AAudioStream_requestPause(AAudioStream *s){assert(s&&!s->closed);s->started=0;return 0;}
static int AAudioStream_requestStop(AAudioStream *s){assert(s&&!s->closed);s->started=0;return 0;}
static int AAudioStream_close(AAudioStream *s){
    pthread_mutex_lock(&s->lock);assert(!s->closed);s->closed=1;
    while(s->in_callback)pthread_cond_wait(&s->idle,&s->lock);
    pthread_mutex_unlock(&s->lock);fake_closes++;return 0;
}
static int AAudioStream_getTimestamp(AAudioStream *s,int clock,int64_t *position,int64_t *stamp){assert(s&&!s->closed);(void)clock;*position=*stamp=0;return 0;}

static _Atomic int fake_update_hold,fake_update_entered,fake_status_hold,fake_status_entered;
static _Atomic int fake_update_error,fake_effect_error,fake_render_calls,fake_destroyed;
static uint64_t fake_voice_generation=1;
static int fake_pcm_referenced;
struct RtgAudioRenderer {int live;uint64_t played,generation;};
static void short_wait(void){struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);}
RtgAudioRenderer *rtg_audio_renderer_create(void){RtgAudioRenderer *r=calloc(1,sizeof(*r));r->live=1;r->generation=fake_voice_generation;return r;}
void rtg_audio_renderer_destroy(RtgAudioRenderer *r){assert(r&&r->live);r->live=0;atomic_fetch_add(&fake_destroyed,1);free(r);}
void rtg_audio_renderer_reset(RtgAudioRenderer *r){assert(r&&r->live);}
int rtg_audio_renderer_effects(RtgAudioRenderer *r,const uint8_t *p,size_t bytes){assert(r&&r->live);(void)p;(void)bytes;return atomic_load(&fake_effect_error);}
int rtg_audio_renderer_update(RtgAudioRenderer *r,const uint8_t *a,size_t an,const uint8_t *b,size_t bn,const uint8_t *c,size_t cn,const uint8_t *d,size_t dn){
    assert(r&&r->live);(void)a;(void)an;(void)b;(void)bn;(void)c;(void)cn;(void)d;(void)dn;
    atomic_store(&fake_update_entered,1);while(atomic_load(&fake_update_hold))short_wait();assert(r->live);if(r->generation!=fake_voice_generation){r->generation=fake_voice_generation;r->played=0;}return atomic_load(&fake_update_error);
}
void rtg_audio_renderer_render(RtgAudioRenderer *r,float *out,size_t frames){assert(r&&r->live);atomic_fetch_add(&fake_render_calls,1);r->played+=frames;for(size_t i=0;i<frames*2;i++)out[i]=.25f;}
void rtg_audio_renderer_voice_status(const RtgAudioRenderer *r,uint8_t *out,size_t bytes){(void)r;(void)out;(void)bytes;assert(!"future core status must never be published");}
void rtg_audio_renderer_voice_checkpoint(const RtgAudioRenderer *r,RtgAudioVoiceCheckpoint *out,size_t count){
    assert(r&&r->live);memset(out,0,count*sizeof(*out));if(count)out[0]=(RtgAudioVoiceCheckpoint){.used=1,.node=7,.generation=r->generation,.played=r->played,.consumed=(uint32_t)(r->played/240)};
}
RtgAudioRendererMetrics rtg_audio_renderer_metrics(const RtgAudioRenderer *r){assert(r&&r->live);return (RtgAudioRendererMetrics){0};}
bool rtg_audio_renderer_uses_memory(const RtgAudioRenderer *r,uint64_t address,uint64_t bytes){assert(r&&r->live);assert(address==0x1000&&bytes==0x2000);return fake_pcm_referenced;}

static uint8_t fake_pool[32],fake_pool_status[16],fake_voice_status[16];
static uint32_t fake_revision(void *p){(void)p;return 0;}
static uint32_t fake_in(void *manager,void *out){(void)manager;(void)out;return 0;}
static uint32_t fake_voice_in(void *manager,void *out,uint64_t *voice_bytes,uint64_t *channel_bytes){(void)manager;memset(out,0,0x188);*voice_bytes=0x188;*channel_bytes=0;return 0;}
static void fake_voice_out(void *manager,const void *status){(void)manager;memcpy(fake_voice_status,status,16);}
static uint32_t fake_pool_in(void *manager,void *out){(void)manager;memcpy(out,fake_pool,32);return 32;}
static void fake_out(void *manager,const void *status){(void)manager;(void)status;}
static void fake_pool_out(void *manager,const void *status){
    (void)manager;memcpy(fake_pool_status,status,16);atomic_store(&fake_status_entered,1);
    while(atomic_load(&fake_status_hold))short_wait();
}
static void *guest_sym(const char *name){
    if(strstr(name,"GetRevision"))return (void *)fake_revision;
    if(strstr(name,"VoiceInfoManager"))return strstr(name,"UpdateVoiceRelated")?(void *)fake_voice_in:(void *)fake_voice_out;
    if(strstr(name,"MemoryPoolManager"))return strstr(name,"UpdateIn")?(void *)fake_pool_in:(void *)fake_pool_out;
    return strstr(name,"UpdateOut")?(void *)fake_out:(void *)fake_in;
}
static _Atomic int fake_events;
void hle_bind_system_event(void *event,int automatic){(void)event;(void)automatic;}
void hle_signal_system_event(void *event){assert(event);atomic_fetch_add(&fake_events,1);}
static void test_log(const char *format,...) {(void)format;}
#define LOG(...) test_log(__VA_ARGS__)
static void die(const char *format,...){va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);abort();}
#define HLE_FORCE(symbol,description,result,name,...) static result name(__VA_ARGS__)
static _Atomic int fake_write_hold,fake_write_entered;
static size_t test_fwrite(const void *data,size_t size,size_t count,FILE *file){if(atomic_load(&fake_write_hold)){atomic_store(&fake_write_entered,1);while(atomic_load(&fake_write_hold))short_wait();}return fwrite(data,size,count,file);}
#define fwrite test_fwrite
#define RTG_AUDIO_RENDERER_HLE_TEST 1
#include "../audio_renderer_hle.inc"

static uint8_t request[4096],response[4096];
static uint64_t config[12],behavior_result;
static void fixture(void){
    memset(config,0,sizeof(config));memset(fake_pool,0,sizeof(fake_pool));
    uint64_t address=0x1000,bytes=0x2000;uint32_t state=2;
    memcpy(fake_pool,&address,8);memcpy(fake_pool+8,&bytes,8);memcpy(fake_pool+16,&state,4);
    config[0]=config[5]=config[6]=1;config[8]=(uintptr_t)&behavior_result;config[9]=(uintptr_t)request;
    config[10]=sizeof(request);config[11]=(uintptr_t)response;
}
static void opened(void){uint64_t handle[2];assert(ar_open(handle,(void *)1,(void *)1)==0);ar_set_running(1);}
static void await_flag(_Atomic int *flag){for(unsigned i=0;i<4000&&!atomic_load(flag);i++)short_wait();assert(atomic_load(flag));}
static uint32_t update(void){return h_rtg_ar_Update(1,0,config);}
static void fake_pump_frames(AAudioStream *s,float *out,int frames){
    pthread_mutex_lock(&s->lock);if(s->closed){pthread_mutex_unlock(&s->lock);return;}s->in_callback++;pthread_mutex_unlock(&s->lock);
    s->callbacks.data(s,s->callbacks.data_ctx,out,frames);
    pthread_mutex_lock(&s->lock);s->in_callback--;pthread_cond_broadcast(&s->idle);pthread_mutex_unlock(&s->lock);
}
static void fake_pump(AAudioStream *s,float *out){fake_pump_frames(s,out,240);}
static void *update_thread(void *unused){(void)unused;assert(update()==0);return NULL;}
static _Atomic int close_done,close_started;
static void *close_thread(void *unused){(void)unused;atomic_store(&close_started,1);ar_close();atomic_store(&close_done,1);return NULL;}
static void concurrent_close(int block_status){
    opened();atomic_store(&fake_update_entered,0);atomic_store(&fake_status_entered,0);
    atomic_store(block_status?&fake_status_hold:&fake_update_hold,1);
    atomic_store(&close_done,0);atomic_store(&close_started,0);
    pthread_t update_id,close_id;pthread_create(&update_id,NULL,update_thread,NULL);
    await_flag(block_status?&fake_status_entered:&fake_update_entered);
    pthread_create(&close_id,NULL,close_thread,NULL);await_flag(&close_started);
    for(int i=0;i<20;i++)short_wait();assert(!atomic_load(&close_done));
    atomic_store(block_status?&fake_status_hold:&fake_update_hold,0);
    pthread_join(update_id,NULL);pthread_join(close_id,NULL);assert(atomic_load(&close_done));
}
static uint64_t played_status(void){uint64_t value;memcpy(&value,fake_voice_status,8);return value;}
static void wait_buffered(void){
    for(unsigned i=0;i<4000;i++){
        if(atomic_load(&ar.write_block)-atomic_load(&ar.read_block)==AR_BLOCK_COUNT)return;
        short_wait();
    }
    assert(!"producer did not refill");
}
static void checkpoint_checks(void){
    float output[480];opened();assert(update()==0);assert(played_status()==0);
    assert(ar.mixer->played==720);assert(atomic_load(&fake_events)==0);
    pthread_mutex_lock(&ar.control);int renders=atomic_load(&fake_render_calls);
    fake_pump_frames(ar.stream,output,120);assert(output[0]==.25f);assert(atomic_load(&ar.read_block)==0);
    assert(atomic_load(&ar.consumed_frames)==120&&atomic_load(&fake_render_calls)==renders);
    pthread_mutex_unlock(&ar.control);assert(update()==0&&played_status()==0);
    pthread_mutex_lock(&ar.control);fake_pump_frames(ar.stream,output,120);pthread_mutex_unlock(&ar.control);
    assert(update()==0&&played_status()==240);assert(ar.status_block*240<=atomic_load(&ar.consumed_frames));assert(atomic_load(&ar.consumed_frames)<=atomic_load(&ar.write_block)*240);wait_buffered();
    fake_voice_generation=2;assert(update()==0&&played_status()==0);
    pthread_mutex_lock(&ar.control);
    uint64_t read=atomic_load(&ar.read_block),write=atomic_load(&ar.write_block);
    for(uint64_t n=read;n<write;n++){
        int old=ar.blocks[n%AR_BLOCK_COUNT].voices[0].generation!=2;
        fake_pump(ar.stream,output);uint8_t status[16];ar_voice_status_locked(status,sizeof status);
        uint64_t played;memcpy(&played,status,8);if(old)assert(played==0);
    }
    pthread_mutex_unlock(&ar.control);wait_buffered();
    pthread_mutex_lock(&ar.control);fake_pump(ar.stream,output);pthread_mutex_unlock(&ar.control);
    assert(update()==0&&played_status()>0);wait_buffered();
    uint64_t consumed=atomic_load(&ar.consumed_frames);dm_rtg_audio_suspend(1);fake_pump(ar.stream,output);
    assert(output[0]==0&&atomic_load(&ar.consumed_frames)==consumed);dm_rtg_audio_suspend(0);
    ar_close();fake_voice_generation=1;
}
static void capture_checks(void){
    char temp[]="/tmp/rtg-audio-lifecycle-XXXXXX";assert(mkdtemp(temp));assert(!setenv("DM_FRAME_DIR",temp,1));
    char marker[512],capture[512];snprintf(marker,sizeof marker,"%s/capture-audio-now",temp);snprintf(capture,sizeof capture,"%s/rendered-audio.f32",temp);
    opened();float output[480];assert(*(uint32_t *)(ar.sdk_handle+8)==48000&&*(uint32_t *)(ar.sdk_handle+0xc)==240);
    FILE *file=fopen(marker,"wb");assert(file);fclose(file);ar.updates=600;assert(update()==0);
    assert(access(marker,F_OK)&&atomic_load(&ar.capture_pending));
    pthread_mutex_lock(&ar.control);uint64_t missing=atomic_load(&ar.underrun_frames);
    for(int i=0;i<4;i++)fake_pump(ar.stream,output);
    assert(atomic_load(&ar.underrun_frames)==missing+240&&output[0]==0);
    assert(atomic_load(&ar.capture_progress)==960);float *first=ar.capture_active;
    assert(first[0]==.25f&&first[720*2]==0);pthread_mutex_unlock(&ar.control);
    file=fopen(marker,"wb");assert(file);fclose(file);ar.updates=1200;assert(update()==0);
    fake_pump(ar.stream,output);assert(atomic_load(&ar.capture_retired)==first);
    assert(atomic_load(&ar.capture_progress)==240&&ar.capture_active!=first);
    for(int i=1;i<2000;i++)fake_pump(ar.stream,output);
    assert(atomic_load(&ar.capture_progress)==480000&&atomic_load(&ar.capture_ready));
    atomic_store(&fake_write_hold,1);atomic_store(&fake_write_entered,0);
    pthread_t writer;pthread_create(&writer,NULL,update_thread,NULL);await_flag(&fake_write_entered);
    /* The blocked 3.84MB file writer cannot block the ordinary PCM producer. */
    for(int i=0;i<8;i++){wait_buffered();fake_pump(ar.stream,output);assert(output[0]==.25f);}
    /* Close must also complete while the writer owns its detached buffer. */
    ar_close();atomic_store(&fake_write_hold,0);pthread_join(writer,NULL);
    struct stat captured;assert(!stat(capture,&captured)&&captured.st_size==480000*2*4);
    ar_close();assert(!unlink(capture));assert(!unlink(strcat(strcpy(marker,temp),"/audio-initial-parameters.bin"))||errno==ENOENT);
    assert(!rmdir(temp));unsetenv("DM_FRAME_DIR");
}
int main(void){
    alarm(30);fixture();float output[480];checkpoint_checks();
    opened();assert(update()==0);fake_pcm_referenced=1;assert(update()==0);uint32_t pool_state;memcpy(&pool_state,fake_pool_status,4);assert(pool_state==2);
    fake_pcm_referenced=0;assert(update()==0);memcpy(&pool_state,fake_pool_status,4);assert(pool_state==3);
    RtgAudioRenderer *mixer=ar.mixer;AAudioStream *old=ar.stream;int attempts=fake_opens;fake_open_failures=1;
    old->callbacks.error(old,old->callbacks.error_ctx,AAUDIO_ERROR_DISCONNECTED);
    assert(update()==0);assert(ar.mixer==mixer&&ar.stream!=old&&fake_opens==attempts+2&&ar.recoveries==1);
    old->callbacks.error(old,old->callbacks.error_ctx,AAUDIO_ERROR_DISCONNECTED);assert(atomic_load(&ar.error)==0);
    fake_pump(ar.stream,output);assert(output[0]==.25f);ar_close();
    for(int i=0;i<2;i++){
        opened();atomic_store(i?&fake_effect_error:&fake_update_error,RTG_AUDIO_UNSUPPORTED);
        assert(update()==0x5299);int renders=atomic_load(&fake_render_calls);uint64_t consumed=atomic_load(&ar.consumed_frames);
        fake_pump(ar.stream,output);assert(output[0]==0&&atomic_load(&fake_render_calls)==renders&&atomic_load(&ar.consumed_frames)==consumed);
        atomic_store(&fake_update_error,0);atomic_store(&fake_effect_error,0);assert(update()==0x5299);ar_close();
    }
    concurrent_close(0);concurrent_close(1);
    opened();attempts=fake_opens;fake_open_failures=8;ar_error(ar.stream,(void *)atomic_load(&ar.generation),AAUDIO_ERROR_DISCONNECTED);
    assert(update()==0x5299);assert(ar.recovery_failed&&fake_opens==attempts+3&&!ar.stream);ar_close();fake_open_failures=0;
    for(int i=0;i<12;i++){opened();ar_error(ar.stream,(void *)atomic_load(&ar.generation),AAUDIO_ERROR_DISCONNECTED);ar_close();}
    opened();assert(update()==0);ar_close();capture_checks();
    for(unsigned i=0;i<fake_stream_count;i++){assert(fake_streams[i]->closed);pthread_mutex_destroy(&fake_streams[i]->lock);pthread_cond_destroy(&fake_streams[i]->idle);free(fake_streams[i]);}
    printf("AUDIO_HLE_LIFECYCLE close_serialization=1 status_lifetime=1 detach=1 recover=1 bounded_failure=1 stale_callback=1 invalid_update_quiescence=1 pcm_ring=1 consumed_checkpoint=1 generation_filter=1 partial_block=1 mute=1 underrun_capture=1 unlocked_writer=1 passed=1 opens=%d closes=%d\n",fake_opens,fake_closes);
    return 0;
}
