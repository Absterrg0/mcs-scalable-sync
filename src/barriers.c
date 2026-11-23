/*
 * barriers.c
 *
 * Paper algorithms (Mellor-Crummey & Scott 1991, Section 3):
 *   central      - centralized sense-reversing counter barrier (paper Alg. 7)
 *   dissem       - dissemination barrier (paper Alg. 9)
 *   mcs_tree     - the paper's new tree barrier (paper Alg. 11): 4-ary arrival,
 *                  binary wake-up tree, every thread spins on local data
 * Baseline:
 *   pthread      - glibc pthread_barrier_t
 * Our extension:
 *   central_sp   - centralized barrier with spin-then-park (futex) waiting
 */
#include "barriers.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

uint32_t g_barrier_spin_limit = 4096;

static void *xalloc(size_t sz) {
    size_t rounded = (sz + PAD - 1) / PAD * PAD;
    void *p = aligned_alloc(PAD, rounded);
    if (!p) abort();
    memset(p, 0, rounded);
    return p;
}

/* Per-thread sense flag, used by the central barriers. */
typedef struct { alignas(PAD) uint32_t local_sense; } sense_state_t;

static void *sense_thread_init(void *b, int id) {
    (void)b; (void)id;
    sense_state_t *s = xalloc(sizeof(*s));
    s->local_sense = 0;
    return s;
}

/* ------------------------------------------------------------------ */
/* Centralized sense-reversing barrier.                                */
typedef struct {
    alignas(PAD) _Atomic uint32_t count;
    alignas(PAD) _Atomic uint32_t sense;
    alignas(PAD) _Atomic uint32_t sleepers;   /* central_sp only */
    uint32_t P;
} central_t;

static void *central_create(int P) {
    central_t *b = xalloc(sizeof(*b));
    b->P = (uint32_t)P;
    atomic_store(&b->count, (uint32_t)P);
    atomic_store(&b->sense, 0);
    return b;
}

static void central_wait(void *bp, void *ts) {
    central_t *b = bp;
    sense_state_t *s = ts;
    s->local_sense ^= 1;
    if (atomic_fetch_sub_explicit(&b->count, 1, memory_order_acq_rel) == 1) {
        atomic_store_explicit(&b->count, b->P, memory_order_relaxed);
        atomic_store_explicit(&b->sense, s->local_sense, memory_order_release);
    } else {
        while (atomic_load_explicit(&b->sense, memory_order_acquire) != s->local_sense)
            cpu_relax();
    }
}

/*
 * Our extension, applied to the barrier side: spin for a bounded time, then
 * sleep on the sense word. The last arriver only issues FUTEX_WAKE when some
 * thread has registered as a sleeper. seq_cst on the sense store, the
 * sleepers increment and both loads makes the "lost wake-up" interleaving
 * impossible (store sense < load sleepers, or inc sleepers < load sense).
 */
static void central_sp_wait(void *bp, void *ts) {
    central_t *b = bp;
    sense_state_t *s = ts;
    uint32_t my = (s->local_sense ^= 1);
    if (atomic_fetch_sub_explicit(&b->count, 1, memory_order_acq_rel) == 1) {
        atomic_store_explicit(&b->count, b->P, memory_order_relaxed);
        atomic_store_explicit(&b->sense, my, memory_order_seq_cst);
        if (atomic_load_explicit(&b->sleepers, memory_order_seq_cst) > 0)
            futex_wake(&b->sense, INT_MAX);
        return;
    }
    for (uint32_t i = 0; i < g_barrier_spin_limit; i++) {
        if (atomic_load_explicit(&b->sense, memory_order_acquire) == my) return;
        cpu_relax();
    }
    atomic_fetch_add_explicit(&b->sleepers, 1, memory_order_seq_cst);
    while (atomic_load_explicit(&b->sense, memory_order_seq_cst) != my)
        futex_wait(&b->sense, my ^ 1);
    atomic_fetch_sub_explicit(&b->sleepers, 1, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* Dissemination barrier.                                              */
#define MAX_ROUNDS 16  /* supports up to 65536 threads */

typedef struct { alignas(PAD) _Atomic uint32_t v; } pflag_t;

typedef struct {
    pflag_t flags[2][MAX_ROUNDS];  /* my flags, written by my partners */
} dissem_node_t;

typedef struct {
    int P, rounds;
    dissem_node_t *nodes;
} dissem_t;

typedef struct {
    alignas(PAD) uint32_t parity, sense;
    dissem_node_t *me;
    _Atomic uint32_t *partner[2][MAX_ROUNDS];
    int rounds;
} dissem_local_t;

static void *dissem_create(int P) {
    dissem_t *b = xalloc(sizeof(*b));
    b->P = P;
    b->rounds = 0;
    while ((1 << b->rounds) < P) b->rounds++;
    b->nodes = xalloc(sizeof(dissem_node_t) * (size_t)P);
    return b;
}

static void dissem_destroy(void *bp) {
    dissem_t *b = bp;
    free(b->nodes);
    free(b);
}

static void *dissem_thread_init(void *bp, int id) {
    dissem_t *b = bp;
    dissem_local_t *l = xalloc(sizeof(*l));
    l->parity = 0;
    l->sense = 1;
    l->rounds = b->rounds;
    l->me = &b->nodes[id];
    for (int k = 0; k < b->rounds; k++) {
        int partner = (id + (1 << k)) % b->P;
        for (int p = 0; p < 2; p++)
            l->partner[p][k] = &b->nodes[partner].flags[p][k].v;
    }
    return l;
}

static void dissem_wait(void *bp, void *ts) {
    (void)bp;
    dissem_local_t *l = ts;
    for (int k = 0; k < l->rounds; k++) {
        atomic_store_explicit(l->partner[l->parity][k], l->sense, memory_order_release);
        while (atomic_load_explicit(&l->me->flags[l->parity][k].v, memory_order_acquire) != l->sense)
            cpu_relax();
    }
    if (l->parity == 1) l->sense ^= 1;
    l->parity ^= 1;
}

/* ------------------------------------------------------------------ */
/* MCS tree barrier (the paper's new barrier).                         */
/*
 * Deviation: the paper packs the four childnotready flags as four bytes of
 * one word and children do plain byte stores. Mixed-size atomics are not
 * portable C11, so we keep them as 4 bits of one word and children clear
 * their bit with atomic fetch_and. The parent still spins on ONE local word.
 */
static _Atomic uint32_t tree_dummy;

typedef struct {
    alignas(PAD) _Atomic uint32_t childnotready;  /* bit j: child j not arrived */
    uint32_t havechild;                            /* bitmask of existing children */
    alignas(PAD) _Atomic uint32_t parentsense;     /* written by wake-up parent */
    _Atomic uint32_t *parent_word;                 /* parent's childnotready */
    uint32_t parent_bit;
    _Atomic uint32_t *child_sense[2];              /* wake-up tree children */
    uint32_t sense;
} tree_node_t;

typedef struct { int P; tree_node_t *nodes; } tree_t;

static void *tree_create(int P) {
    tree_t *b = xalloc(sizeof(*b));
    b->P = P;
    b->nodes = xalloc(sizeof(tree_node_t) * (size_t)P);
    for (int i = 0; i < P; i++) {
        tree_node_t *n = &b->nodes[i];
        n->havechild = 0;
        for (int j = 0; j < 4; j++)
            if (4 * i + j + 1 < P) n->havechild |= 1u << j;
        atomic_store(&n->childnotready, n->havechild);
        atomic_store(&n->parentsense, 0);
        if (i == 0) {
            n->parent_word = &tree_dummy;
            n->parent_bit = 0;
        } else {
            n->parent_word = &b->nodes[(i - 1) / 4].childnotready;
            n->parent_bit = 1u << ((i - 1) % 4);
        }
        n->child_sense[0] = (2 * i + 1 < P) ? &b->nodes[2 * i + 1].parentsense : &tree_dummy;
        n->child_sense[1] = (2 * i + 2 < P) ? &b->nodes[2 * i + 2].parentsense : &tree_dummy;
        n->sense = 1;
    }
    return b;
}

static void tree_destroy(void *bp) {
    tree_t *b = bp;
    free(b->nodes);
    free(b);
}

static void *tree_thread_init(void *bp, int id) { return &((tree_t *)bp)->nodes[id]; }

static void tree_wait(void *bp, void *ts) {
    (void)bp;
    tree_node_t *n = ts;
    /* Arrival: wait for my (up to 4) children, then tell my parent. */
    while (atomic_load_explicit(&n->childnotready, memory_order_acquire) != 0)
        cpu_relax();
    atomic_store_explicit(&n->childnotready, n->havechild, memory_order_relaxed);
    atomic_fetch_and_explicit(n->parent_word, ~n->parent_bit, memory_order_release);
    /* Wake-up: non-root waits for its wake-up parent, then wakes 2 children. */
    if (n->parent_word != &tree_dummy) {
        while (atomic_load_explicit(&n->parentsense, memory_order_acquire) != n->sense)
            cpu_relax();
    }
    atomic_store_explicit(n->child_sense[0], n->sense, memory_order_release);
    atomic_store_explicit(n->child_sense[1], n->sense, memory_order_release);
    n->sense ^= 1;
}

/* ------------------------------------------------------------------ */
/* Baseline: pthread_barrier_t.                                         */
typedef struct { alignas(PAD) pthread_barrier_t b; } pbar_t;

static void *pbar_create(int P) {
    pbar_t *b = xalloc(sizeof(*b));
    pthread_barrier_init(&b->b, NULL, (unsigned)P);
    return b;
}
static void pbar_destroy(void *bp) {
    pthread_barrier_destroy(&((pbar_t *)bp)->b);
    free(bp);
}
static void *pbar_thread_init(void *b, int id) { (void)id; return b; }
static void pbar_wait(void *bp, void *ts) { (void)ts; pthread_barrier_wait(&((pbar_t *)bp)->b); }

static void generic_destroy(void *p) { free(p); }

/* ------------------------------------------------------------------ */
static const barrier_ops_t BARRIERS[] = {
    {"central", "centralized sense-reversing barrier (paper)", central_create, generic_destroy, sense_thread_init, central_wait},
    {"dissem", "dissemination barrier (paper)", dissem_create, dissem_destroy, dissem_thread_init, dissem_wait},
    {"mcs_tree", "MCS 4-ary arrival / binary wake-up tree barrier (paper)", tree_create, tree_destroy, tree_thread_init, tree_wait},
    {"central_sp", "central barrier with spin-then-park (our extension)", central_create, generic_destroy, sense_thread_init, central_sp_wait},
    {"pthread", "glibc pthread_barrier_t (baseline)", pbar_create, pbar_destroy, pbar_thread_init, pbar_wait},
};

const barrier_ops_t *barrier_all(int *n) {
    *n = (int)(sizeof(BARRIERS) / sizeof(BARRIERS[0]));
    return BARRIERS;
}

const barrier_ops_t *barrier_find(const char *name) {
    int n;
    const barrier_ops_t *all = barrier_all(&n);
    for (int i = 0; i < n; i++)
        if (strcmp(all[i].name, name) == 0) return &all[i];
    return NULL;
}
