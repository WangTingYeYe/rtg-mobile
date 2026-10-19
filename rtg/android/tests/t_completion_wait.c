#include "../nvn_completion_wait.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static DmNvnCompletionWait changed;
static _Atomic unsigned completed,ready;
static unsigned payload;
static void *consumer(void *argument) {
    unsigned target=(unsigned)(uintptr_t)argument;
    atomic_fetch_add(&ready,1);
    for (;;) {
        uint32_t epoch=dm_nvn_completion_epoch(&changed);
        if(atomic_load(&completed)>=target)break;
        assert(!dm_nvn_completion_sleep(&changed,epoch,UINT64_MAX));
    }
    assert(payload==0x12345678);
    return NULL;
}
int main(void) {
    uint32_t epoch=dm_nvn_completion_epoch(&changed);
    dm_nvn_completion_notify(&changed);
    /* Publication between predicate check and sleeping must not lose a wake. */
    assert(!dm_nvn_completion_sleep(&changed,epoch,UINT64_MAX));
    epoch=dm_nvn_completion_epoch(&changed);
    assert(dm_nvn_completion_sleep(&changed,epoch,0)==ETIMEDOUT);
    assert(dm_nvn_completion_sleep(&changed,epoch,2000000)==ETIMEDOUT);
    for(unsigned round=1;round<=100;round++){
        pthread_t threads[4];atomic_store(&ready,0);
        for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,consumer,(void *)(uintptr_t)round));
        while(atomic_load(&ready)!=4)sched_yield();
        /* Spurious/unrelated notification cannot satisfy the real predicate. */
        dm_nvn_completion_notify(&changed);
        payload=0x12345678;atomic_store(&completed,round);dm_nvn_completion_notify(&changed);
        for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    }
    puts("PASS: publish-before-sleep, timeout, unrelated wakes, 400 waiters, data visibility");
}
