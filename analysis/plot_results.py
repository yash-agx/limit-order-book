"""Read the benchmark CSVs in results/, print a summary table and draw the charts.

    python analysis/plot_results.py

Every number shown is the median of the repeated runs for that book size.
"""
import csv
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")                               # draw straight to files, no window needed
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"

# Colours
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
BLUE, ORANGE, AQUA = "#2a78d6", "#eb6834", "#1baf7a"

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


def read_csv(name):
    with open(RESULTS / name, newline="") as f:
        return list(csv.DictReader(f))


def medians(rows, column):
    """{book_size: median of `column` across the repeats for that size}"""
    by_size = {}
    for row in rows:
        by_size.setdefault(int(row["book_size"]), []).append(float(row[column]))
    return {size: statistics.median(values) for size, values in sorted(by_size.items())}


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


def title(fig, heading, subtitle):
    fig.text(0.02, 0.97, heading, fontsize=13, fontweight="bold", va="top")
    fig.text(0.02, 0.925, subtitle, fontsize=10, color=INK_SECONDARY, va="top")


# ---------- table ----------

def print_table(summary, by_type):
    cols = ["orders_per_sec", "avg_ns", "p50_ns", "p99_ns", "p999_ns", "max_ns", "book_memory_mb", "peak_memory_mb"]
    med = {c: medians(summary, c) for c in cols}
    sizes = list(med["p50_ns"])

    header = ["book size", "orders/sec", "avg ns", "p50 ns", "p99 ns", "p99.9 ns", "worst ms", "book MB", "bytes/order", "peak MB"]
    table = []
    for s in sizes:
        table.append([
            f"{s:,}", f"{med['orders_per_sec'][s]:,.0f}", f"{med['avg_ns'][s]:.0f}", f"{med['p50_ns'][s]:.0f}",
            f"{med['p99_ns'][s]:.0f}", f"{med['p999_ns'][s]:.0f}", f"{med['max_ns'][s] / 1e6:.2f}",
            f"{med['book_memory_mb'][s]:.2f}", f"{med['book_memory_mb'][s] * 1024 * 1024 / s:.0f}",
            f"{med['peak_memory_mb'][s]:.0f}",
        ])
    widths = [max(len(header[i]), *(len(r[i]) for r in table)) for i in range(len(header))]
    print("  ".join(h.rjust(w) for h, w in zip(header, widths)))
    for r in table:
        print("  ".join(v.rjust(w) for v, w in zip(r, widths)))

    with open(RESULTS / "summary_median.csv", "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["book_size"] + cols)
        for s in sizes:
            out.writerow([s] + [med[c][s] for c in cols])

    print("\nMedian (p50) latency in ns by kind of action:")
    kinds = ["limit", "market", "cancel", "modify"]
    print("  ".join(["book size".rjust(10)] + [k.rjust(8) for k in kinds]))
    for s in sizes:
        cells = []
        for k in kinds:
            rows = [r for r in by_type if r["action"] == k]
            cells.append(f"{medians(rows, 'p50_ns')[s]:.0f}".rjust(8))
        print("  ".join([f"{s:,}".rjust(10)] + cells))


# ---------- charts ----------

def plot_latency_distribution(summary, hist):
    p50, p99 = medians(summary, "p50_ns"), medians(summary, "p99_ns")
    sizes = list(p50)

    fig, axes = plt.subplots(len(sizes), 1, figsize=(8, 7.6), sharex=True, sharey=True)
    title(fig, "Latency per action, by book size",
          "Share of actions in each latency bucket. Rare slower actions (up to a few ms) are off the right edge.")

    for ax, size in zip(axes, sizes):
        rows = [r for r in hist if int(r["book_size"]) == size]
        total = sum(int(r["count"]) for r in rows)
        lo = [float(r["bucket_lo_ns"]) for r in rows]
        width = [float(r["bucket_hi_ns"]) - float(r["bucket_lo_ns"]) for r in rows]
        share = [100 * int(r["count"]) / total for r in rows]

        ax.bar(lo, share, width=width, align="edge", color=BLUE, edgecolor=SURFACE, linewidth=0.6)
        ax.set_xscale("log")
        ax.set_xlim(10, 100_000)
        ax.grid(axis="x", visible=False)
        ax.set_ylabel("% of actions")

        top = max(share)
        for value, name in ((p50[size], "p50"), (p99[size], "p99")):
            ax.axvline(value, color=INK_SECONDARY, linewidth=1, linestyle=(0, (3, 3)))
            ax.text(value * 1.06, top * 0.97, f"{name}  {ns_label(value)}", color=INK_SECONDARY, fontsize=9, va="top")
        ax.text(0.01, 0.92, f"{size_label(size)} resting orders", transform=ax.transAxes,
                fontweight="bold", va="top")

    axes[-1].set_xticks([10, 100, 1000, 10_000, 100_000])
    axes[-1].set_xticklabels(["10 ns", "100 ns", "1 µs", "10 µs", "100 µs"])
    axes[-1].set_xlabel("Latency of one action (log scale)")
    fig.subplots_adjust(left=0.09, right=0.97, top=0.87, bottom=0.08, hspace=0.12)
    fig.savefig(RESULTS / "latency_distribution.png", dpi=160)
    plt.close(fig)


def plot_throughput(summary):
    rate = medians(summary, "orders_per_sec")
    sizes = list(rate)
    lowest = {s: min(float(r["orders_per_sec"]) for r in summary if int(r["book_size"]) == s) for s in sizes}
    highest = {s: max(float(r["orders_per_sec"]) for r in summary if int(r["book_size"]) == s) for s in sizes}

    fig, ax = plt.subplots(figsize=(8, 4.8))
    title(fig, "Throughput falls as the book grows",
          "Actions processed per second (median of the repeats; bars show the slowest and fastest repeat).")

    millions = [rate[s] / 1e6 for s in sizes]
    ax.vlines(sizes, [lowest[s] / 1e6 for s in sizes], [highest[s] / 1e6 for s in sizes], color=AXIS, linewidth=2)
    ax.plot(sizes, millions, color=BLUE, linewidth=2, marker="o", markersize=7,
            markeredgecolor=SURFACE, markeredgewidth=1.5)
    for s, m in zip(sizes, millions):
        ax.annotate(f"{m:.2f}M", (s, m), textcoords="offset points", xytext=(8, 8), color=INK)

    ax.set_xscale("log")
    ax.set_xticks(sizes)
    ax.set_xticklabels([size_label(s) for s in sizes])
    ax.minorticks_off()
    ax.set_xlim(sizes[0] / 1.8, sizes[-1] * 2.4)
    ax.set_ylim(0, max(millions) * 1.25)
    ax.grid(axis="x", visible=False)
    ax.set_xlabel("Resting orders in the book (log scale)")
    ax.set_ylabel("Million actions per second")
    fig.subplots_adjust(left=0.09, right=0.97, top=0.82, bottom=0.12)
    fig.savefig(RESULTS / "throughput_vs_book_size.png", dpi=160)
    plt.close(fig)


def plot_latency_percentiles(summary):
    series = [("p99.9", "p999_ns", AQUA), ("p99", "p99_ns", ORANGE), ("p50 (median)", "p50_ns", BLUE)]
    fig, ax = plt.subplots(figsize=(8, 4.8))
    title(fig, "Latency percentiles as the book grows",
          "Time for one action. The tail (p99, p99.9) is many times slower than the typical action.")

    for name, column, colour in series:
        values = medians(summary, column)
        sizes = list(values)
        ax.plot(sizes, list(values.values()), color=colour, linewidth=2, marker="o", markersize=7,
                markeredgecolor=SURFACE, markeredgewidth=1.5, label=name)
        ax.annotate(f"{name.split()[0]}  {ns_label(values[sizes[-1]])}", (sizes[-1], values[sizes[-1]]),
                    textcoords="offset points", xytext=(10, -3), color=INK)

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xticks(sizes)
    ax.set_xticklabels([size_label(s) for s in sizes])
    ax.minorticks_off()
    ax.set_xlim(sizes[0] / 1.8, sizes[-1] * 6)
    ax.set_yticks([100, 1000, 10_000, 100_000])
    ax.set_yticklabels(["100 ns", "1 µs", "10 µs", "100 µs"])
    ax.set_ylim(100, 100_000)
    ax.grid(axis="x", visible=False)
    ax.set_xlabel("Resting orders in the book (log scale)")
    ax.set_ylabel("Latency (log scale)")
    ax.legend(frameon=False, loc="upper left", ncol=3, labelcolor=INK_SECONDARY)
    fig.subplots_adjust(left=0.11, right=0.97, top=0.82, bottom=0.12)
    fig.savefig(RESULTS / "latency_percentiles_vs_book_size.png", dpi=160)
    plt.close(fig)


def main():
    summary = read_csv("summary.csv")
    hist = read_csv("latency_hist.csv")
    by_type = read_csv("latency_by_type.csv")

    print_table(summary, by_type)
    plot_latency_distribution(summary, hist)
    plot_throughput(summary)
    plot_latency_percentiles(summary)
    print(f"\nCharts saved in {RESULTS}")


if __name__ == "__main__":
    main()
