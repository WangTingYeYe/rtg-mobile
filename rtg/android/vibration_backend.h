#pragma once
#include <stdint.h>
typedef struct { float amplitude_low,frequency_low,amplitude_high,frequency_high; } dm_vibration_value;
/* dm_android_vibration_wait returns epoch << 16 | sharpness << 8 | intensity, or -1.
 * Keep this packing in one place; the JVM decoder in GameHaptics uses it. */
#define DM_VIBRATION_EVENT(epoch,sharpness,intensity) \
    (((int64_t)(uint32_t)(epoch)<<16)|((int64_t)((sharpness)&0xff)<<8)|((intensity)&0xff))
#define DM_VIBRATION_EPOCH(event) ((uint32_t)((event)>>16))
#define DM_VIBRATION_SHARPNESS(event) ((int)(((event)>>8)&0xff))
#define DM_VIBRATION_INTENSITY(event) ((int)((event)&0xff))
/* Blocking consumer belongs to the Java vibration worker, never the game thread. */
uint64_t dm_android_vibration_bind(int available);
uint32_t dm_android_vibration_active(uint64_t session,int active);
void dm_android_vibration_unbind(uint64_t session);
int64_t dm_android_vibration_wait(uint64_t session);
int dm_android_vibration_permitted(void);
int dm_android_vibration_handles(uint32_t *out,int capacity,uint32_t id,uint32_t style);
int dm_android_vibration_initialize(uint32_t handle);
int dm_android_vibration_send(uint32_t handle,const dm_vibration_value *value);
