/*
 * common.h - small portable helpers shared by the lock and barrier code.
 *
 * Everything here is C11 + Linux (futex). The original paper (Mellor-Crummey
 * & Scott, TOCS 1991) assumed sequentially consistent memory; we use C11
 * atomics with explicit acquire/release orderings instead (see report §4).
 */
#ifndef MCS_COMMON_H
#define MCS_COMMON_H

#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdalign.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <linux/futex.h>
#include <sys/syscall.h>

/*
 * Padding unit. Cache lines are 64 B on x86, but Intel's adjacent-line
 * ("spatial") prefetcher pulls lines in 128 B pairs, so hot words that are
 * written by different threads are kept 128 B apart.
 */
#define PAD 128

/* CPU hint inside spin loops: x86 PAUSE / ARM YIELD. */
static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}

/* Busy-wait for n "pause units" - the paper's pause(delay). */
static inline void spin_pause(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) cpu_relax();
}

/* Thin futex wrappers (process-private futexes). */
static inline void futex_wait(_Atomic uint32_t *addr, uint32_t expected) {
    syscall(SYS_futex, (uint32_t *)addr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
}
static inline void futex_wake(_Atomic uint32_t *addr, int n) {
    syscall(SYS_futex, (uint32_t *)addr, FUTEX_WAKE_PRIVATE, n, NULL, NULL, 0);
}

static inline double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline unsigned next_pow2(unsigned x) {
    unsigned p = 1;
    while (p < x) p <<= 1;
    return p;
}

#endif
