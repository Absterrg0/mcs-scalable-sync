/*
 * locks.h - spin locks from Mellor-Crummey & Scott (TOCS 1991, Section 2)
 * plus baselines and our extension (MCS-SP, spin-then-park MCS).
 *
 * Every lock is used through the same small vtable so that the benchmark
 * harness treats all of them identically (same indirect-call overhead).
 */
#ifndef MCS_LOCKS_H
#define MCS_LOCKS_H

#include "common.h"

/* MCS queue node ("qnode" in the paper). One per thread, padded. */
typedef struct mcs_node {
    alignas(PAD) _Atomic(struct mcs_node *) next;
    /*
     * locked:  plain MCS uses 1 = must wait, 0 = lock granted.
     * MCS-SP adds 2 = waiter is (about to be) parked in futex_wait.
     */
    _Atomic uint32_t locked;
    char pad[PAD - sizeof(void *) - sizeof(uint32_t)];
} mcs_node_t;

/* Per-thread state that some locks need (qnode, array slot). */
typedef struct thread_ctx {
    mcs_node_t node;
    alignas(PAD) unsigned slot;   /* Anderson: my place in the array */
    uint32_t spin_limit;          /* MCS-SP: spins before parking     */
} thread_ctx_t;

typedef struct lock_ops {
    const char *name;
    const char *desc;
    void *(*create)(int max_threads);
    void (*destroy)(void *l);
    void (*acquire)(void *l, thread_ctx_t *t);
    void (*release)(void *l, thread_ctx_t *t);
} lock_ops_t;

/* Tunables (set from the command line before locks are used). */
extern uint32_t g_tas_backoff_base;     /* initial backoff, pause units  */
extern uint32_t g_tas_backoff_cap;      /* maximum backoff, pause units  */
extern uint32_t g_ticket_backoff_base;  /* pause units per waiter ahead  */
extern uint32_t g_mcssp_spin_limit;     /* MCS-SP: spins before parking  */
extern uint32_t g_mcsspw_depth;         /* MCS-SPW: waiters to pre-wake  */

const lock_ops_t *lock_find(const char *name);
const lock_ops_t *lock_all(int *n);

#endif
