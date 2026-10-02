"""Run the benchmark for every book size and rebuild the CSVs in results/.

Each size runs in its own fresh bench.exe process, so one size's memory use
can't leak into the next size's memory numbers.

    python analysis/run_bench.py
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent      # the order-book folder
BENCH = ROOT / "build" / "bench.exe"
RESULTS = ROOT / "results"

BOOK_SIZES = [1_000, 10_000, 100_000, 1_000_000]   # resting orders in the book before measuring
ACTIONS = 1_000_000                                # actions measured per run
REPEATS = 5                                        # measured runs per size (after 2 warm-up runs)


def main():
    if not BENCH.exists():
        sys.exit(f"{BENCH} not found - build it first: cmake --build build")

    RESULTS.mkdir(exist_ok=True)
    for name in ("summary.csv", "latency_hist.csv", "latency_by_type.csv"):
        (RESULTS / name).unlink(missing_ok=True)   # bench.exe appends, so start from empty files

    for size in BOOK_SIZES:
        subprocess.run([str(BENCH), str(size), str(ACTIONS), str(REPEATS), str(RESULTS)], check=True)

    print(f"Results written to {RESULTS}")


if __name__ == "__main__":
    main()
