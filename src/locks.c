/*
 * locks.c - implementations.
 *
 * Paper algorithms (Mellor-Crummey & Scott 1991, Section 2):
 *   tas_backoff  - test_and_set with exponential backoff      (paper Alg. 1)
 *   ticket       - ticket lock with proportional backoff      (paper Alg. 2)
 *   anderson     - Anderson's array-based queuing lock        (paper Alg. 3)
 *   mcs          - MCS list-based queuing lock                (paper Alg. 5)
 *   mcs_swap     - MCS lock using only fetch_and_store        (paper Alg. 6)
 *
 * Baselines / ablations (not in the paper's figures):
 *   tas          - naive test_and_set, no backoff
 *   ttas         - test-and-test_and_set, no backoff
 *   anderson_np  - Anderson lock with slots NOT padded (false sharing)
 *   pthread      - glibc pthread_mutex_t (futex based, non-FIFO)
 *
 * Our extension:
 *   mcs_sp       - MCS with spin-then-park waiting (futex) - see report §5
 */
#include "locks.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

uint32_t g_tas_backoff_base = 1;
uint32_t g_tas_backoff_cap = 1024;
uint32_t g_ticket_backoff_base = 1;
uint32_t g_mcssp_spin_limit = 1024;

static void *xalloc(size_t sz) {
    size_t rounded = (sz + PAD - 1) / PAD * PAD;
    void *p = aligned_alloc(PAD, rounded);
    if (!p) abort();
    memset(p, 0, rounded);
    return p;
}

/* ------------------------------------------------------------------ */
/* Simple word lock used by tas / ttas / tas_backoff.                  */
typedef struct {
    alignas(PAD) _Atomic uint32_t flag;   /* 0 = unlocked, 1 = locked */
} word_lock_t;

static void *word_create(int n) { (void)n; return xalloc(sizeof(word_lock_t)); }
static void generic_destroy(void *l) { free(l); }

static void word_release(void *lp, thread_ctx_t *t) {
    (void)t;
    word_lock_t *l = lp;
    atomic_store_explicit(&l->flag, 0, memory_order_release);
}

/* tas: spin directly on test_and_set (every attempt is a write). */
static void tas_acquire(void *lp, thread_ctx_t *t) {
    (void)t;
    word_lock_t *l = lp;
    while (atomic_exchange_explicit(&l->flag, 1, memory_order_acquire))
        cpu_relax();
}

/* ttas: read until it looks free, then try test_and_set. */
static void ttas_acquire(void *lp, thread_ctx_t *t) {
    (void)t;
    word_lock_t *l = lp;
    for (;;) {
        if (!atomic_exchange_explicit(&l->flag, 1, memory_order_acquire))
            return;
        while (atomic_load_explicit(&l->flag, memory_order_relaxed))
            cpu_relax();
    }
}

/* Paper Alg. 1: test_and_set with exponential backoff. */
static void tas_backoff_acquire(void *lp, thread_ctx_t *t) {
    (void)t;
    word_lock_t *l = lp;
    uint32_t delay = g_tas_backoff_base;
    while (atomic_exchange_explicit(&l->flag, 1, memory_order_acquire)) {
        spin_pause(delay);
        delay = delay * 2 < g_tas_backoff_cap ? delay * 2 : g_tas_backoff_cap;
    }
}

/* ------------------------------------------------------------------ */
/* Paper Alg. 2: ticket lock with proportional backoff.                */
typedef struct {
    alignas(PAD) _Atomic uint32_t next_ticket;
    alignas(PAD) _Atomic uint32_t now_serving;
} ticket_lock_t;

static void *ticket_create(int n) { (void)n; return xalloc(sizeof(ticket_lock_t)); }

static void ticket_acquire(void *lp, thread_ctx_t *t) {
    (void)t;
    ticket_lock_t *l = lp;
    uint32_t my = atomic_fetch_add_explicit(&l->next_ticket, 1, memory_order_relaxed);
    for (;;) {
        uint32_t cur = atomic_load_explicit(&l->now_serving, memory_order_acquire);
        if (cur == my) return;
        /* Wait in proportion to the number of holders ahead of us. */
        spin_pause(g_ticket_backoff_base * (my - cur));
    }
}

static void ticket_release(void *lp, thread_ctx_t *t) {
    (void)t;
    ticket_lock_t *l = lp;
    /* Only the holder writes now_serving, so a plain load + store is fine. */
    uint32_t cur = atomic_load_explicit(&l->now_serving, memory_order_relaxed);
    atomic_store_explicit(&l->now_serving, cur + 1, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* Paper Alg. 3: Anderson's array-based queuing lock.                  */
/*
 * Deviation from the paper: the paper handles wrap-around with an extra
 * atomic_add(-N). We size the array to a power of two >= #threads and let
 * the unsigned counter wrap naturally, which is equivalent and cheaper.
 */
typedef struct { alignas(PAD) _Atomic uint32_t has_lock; } padded_slot_t;

typedef struct {
    alignas(PAD) _Atomic uint32_t next_slot;
    unsigned mask;
    bool padded;
    void *slots;  /* padded_slot_t[] or _Atomic uint32_t[] */
} anderson_lock_t;

static _Atomic uint32_t *anderson_slot(anderson_lock_t *l, unsigned i) {
    if (l->padded) return &((padded_slot_t *)l->slots)[i].has_lock;
    return &((_Atomic uint32_t *)l->slots)[i];
}

static void *anderson_create_impl(int n, bool padded) {
    anderson_lock_t *l = xalloc(sizeof(*l));
    unsigned cap = next_pow2((unsigned)(n < 2 ? 2 : n));
    l->mask = cap - 1;
    l->padded = padded;
    l->slots = xalloc(cap * (padded ? sizeof(padded_slot_t) : sizeof(uint32_t)));
    atomic_store(anderson_slot(l, 0), 1);  /* slot 0 starts with the lock */
    return l;
}
static void *anderson_create(int n) { return anderson_create_impl(n, true); }
static void *anderson_np_create(int n) { return anderson_create_impl(n, false); }

static void anderson_destroy(void *lp) {
    anderson_lock_t *l = lp;
    free(l->slots);
    free(l);
}

static void anderson_acquire(void *lp, thread_ctx_t *t) {
    anderson_lock_t *l = lp;
    unsigned my = atomic_fetch_add_explicit(&l->next_slot, 1, memory_order_relaxed) & l->mask;
    _Atomic uint32_t *s = anderson_slot(l, my);
    while (!atomic_load_explicit(s, memory_order_acquire))
        cpu_relax();
    atomic_store_explicit(s, 0, memory_order_relaxed);  /* reset for reuse */
    t->slot = my;
}

static void anderson_release(void *lp, thread_ctx_t *t) {
    anderson_lock_t *l = lp;
    atomic_store_explicit(anderson_slot(l, (t->slot + 1) & l->mask), 1, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* Paper Alg. 5: the MCS list-based queuing lock.                      */
typedef struct {
    alignas(PAD) _Atomic(mcs_node_t *) tail;
} mcs_lock_t;

static void *mcs_create(int n) { (void)n; return xalloc(sizeof(mcs_lock_t)); }

static void mcs_acquire(void *lp, thread_ctx_t *t) {
    mcs_lock_t *l = lp;
    mcs_node_t *me = &t->node;
    atomic_store_explicit(&me->next, NULL, memory_order_relaxed);
    mcs_node_t *pred = atomic_exchange_explicit(&l->tail, me, memory_order_acq_rel);
    if (pred == NULL) return;                     /* lock was free */
    atomic_store_explicit(&me->locked, 1, memory_order_relaxed);
    atomic_store_explicit(&pred->next, me, memory_order_release);
    while (atomic_load_explicit(&me->locked, memory_order_acquire))
        cpu_relax();                              /* spin on MY OWN node */
}

static void mcs_release(void *lp, thread_ctx_t *t) {
    mcs_lock_t *l = lp;
    mcs_node_t *me = &t->node;
    mcs_node_t *succ = atomic_load_explicit(&me->next, memory_order_acquire);
    if (succ == NULL) {
        mcs_node_t *expected = me;
        if (atomic_compare_exchange_strong_explicit(&l->tail, &expected, NULL,
                                                    memory_order_release, memory_order_relaxed))
            return;                               /* no one waiting */
        /* Someone swapped in behind us but has not linked yet. */
        while ((succ = atomic_load_explicit(&me->next, memory_order_acquire)) == NULL)
            cpu_relax();
    }
    atomic_store_explicit(&succ->locked, 0, memory_order_release);
}

/*
 * Paper Alg. 6: MCS release using only fetch_and_store (no CAS).
 * Gives up strict FIFO order in a rare race ("usurper"), but keeps
 * local spinning and starvation freedom.
 */
static void mcs_swap_release(void *lp, thread_ctx_t *t) {
    mcs_lock_t *l = lp;
    mcs_node_t *me = &t->node;
    mcs_node_t *succ = atomic_load_explicit(&me->next, memory_order_acquire);
    if (succ == NULL) {
        mcs_node_t *old_tail = atomic_exchange_explicit(&l->tail, NULL, memory_order_acq_rel);
        if (old_tail == me) return;               /* really no one waiting */
        /* Put the stolen queue back; whoever swapped in meanwhile is the usurper. */
        mcs_node_t *usurper = atomic_exchange_explicit(&l->tail, old_tail, memory_order_acq_rel);
        while ((succ = atomic_load_explicit(&me->next, memory_order_acquire)) == NULL)
            cpu_relax();
        if (usurper != NULL) {
            /* Usurper got the lock; splice our waiters behind it. */
            atomic_store_explicit(&usurper->next, succ, memory_order_release);
            return;
        }
    }
    atomic_store_explicit(&succ->locked, 0, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* Our extension: MCS-SP (spin-then-park MCS).                         */
/*
 * Problem: in plain MCS the lock is handed to the NEXT thread in the queue.
 * If that thread has been preempted (more threads than cores), the lock sits
 * idle for a whole scheduler time slice while every other waiter burns its
 * own slice spinning. Throughput collapses (we measure >100x).
 *
 * Fix: a waiter spins on its own node for at most `spin_limit` iterations
 * (the common, fast case is unchanged), then announces it is going to sleep
 * by CAS-ing locked 1 -> 2 and blocks in futex_wait. The releaser hands off
 * with an atomic exchange; only if it sees 2 does it pay for a futex_wake
 * system call. Sleeping waiters give their CPU back to the lock holder.
 *
 * Race analysis (see report §5.2):
 *   - waiter CAS 1->2 before releaser exchange: releaser sees 2, wakes it.
 *   - releaser exchange first: waiter's CAS fails (sees 0), no sleep.
 *   - futex_wait(addr, 2) returns at once if the word is no longer 2, so a
 *     wake-up can never be lost between the CAS and the sleep.
 *   - late/spurious wakes are harmless: the waiter re-checks in a loop.
 * The successor-link wait in release also yields the CPU after spinning.
 */
static void mcs_sp_acquire(void *lp, thread_ctx_t *t) {
    mcs_lock_t *l = lp;
    mcs_node_t *me = &t->node;
    atomic_store_explicit(&me->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&me->locked, 1, memory_order_relaxed);
    mcs_node_t *pred = atomic_exchange_explicit(&l->tail, me, memory_order_acq_rel);
    if (pred == NULL) return;
    atomic_store_explicit(&pred->next, me, memory_order_release);

    /* Phase 1: bounded local spinning (identical to MCS). */
    for (uint32_t i = 0; i < t->spin_limit; i++) {
        if (atomic_load_explicit(&me->locked, memory_order_acquire) == 0) return;
        cpu_relax();
    }
    /* Phase 2: announce parking, then sleep until handed the lock. */
    uint32_t expected = 1;
    if (!atomic_compare_exchange_strong_explicit(&me->locked, &expected, 2,
                                                 memory_order_acquire, memory_order_acquire))
        return;                                   /* granted while deciding */
    while (atomic_load_explicit(&me->locked, memory_order_acquire) != 0)
        futex_wait(&me->locked, 2);
}

/*
 * MCS-SPW: MCS-SP + wake-ahead (second step of our extension, report §5.4).
 *
 * With plain spin-then-park, once waiters are asleep every hand-off pays the
 * full wake-up latency L (futex_wake -> scheduler -> thread running), so the
 * lock passes at most once per L. Here the NEW holder walks up to
 * g_mcsspw_depth nodes down the queue and pre-wakes any parked waiter
 * (CAS 2 -> 1, futex_wake), so its wake-up overlaps the critical sections
 * ahead of it. The queue is still strictly FIFO.
 *
 * Safety of the walk: while we hold the lock, no node behind us can be
 * granted (only our release starts that chain), so every node reachable
 * from me->next is still enqueued and its memory is live.
 * A pre-woken waiter sees locked == 1 and simply goes back to spinning
 * (and may park again). If the pre-wake CAS 2->1 wins against nothing
 * (releaser later exchanges 1 -> 0), the releaser correctly skips the
 * futex_wake because the waiter is already awake.
 */
uint32_t g_mcsspw_depth = 2;

static void mcs_spw_wait(mcs_node_t *me, uint32_t spin_limit) {
    for (;;) {
        for (uint32_t i = 0; i < spin_limit; i++) {
            if (atomic_load_explicit(&me->locked, memory_order_acquire) == 0) return;
            cpu_relax();
        }
        uint32_t expected = 1;
        if (!atomic_compare_exchange_strong_explicit(&me->locked, &expected, 2,
                                                     memory_order_acquire, memory_order_acquire))
            if (expected == 0) return;
        uint32_t v;
        while ((v = atomic_load_explicit(&me->locked, memory_order_acquire)) == 2)
            futex_wait(&me->locked, 2);
        if (v == 0) return;
        /* v == 1: pre-woken by a holder ahead of us; spin again. */
    }
}

static void mcs_spw_acquire(void *lp, thread_ctx_t *t) {
    mcs_lock_t *l = lp;
    mcs_node_t *me = &t->node;
    atomic_store_explicit(&me->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&me->locked, 1, memory_order_relaxed);
    mcs_node_t *pred = atomic_exchange_explicit(&l->tail, me, memory_order_acq_rel);
    if (pred != NULL) {
        atomic_store_explicit(&pred->next, me, memory_order_release);
        mcs_spw_wait(me, t->spin_limit);
    }
    /* We hold the lock: pre-wake parked waiters near the head of the queue. */
    mcs_node_t *n = atomic_load_explicit(&me->next, memory_order_acquire);
    for (uint32_t d = 0; d < g_mcsspw_depth && n; d++) {
        uint32_t two = 2;
        if (atomic_load_explicit(&n->locked, memory_order_relaxed) == 2 &&
            atomic_compare_exchange_strong_explicit(&n->locked, &two, 1,
                                                    memory_order_relaxed, memory_order_relaxed))
            futex_wake(&n->locked, 1);
        n = atomic_load_explicit(&n->next, memory_order_acquire);
    }
}

static void mcs_sp_release(void *lp, thread_ctx_t *t) {
    mcs_lock_t *l = lp;
    mcs_node_t *me = &t->node;
    mcs_node_t *succ = atomic_load_explicit(&me->next, memory_order_acquire);
    if (succ == NULL) {
        mcs_node_t *expected = me;
        if (atomic_compare_exchange_strong_explicit(&l->tail, &expected, NULL,
                                                    memory_order_release, memory_order_relaxed))
            return;
        /* Successor is between its swap and its link: it may be preempted
         * right there, so stop spinning after a while and yield the CPU. */
        uint32_t spins = 0;
        while ((succ = atomic_load_explicit(&me->next, memory_order_acquire)) == NULL) {
            if (++spins < t->spin_limit) cpu_relax();
            else sched_yield();
        }
    }
    uint32_t old = atomic_exchange_explicit(&succ->locked, 0, memory_order_release);
    if (old == 2) futex_wake(&succ->locked, 1);   /* only pay a syscall if needed */
}

/* ------------------------------------------------------------------ */
/* Baseline: glibc pthread mutex.                                      */
typedef struct { alignas(PAD) pthread_mutex_t m; } pmutex_t;

static void *pthread_create_lock(int n) {
    (void)n;
    pmutex_t *l = xalloc(sizeof(*l));
    pthread_mutex_init(&l->m, NULL);
    return l;
}
static void pthread_destroy_lock(void *lp) {
    pmutex_t *l = lp;
    pthread_mutex_destroy(&l->m);
    free(l);
}
static void pthread_acquire(void *lp, thread_ctx_t *t) { (void)t; pthread_mutex_lock(&((pmutex_t *)lp)->m); }
static void pthread_release(void *lp, thread_ctx_t *t) { (void)t; pthread_mutex_unlock(&((pmutex_t *)lp)->m); }

/* ------------------------------------------------------------------ */
static const lock_ops_t LOCKS[] = {
    {"tas", "test_and_set, no backoff (baseline)", word_create, generic_destroy, tas_acquire, word_release},
    {"ttas", "test-and-test_and_set (baseline)", word_create, generic_destroy, ttas_acquire, word_release},
    {"tas_backoff", "test_and_set + exponential backoff (paper Alg. 1)", word_create, generic_destroy, tas_backoff_acquire, word_release},
    {"ticket", "ticket lock + proportional backoff (paper Alg. 2)", ticket_create, generic_destroy, ticket_acquire, ticket_release},
    {"anderson", "Anderson array queue lock, padded (paper Alg. 3)", anderson_create, anderson_destroy, anderson_acquire, anderson_release},
    {"anderson_np", "Anderson array lock, unpadded (ablation)", anderson_np_create, anderson_destroy, anderson_acquire, anderson_release},
    {"mcs", "MCS list-based queue lock (paper Alg. 5)", mcs_create, generic_destroy, mcs_acquire, mcs_release},
    {"mcs_swap", "MCS with fetch_and_store-only release (paper Alg. 6)", mcs_create, generic_destroy, mcs_acquire, mcs_swap_release},
    {"mcs_sp", "MCS-SP: spin-then-park MCS (our extension)", mcs_create, generic_destroy, mcs_sp_acquire, mcs_sp_release},
    {"mcs_spw", "MCS-SPW: MCS-SP + wake-ahead (our extension, step 2)", mcs_create, generic_destroy, mcs_spw_acquire, mcs_sp_release},
    {"pthread", "glibc pthread_mutex (baseline)", pthread_create_lock, pthread_destroy_lock, pthread_acquire, pthread_release},
};

const lock_ops_t *lock_all(int *n) {
    *n = (int)(sizeof(LOCKS) / sizeof(LOCKS[0]));
    return LOCKS;
}

const lock_ops_t *lock_find(const char *name) {
    int n;
    const lock_ops_t *all = lock_all(&n);
    for (int i = 0; i < n; i++)
        if (strcmp(all[i].name, name) == 0) return &all[i];
    return NULL;
}
