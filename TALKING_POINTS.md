# Prep sheet for the professor meeting

## 30-second pitch

> "I picked Mellor-Crummey & Scott's 1991 paper on scalable synchronization, the one that introduced
> the **MCS lock**. It won the Dijkstra Prize, and the Linux kernel's spinlock is still built on it. I implemented
> 5 of its spin locks and 3 of its barriers in C11, re-ran its experiments on my 12-thread hybrid Intel
> laptop, and checked which conclusions survive 35 years. They mostly do, with two surprises. Then I fixed the one
> weakness the paper itself admits: FIFO locks fall apart when threads get preempted. My version, **MCS-SP**
> (spin-then-park), is 9–29× faster than MCS and uses 25–70× less CPU when there are more threads than
> cores. It costs nothing when there aren't."

## The paper in four sentences

1. Busy-waiting is slow when every waiter spins on the **same** memory word, because every release
   invalidates everyone's cache copy, so each hand-off costs O(P) coherence traffic.
2. Fix: **each waiter spins on its own local flag**, and the releaser ends exactly one spin with one remote write.
3. MCS lock: a queue of per-thread nodes. Acquire = `swap` yourself into `tail`, link behind your
   predecessor, spin on **your own** node. Release = clear your successor's flag (or `CAS` tail back to
   null). O(1) space per lock, O(1) remote refs per acquire, FIFO.
4. Same idea for barriers: a 4-ary arrival tree with a binary wake-up tree; everyone spins locally.

## What I built (all in `src/`)

- Paper algorithms: Alg. 1 TAS+exp. backoff, Alg. 2 ticket+prop. backoff, Alg. 3 Anderson array lock,
  Alg. 5 MCS, Alg. 6 MCS swap-only release; barriers Alg. 7 central, Alg. 9 dissemination, Alg. 11 MCS tree.
- Baselines: naive TAS, TTAS, glibc `pthread_mutex` / `pthread_barrier`, unpadded Anderson (ablation).
- **Extension:** `mcs_sp` (spin-then-park), `mcs_spw` (+ wake-ahead), `central_sp` (barrier version).
- Every benchmark run doubles as a correctness test: a non-atomic counter must equal the total acquisitions.

## Key numbers to remember

| Claim | Number |
|---|---|
| Uncontended latency: TAS 10.5 ns, MCS 17.5 ns | MCS = 1.68× TAS (paper: 1.31× on Symmetry) |
| ~670× faster than 1991 | 7.0 µs → 10.5 ns per acquire/release |
| 8 threads, small CS: scalable locks comparable | MCS 5.9, ticket 5.3, Anderson 5.1, TAS-backoff 4.8 M/s |
| pthread_mutex at 8 threads | 2.8 M/s, about half of MCS |
| Unfair locks look fast (empty CS, 8 thr.) | TAS-backoff 20 ns/acq vs MCS 147 ns, but fairness 0.87 (TTAS: 0.24) |
| Cache line transfer between cores | ~45 ns (28 ns between SMT siblings) |
| Futex wake-up latency | ~2.5–2.8 µs |
| **MCS collapse at 24–48 threads** | 0.012 / 0.004 M/s (500–1500× slower), slowest thread 1–18 acquisitions/s |
| **MCS-SP at 24–48 threads** | 0.10–0.11 M/s (**9–29×** MCS), fairness 0.995, **25–70× less CPU** |
| MCS-SP cost when not oversubscribed | within 7% of MCS |
| MCS-SPW at the 12-thread cliff | 1.38 M/s vs 0.18 (MCS-SP) / 0.89 (MCS); depth ≥3: 2.1–2.9 M/s |
| Barriers ≤8 threads | dissemination fastest (0.11–0.43 µs); pthread 10–25× slower |
| Barriers oversubscribed | spin barriers 5–23 ms/episode; central_sp 19–66× faster |

## The improvement, explained simply

- **Problem:** MCS hands the lock to a *specific* next thread. If the OS has paused that thread, nobody
  can use the lock until the scheduler runs it again, which takes milliseconds. Meanwhile everyone else in the queue spins
  and wastes their CPU time. The paper says so itself (§4.3): FIFO locks are "likely to waste CPU
  resources if spinning processes may be preempted". Its advice is to switch to an unfair TAS lock.
- **My fix (MCS-SP):** keep the MCS queue, but a waiter only spins for a while (1024 PAUSEs ≈ 25–50 µs)
  and then **sleeps in the kernel (futex)**. The releaser uses an atomic `exchange` instead of a plain store. Only if it
  sees "successor is asleep" (value 2) does it pay for a wake-up syscall. The fast path is unchanged.
- **Why there's no lost wake-up:** the waiter's CAS 1→2 and the releaser's exchange are ordered on the same
  word. Either the releaser sees 2 and wakes, or the waiter sees 0 and never sleeps. `futex_wait(addr, 2)`
  refuses to sleep if the word already changed.
- **Step 2 (MCS-SPW, wake-ahead):** the new lock holder pre-wakes the next 2 parked waiters, so their wake-up
  overlaps the critical sections in front of them. It is still strictly FIFO.

## Be upfront about these (they make the conversation better, not worse)

1. **glibc's pthread_mutex beats MCS-SP by ~23× under heavy oversubscription.** It's *not FIFO*: running
   threads barge past sleeping ones. A strict FIFO lock must wait for the designated (sleeping) successor,
   so it can't beat ~1 hand-off per wake-up latency (≤ ~0.4 M/s). That gap is the *price of fairness*, and
   I measured it.
2. **Wake-ahead hurts at 24–48 threads** (extra syscalls inside the critical section, and woken threads
   compete for scarce CPUs). It's a tool for near-saturation only, so adaptive depth is future work.
3. **Spin limit has no universal best value.** At 8 threads, ≤256 PAUSEs is a 25× cliff (convoy); at 24
   threads, shorter is better. My convoy model explains it: if spin time < (queue length × wake latency),
   parked waiters make everyone behind them park too.
4. **Noisy machine:** a laptop with a normal desktop running, not pinned (on purpose). 12 threads is bimodal. I report
   medians of 3 runs with min–max bars and don't claim differences under ~15%.
5. **Not done:** ThreadSanitizer (libtsan not installed), Arm testing, Alg. 4/8/10, NUMA, real apps.
6. **Prior work exists:** spin-then-block is an old idea (Karlin et al. SOSP'91), and preemption-adaptive
   queue locks exist (He, Scherer & Scott 2005, time-published locks; Dice's Malthusian locks 2017; shuffle
   locks SOSP'19). My contribution is the *minimal* change to the paper's own Alg. 5, plus measurements
   showing where it does and doesn't help.

## Questions he might ask, and short answers

- **Why does MCS need `acq_rel` on the swap?** Acquire, because if the lock was free we synchronize with the last
  releaser's CAS. Release, so that our node's initialization is visible to our successor.
- **Why must `pred->next = me` be a release store?** So that our `locked = 1` is ordered before the
  releaser's `locked = 0`. Otherwise we could overwrite the grant and spin forever.
- **Why pad to 128 bytes, not 64?** Intel's adjacent-line prefetcher pulls 64 B lines in pairs.
- **Why is ticket the cheapest uncontended lock?** One `lock xadd` and a load. MCS needs `xchg` + `cmpxchg`.
- **Why does dissemination win now when it lost on the Symmetry?** The Symmetry had one shared bus, so
  O(P log P) messages serialized. Today's on-chip interconnect moves lines between different core pairs
  in parallel, which is the Butterfly-like property the paper itself said favors dissemination (§5).
- **Why not just use pthread_mutex?** If you don't need FIFO, under oversubscription you should! With threads ≤
  cores, MCS/MCS-SP is ~2× faster than pthread_mutex and strictly fair. Fair locks matter for tail latency
  and avoiding starvation.
- **What would you do next?** Adaptive waiting: measure wake latency online, spin for about that long in *time*
  (not iterations, since a PAUSE costs 50 ns on P-cores and 25 ns on E-cores), and adapt wake-ahead depth to how often
  waiters park. Then test on a cloud VM where the hypervisor preempts vCPUs.
- **How do you know it's correct?** Every run checks mutual exclusion with a non-atomic counter. The barrier
  test checks nobody leaves early. I also have the written race argument. TSan/Arm are the honest gaps.

## Live demo (≈1 minute)

```sh
make
./lockbench -l mcs,mcs_sp,pthread -t 6,24 -c 4 -k 200 -d 1 -r 1 | column -t -s,
```

Watch the `mops` column: MCS collapses at 24 threads, MCS-SP doesn't, and every row says `check=ok`.
