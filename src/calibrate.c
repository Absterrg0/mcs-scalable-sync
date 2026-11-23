/*
 * calibrate - hardware/OS constants that explain the lock results:
 *   1. cost of one PAUSE (the unit of all backoff / spin limits)
 *   2. one-way cache-line transfer latency between pairs of CPUs
 *      (the modern equivalent of the paper's "remote reference")
 *   3. futex wake-up latency (what a parked waiter costs on hand-off)
 *
 * usage: calibrate [cpuA,cpuB ...]   e.g. calibrate 0,1 0,2 0,4 4,5 4,8
 */
#include "common.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static void pin(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}

static double pause_ns(int cpu) {
    pin(cpu);
    const int N = 2000000;
    spin_pause(100000); /* warm up / let the core clock up */
    double t0 = now_sec();
    spin_pause(N);
    return (now_sec() - t0) / N * 1e9;
}

/* ---- cache-line ping-pong ---- */
static alignas(PAD) _Atomic uint32_t line;
static alignas(PAD) _Atomic uint32_t fx[2];
enum { ROUNDS = 200000, FROUNDS = 20000 };
typedef struct { int cpu, parity, mode; } arg_t;

static void *pp_thread(void *p) {
    arg_t *a = p;
    if (a->cpu >= 0) pin(a->cpu);
    if (a->mode == 0) {
        for (uint32_t i = a->parity; i < 2 * ROUNDS; i += 2) {
            while (atomic_load_explicit(&line, memory_order_acquire) != i) cpu_relax();
            atomic_store_explicit(&line, i + 1, memory_order_release);
        }
    } else {
        /* futex ping-pong: wait on my word, then wake the other side */
        int me = a->parity, other = 1 - me;
        for (int i = 0; i < FROUNDS; i++) {
            if (me == 1 || i > 0) {
                while (atomic_load(&fx[me]) == 0) futex_wait(&fx[me], 0);
                atomic_store(&fx[me], 0);
            }
            atomic_store(&fx[other], 1);
            futex_wake(&fx[other], 1);
        }
    }
    return NULL;
}

static double pingpong(int a, int b, int mode) {
    pthread_t t1, t2;
    arg_t x = {a, 0, mode}, y = {b, 1, mode};
    atomic_store(&line, 0);
    atomic_store(&fx[0], 0);
    atomic_store(&fx[1], 0);
    double t0 = now_sec();
    pthread_create(&t1, NULL, pp_thread, &x);
    pthread_create(&t2, NULL, pp_thread, &y);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    double el = now_sec() - t0;
    return el / (2.0 * (mode == 0 ? ROUNDS : FROUNDS)) * 1e9;
}

int main(int argc, char **argv) {
    printf("measurement,cpus,ns\n");
    printf("pause,%d,%.2f\n", 0, pause_ns(0));
    int ecpu = argc > 1 ? atoi(argv[argc - 1]) : 4; /* any E-core id */
    for (int i = 1; i < argc; i++) {
        int a, b;
        if (sscanf(argv[i], "%d,%d", &a, &b) == 2 && b >= 4) { ecpu = b; break; }
    }
    printf("pause,%d,%.2f\n", ecpu, pause_ns(ecpu));
    for (int i = 1; i < argc; i++) {
        int a, b;
        if (sscanf(argv[i], "%d,%d", &a, &b) != 2) continue;
        double best = 1e18;
        for (int r = 0; r < 5; r++) { double v = pingpong(a, b, 0); if (v < best) best = v; }
        printf("line_transfer_oneway,%d-%d,%.1f\n", a, b, best);
    }
    double fbest = 1e18, fun = 1e18;
    for (int r = 0; r < 5; r++) {
        double v = pingpong(0, 2, 1); if (v < fbest) fbest = v;
        v = pingpong(-1, -1, 1); if (v < fun) fun = v;
    }
    printf("futex_wakeup_oneway,0-2,%.0f\n", fbest);
    printf("futex_wakeup_oneway,unpinned,%.0f\n", fun);
    return 0;
}
