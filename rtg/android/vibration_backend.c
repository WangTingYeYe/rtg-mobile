#include "vibration_backend.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

_Static_assert(sizeof(dm_vibration_value)==16,"nn::hid::VibrationValue ABI");
/* Handle ABI: style index, player number, device index, reserved (one byte each).
 * The logical pad's two rumble channels mix into one phone motor. References:
 * libnx hid.h / hid.c. API 29's generic vibrator cannot express frequencies;
 * use the stronger band/channel instead of doubling the strength. */
static const uint32_t handles[]={0x00000003,0x00010003,0x00002004,0x00012004};
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static uint64_t session;
static uint32_t epoch;
static int available,foreground,initialized[4],channels[4],amplitude;
enum { QUEUE=64 };
static int queue[QUEUE],head,count;
static void publish(int value) {
    if(value==amplitude)return;
    amplitude=value;
    /* Never accumulate seconds of delayed effects if the OS stalls the worker. */
    if(count==QUEUE){head=0;count=0;}
    queue[(head+count)%QUEUE]=value;count++;pthread_cond_signal(&changed);
}
static void reset(void) {
    memset(channels,0,sizeof channels);head=count=0;amplitude=-1;publish(0);
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
    if(owner==session){result=((int64_t)epoch<<8)|queue[head];head=(head+1)%QUEUE;count--;}
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
    float strength=fmaxf(v->amplitude_low,v->amplitude_high);
    int value=(int)lroundf(fminf(1.f,fmaxf(0.f,strength))*255.f);
    pthread_mutex_lock(&lock);
    if(!initialized[i]){pthread_mutex_unlock(&lock);return EINVAL;}
    if(available&&foreground){
        channels[i]=value;int mixed=0;for(int j=0;j<4;j++)if(channels[j]>mixed)mixed=channels[j];
        publish(mixed);
    }
    pthread_mutex_unlock(&lock);return 0;
}
