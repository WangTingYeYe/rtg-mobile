#include "nvn_translation_queue.h"
#include "nvn_completion_wait.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
DmNvnCompletionWait dm_nvn_sync_completion;

static void checked(int result) { if (result) abort(); }
static _Thread_local uint64_t current_ticket;
uint64_t dm_nvn_translation_current_ticket(void) { return current_ticket; }
static void invoke(dm_nvn_translation_queue *q,void *payload,uint64_t ticket) {
    current_ticket=ticket;
    q->run(payload);
    current_ticket=0;
}
static void *translate(void *arg) {
    dm_nvn_translation_queue *q = arg;
    pthread_setname_np(pthread_self(), "nvn-translate");
    checked(pthread_mutex_lock(&q->mutex));
    for (;;) {
        while (q->inline_active || (q->completed == q->submitted && !q->stopping)) {
            if (q->idle) q->idle(1);
            checked(pthread_cond_wait(&q->changed, &q->mutex));
            if (q->idle) q->idle(0);
        }
        if (q->completed == q->submitted && q->stopping) break;
        uint64_t ticket = q->completed + 1;
        void *payload = q->jobs[ticket % DM_NVN_TRANSLATION_CAPACITY];
        checked(pthread_mutex_unlock(&q->mutex));
        invoke(q,payload,ticket);
        checked(pthread_mutex_lock(&q->mutex));
        q->completed = ticket;
        checked(pthread_cond_broadcast(&q->changed));
    }
    checked(pthread_mutex_unlock(&q->mutex));
    return NULL;
}

int dm_nvn_translation_init(dm_nvn_translation_queue *q, void (*run)(void *), int synchronous,
                            void (*idle)(int), void (*prepare)(void *,uint64_t)) {
    memset(q, 0, sizeof(*q));
    q->run = run;
    q->idle = idle;
    q->prepare = prepare;
    q->synchronous = synchronous != 0;
    checked(pthread_mutex_init(&q->mutex, NULL));
    pthread_condattr_t condition;
    checked(pthread_condattr_init(&condition));
    checked(pthread_condattr_setclock(&condition, CLOCK_MONOTONIC));
    checked(pthread_cond_init(&q->changed, &condition));
    checked(pthread_condattr_destroy(&condition));
    if (q->synchronous) return 0;
    pthread_attr_t attr;
    checked(pthread_attr_init(&attr));
    checked(pthread_attr_setstacksize(&attr, 8u << 20));
    int error = pthread_create(&q->thread, &attr, translate, q);
    checked(pthread_attr_destroy(&attr));
    if (error) q->synchronous = 1;
    return error;
}

uint64_t dm_nvn_translation_push(dm_nvn_translation_queue *q, void *payload) {
    return dm_nvn_translation_push_inline(q,payload,0);
}
uint64_t dm_nvn_translation_push_inline(dm_nvn_translation_queue *q,void *payload,int allow_inline) {
    checked(pthread_mutex_lock(&q->mutex));
    while (q->submitted - q->completed >= DM_NVN_TRANSLATION_CAPACITY)
        checked(pthread_cond_wait(&q->changed, &q->mutex));
    if (q->stopping || q->submitted == UINT64_MAX) abort();
    uint64_t ticket = ++q->submitted;
    if (q->prepare) q->prepare(payload,ticket);
    if (q->synchronous || (allow_inline && q->completed == ticket - 1)) {
        /* Multiple fallback producers still execute in ticket order. Never
         * hold the queue mutex during translation: timed waits must work. */
        while (q->completed != ticket - 1)
            checked(pthread_cond_wait(&q->changed, &q->mutex));
        q->inline_active=1;
        checked(pthread_mutex_unlock(&q->mutex));
        invoke(q,payload,ticket);
        checked(pthread_mutex_lock(&q->mutex));
        q->completed = ticket;
        q->inline_active=0;
    } else {
        q->jobs[ticket % DM_NVN_TRANSLATION_CAPACITY] = payload;
    }
    checked(pthread_cond_broadcast(&q->changed));
    checked(pthread_mutex_unlock(&q->mutex));
    return ticket;
}

uint64_t dm_nvn_translation_tail(dm_nvn_translation_queue *q) {
    checked(pthread_mutex_lock(&q->mutex));
    uint64_t ticket = q->submitted;
    checked(pthread_mutex_unlock(&q->mutex));
    return ticket;
}

int dm_nvn_translation_wait(dm_nvn_translation_queue *q, uint64_t ticket, uint64_t timeout_ns) {
    struct timespec deadline = {0};
    if (timeout_ns && timeout_ns != UINT64_MAX) {
        checked(clock_gettime(CLOCK_MONOTONIC, &deadline));
        uint64_t ns = (uint64_t)deadline.tv_nsec + timeout_ns % 1000000000;
        deadline.tv_sec += (time_t)(timeout_ns / 1000000000 + ns / 1000000000);
        deadline.tv_nsec = (long)(ns % 1000000000);
    }
    checked(pthread_mutex_lock(&q->mutex));
    while (q->completed < ticket) {
        if (!timeout_ns) break;
        int error = timeout_ns == UINT64_MAX ? pthread_cond_wait(&q->changed, &q->mutex) :
            pthread_cond_timedwait(&q->changed, &q->mutex, &deadline);
        if (error == ETIMEDOUT) break;
        checked(error);
    }
    int complete = q->completed >= ticket;
    checked(pthread_mutex_unlock(&q->mutex));
    return complete;
}

void dm_nvn_translation_destroy(dm_nvn_translation_queue *q) {
    checked(pthread_mutex_lock(&q->mutex));
    q->stopping = 1;
    checked(pthread_cond_broadcast(&q->changed));
    checked(pthread_mutex_unlock(&q->mutex));
    if (!q->synchronous) checked(pthread_join(q->thread, NULL));
    checked(pthread_cond_destroy(&q->changed));
    checked(pthread_mutex_destroy(&q->mutex));
}
