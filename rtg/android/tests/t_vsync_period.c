#include "../nvn_vsync_period.h"
#include "../../gfx/nvn/nvn_pace.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    const uint64_t actual=16597600,nominal=1000000000/60;
    DmVsyncPeriod s={0};uint64_t now=1000000000;
    dm_nvn_vsync_sample(&s,now);
    for(unsigned i=0;i<360;i++){now+=actual*(i%31==0?2:1);dm_nvn_vsync_sample(&s,now);}
    assert(s.period==actual);
    uint64_t adjusted=dm_nvn_vsync_match_period(nominal,s.period);
    assert(adjusted==actual);
    assert(dm_nvn_vsync_match_period(2*nominal,s.period)==2*actual);
    assert(dm_nvn_vsync_match_period(nominal,actual/2)==actual);
    assert(dm_nvn_vsync_match_period(nominal,1000000000/90)==nominal);
    nvn_pacer old={0},fixed={0};uint64_t old_time=1000000000,new_time=old_time;
    uint64_t old_slot=0,new_slot=0;unsigned old_repeats=0,new_repeats=0;
    for(unsigned i=0;i<3600;i++) {
        old_time+=nvn_pace(&old,old_time,nominal);
        new_time+=nvn_pace(&fixed,new_time,adjusted);
        uint64_t a=old_time/actual,b=new_time/actual;
        if(i){old_repeats+=a>old_slot+1;new_repeats+=b>new_slot+1;}
        old_slot=a;new_slot=b;old_time+=4000000;new_time+=4000000;
    }
    assert(old_repeats>=14&&new_repeats==0);
    assert(dm_nvn_vsync_match_period(0,actual)==0);
    assert(dm_nvn_vsync_match_period(nominal,0)==nominal);
    for(unsigned i=0;i<360;i++){now+=actual/2;dm_nvn_vsync_sample(&s,now);}
    assert(s.period==actual/2); /* Foreground refresh changes to 120Hz. */
    assert(dm_nvn_vsync_match_period(nominal,s.period)==actual);
    for(unsigned i=0;i<360;i++){now+=actual;dm_nvn_vsync_sample(&s,now);}
    assert(dm_nvn_vsync_match_period(nominal,s.period)==actual);
    dm_nvn_vsync_sample(&s,0);assert(!s.period&&!s.last);
    puts("PASS: observed 60.25Hz mismatch, missed callbacks, 120Hz divisor, 90Hz fallback, lifecycle reset; periodic repeats 14+ -> 0");
}
