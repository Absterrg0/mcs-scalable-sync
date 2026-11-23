/*
 * lockbench - throughput / fairness / CPU-cost benchmark for spin locks.
 *
 * Each of T threads repeatedly: acquire; critical section; release; think.
 * The critical section increments a counter protected only by the lock
 * (plain, non-atomic) plus `cs_work` read-modify-writes of shared cache
 * lines. At the end we check counter == sum of per-thread acquisitions, so
 * every benchmark run is also a mutual-exclusion test.
 *
 * Output: one CSV row per (lock, threads, rep).
 */
#include "locks.h"

#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define MAX_THREADS 1024
#define CS_LINES 8

typedef struct { alignas(PAD) volatile uint64_t x; } shared_line_t;

static struct {
    alignas(PAD) uint64_t counter;            /* protected by the lock */
    shared_line_t lines[CS_LINES];            /* protected by the lock */
} g_shared;

static alignas(PAD) _Atomic int g_stop;
static pthread_barrier_t g_start;

static const lock_ops_t *g_ops;
static void *g_lock;
static int g_cs_work, g_think, g_pin;

typedef struct {
    alignas(PAD) uint64_t ops;
    int id;
    int cpu;
    thread_ctx_t *ctx;
} worker_t;

static void pin_self(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static void *worker(void *arg) {
    worker_t *w = arg;
    if (g_pin) pin_self(w->cpu);
    /* First-touch the per-thread context on this thread. */
    w->ctx = aligned_alloc(PAD, sizeof(thread_ctx_t));
    memset(w->ctx, 0, sizeof(thread_ctx_t));
    w->ctx->spin_limit = g_mcssp_spin_limit;
    uint64_t rng = 0x9E3779B97F4A7C15ull * (uint64_t)(w->id + 1);
    uint64_t ops = 0;
    pthread_barrier_wait(&g_start);
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        g_ops->acquire(g_lock, w->ctx);
        g_shared.counter++;
        for (int i = 0; i < g_cs_work; i++) g_shared.lines[i % CS_LINES].x++;
        g_ops->release(g_lock, w->ctx);
        ops++;
        for (int i = 0; i < g_think; i++) {            /* private "work" */
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            __asm__ __volatile__("" : "+r"(rng));
        }
    }
    w->ops = ops;
    return NULL;
}

static double cpu_seconds(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 +
           (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
}

static int run_one(const lock_ops_t *ops, int T, double duration, int rep, int ncpu, const int *cpus) {
    g_ops = ops;
    g_lock = ops->create(T);
    g_shared.counter = 0;
    atomic_store(&g_stop, 0);
    pthread_barrier_init(&g_start, NULL, (unsigned)T + 1);

    worker_t *ws = aligned_alloc(PAD, sizeof(worker_t) * (size_t)T);
    pthread_t *th = malloc(sizeof(pthread_t) * (size_t)T);
    for (int i = 0; i < T; i++) {
        memset(&ws[i], 0, sizeof(ws[i]));
        ws[i].id = i;
        ws[i].cpu = cpus[i % ncpu];
        pthread_create(&th[i], NULL, worker, &ws[i]);
    }
    pthread_barrier_wait(&g_start);
    double cpu0 = cpu_seconds();
    double t0 = now_sec();
    struct timespec ts = {(time_t)duration, (long)((duration - (time_t)duration) * 1e9)};
    nanosleep(&ts, NULL);
    atomic_store(&g_stop, 1);
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    double elapsed = now_sec() - t0;
    double cpu = cpu_seconds() - cpu0;

    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    double sum = 0, sumsq = 0;
    for (int i = 0; i < T; i++) {
        uint64_t o = ws[i].ops;
        total += o;
        if (o < mn) mn = o;
        if (o > mx) mx = o;
        sum += (double)o;
        sumsq += (double)o * (double)o;
        free(ws[i].ctx);
    }
    /* Jain's fairness index: 1 = perfectly fair, 1/T = one thread got all. */
    double jain = sumsq > 0 ? (sum * sum) / ((double)T * sumsq) : 0;
    int ok = g_shared.counter == total;

    printf("%s,%d,%d,%d,%d,%.4f,%llu,%.4f,%.4f,%llu,%llu,%.4f,%.2f,%s\n", ops->name, T, rep, g_cs_work, g_think,
           elapsed, (unsigned long long)total, total / elapsed / 1e6, jain, (unsigned long long)mn,
           (unsigned long long)mx, cpu, total ? cpu / (total / 1e6) : 0.0, ok ? "ok" : "FAIL");
    fflush(stdout);

    pthread_barrier_destroy(&g_start);
    ops->destroy(g_lock);
    free(ws);
    free(th);
    if (!ok) fprintf(stderr, "MUTUAL EXCLUSION VIOLATED: lock=%s counter=%llu total=%llu\n", ops->name,
                     (unsigned long long)g_shared.counter, (unsigned long long)total);
    return ok;
}

static void usage(void) {
    int n;
    const lock_ops_t *all = lock_all(&n);
    fprintf(stderr,
            "usage: lockbench [options]\n"
            "  -l, --locks a,b,c     locks to run (default: all)\n"
            "  -t, --threads 1,2,4   thread counts (default: 1,2,4,8)\n"
            "  -d, --duration SEC    seconds per run (default 0.5)\n"
            "  -r, --reps N          repetitions (default 3)\n"
            "  -c, --cs-work N       shared-line RMWs inside the critical section (default 0)\n"
            "  -k, --think N         private work iterations outside (default 0)\n"
            "  -s, --spin-limit N    MCS-SP spins before parking (default %u)\n"
            "  -w, --wake-depth N    MCS-SPW: queue depth to pre-wake (default 2)\n"
            "  -p, --pin             pin threads round-robin to CPUs\n"
            "  -H, --no-header       do not print the CSV header\n"
            "locks:\n",
            g_mcssp_spin_limit);
    for (int i = 0; i < n; i++) fprintf(stderr, "  %-12s %s\n", all[i].name, all[i].desc);
}

int main(int argc, char **argv) {
    char locks_arg[1024] = "", threads_arg[1024] = "1,2,4,8";
    double duration = 0.5;
    int reps = 3, header = 1;
    static struct option longopts[] = {
        {"locks", required_argument, 0, 'l'},    {"threads", required_argument, 0, 't'},
        {"duration", required_argument, 0, 'd'}, {"reps", required_argument, 0, 'r'},
        {"cs-work", required_argument, 0, 'c'},  {"think", required_argument, 0, 'k'},
        {"spin-limit", required_argument, 0, 's'}, {"pin", no_argument, 0, 'p'},
        {"wake-depth", required_argument, 0, 'w'},
        {"no-header", no_argument, 0, 'H'},      {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "l:t:d:r:c:k:s:w:pHh", longopts, NULL)) != -1) {
        switch (c) {
        case 'l': snprintf(locks_arg, sizeof locks_arg, "%s", optarg); break;
        case 't': snprintf(threads_arg, sizeof threads_arg, "%s", optarg); break;
        case 'd': duration = atof(optarg); break;
        case 'r': reps = atoi(optarg); break;
        case 'c': g_cs_work = atoi(optarg); break;
        case 'k': g_think = atoi(optarg); break;
        case 's': g_mcssp_spin_limit = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'w': g_mcsspw_depth = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'p': g_pin = 1; break;
        case 'H': header = 0; break;
        default: usage(); return c == 'h' ? 0 : 2;
        }
    }

    cpu_set_t avail;
    sched_getaffinity(0, sizeof(avail), &avail);
    int cpus[CPU_SETSIZE], ncpu = 0;
    for (int i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &avail)) cpus[ncpu++] = i;

    int nlocks;
    const lock_ops_t *all = lock_all(&nlocks);
    const lock_ops_t *sel[64];
    int nsel = 0;
    if (!locks_arg[0] || strcmp(locks_arg, "all") == 0) {
        for (int i = 0; i < nlocks; i++) sel[nsel++] = &all[i];
    } else {
        for (char *tok = strtok(locks_arg, ","); tok; tok = strtok(NULL, ",")) {
            const lock_ops_t *o = lock_find(tok);
            if (!o) { fprintf(stderr, "unknown lock: %s\n", tok); usage(); return 2; }
            sel[nsel++] = o;
        }
    }
    int threads[256], nthreads = 0;
    for (char *tok = strtok(threads_arg, ","); tok && nthreads < 256; tok = strtok(NULL, ",")) {
        int t = atoi(tok);
        if (t < 1 || t > MAX_THREADS) { fprintf(stderr, "bad thread count %s\n", tok); return 2; }
        threads[nthreads++] = t;
    }

    if (header)
        printf("lock,threads,rep,cs_work,think,elapsed_s,total_ops,mops,jain,min_ops,max_ops,cpu_s,cpu_s_per_mop,check\n");
    int all_ok = 1;
    for (int r = 0; r < reps; r++)            /* reps outermost: spreads drift */
        for (int ti = 0; ti < nthreads; ti++)
            for (int li = 0; li < nsel; li++)
                all_ok &= run_one(sel[li], threads[ti], duration, r, ncpu, cpus);
    return all_ok ? 0 : 1;
}
