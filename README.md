# Scalable Synchronization on a Modern Multicore: Reproducing and Extending Mellor-Crummey & Scott (1991)

Implementation, replication and extension of

> J. M. Mellor-Crummey and M. L. Scott, **"Algorithms for Scalable Synchronization on
> Shared-Memory Multiprocessors,"** *ACM Transactions on Computer Systems* 9(1):21–65, 1991.
> (Dijkstra Prize 2006; the MCS lock is the basis of the Linux kernel's `qspinlock`.)

The full write-up is in [`paper/report.pdf`](paper/report.pdf).

## What is in here

| Path | What |
|---|---|
| `src/locks.c` | Spin locks from the paper (Alg. 1, 2, 3, 5, 6), baselines, an ablation, and **MCS-SP** (our extension) |
| `src/barriers.c` | Barriers from the paper (Alg. 7, 9, 11), a pthread baseline, and our spin-then-park central barrier |
| `src/lockbench.c` | Throughput / fairness / CPU-cost benchmark; every run also checks mutual exclusion |
| `src/barrierbench.c` | Time per barrier episode; every run also checks that no thread leaves early |
| `scripts/run_experiments.sh` | Runs experiments E1–E7 from the report (~9 min) |
| `scripts/plot.py` | Makes all figures and tables in `figures/` |
| `results/` | Raw CSVs from the run used in the report, plus `env.txt` (machine details) |
| `paper/` | LaTeX source and the compiled report |

| Lock | Source |
|---|---|
| `tas_backoff` | test_and_set + exponential backoff (paper Alg. 1) |
| `ticket` | ticket lock + proportional backoff (paper Alg. 2) |
| `anderson` | Anderson's array-based queue lock (paper Alg. 3) |
| `mcs` | MCS list-based queue lock (paper Alg. 5) |
| `mcs_swap` | MCS release with fetch_and_store only (paper Alg. 6) |
| `tas`, `ttas` | naive baselines (no backoff) |
| `anderson_np` | Anderson lock without cache-line padding (false-sharing ablation) |
| `pthread` | glibc `pthread_mutex_t` |
| **`mcs_sp`** | **MCS-SP: MCS with bounded spinning, then futex parking (ours, step 1)** |
| **`mcs_spw`** | **MCS-SPW: MCS-SP + wake-ahead of the next queued waiters (ours, step 2)** |

## Build and run

Needs Linux, a C11 compiler and pthreads. The plots need Python with matplotlib and pandas
(`uv` installs them on the fly).

```sh
make                 # builds ./lockbench and ./barrierbench
make test            # stress tests: mutual exclusion + barrier safety, 1..32 threads
make test-tsan       # optional: ThreadSanitizer build (needs libtsan installed)

./lockbench --help
./lockbench -l mcs,mcs_sp,pthread -t 4,12,24 -c 4 -k 200 -d 1 -r 3
./barrierbench -t 2,4,8 -e 50000

scripts/run_experiments.sh results          # all experiments
uv run --with matplotlib --with pandas scripts/plot.py results figures
cd paper && tectonic report.tex             # rebuild the PDF
```

`lockbench` output columns: `lock, threads, rep, cs_work, think, elapsed_s, total_ops, mops`
(million acquisitions/s), `jain` (Jain fairness index, 1 = fair), `min_ops, max_ops` (per thread),
`cpu_s` (CPU-seconds used by the process), `cpu_s_per_mop`, `check` (`ok` = mutual exclusion held).

## The extension in one paragraph

The paper's FIFO locks (ticket, Anderson, MCS) always hand the lock to the *next* waiter. If that
waiter has been preempted, which happens whenever there are more runnable threads than cores, the lock
stays idle for a scheduler time slice while every other waiter burns CPU spinning. The paper names this
weakness itself ("likely to waste CPU resources if spinning processes may be preempted") and suggests
falling back to a test_and_set lock. **MCS-SP** keeps MCS's FIFO queue and local spinning, but a
waiter spins only for a bounded number of iterations and then parks in `futex_wait`. The releaser hands
over with one atomic exchange and makes the `futex_wake` system call only if the successor actually
went to sleep. When threads ≤ cores, it behaves like MCS. When they are not, it avoids MCS's collapse.
Once waiters are asleep, though, every FIFO hand-off pays a full wake-up latency. **MCS-SPW** (step 2)
hides that latency: each new lock holder pre-wakes the next one or two parked waiters in the queue, so
their wake-up overlaps the critical sections ahead of them. The queue stays strictly FIFO.
The same spin-then-park idea applied to the paper's centralized barrier gives `central_sp`.
