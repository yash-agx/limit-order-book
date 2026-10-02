"""Read the benchmark CSVs in results/, print the comparison tables and draw the charts.

    python analysis/plot_results.py

Every engine was measured several times at every book size, on two order flows:
  standard - sequential order ids, prices within $2 of the mid
  hard     - random order ids, a price band of up to 10,000 levels, orders clustered at the best price

The tables show the median of the runs and the spread (slowest to fastest run).

Noise rule: two results count as different only if their spreads do not overlap, i.e.
every run of one was faster than every run of the other. Anything else is reported as
"within noise" - the difference is smaller than the run-to-run variation.
"""
import csv
import math
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")                               # draw straight to files, no window needed
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"

FLOWS = ["standard", "hard"]
FLOW_TITLE = {"standard": "Standard flow", "hard": "Hard flow"}

# The engines compared head to head, and the ablation path from A to C and on to D.
MAIN = ["A", "B", "C4", "D"]
ABLATION = ["A", "C1", "C2", "C3", "C4", "D"]
NAME = {"A": "A  std::map + std::list", "B": "B  flat arrays", "C4": "C  custom structures",
        "D": "D  C + flat price levels"}
SHORT = {"A": "A", "B": "B", "C4": "C", "D": "D"}
STEP = {"A": "A\nbaseline", "C1": "+ pool", "C2": "+ intrusive\nlist", "C3": "+ hash\nmap",
        "C4": "+ layout\n(= C)", "D": "+ flat levels\n(= D)"}

# Colours
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
BLUE, ORANGE, AQUA, YELLOW = "#2a78d6", "#eb6834", "#1baf7a", "#eda100"
COLOUR = {"A": BLUE, "B": ORANGE, "C4": AQUA, "D": YELLOW}   # an engine keeps its colour in every chart

plt.rcParams.update({
    "font.family": ["Segoe UI", "DejaVu Sans"],
    "font.size": 10,
    "figure.facecolor": SURFACE,
    "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE,
    "text.color": INK,
    "axes.labelcolor": INK_SECONDARY,
    "axes.edgecolor": AXIS,
    "xtick.color": MUTED,
    "ytick.color": MUTED,
    "xtick.labelcolor": INK_SECONDARY,
    "ytick.labelcolor": INK_SECONDARY,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.grid": True,
    "grid.color": GRID,
    "grid.linewidth": 0.8,
    "axes.axisbelow": True,
})


# ---------- reading the data ----------

def read_csv(name):
    with open(RESULTS / name, newline="") as f:
        return list(csv.DictReader(f))


class Results:
    """The rows of summary.csv for one order flow, with easy access to the runs of one engine at one book size."""

    def __init__(self, rows, flow):
        self.flow = flow
        self.rows = [r for r in rows if r["flow"] == flow]
        self.sizes = sorted({int(r["book_size"]) for r in self.rows})

    def runs(self, engine, size, column):
        return [float(r[column]) for r in self.rows if r["engine"] == engine and int(r["book_size"]) == size]

    def median(self, engine, size, column):
        return statistics.median(self.runs(engine, size, column))


def differs(a, b):
    """True if the two sets of runs don't overlap at all (see the noise rule above)."""
    return max(a) < min(b) or max(b) < min(a)


def size_label(size):
    return f"{size // 1_000_000}M" if size >= 1_000_000 else f"{size // 1000}k"


def ns_label(ns):
    if ns >= 1_000_000:
        return f"{ns / 1_000_000:.1f} ms"
    if ns >= 10_000:
        return f"{ns / 1000:.0f} µs"
    if ns >= 1000:
        return f"{ns / 1000:.1f} µs"
    return f"{ns:.0f} ns"


def print_rows(header, rows):
    widths = [max(len(header[i]), *(len(r[i]) for r in rows)) for i in range(len(header))]
    print("  ".join(h.rjust(w) for h, w in zip(header, widths)))
    for r in rows:
        print("  ".join(v.rjust(w) for v, w in zip(r, widths)))


# ---------- tables ----------

def compare(res, engine, other, size, column="orders_per_sec"):
    """'x2.41' if engine differs from other by more than the noise, else 'x2.41 (within noise)'."""
    mine, theirs = res.runs(engine, size, column), res.runs(other, size, column)
    ratio = statistics.median(mine) / statistics.median(theirs)
    return f"x{ratio:.2f}" if differs(mine, theirs) else f"x{ratio:.2f} (within noise)"


def ranking(res, size, column="orders_per_sec", higher_is_better=True):
    """Engines from best to worst, e.g. 'D > B > C > A'. '~' joins two neighbours that are within noise."""
    order = sorted(MAIN, key=lambda e: res.median(e, size, column), reverse=higher_is_better)
    text = SHORT[order[0]]
    for before, after in zip(order, order[1:]):
        clear = differs(res.runs(before, size, column), res.runs(after, size, column))
        text += (" > " if clear else " ~ ") + SHORT[after]
    return text


def print_main_table(res):
    print(f"\n=== {FLOW_TITLE[res.flow]}: A vs B vs C vs D, median of the runs [slowest - fastest run] ===")
    header = ["book", "engine", "M actions/s", "[spread]", "vs A", "p50 ns", "p99 ns", "p99.9 ns", "worst µs"]
    rows = []
    for size in res.sizes:
        for e in MAIN:
            rate = res.runs(e, size, "orders_per_sec")
            rows.append([
                size_label(size), SHORT[e], f"{statistics.median(rate) / 1e6:.2f}",
                f"[{min(rate) / 1e6:.2f} - {max(rate) / 1e6:.2f}]",
                "-" if e == "A" else compare(res, e, "A", size),
                f"{res.median(e, size, 'p50_ns'):.0f}", f"{res.median(e, size, 'p99_ns'):.0f}",
                f"{res.median(e, size, 'p999_ns'):.0f}", f"{res.median(e, size, 'max_ns') / 1000:.0f}",
            ])
    print_rows(header, rows)

    print(f"\n{FLOW_TITLE[res.flow]} - ranking, best first ('~' = within noise of its neighbour):")
    for size in res.sizes:
        print(f"  {size_label(size):>4}  throughput: {ranking(res, size):<16}"
              f"  p99.9 latency: {ranking(res, size, 'p999_ns', higher_is_better=False)}")

    print(f"\n{FLOW_TITLE[res.flow]} - D against B and C (throughput):")
    for size in res.sizes:
        print(f"  {size_label(size):>4}  D is {compare(res, 'D', 'B', size)} of B,  {compare(res, 'D', 'C4', size)} of C")


def print_ablation(res):
    print("\n=== Ablation (standard flow): throughput after each step, and the change that step made ===")
    header = ["book", "step", "M actions/s", "[spread]", "vs previous step", "p50 ns", "p99 ns", "book MB"]
    rows = []
    for size in res.sizes:
        for i, e in enumerate(ABLATION):
            rate = res.runs(e, size, "orders_per_sec")
            rows.append([
                size_label(size), STEP[e].replace("\n", " "), f"{statistics.median(rate) / 1e6:.2f}",
                f"[{min(rate) / 1e6:.2f} - {max(rate) / 1e6:.2f}]",
                "-" if i == 0 else compare(res, e, ABLATION[i - 1], size),
                f"{res.median(e, size, 'p50_ns'):.0f}", f"{res.median(e, size, 'p99_ns'):.0f}",
                f"{res.median(e, size, 'book_memory_mb'):.2f}",
            ])
    print_rows(header, rows)


def print_memory(results):
    print("\n=== Book memory in MB: right after filling the book -> after 1M more actions (book size unchanged) ===")
    header = ["book"] + [f"{SHORT[e]} {flow}" for flow in FLOWS for e in MAIN]
    rows = []
    for size in results["standard"].sizes:
        row = [size_label(size)]
        for flow in FLOWS:
            for e in MAIN:
                res = results[flow]
                row.append(f"{res.median(e, size, 'book_memory_mb'):.2f} -> {res.median(e, size, 'book_memory_end_mb'):.2f}")
        rows.append(row)
    print_rows(header, rows)


def print_by_kind(res, by_type):
    print(f"\n=== {FLOW_TITLE[res.flow]}: typical (p50) latency in ns by kind of action ===")
    kinds = ["limit", "market", "cancel", "modify"]
    rows = []
    for size in res.sizes:
        for e in MAIN:
            cells = []
            for k in kinds:
                values = [float(r["p50_ns"]) for r in by_type if r["flow"] == res.flow and r["engine"] == e
                          and int(r["book_size"]) == size and r["action"] == k]
                cells.append(f"{statistics.median(values):.0f}")
            rows.append([size_label(size), SHORT[e]] + cells)
    print_rows(["book", "engine"] + kinds, rows)


def write_median_csv(results):
    columns = ["orders_per_sec", "avg_ns", "p50_ns", "p99_ns", "p999_ns", "max_ns",
               "book_memory_mb", "book_memory_end_mb", "price_levels"]
    with open(RESULTS / "summary_median.csv", "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["flow", "engine", "book_size", "runs", "orders_per_sec_min", "orders_per_sec_max"] + columns)
        for res in results.values():
            for size in res.sizes:
                for e in sorted({r["engine"] for r in res.rows}):
                    rate = res.runs(e, size, "orders_per_sec")
                    out.writerow([res.flow, e, size, len(rate), min(rate), max(rate)]
                                 + [res.median(e, size, c) for c in columns])


# ---------- charts ----------

def title(fig, heading, subtitle, top=0.97, gap=0.045):
    fig.text(0.02, top, heading, fontsize=13, fontweight="bold", va="top")
    fig.text(0.02, top - gap, subtitle, fontsize=10, color=INK_SECONDARY, va="top")


def book_size_axis(ax, sizes, right_room=2.4):
    ax.set_xscale("log")
    ax.set_xticks(sizes)
    ax.set_xticklabels([size_label(s) for s in sizes])
    ax.minorticks_off()
    ax.set_xlim(sizes[0] / 1.8, sizes[-1] * right_room)
    ax.grid(axis="x", visible=False)


def end_labels(ax, res, column, scale=1.0, log=False):
    """Write each engine's letter next to the last point of its line, nudging labels that would collide."""
    last = res.sizes[-1]
    points = sorted(((res.median(e, last, column) * scale, e) for e in MAIN))
    low, high = ax.get_ylim()
    placed = []
    for value, e in points:
        value = max(value, low)
        pos = (math.log10(value) - math.log10(low)) / (math.log10(high) - math.log10(low)) if log \
            else (value - low) / (high - low)
        if placed and pos - placed[-1] < 0.05:
            pos = placed[-1] + 0.05                     # keep at least 5% of the axis height apart
        placed.append(pos)
        # x is in data units (just right of the last point), y is a fraction of the axis height
        ax.text(last * 1.3, pos, SHORT[e], transform=ax.get_xaxis_transform(), color=INK, va="center")


def plot_throughput(results):
    fig, axes = plt.subplots(1, 2, figsize=(11, 5), sharey=True)
    title(fig, "Throughput by engine as the book grows, on both order flows",
          "Million actions per second. Dots are the median run; the grey bars span the slowest to fastest run.")

    for ax, flow in zip(axes, FLOWS):
        res = results[flow]
        for e in MAIN:
            med = [res.median(e, s, "orders_per_sec") / 1e6 for s in res.sizes]
            lo = [min(res.runs(e, s, "orders_per_sec")) / 1e6 for s in res.sizes]
            hi = [max(res.runs(e, s, "orders_per_sec")) / 1e6 for s in res.sizes]
            ax.vlines(res.sizes, lo, hi, color=AXIS, linewidth=3, zorder=2)
            ax.plot(res.sizes, med, color=COLOUR[e], linewidth=2, marker="o", markersize=7,
                    markeredgecolor=SURFACE, markeredgewidth=1.5, label=NAME[e], zorder=3)
        book_size_axis(ax, res.sizes, right_room=3)
        ax.set_title(FLOW_TITLE[flow], fontsize=10, fontweight="bold", loc="left")
        ax.set_xlabel("Resting orders in the book (log scale)")

    axes[0].set_ylim(0, None)
    for ax, flow in zip(axes, FLOWS):
        end_labels(ax, results[flow], "orders_per_sec", scale=1e-6)
    axes[0].set_ylabel("Million actions per second")
    axes[1].legend(frameon=False, loc="upper right", labelcolor=INK_SECONDARY)
    fig.subplots_adjust(left=0.07, right=0.97, top=0.80, bottom=0.11, wspace=0.06)
    fig.savefig(RESULTS / "throughput_vs_book_size.png", dpi=160)
    plt.close(fig)


def plot_latency_percentiles(res):
    panels = [("p50_ns", "p50 (typical action)"), ("p99_ns", "p99 (1 in 100 is slower)"), ("p999_ns", "p99.9 (1 in 1,000 is slower)")]
    fig, axes = plt.subplots(1, 3, figsize=(11, 4.8), sharey=True)
    title(fig, f"Latency percentiles by engine - {FLOW_TITLE[res.flow].lower()}",
          "Time for one action, median of the runs. Lower is better. Log scale on both axes.", top=0.975, gap=0.05)

    for ax, (column, name) in zip(axes, panels):
        for e in MAIN:
            values = [res.median(e, s, column) for s in res.sizes]
            ax.plot(res.sizes, values, color=COLOUR[e], linewidth=2, marker="o", markersize=6,
                    markeredgecolor=SURFACE, markeredgewidth=1.5, label=NAME[e])
        book_size_axis(ax, res.sizes, right_room=3.5)
        ax.set_yscale("log")
        ax.set_title(name, fontsize=10, color=INK, loc="left")
        ax.set_xlabel("Resting orders")

    axes[0].set_yticks([10, 100, 1000, 10_000])
    axes[0].set_yticklabels(["10 ns", "100 ns", "1 µs", "10 µs"])
    axes[0].set_ylim(20, 40_000)
    for ax, (column, _) in zip(axes, panels):
        end_labels(ax, res, column, log=True)
    axes[0].legend(frameon=False, loc="upper left", labelcolor=INK_SECONDARY, fontsize=9)
    fig.subplots_adjust(left=0.07, right=0.97, top=0.78, bottom=0.12, wspace=0.08)
    fig.savefig(RESULTS / f"latency_percentiles_{res.flow}.png", dpi=160)
    plt.close(fig)


def plot_ablation(res):
    fig, axes = plt.subplots(2, 2, figsize=(11.5, 7.6), sharey=True)
    title(fig, "Ablation: what each optimisation added on the way from A to C, then D",
          "Standard flow. Throughput in million actions per second (median; whiskers span the slowest to fastest run).\n"
          "The label over each bar is the change from the bar before it; 'noise' means smaller than run-to-run variation.",
          top=0.98, gap=0.035)

    x = range(len(ABLATION))
    for ax, size in zip(axes.flat, res.sizes):
        med = [res.median(e, size, "orders_per_sec") / 1e6 for e in ABLATION]
        lo = [min(res.runs(e, size, "orders_per_sec")) / 1e6 for e in ABLATION]
        hi = [max(res.runs(e, size, "orders_per_sec")) / 1e6 for e in ABLATION]

        ax.bar(x, med, width=0.62, color=BLUE)
        ax.vlines(x, lo, hi, color=INK_SECONDARY, linewidth=1.5)
        for i, e in enumerate(ABLATION):
            if i == 0:
                label = f"{med[i]:.1f}M"
            else:
                before, now = res.runs(ABLATION[i - 1], size, "orders_per_sec"), res.runs(e, size, "orders_per_sec")
                change = 100 * (med[i] / med[i - 1] - 1)
                label = f"{med[i]:.1f}M\n{change:+.0f}%" if differs(before, now) else f"{med[i]:.1f}M\nnoise"
            ax.annotate(label, (i, hi[i]), textcoords="offset points", xytext=(0, 4), ha="center", va="bottom",
                        fontsize=9, color=INK)

        ax.set_xticks(list(x))
        ax.set_xticklabels([STEP[e] for e in ABLATION], fontsize=9)
        ax.grid(axis="x", visible=False)
        ax.set_title(f"{size_label(size)} resting orders", fontsize=10, fontweight="bold", loc="left")
        ax.margins(y=0.22)

    for ax in axes[:, 0]:
        ax.set_ylabel("Million actions per second")
    fig.subplots_adjust(left=0.06, right=0.98, top=0.83, bottom=0.08, hspace=0.42, wspace=0.05)
    fig.savefig(RESULTS / "ablation_throughput.png", dpi=160)
    plt.close(fig)


def plot_memory(results):
    fig, axes = plt.subplots(1, 2, figsize=(11, 5), sharey=True)
    title(fig, "Book memory after 1,000,000 actions, by engine",
          "The number of resting orders is the same at the start and the end of the run. Log scale on both axes.")

    for ax, flow in zip(axes, FLOWS):
        res = results[flow]
        for e in MAIN:
            values = [max(res.median(e, s, "book_memory_end_mb"), 0.05) for s in res.sizes]
            ax.plot(res.sizes, values, color=COLOUR[e], linewidth=2, marker="o", markersize=7,
                    markeredgecolor=SURFACE, markeredgewidth=1.5, label=NAME[e])
        book_size_axis(ax, res.sizes, right_room=3)
        ax.set_yscale("log")
        ax.set_title(FLOW_TITLE[flow], fontsize=10, fontweight="bold", loc="left")
        ax.set_xlabel("Resting orders in the book (log scale)")

    axes[0].set_yticks([0.1, 1, 10, 100])
    axes[0].set_yticklabels(["0.1 MB", "1 MB", "10 MB", "100 MB"])
    axes[0].set_ylim(0.05, 400)
    axes[0].legend(frameon=False, loc="upper left", labelcolor=INK_SECONDARY)
    fig.subplots_adjust(left=0.08, right=0.97, top=0.80, bottom=0.11, wspace=0.06)
    fig.savefig(RESULTS / "memory_after_flow.png", dpi=160)
    plt.close(fig)


def plot_latency_distribution(res, hist, size):
    fig, axes = plt.subplots(len(MAIN), 1, figsize=(8, 8), sharex=True, sharey=True)
    title(fig, f"Latency per action at {size_label(size)} resting orders - {FLOW_TITLE[res.flow].lower()}",
          "Share of actions in each latency bucket, all runs pooled. Rare slower actions are off the right edge.",
          top=0.98, gap=0.037)

    for ax, e in zip(axes, MAIN):
        buckets = {}
        for r in hist:
            if r["flow"] == res.flow and r["engine"] == e and int(r["book_size"]) == size:
                key = (float(r["bucket_lo_ns"]), float(r["bucket_hi_ns"]))
                buckets[key] = buckets.get(key, 0) + int(r["count"])
        total = sum(buckets.values())
        lo = [k[0] for k in buckets]
        width = [k[1] - k[0] for k in buckets]
        share = [100 * c / total for c in buckets.values()]

        ax.bar(lo, share, width=width, align="edge", color=COLOUR[e], edgecolor=SURFACE, linewidth=0.6)
        ax.set_xscale("log")
        ax.set_xlim(10, 10_000)
        ax.grid(axis="x", visible=False)
        ax.set_ylabel("% of actions")

        for column, name, height in (("p50_ns", "p50", 0.80), ("p99_ns", "p99", 0.62)):
            value = res.median(e, size, column)
            ax.axvline(value, color=INK_SECONDARY, linewidth=1, linestyle=(0, (3, 3)))
            ax.text(value * 1.06, height, f"{name}  {ns_label(value)}", transform=ax.get_xaxis_transform(),
                    color=INK_SECONDARY, fontsize=9, va="top")
        ax.text(0.99, 0.92, NAME[e], transform=ax.transAxes, fontweight="bold", va="top", ha="right")

    axes[-1].set_xticks([10, 100, 1000, 10_000])
    axes[-1].set_xticklabels(["10 ns", "100 ns", "1 µs", "10 µs"])
    axes[-1].set_xlabel("Latency of one action (log scale)")
    fig.subplots_adjust(left=0.09, right=0.97, top=0.89, bottom=0.07, hspace=0.12)
    fig.savefig(RESULTS / f"latency_distribution_{size_label(size)}_{res.flow}.png", dpi=160)
    plt.close(fig)


def main():
    rows = read_csv("summary.csv")
    hist = read_csv("latency_hist.csv")
    by_type = read_csv("latency_by_type.csv")
    results = {flow: Results(rows, flow) for flow in FLOWS}

    for flow in FLOWS:
        print_main_table(results[flow])
    print_ablation(results["standard"])
    print_memory(results)
    for flow in FLOWS:
        print_by_kind(results[flow], by_type)

    checks = {(r["flow"], int(r["book_size"]), r["trade_checksum"]) for r in rows}
    groups = {(r["flow"], int(r["book_size"])) for r in rows}
    print("\nTrade checksums:", "every engine produced identical trades in every experiment"
          if len(checks) == len(groups) else "MISMATCH")
    for flow in FLOWS:
        res = results[flow]
        levels = ", ".join(f"{size_label(s)}: {res.median('A', s, 'price_levels'):.0f}" for s in res.sizes)
        print(f"Price levels in use at the end ({flow} flow): {levels}")

    write_median_csv(results)
    plot_throughput(results)
    plot_ablation(results["standard"])
    plot_memory(results)
    for flow in FLOWS:
        plot_latency_percentiles(results[flow])
        plot_latency_distribution(results[flow], hist, results[flow].sizes[-1])
    print(f"\nCharts saved in {RESULTS}")


if __name__ == "__main__":
    main()
