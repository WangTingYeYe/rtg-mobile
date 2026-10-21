#include "../vibration_backend.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>

static void expect(uint64_t session,uint32_t epoch,int intensity,int sharpness){
    int64_t event=dm_android_vibration_wait(session);
    if(!(event>=0&&DM_VIBRATION_EPOCH(event)==epoch&&DM_VIBRATION_SHARPNESS(event)==sharpness&&DM_VIBRATION_INTENSITY(event)==intensity))
        fprintf(stderr,"vibration event=%lld expected epoch=%u intensity=%d sharpness=%d\n",
                (long long)event,epoch,intensity,sharpness);
    assert(event>=0&&DM_VIBRATION_EPOCH(event)==epoch&&DM_VIBRATION_SHARPNESS(event)==sharpness&&DM_VIBRATION_INTENSITY(event)==intensity);
}
static void *wait_closed(void *arg){assert(dm_android_vibration_wait(*(uint64_t *)arg)==-1);return NULL;}
int main(void){
    uint32_t handles[3]={0,0,0xdeadbeef};
    uint64_t session=dm_android_vibration_bind(0);
    assert(!dm_android_vibration_permitted());
    assert(!dm_android_vibration_handles(handles,3,0,1));
    session=dm_android_vibration_bind(1);
    assert(dm_android_vibration_permitted());
    assert(dm_android_vibration_handles(handles,3,0,1)==2);
    assert(handles[0]==3&&handles[1]==0x10003&&handles[2]==0xdeadbeef);
    assert(dm_android_vibration_handles(NULL,0,0,1)==0);
    assert(dm_android_vibration_handles(NULL,1,0,1)==-EINVAL);
    assert(dm_android_vibration_handles(handles,2,1,1)==0);
    assert(dm_android_vibration_handles(handles,2,0,2)==0);
    dm_vibration_value half={.amplitude_low=.5,.frequency_low=160,.amplitude_high=.25,.frequency_high=320};
    dm_vibration_value full={.amplitude_low=1,.frequency_low=160},zero={0};
    assert(dm_android_vibration_send(handles[0],&half)==EINVAL);
    assert(!dm_android_vibration_initialize(handles[0]));
    assert(!dm_android_vibration_initialize(handles[1]));
    assert(dm_android_vibration_initialize(0xdeadbeef)==EINVAL);
    uint32_t epoch=dm_android_vibration_active(session,1);expect(session,epoch,0,0);
    /* Both channels combine by band energy, not by max amplitude: half is
     * sqrt(.5^2 + .25^2) = 0.559, and the carrier weights sharpness. */
    assert(!dm_android_vibration_send(handles[0],&half));expect(session,epoch,143,112);
    assert(!dm_android_vibration_send(handles[1],&full));expect(session,epoch,255,105);
    assert(!dm_android_vibration_send(handles[0],&zero));expect(session,epoch,255,102);
    assert(!dm_android_vibration_send(handles[1],&zero));expect(session,epoch,0,0);
    /* A complete short pulse between consumer wakes must not vanish. */
    assert(!dm_android_vibration_send(handles[0],&half));
    assert(!dm_android_vibration_send(handles[0],&zero));
    expect(session,epoch,143,112);expect(session,epoch,0,0);
    dm_vibration_value bad=half;bad.amplitude_low=NAN;
    assert(dm_android_vibration_send(handles[0],&bad)==EINVAL);
    assert(dm_android_vibration_send(handles[0],NULL)==EINVAL);
    /* Pausing discards queued effects; background sends cannot restart motor. */
    assert(!dm_android_vibration_send(handles[0],&full));
    epoch=dm_android_vibration_active(session,0);expect(session,epoch,0,0);
    assert(!dm_android_vibration_send(handles[0],&full));
    epoch=dm_android_vibration_active(session,1);expect(session,epoch,0,0);
    /* Repeated equal values don't fill the queue. */
    for(unsigned i=0;i<100000;i++)assert(!dm_android_vibration_send(handles[0],&half));
    assert(!dm_android_vibration_send(handles[0],&zero));expect(session,epoch,143,112);expect(session,epoch,0,0);
    pthread_t worker;assert(!pthread_create(&worker,NULL,wait_closed,&session));
    dm_android_vibration_unbind(session);assert(!pthread_join(worker,NULL));
    assert(!dm_android_vibration_permitted());
    uint64_t next=dm_android_vibration_bind(1);assert(next!=session);
    dm_android_vibration_unbind(session);assert(dm_android_vibration_permitted());
    assert(dm_android_vibration_wait(session)==-1);
    assert(dm_android_vibration_handles(handles,2,0x20,2)==2);
    assert(handles[0]==0x2004&&handles[1]==0x12004);
    dm_android_vibration_unbind(next);
    puts("PASS vibration ABI, enumeration, two-band energy mix, sharpness, short pulses, deduplication, invalid values, pause/resume, blocking shutdown and stale owner");
    return 0;
}