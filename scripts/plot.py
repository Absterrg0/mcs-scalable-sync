"""Make every figure and table used in the report from results/*.csv.

Run:  uv run --with matplotlib --with pandas scripts/plot.py [results_dir] [figures_dir]
"""
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

RES = Path(sys.argv[1] if len(sys.argv) > 1 else "results")
FIG = Path(sys.argv[2] if len(sys.argv) > 2 else "figures")
FIG.mkdir(exist_ok=True)

# One fixed colour + marker per entity across all figures (identity never by colour alone).
STYLE = {
    "mcs":         ("#2a78d6", "o", "-",  "MCS (Alg. 5)"),
    "mcs_sp":      ("#eb6834", "*", "-",  "MCS-SP (ours)"),
    "ticket":      ("#1baf7a", "s", "-",  "Ticket + prop. backoff (Alg. 2)"),
    "tas_backoff": ("#eda100", "^", "-",  "TAS + exp. backoff (Alg. 1)"),
    "anderson":    ("#e87ba4", "D", "-",  "Anderson array (Alg. 3)"),
    "pthread":     ("#008300", "P", "--", "pthread_mutex (glibc)"),
    "mcs_swap":    ("#6250d6", "v", "-",  "MCS swap-only (Alg. 6)"),
    "mcs_spw":     ("#e34948", "h", "-",  "MCS-SPW (ours, +wake-ahead)"),
    "ttas":        ("#52514e", "x", "--", "TTAS (no backoff)"),
    "tas":         ("#9a9994", "x", ":",  "TAS (no backoff)"),
    "anderson_np": ("#b3b2ad", "d", ":",  "Anderson, unpadded"),
}
BSTYLE = {
    "central":    ("#2a78d6", "o", "-",  "Central sense-reversing (Alg. 7)"),
    "dissem":     ("#1baf7a", "s", "-",  "Dissemination (Alg. 9)"),
    "mcs_tree":   ("#eda100", "^", "-",  "MCS tree (Alg. 11)"),
    "central_sp": ("#eb6834", "*", "-",  "Central spin-then-park (ours)"),
    "pthread":    ("#008300", "P", "--", "pthread_barrier (glibc)"),
}

plt.rcParams.update({
    "font.family": "serif", "font.size": 8.5, "axes.titlesize": 9, "axes.labelsize": 8.5,
    "legend.fontsize": 7, "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": "#e4e3df", "grid.linewidth": 0.6, "axes.edgecolor": "#8a8984",
    "xtick.color": "#52514e", "ytick.color": "#52514e", "axes.labelcolor": "#0b0b0b",
    "lines.linewidth": 1.6, "lines.markersize": 5, "savefig.dpi": 220, "savefig.bbox": "tight",
})


def load(name):
    p = RES / name
    return pd.read_csv(p) if p.exists() else None


def agg(df, key, val):
    g = df.groupby([key, "threads"])[val]
    return g.median().unstack(0), g.min().unstack(0), g.max().unstack(0)


def line_panel(ax, df, keys, val, style, ylabel, logy=False, transform=None):
    d = df.copy()
    if transform:
        d[val] = transform(d)
    med, lo, hi = agg(d, d.columns[0], val)
    for k in keys:
        if k not in med:
            continue
        c, m, ls, lab = style[k]
        x = med.index.values
        ax.errorbar(x, med[k], yerr=[med[k] - lo[k], hi[k] - med[k]], color=c, marker=m, ls=ls,
                    label=lab, capsize=2, elinewidth=0.8, markeredgecolor="white" if m not in "x+" else c,
                    markeredgewidth=0.6, ms=7 if m == "*" else 5)
    ax.set_xlabel("threads")
    ax.set_ylabel(ylabel)
    if logy:
        ax.set_yscale("log")


def ns_per_op(d):
    return 1e3 / d["mops"]


PAPER = ["tas_backoff", "ticket", "anderson", "mcs", "mcs_swap"]
OTHERS = ["tas", "ttas", "anderson_np", "pthread", "mcs", "mcs_sp", "mcs_spw"]

# ---------------- Fig 1: scaling, empty CS -----------------
e1 = load("e1_scaling_empty.csv")
if e1 is not None:
    fig, axs = plt.subplots(1, 2, figsize=(7.2, 2.7), sharey=True)
    line_panel(axs[0], e1, PAPER, "ns", STYLE, "time per acquire/release (ns)", True, ns_per_op)
    axs[0].set_title("(a) Algorithms from the paper")
    line_panel(axs[1], e1, OTHERS, "ns", STYLE, "", True, ns_per_op)
    axs[1].set_title("(b) Baselines, ablation and MCS-SP")
    for a in axs:
        a.legend(frameon=False, loc="upper left")
    fig.savefig(FIG / "fig1_scaling_empty.pdf")
    fig.savefig(FIG / "fig1_scaling_empty.png")
    plt.close(fig)

# ---------------- Fig 2: scaling, small CS -----------------
e2 = load("e2_scaling_cs.csv")
if e2 is not None:
    fig, axs = plt.subplots(1, 2, figsize=(7.2, 3.2), layout="constrained")
    keys = ["ttas", "tas_backoff", "ticket", "anderson", "mcs", "mcs_sp", "mcs_spw", "pthread"]
    line_panel(axs[0], e2, keys, "mops", STYLE, "throughput (M acquisitions/s)")
    axs[0].set_title("(a) Throughput")
    h, l = axs[0].get_legend_handles_labels()
    fig.legend(h, l, loc="outside lower center", ncol=4, frameon=False)
    line_panel(axs[1], e2, keys, "jain", STYLE, "Jain fairness index")
    axs[1].set_title("(b) Fairness (1.0 = perfectly fair)")
    axs[1].set_ylim(0, 1.05)
    fig.savefig(FIG / "fig2_scaling_cs.pdf")
    fig.savefig(FIG / "fig2_scaling_cs.png")
    plt.close(fig)

# ---------------- Fig 3: oversubscription -----------------
e3 = load("e3_oversub.csv")
if e3 is not None:
    keys = ["tas_backoff", "ticket", "anderson", "mcs", "mcs_sp", "mcs_spw", "pthread"]
    tcounts = sorted(e3.threads.unique())
    med = e3.groupby(["lock", "threads"])[["mops", "jain", "cpu_s_per_mop"]].median()
    fig, axs = plt.subplots(1, 3, figsize=(7.4, 3.0), layout="constrained")
    w = 0.8 / len(keys)
    for i, k in enumerate(keys):
        c, m, ls, lab = STYLE[k]
        xs = [j + (i - (len(keys) - 1) / 2) * w for j in range(len(tcounts))]
        for ax, col in zip(axs, ["mops", "jain", "cpu_s_per_mop"]):
            vals = [med.loc[(k, t), col] if (k, t) in med.index else float("nan") for t in tcounts]
            ax.bar(xs, vals, width=w * 0.88, color=c, label=lab, edgecolor="white", linewidth=0.5,
                   hatch="////" if k in ("mcs_sp", "mcs_spw") else None)
    for ax, title, yl in zip(axs, ["(a) Throughput", "(b) Fairness", "(c) CPU cost"],
                             ["M acquisitions/s (log)", "Jain index", "CPU-s per M acquisitions (log)"]):
        ax.set_xticks(range(len(tcounts)))
        ax.set_xticklabels([str(t) for t in tcounts])
        ax.set_xlabel("threads")
        ax.set_title(title)
        ax.set_ylabel(yl)
        ax.grid(axis="x", visible=False)
    axs[0].set_yscale("log")
    axs[2].set_yscale("log")
    axs[1].set_ylim(0, 1.05)
    h, l = axs[0].get_legend_handles_labels()
    fig.legend(h, l, loc="outside upper center", ncol=4, frameon=False)
    fig.savefig(FIG / "fig3_oversub.pdf")
    fig.savefig(FIG / "fig3_oversub.png")
    plt.close(fig)

# ---------------- Fig 4: spin-limit sweep -----------------
e4 = load("e4_spin_sweep.csv")
if e4 is not None:
    fig, ax = plt.subplots(figsize=(3.5, 2.5))
    tset = sorted(e4.threads.unique())
    for lk in ["mcs_sp", "mcs_spw"]:
        c, m, _, lab = STYLE[lk]
        for t, ls in zip(tset, ["-", "--"]):
            d = e4[(e4.threads == t) & (e4.lock == lk)].groupby("spin_limit")["mops"]
            if d.ngroups == 0:
                continue
            x = [max(s, 1) for s in d.median().index]
            ax.errorbar(x, d.median().values, yerr=[d.median() - d.min(), d.max() - d.median()],
                        color=c, marker=m, ls=ls, capsize=2, label=f"{lab.split(' (')[0]}, {t} thr.",
                        markeredgecolor="white")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("spin limit before parking (PAUSE iterations; 0 plotted at 1)")
    ax.set_ylabel("M acquisitions/s")
    ax.legend(frameon=False)
    fig.savefig(FIG / "fig4_spin_sweep.pdf")
    fig.savefig(FIG / "fig4_spin_sweep.png")
    plt.close(fig)

# ---------------- Fig 6: wake-ahead depth -----------------
e7 = load("e7_wake_depth.csv")
if e7 is not None:
    fig, ax = plt.subplots(figsize=(3.5, 2.9), layout="constrained")
    ramp = ["#9ec5f4", "#5598e7", "#256abf", "#0d366b"]  # one hue, light->dark = more threads
    for t, c, m in zip(sorted(e7.threads.unique()), ramp, ["o", "s", "^", "D"]):
        d = e7[e7.threads == t].groupby("wake_depth")["mops"]
        ax.errorbar(d.median().index, d.median().values, yerr=[d.median() - d.min(), d.max() - d.median()],
                    color=c, marker=m, capsize=2, label=f"{t} threads", markeredgecolor="white")
    ax.set_yscale("log")
    ax.set_xlabel("wake-ahead depth (0 = MCS-SP behaviour)")
    ax.set_ylabel("M acquisitions/s (log)")
    fig.legend(*ax.get_legend_handles_labels(), loc="outside lower center", ncol=4, frameon=False, fontsize=6.5)
    fig.savefig(FIG / "fig6_wake_depth.pdf")
    fig.savefig(FIG / "fig6_wake_depth.png")
    plt.close(fig)

# ---------------- Fig 5: barriers -----------------
e5, e6 = load("e5_barriers.csv"), load("e6_barriers_oversub.csv")
if e5 is not None:
    fig, axs = plt.subplots(1, 2, figsize=(7.2, 2.6), gridspec_kw={"width_ratios": [1.5, 1]})
    line_panel(axs[0], e5, list(BSTYLE), "us_per_episode", BSTYLE, "time per barrier episode (µs)", True)
    axs[0].set_title("(a) threads ≤ hardware threads")
    axs[0].legend(frameon=False, loc="upper left", fontsize=6.5)
    if e6 is not None:
        med = e6.groupby(["barrier", "threads"])["us_per_episode"].median()
        tc = sorted(e6.threads.unique())
        keys = list(BSTYLE)
        w = 0.8 / len(keys)
        for i, k in enumerate(keys):
            c, m, ls, lab = BSTYLE[k]
            xs = [j + (i - (len(keys) - 1) / 2) * w for j in range(len(tc))]
            axs[1].bar(xs, [med.get((k, t), float("nan")) for t in tc], width=w * 0.88, color=c,
                       edgecolor="white", linewidth=0.5, hatch="////" if k == "central_sp" else None)
        axs[1].set_xticks(range(len(tc)))
        axs[1].set_xticklabels([str(t) for t in tc])
        axs[1].set_yscale("log")
        axs[1].set_xlabel("threads")
        axs[1].set_ylabel("µs per episode (log)")
        axs[1].set_title("(b) oversubscribed")
        axs[1].grid(axis="x", visible=False)
    fig.savefig(FIG / "fig5_barriers.pdf")
    fig.savefig(FIG / "fig5_barriers.png")
    plt.close(fig)

# ---------------- Tables (LaTeX fragments + console) -----------------
out = []
if e1 is not None:
    one = e1[e1.threads == 1].groupby("lock")["mops"].median()
    base = 1e3 / one["tas_backoff"]
    order = ["tas", "ttas", "tas_backoff", "ticket", "anderson", "anderson_np", "mcs", "mcs_swap", "mcs_sp", "mcs_spw", "pthread"]
    rows = [f"{STYLE[k][3]} & {1e3 / one[k]:.1f} & {1e3 / one[k] / base:.2f}\\\\" for k in order if k in one]
    (FIG / "tab_latency.tex").write_text("\n".join(rows) + "\n")
    out.append("Uncontended latency (ns, ratio vs tas_backoff):\n" + "\n".join(rows))
    hmax = e1.threads.max()
    m = e1[e1.threads == hmax].groupby("lock")[["mops", "jain"]].median()
    out.append(f"E1 at {hmax} threads:\n" + m.assign(ns=1e3 / m.mops).round(3).to_string())
if e3 is not None:
    m = e3.groupby(["threads", "lock"])[["mops", "jain", "cpu_s_per_mop", "min_ops"]].median()
    out.append("E3 oversubscription:\n" + m.round(4).to_string())
    rows = []
    for t in sorted(e3.threads.unique()):
        r = m.loc[t]
        cells = " & ".join(f"{r.loc[k, 'mops']:.2f}" if k in r.index else "--"
                           for k in ["tas_backoff", "ticket", "anderson", "mcs", "mcs_sp", "mcs_spw", "pthread"])
        rows.append(f"{t} & {cells}\\\\")
    (FIG / "tab_oversub.tex").write_text("\n".join(rows) + "\n")
if e2 is not None:
    m = e2.groupby(["threads", "lock"])[["mops", "jain"]].median()
    out.append("E2:\n" + m.round(3).to_string())
if e4 is not None:
    out.append("E4:\n" + e4.groupby(["lock", "threads", "spin_limit"])["mops"].median().round(3).to_string())
if e7 is not None:
    out.append("E7:\n" + e7.groupby(["threads", "wake_depth"])[["mops", "jain", "cpu_s_per_mop"]].median().round(3).to_string())
if e5 is not None:
    out.append("E5:\n" + e5.groupby(["threads", "barrier"])["us_per_episode"].median().round(2).to_string())
if e6 is not None:
    out.append("E6:\n" + e6.groupby(["threads", "barrier"])[["us_per_episode", "cpu_s"]].median().round(1).to_string())
(FIG / "summary.txt").write_text("\n\n".join(out) + "\n")
print("\n\n".join(out))
