#pragma once
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* A notification epoch, not a completion predicate. Read the epoch BEFORE
 * testing the real fence/generation, then sleep only while it is unchanged.
 * Futex's atomic comparison closes the publish-before-sleep race. */
typedef struct {
    _Atomic uint32_t epoch;
} DmNvnCompletionWait;

static inline uint32_t dm_nvn_completion_epoch(DmNvnCompletionWait *wait) {
    return atomic_load_explicit(&wait->epoch, memory_order_acquire);
}
static inline void dm_nvn_completion_notify(DmNvnCompletionWait *wait) {
    atomic_fetch_add_explicit(&wait->epoch, 1, memory_order_release);
    syscall(SYS_futex, &wait->epoch, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
}
static inline int dm_nvn_completion_sleep(DmNvnCompletionWait *wait, uint32_t epoch,
                                         uint64_t timeout_ns) {
    if (!timeout_ns) return ETIMEDOUT;
    struct timespec timeout = {(time_t)(timeout_ns / 1000000000), (long)(timeout_ns % 1000000000)};
    int result = syscall(SYS_futex, &wait->epoch, FUTEX_WAIT_PRIVATE, epoch,
                         timeout_ns == UINT64_MAX ? NULL : &timeout, NULL, 0);
    int error = result < 0 ? errno : 0;
    return error == EAGAIN || error == EINTR ? 0 : error;
}
