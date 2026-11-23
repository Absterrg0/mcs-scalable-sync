/*
 * barriers.h - barriers from Mellor-Crummey & Scott (TOCS 1991, Section 3)
 * plus a pthread baseline and our spin-then-park central barrier.
 */
#ifndef MCS_BARRIERS_H
#define MCS_BARRIERS_H

#include "common.h"

typedef struct barrier_ops {
    const char *name;
    const char *desc;
    void *(*create)(int nthreads);
    void (*destroy)(void *b);
    /* Per-thread state: created by the thread itself (first-touch). */
    void *(*thread_init)(void *b, int id);
    void (*wait)(void *b, void *tstate);
} barrier_ops_t;

extern uint32_t g_barrier_spin_limit;  /* central_sp: spins before parking */

const barrier_ops_t *barrier_find(const char *name);
const barrier_ops_t *barrier_all(int *n);

#endif
