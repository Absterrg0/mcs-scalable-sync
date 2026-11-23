/*
 * barrierbench - time per barrier episode, with a correctness check.
 *
 * Each thread runs E episodes. Before episode e it publishes "I reached e";
 * after the barrier it checks that every other thread has reached e (and is
 * at most at e+1). Any early release is reported as FAIL.
 */
#include "barriers.h"

#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define MAX_THREADS 1024

typedef struct { alignas(PAD) _Atomic uint64_t v; } progress_t;

static progress_t *g_progress;
static const barrier_ops_t *g_ops;
static void *g_bar;
static int g_T, g_check = 1;
static long g_episodes;
static pthread_barrier_t g_start;
static _Atomic int g_fail;

typedef struct { int id; } warg_t;

static void *worker(void *arg) {
    int id = ((warg_t *)arg)->id;
    void *ts = g_ops->thread_init(g_bar, id);
    pthread_barrier_wait(&g_start);
    for (long e = 1; e <= g_episodes; e++) {
        if (g_check) atomic_store_explicit(&g_progress[id].v, (uint64_t)e, memory_order_relaxed);
        g_ops->wait(g_bar, ts);
        if (g_check) {
            int j = (int)(e % g_T);  /* check one peer per episode (cheap) */
            uint64_t p = atomic_load_explicit(&g_progress[j].v, memory_order_relaxed);
            if (p < (uint64_t)e || p > (uint64_t)e + 1) atomic_store(&g_fail, 1);
        }
    }
    return NULL;
}

static double cpu_seconds(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 +
           (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
}

int main(int argc, char **argv) {
    char bars_arg[512] = "", threads_arg[512] = "2,4,8";
    int reps = 3, header = 1;
    g_episodes = 100000;
    int c;
    while ((c = getopt(argc, argv, "b:t:e:r:s:nH")) != -1) {
        switch (c) {
        case 'b': snprintf(bars_arg, sizeof bars_arg, "%s", optarg); break;
        case 't': snprintf(threads_arg, sizeof threads_arg, "%s", optarg); break;
        case 'e': g_episodes = atol(optarg); break;
        case 'r': reps = atoi(optarg); break;
        case 's': g_barrier_spin_limit = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'n': g_check = 0; break;
        case 'H': header = 0; break;
        default:
            fprintf(stderr, "usage: barrierbench [-b central,dissem,mcs_tree,central_sp,pthread] [-t 2,4,8] "
                            "[-e episodes] [-r reps] [-s spin_limit] [-n no-check] [-H]\n");
            return 2;
        }
    }
    int nb;
    const barrier_ops_t *all = barrier_all(&nb);
    const barrier_ops_t *sel[32];
    int nsel = 0;
    if (!bars_arg[0]) {
        for (int i = 0; i < nb; i++) sel[nsel++] = &all[i];
    } else {
        for (char *tok = strtok(bars_arg, ","); tok; tok = strtok(NULL, ",")) {
            const barrier_ops_t *o = barrier_find(tok);
            if (!o) { fprintf(stderr, "unknown barrier %s\n", tok); return 2; }
            sel[nsel++] = o;
        }
    }
    int threads[128], nt = 0;
    for (char *tok = strtok(threads_arg, ","); tok && nt < 128; tok = strtok(NULL, ",")) threads[nt++] = atoi(tok);

    if (header) printf("barrier,threads,rep,episodes,elapsed_s,us_per_episode,cpu_s,check\n");
    int all_ok = 1;
    for (int r = 0; r < reps; r++)
        for (int ti = 0; ti < nt; ti++)
            for (int bi = 0; bi < nsel; bi++) {
                g_T = threads[ti];
                if (g_T < 1 || g_T > MAX_THREADS) return 2;
                g_ops = sel[bi];
                g_bar = g_ops->create(g_T);
                g_progress = aligned_alloc(PAD, sizeof(progress_t) * (size_t)g_T);
                memset(g_progress, 0, sizeof(progress_t) * (size_t)g_T);
                atomic_store(&g_fail, 0);
                pthread_barrier_init(&g_start, NULL, (unsigned)g_T + 1);
                pthread_t th[MAX_THREADS];
                warg_t args[MAX_THREADS];
                for (int i = 0; i < g_T; i++) {
                    args[i].id = i;
                    pthread_create(&th[i], NULL, worker, &args[i]);
                }
                pthread_barrier_wait(&g_start);
                double cpu0 = cpu_seconds(), t0 = now_sec();
                for (int i = 0; i < g_T; i++) pthread_join(th[i], NULL);
                double el = now_sec() - t0, cpu = cpu_seconds() - cpu0;
                int ok = !atomic_load(&g_fail);
                all_ok &= ok;
                printf("%s,%d,%d,%ld,%.4f,%.4f,%.4f,%s\n", g_ops->name, g_T, r, g_episodes, el,
                       el / (double)g_episodes * 1e6, cpu, ok ? "ok" : "FAIL");
                fflush(stdout);
                pthread_barrier_destroy(&g_start);
                g_ops->destroy(g_bar);
                free(g_progress);
            }
    return all_ok ? 0 : 1;
}
