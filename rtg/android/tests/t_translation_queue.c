/* Production pthread queue with controlled stalls; no Android/GPU required. */
#include "../nvn_translation_queue.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int fail_create;
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *p) {
    if(fail_create){fail_create=0;return EAGAIN;}
    return __real_pthread_create(t,a,f,p);
}
typedef struct { uint64_t ticket,observed;int block;pthread_t runner; } job;
static pthread_mutex_t gate_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv=PTHREAD_COND_INITIALIZER;
static int entered,released;
static _Atomic int active;
static uint64_t executed;
static void run(void *p) {
    job *j=p;assert(atomic_fetch_add(&active,1)==0);
    j->runner=pthread_self();
    if(j->block){pthread_mutex_lock(&gate_mu);entered=1;pthread_cond_broadcast(&gate_cv);
        while(!released)pthread_cond_wait(&gate_cv,&gate_mu);
        pthread_mutex_unlock(&gate_mu);}
    j->observed=++executed;
    assert(dm_nvn_translation_current_ticket()==j->observed);
    assert(atomic_fetch_sub(&active,1)==1);
}
static uint64_t ns(void) {struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
typedef struct { dm_nvn_translation_queue *q;job *jobs;int n,inline_work;_Atomic int done; } producer;
static void *produce(void *p) {
    producer *v=p;for(int i=0;i<v->n;i++)v->jobs[i].ticket=dm_nvn_translation_push_inline(v->q,&v->jobs[i],v->inline_work&&(i%2==0));
    atomic_store(&v->done,1);return NULL;
}
static void blocked_queue(void) {
    dm_nvn_translation_queue q;job jobs[33]={0};executed=0;entered=released=0;jobs[0].block=1;
    assert(!dm_nvn_translation_init(&q,run,0,NULL,NULL));
    for(int i=0;i<32;i++)jobs[i].ticket=dm_nvn_translation_push(&q,&jobs[i]);
    pthread_mutex_lock(&gate_mu);while(!entered)pthread_cond_wait(&gate_cv,&gate_mu);pthread_mutex_unlock(&gate_mu);
    producer p={.q=&q,.jobs=&jobs[32],.n=1};pthread_t t;assert(!pthread_create(&t,NULL,produce,&p));
    assert(!dm_nvn_translation_wait(&q,1,0));uint64_t start=ns();
    assert(!dm_nvn_translation_wait(&q,32,10000000));
    assert(ns()-start>=10000000 && ns()-start<1000000000);
    assert(dm_nvn_translation_tail(&q)==32 && !atomic_load(&p.done));
    pthread_mutex_lock(&gate_mu);released=1;pthread_cond_broadcast(&gate_cv);pthread_mutex_unlock(&gate_mu);
    assert(!pthread_join(t,NULL));assert(dm_nvn_translation_wait(&q,33,UINT64_MAX));
    for(int i=0;i<33;i++)assert(jobs[i].ticket==(uint64_t)i+1 && jobs[i].observed==jobs[i].ticket);
    dm_nvn_translation_destroy(&q);
    puts("TRANSLATION_BACKPRESSURE capacity=32 jobs=33 poll_and_timeout=passed");
}
static void concurrent(int mode) {
    enum { PRODUCERS=4, JOBS=5000 };
    job *jobs=calloc(PRODUCERS*JOBS,sizeof(*jobs));assert(jobs);executed=0;
    dm_nvn_translation_queue q;fail_create=mode==2;
    int error=dm_nvn_translation_init(&q,run,mode==1,NULL,NULL);
    assert(error==(mode==2?EAGAIN:0) && q.synchronous==(mode==1||mode==2));
    producer p[PRODUCERS];pthread_t t[PRODUCERS];
    for(int i=0;i<PRODUCERS;i++){p[i]=(producer){.q=&q,.jobs=jobs+i*JOBS,.n=JOBS,.inline_work=mode==3};assert(!pthread_create(&t[i],NULL,produce,&p[i]));}
    for(int i=0;i<PRODUCERS;i++)assert(!pthread_join(t[i],NULL));
    /* Destruction must drain the final jobs too. */
    dm_nvn_translation_destroy(&q);
    assert(executed==PRODUCERS*JOBS && !atomic_load(&active));
    for(int i=0;i<PRODUCERS*JOBS;i++)assert(jobs[i].ticket==jobs[i].observed);
    free(jobs);printf("TRANSLATION_FIFO mode=%d producers=%d jobs=%d passed=1\n",mode,PRODUCERS,PRODUCERS*JOBS);
}
static void inline_order(void) {
    dm_nvn_translation_queue q;job jobs[4]={0};executed=0;entered=released=0;jobs[0].block=1;
    assert(!dm_nvn_translation_init(&q,run,0,NULL,NULL));
    producer p={.q=&q,.jobs=jobs,.n=1,.inline_work=1};pthread_t t;assert(!pthread_create(&t,NULL,produce,&p));
    pthread_mutex_lock(&gate_mu);while(!entered)pthread_cond_wait(&gate_cv,&gate_mu);pthread_mutex_unlock(&gate_mu);
    assert(pthread_equal(jobs[0].runner,t));
    for(int i=1;i<4;i++)jobs[i].ticket=dm_nvn_translation_push_inline(&q,&jobs[i],i==2);
    assert(!dm_nvn_translation_wait(&q,4,1000000));
    pthread_mutex_lock(&gate_mu);released=1;pthread_cond_broadcast(&gate_cv);pthread_mutex_unlock(&gate_mu);
    assert(!pthread_join(t,NULL));assert(dm_nvn_translation_wait(&q,4,UINT64_MAX));
    for(int i=0;i<4;i++)assert(jobs[i].ticket==jobs[i].observed&&jobs[i].ticket==(uint64_t)i+1);
    dm_nvn_translation_destroy(&q);
    puts("TRANSLATION_INLINE idle=caller busy=worker no_overlap=passed fifo=passed");
}
int main(void) {inline_order();blocked_queue();for(int i=0;i<4;i++)concurrent(i);puts("TRANSLATION_QUEUE passed=1 ordered_jobs=80037 modes=4");return 0;}
