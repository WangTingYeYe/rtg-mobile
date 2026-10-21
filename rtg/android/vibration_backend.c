#include "vibration_backend.h"
#include "../host/dm.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

_Static_assert(sizeof(dm_vibration_value)==16,"nn::hid::VibrationValue ABI");
/* Handle ABI: style index, player number, device index, reserved (one byte each).
 * The logical pad's two rumble channels mix into one phone motor. References:
 * libnx hid.h / hid.c. Switch passes an amplitude and a carrier frequency for
 * the low and high band; combine their energy and weight the carrier into a
 * 0..1 sharpness. Android exposes amplitude, so the JVM side keeps 256 levels
 * and uses the sharpness for its pulse envelope. */
static const uint32_t handles[]={0x00000003,0x00010003,0x00002004,0x00012004};
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static uint64_t session;
static uint32_t epoch;
static int available,foreground,initialized[4];
static dm_vibration_value channels[4];
typedef struct { int intensity, sharpness; } pulse;
enum { QUEUE=64 };
static pulse queue[QUEUE],published={0,0};
static int head,count;

static int same(pulse a,pulse b){return a.intensity==b.intensity&&a.sharpness==b.sharpness;}
static void publish(pulse value) {
    if(same(value,published))return;
    published=value;
    /* Never accumulate seconds of delayed effects if the OS stalls the worker. */
    if(count==QUEUE){head=0;count=0;}
    queue[(head+count)%QUEUE]=value;count++;pthread_cond_signal(&changed);
}
static void reset(void) {
    memset(channels,0,sizeof channels);head=count=0;published=(pulse){-1,-1};
    publish((pulse){0,0});
}
/* Touchable's mono mix: take the stronger channel of each band, add their
 * energies, and weight the log carrier frequency into sharpness. */
static pulse mixed(void) {
    const dm_vibration_value *low=&channels[0],*high=&channels[0];
    for(int i=1;i<4;i++){
        if(channels[i].amplitude_low>low->amplitude_low)low=&channels[i];
        if(channels[i].amplitude_high>high->amplitude_high)high=&channels[i];
    }
    float low_power=low->amplitude_low*low->amplitude_low,high_power=high->amplitude_high*high->amplitude_high;
    float power=low_power+high_power;
    if(!(power>0))return (pulse){0,0};
    float intensity=fminf(1.f,sqrtf(power));
    float sharpness=(low_power*log2f(fmaxf(low->frequency_low,40.f)/40.f)+
                     high_power*log2f(fmaxf(high->frequency_high,40.f)/40.f))/(5.f*power);
    sharpness=fminf(1.f,fmaxf(0.f,sharpness));
    return (pulse){(int)lroundf(intensity*255.f),(int)lroundf(sharpness*255.f)};
}
static void publish_mix(void) {
    pulse value=mixed();
    static int logged;
    if(value.intensity&&logged<8){logged++;LOG("[vibration] intensity=%d sharpness=%d",value.intensity,value.sharpness);}
    publish(value);
}
uint64_t dm_android_vibration_bind(int supported) {
    pthread_mutex_lock(&lock);session++;epoch++;available=!!supported;foreground=0;reset();
    pthread_cond_broadcast(&changed);uint64_t result=session;pthread_mutex_unlock(&lock);return result;
}
uint32_t dm_android_vibration_active(uint64_t owner,int on) {
    pthread_mutex_lock(&lock);
    if(owner==session){foreground=!!on;epoch++;reset();}
    uint32_t result=epoch;pthread_mutex_unlock(&lock);return result;
}
void dm_android_vibration_unbind(uint64_t owner) {
    pthread_mutex_lock(&lock);
    if(owner==session){session++;epoch++;available=foreground=0;reset();pthread_cond_broadcast(&changed);}
    pthread_mutex_unlock(&lock);
}
int64_t dm_android_vibration_wait(uint64_t owner) {
    pthread_mutex_lock(&lock);
    while(owner==session&&!count)pthread_cond_wait(&changed,&lock);
    int64_t result=-1;
    if(owner==session){
        pulse value=queue[head];head=(head+1)%QUEUE;count--;
        result=DM_VIBRATION_EVENT(epoch,value.sharpness,value.intensity);
    }
    pthread_mutex_unlock(&lock);return result;
}
int dm_android_vibration_permitted(void) {
    pthread_mutex_lock(&lock);int result=available;pthread_mutex_unlock(&lock);return result;
}
int dm_android_vibration_handles(uint32_t *out,int capacity,uint32_t id,uint32_t style) {
    if(capacity<0||(capacity&&!out))return -EINVAL;
    int first=id==0&&(style&1)?0:id==0x20&&(style&2)?2:-1;
    if(first<0||!dm_android_vibration_permitted())return 0;
    int n=capacity<2?capacity:2;for(int i=0;i<n;i++)out[i]=handles[first+i];return n;
}
static int slot(uint32_t handle) {for(int i=0;i<4;i++)if(handles[i]==handle)return i;return -1;}
int dm_android_vibration_initialize(uint32_t handle) {
    int i=slot(handle);if(i<0)return EINVAL;
    pthread_mutex_lock(&lock);initialized[i]=1;pthread_mutex_unlock(&lock);return 0;
}
int dm_android_vibration_send(uint32_t handle,const dm_vibration_value *v) {
    int i=slot(handle);
    if(i<0||!v||!isfinite(v->amplitude_low)||!isfinite(v->amplitude_high)||
       !isfinite(v->frequency_low)||!isfinite(v->frequency_high))return EINVAL;
    pthread_mutex_lock(&lock);
    if(!initialized[i]){pthread_mutex_unlock(&lock);return EINVAL;}
    channels[i]=*v;
    if(available&&foreground)publish_mix();
    pthread_mutex_unlock(&lock);return 0;
}