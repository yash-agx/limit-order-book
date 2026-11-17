"""Read results_itch/, print the real-data tables and draw the charts.

    python analysis/plot_itch.py

What the numbers are
--------------------
Each engine replayed a full day (09:30-16:00) of Nasdaq TotalView-ITCH 5.0 book events for
one symbol, 5 times, in its own process. Tables show the median run and the spread (slowest
to fastest run), and use the same noise rule as analysis/plot_results.py: two results differ
only if their spreads do not overlap.

ITCH is a feed of events Nasdaq has already matched, so its adds never cross the book. These
numbers measure the engines on real order-book *update* flow, not on real matching - the
"trades" and "crossed" columns exist to show exactly that.

The last table lines the real-data throughput up against the synthetic result (from
results/summary.csv) at whichever synthetic book size is closest to the real book, so a
change in the ranking between synthetic and real data is easy to see.
"""
import csv
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")                               # draw straight to files, no window needed
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results_itch"
SYNTHETIC = ROOT / "results"

ENGINES = ["A", "B", "C4", "D"]
SHORT = {"A": "A", "B": "B", "C4": "C", "D": "D"}
NAME = {"A": "A  std::map + std::list", "B": "B  flat arrays", "C4": "C  custom structures",
        "D": "D  C + flat price levels"}

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
BLUE, ORANGE, AQUA, YELLOW = "#2a78d6", "#eb6834", "#1baf7a", "#eda100"
COLOUR = {"A": BLUE, "B": ORANGE, "C4": AQUA, "D": YELLOW}

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

def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


class Replays:
    """The rows of itch_summary.csv, with easy access to the runs of one engine on one symbol."""

    def __init__(self, rows):
        self.rows = rows
        # Symbols ordered by how busy they are, so the tables read from quietest to busiest.
        self.symbols = sorted({r["symbol"] for r in rows}, key=lambda s: self.actions(s))

    def runs(self, symbol, engine, column):
        return [float(r[column]) for r in self.rows if r["symbol"] == symbol and r["engine"] == engine]

    def median(self, symbol, engine, column):
        return statistics.median(self.runs(symbol, engine, column))

    def info(self, symbol, column):
        """A column that is the same for every engine and run (action counts, book size, ...)."""
        return int(float(next(r[column] for r in self.rows if r["symbol"] == symbol)))

    def info_engine(self, symbol, engine, column):
        """A correctness column for one engine. Every run should agree; take the largest if not."""
        return max(int(float(r[column])) for r in self.rows
                   if r["symbol"] == symbol and r["engine"] == engine)

    def actions(self, symbol):
        return self.info(symbol, "actions")


class Synthetic:
    """The synthetic results, for the side-by-side comparison."""

    def __init__(self, rows, flow):
        self.rows = [r for r in rows if r["flow"] == flow]
        self.sizes = sorted({int(r["book_size"]) for r in self.rows})

    def nearest_size(self, orders):
        return min(self.sizes, key=lambda s: abs(s - orders))

    def runs(self, engine, size, column):
        return [float(r[column]) for r in self.rows if r["engine"] == engine and int(r["book_size"]) == size]

    def median(self, engine, size, column):
        return statistics.median(self.runs(engine, size, column))


def differs(a, b):
    """True if the two sets of runs don't overlap at all (the noise rule)."""
    return max(a) < min(b) or max(b) < min(a)


def print_rows(header, rows):
    widths = [max(len(header[i]), *(len(r[i]) for r in rows)) for i in range(len(header))]
    print("  ".join(h.rjust(w) for h, w in zip(header, widths)))
    for r in rows:
        print("  ".join(v.rjust(w) for v, w in zip(r, widths)))


def compare(mine, theirs):
    """'x2.41', or 'x2.41 (within noise)' if the spreads overlap."""
    ratio = statistics.median(mine) / statistics.median(theirs)
    return f"x{ratio:.2f}" if differs(mine, theirs) else f"x{ratio:.2f} (within noise)"


def ranking(res, symbol, column="orders_per_sec", higher_is_better=True):
    """Engines best to worst, e.g. 'D > C > B > A'. '~' joins neighbours that are within noise."""
    order = sorted(ENGINES, key=lambda e: res.median(symbol, e, column), reverse=higher_is_better)
    text = SHORT[order[0]]
    for before, after in zip(order, order[1:]):
        clear = differs(res.runs(symbol, before, column), res.runs(symbol, after, column))
        text += (" > " if clear else " ~ ") + SHORT[after]
    return text


# ---------- tables ----------

def print_dataset(res):
    print("\n=== The data: one trading day (09:30-16:00) of Nasdaq TotalView-ITCH 5.0 per symbol ===")
    header = ["symbol", "date", "ITCH msgs", "= actions", "adds", "cancels", "modifies",
              "09:30 book", "peak book", "end book", "levels", "price span"]
    rows = [[
        s, str(res.info(s, "date")), f"{res.info(s, 'messages'):,}", f"{res.actions(s):,}",
        f"{res.info(s, 'adds'):,}", f"{res.info(s, 'cancels'):,}", f"{res.info(s, 'modifies'):,}",
        f"{res.info(s, 'prefill'):,}", f"{res.info(s, 'peak_live'):,}",
        f"{res.info(s, 'final_book_size'):,}", f"{res.info(s, 'price_levels'):,}",
        f"{res.info(s, 'price_span'):,}",
    ] for s in res.symbols]
    print_rows(header, rows)


def print_honesty(res):
    print("\n=== Does real ITCH flow ever cross the book? (the point of the caveat) ===")
    header = ["symbol", "engine", "trades from the 09:30 prefill", "trades during the replay", "times book was crossed"]
    rows = []
    for s in res.symbols:
        for e in ENGINES:
            rows.append([s, SHORT[e], f"{res.info_engine(s, e, 'prefill_trades'):,}",
                         f"{res.info_engine(s, e, 'flow_trades'):,}",
                         f"{res.info_engine(s, e, 'crossings'):,}"])
    print_rows(header, rows)


def print_main_table(res):
    print("\n=== Real ITCH replay: A vs B vs C vs D, median of 5 runs [slowest - fastest] ===")
    header = ["symbol", "engine", "M actions/s", "[spread]", "vs A", "p50 ns", "p99 ns", "p99.9 ns",
              "worst µs", "book MB", "after MB"]
    rows = []
    for s in res.symbols:
        for e in ENGINES:
            rate = res.runs(s, e, "orders_per_sec")
            rows.append([
                s, SHORT[e], f"{statistics.median(rate) / 1e6:.2f}",
                f"[{min(rate) / 1e6:.2f} - {max(rate) / 1e6:.2f}]",
                "-" if e == "A" else compare(rate, res.runs(s, "A", "orders_per_sec")),
                f"{res.median(s, e, 'p50_ns'):.0f}", f"{res.median(s, e, 'p99_ns'):.0f}",
                f"{res.median(s, e, 'p999_ns'):.0f}", f"{res.median(s, e, 'max_ns') / 1000:.0f}",
                f"{res.median(s, e, 'book_memory_mb'):.2f}",
                f"{res.median(s, e, 'book_memory_end_mb'):.2f}",
            ])
    print_rows(header, rows)

    print("\nRanking, best first ('~' = within noise of its neighbour):")
    for s in res.symbols:
        print(f"  {s:<6} throughput: {ranking(res, s):<18}"
              f" p99: {ranking(res, s, 'p99_ns', False):<18}"
              f" p99.9: {ranking(res, s, 'p999_ns', False)}")


def print_vs_synthetic(res, synthetic):
    print("\n=== Real data against the synthetic flows ===")
    print("Each symbol is compared with the synthetic book size closest to its peak real book size.")
    header = ["symbol", "peak book", "nearest synthetic", "engine",
              "real M/s", "standard M/s", "hard M/s", "real vs standard", "real vs hard"]
    rows = []
    for s in res.symbols:
        peak = res.info(s, "peak_live")
        size = synthetic["standard"].nearest_size(peak)
        for e in ENGINES:
            real = res.runs(s, e, "orders_per_sec")
            std = synthetic["standard"].runs(e, size, "orders_per_sec")
            hard = synthetic["hard"].runs(e, size, "orders_per_sec")
            rows.append([
                s, f"{peak:,}", f"{size:,}", SHORT[e],
                f"{statistics.median(real) / 1e6:.2f}", f"{statistics.median(std) / 1e6:.2f}",
                f"{statistics.median(hard) / 1e6:.2f}",
                compare(real, std), compare(real, hard),
            ])
    print_rows(header, rows)

    print("\nRanking on each flow, for the same book size:")
    for s in res.symbols:
        size = synthetic["standard"].nearest_size(res.info(s, "peak_live"))
        line = f"  {s:<6} ({size:,} orders)  real: {ranking(res, s):<18}"
        for flow in ("standard", "hard"):
            syn = synthetic[flow]
            order = sorted(ENGINES, key=lambda e: syn.median(e, size, "orders_per_sec"), reverse=True)
            text = SHORT[order[0]]
            for before, after in zip(order, order[1:]):
                clear = differs(syn.runs(before, size, "orders_per_sec"), syn.runs(after, size, "orders_per_sec"))
                text += (" > " if clear else " ~ ") + SHORT[after]
            line += f" {flow}: {text:<18}"
        print(line)


def print_by_kind(res, by_type):
    print("\n=== Typical (p50) latency in ns by kind of action (real data) ===")
    kinds = ["limit", "cancel", "modify"]
    rows = []
    for s in res.symbols:
        for e in ENGINES:
            cells = []
            for k in kinds:
                values = [float(r["p50_ns"]) for r in by_type
                          if r["symbol"] == s and r["engine"] == e and r["action"] == k]
                cells.append(f"{statistics.median(values):.0f}" if values else "-")
            rows.append([s, SHORT[e]] + cells)
    print_rows(["symbol", "engine"] + kinds, rows)


def write_median_csv(res):
    columns = ["orders_per_sec", "avg_ns", "p50_ns", "p99_ns", "p999_ns", "max_ns",
               "book_memory_mb", "book_memory_end_mb", "price_levels", "final_book_size"]
    with open(RESULTS / "itch_summary_median.csv", "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["symbol", "engine", "runs", "actions", "peak_live",
                      "orders_per_sec_min", "orders_per_sec_max"] + columns)
        for s in res.symbols:
            for e in ENGINES:
                rate = res.runs(s, e, "orders_per_sec")
                out.writerow([s, e, len(rate), res.actions(s), res.info(s, "peak_live"),
                              min(rate), max(rate)] + [res.median(s, e, c) for c in columns])


# ---------- charts ----------

def title(fig, heading, subtitle, top=0.97, gap=0.045):
    fig.text(0.02, top, heading, fontsize=13, fontweight="bold", va="top")
    fig.text(0.02, top - gap, subtitle, fontsize=10, color=INK_SECONDARY, va="top")


def plot_throughput(res):
    fig, ax = plt.subplots(figsize=(10, 5))
    title(fig, "Throughput on real Nasdaq ITCH data, by engine and symbol",
          "Million actions per second replaying one trading day of book events. Bars are the median of 5 runs;\n"
          "the whiskers span the slowest to fastest run. ITCH adds never cross, so this is update flow, not matching.",
          top=0.98, gap=0.05)

    width = 0.2
    spots = range(len(res.symbols))
    for i, e in enumerate(ENGINES):
        med = [res.median(s, e, "orders_per_sec") / 1e6 for s in res.symbols]
        lo = [min(res.runs(s, e, "orders_per_sec")) / 1e6 for s in res.symbols]
        hi = [max(res.runs(s, e, "orders_per_sec")) / 1e6 for s in res.symbols]
        at = [x + (i - 1.5) * width for x in spots]
        ax.bar(at, med, width=width, color=COLOUR[e], label=NAME[e])
        ax.vlines(at, lo, hi, color=INK_SECONDARY, linewidth=1.2)
        for x, value, top in zip(at, med, hi):
            ax.annotate(f"{value:.1f}", (x, top), textcoords="offset points", xytext=(0, 3),
                        ha="center", fontsize=8, color=INK)

    ax.set_xticks(list(spots))
    ax.set_xticklabels([f"{s}\n{res.actions(s) / 1e6:.1f}M actions" for s in res.symbols], fontsize=9)
    ax.grid(axis="x", visible=False)
    ax.set_ylabel("Million actions per second")
    ax.margins(y=0.18)
    ax.legend(frameon=False, loc="upper left", labelcolor=INK_SECONDARY, fontsize=9)
    fig.subplots_adjust(left=0.07, right=0.98, top=0.78, bottom=0.12)
    fig.savefig(RESULTS / "itch_throughput.png", dpi=160)
    plt.close(fig)


def plot_latency(res):
    panels = [("p50_ns", "p50 (typical action)"), ("p99_ns", "p99 (1 in 100 slower)"),
              ("p999_ns", "p99.9 (1 in 1,000 slower)")]
    fig, axes = plt.subplots(1, 3, figsize=(11.5, 4.8), sharey=True)
    title(fig, "Latency percentiles on real ITCH data",
          "Time for one action, median of 5 runs. Lower is better.", top=0.975, gap=0.05)

    width = 0.2
    spots = range(len(res.symbols))
    for ax, (column, name) in zip(axes, panels):
        for i, e in enumerate(ENGINES):
            values = [res.median(s, e, column) for s in res.symbols]
            ax.bar([x + (i - 1.5) * width for x in spots], values, width=width,
                   color=COLOUR[e], label=NAME[e])
        ax.set_xticks(list(spots))
        ax.set_xticklabels(res.symbols, fontsize=9)
        ax.grid(axis="x", visible=False)
        ax.set_yscale("log")
        ax.set_title(name, fontsize=10, loc="left")

    axes[0].set_yticks([10, 100, 1000, 10_000])
    axes[0].set_yticklabels(["10 ns", "100 ns", "1 µs", "10 µs"])
    axes[0].set_ylabel("Latency of one action")
    axes[0].legend(frameon=False, loc="upper left", labelcolor=INK_SECONDARY, fontsize=8)
    fig.subplots_adjust(left=0.08, right=0.98, top=0.80, bottom=0.10, wspace=0.08)
    fig.savefig(RESULTS / "itch_latency_percentiles.png", dpi=160)
    plt.close(fig)


def plot_real_vs_synthetic(res, synthetic):
    """One panel per engine: real throughput next to the two synthetic flows at the same book size."""
    fig, axes = plt.subplots(1, len(ENGINES), figsize=(12, 4.6), sharey=True)
    title(fig, "Real ITCH data against the synthetic order flows",
          "Million actions per second. Each symbol is compared with the synthetic book size closest to its peak\n"
          "real book size, so the three bars in a group are the same engine on the same amount of work.",
          top=0.98, gap=0.05)

    width = 0.26
    spots = range(len(res.symbols))
    for ax, e in zip(axes, ENGINES):
        sizes = [synthetic["standard"].nearest_size(res.info(s, "peak_live")) for s in res.symbols]
        series = [
            ("real ITCH", [res.median(s, e, "orders_per_sec") / 1e6 for s in res.symbols], COLOUR[e]),
            ("synthetic standard", [synthetic["standard"].median(e, z, "orders_per_sec") / 1e6
                                    for z in sizes], MUTED),
            ("synthetic hard", [synthetic["hard"].median(e, z, "orders_per_sec") / 1e6
                                for z in sizes], AXIS),
        ]
        for i, (label, values, colour) in enumerate(series):
            ax.bar([x + (i - 1) * width for x in spots], values, width=width, color=colour, label=label)
        ax.set_xticks(list(spots))
        ax.set_xticklabels(res.symbols, fontsize=8, rotation=45, ha="right")
        ax.grid(axis="x", visible=False)
        ax.set_title(NAME[e], fontsize=10, fontweight="bold", loc="left")

    axes[0].set_ylabel("Million actions per second")
    axes[0].legend(frameon=False, loc="upper left", labelcolor=INK_SECONDARY, fontsize=8)
    fig.subplots_adjust(left=0.06, right=0.98, top=0.76, bottom=0.17, wspace=0.08)
    fig.savefig(RESULTS / "itch_vs_synthetic.png", dpi=160)
    plt.close(fig)


def main():
    rows = read_csv(RESULTS / "itch_summary.csv")
    by_type = read_csv(RESULTS / "itch_latency_by_type.csv")
    res = Replays(rows)

    print_dataset(res)
    print_honesty(res)
    print_main_table(res)
    print_by_kind(res, by_type)

    synthetic_rows = read_csv(SYNTHETIC / "summary.csv")
    synthetic = {flow: Synthetic(synthetic_rows, flow) for flow in ("standard", "hard")}
    print_vs_synthetic(res, synthetic)

    # Every engine must have ended on exactly the same book, and produced the same trades.
    print()
    for s in res.symbols:
        fingerprints = {(r["trade_checksum"], r["final_book_size"], r["best_bid"], r["best_ask"],
                         r["price_levels"]) for r in rows if r["symbol"] == s}
        crossed = sum(int(r["crossings"]) for r in rows if r["symbol"] == s)
        print(f"{s:<6} {'all engines ended on the same book' if len(fingerprints) == 1 else 'MISMATCH'}"
              f"; book was crossed {crossed} times")

    write_median_csv(res)
    plot_throughput(res)
    plot_latency(res)
    plot_real_vs_synthetic(res, synthetic)
    print(f"\nCharts saved in {RESULTS}")


if __name__ == "__main__":
    main()
