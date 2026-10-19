#pragma once
#include <stdint.h>
#include <string.h>
typedef struct {uint64_t last,seed,sum,period;unsigned ticks;} DmVsyncPeriod;
static inline void dm_nvn_vsync_sample(DmVsyncPeriod *s,uint64_t stamp) {
    if(!stamp){memset(s,0,sizeof *s);return;}
    uint64_t old=s->last;s->last=stamp;
    if(!old||stamp<=old)return;
    uint64_t delta=stamp-old;
    if(delta<4000000||delta>100000000){s->seed=s->sum=s->period=s->ticks=0;return;}
    if(!s->seed){if(delta>25000000)return;s->seed=delta;}
    unsigned ticks=(unsigned)((delta+s->seed/2)/s->seed);
    uint64_t expected=s->seed*ticks,error=delta>expected?delta-expected:expected-delta;
    if(!ticks||error>s->seed/6){s->seed=s->sum=s->period=s->ticks=0;return;}
    s->sum+=delta;s->ticks+=ticks;
    if(s->ticks>=120){s->period=s->sum/s->ticks;s->seed=s->period;s->sum=s->ticks=0;}
}
/* Correct small discrepancies between advertised and measured refresh only.
 * At 90 Hz, for example, this must not turn a 60 FPS request into 45 or 90. */
static inline uint64_t dm_nvn_vsync_match_period(uint64_t requested,uint64_t measured) {
    if(!requested||measured<4000000||measured>25000000)return requested;
    uint64_t stride=(requested+measured/2)/measured;
    if(!stride||stride>8)return requested;
    uint64_t matched=measured*stride,difference=matched>requested?matched-requested:requested-matched;
    return difference<=requested/100?matched:requested;
}
