#pragma once
#include <pthread.h>
#include <stdint.h>

/* One ordered CPU translator; GPU retirement belongs to the backend. Capacity
 * includes the running job. Payload ownership transfers to run() on push.
 * Callbacks must not push to, wait on, or destroy their own queue. */
#define DM_NVN_TRANSLATION_CAPACITY 32
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    void (*run)(void *payload);
    void (*prepare)(void *payload, uint64_t ticket);
    void (*idle)(int waiting);
    void *jobs[DM_NVN_TRANSLATION_CAPACITY];
    uint64_t submitted, completed;
    int synchronous, stopping, inline_active;
} dm_nvn_translation_queue;

/* A nonzero thread-creation result selects the ordered synchronous fallback.
 * Other pthread initialization errors are fatal. Initialize once before use. */
/* Optional idle hook runs on the worker with the queue mutex held. It must
 * only do local accounting (no queue calls or blocking service calls). */
int dm_nvn_translation_init(dm_nvn_translation_queue *q, void (*run)(void *), int synchronous,
                            void (*idle)(int), void (*prepare)(void *, uint64_t));
/* prepare runs under the queue mutex before publishing the job. It may mark
 * CPU-visible dependencies, but must not block or call the queue. */
uint64_t dm_nvn_translation_current_ticket(void);
uint64_t dm_nvn_translation_push(dm_nvn_translation_queue *q, void *payload);
/* May execute on the caller only when the queue is idle. Set allow_inline
 * only for jobs that cannot wait on future work from the submitting CPU. */
uint64_t dm_nvn_translation_push_inline(dm_nvn_translation_queue *q, void *payload,int allow_inline);
uint64_t dm_nvn_translation_tail(dm_nvn_translation_queue *q);
int dm_nvn_translation_wait(dm_nvn_translation_queue *q, uint64_t ticket, uint64_t timeout_ns);
/* Stop only after all producers have stopped; executes all accepted jobs. */
void dm_nvn_translation_destroy(dm_nvn_translation_queue *q);
