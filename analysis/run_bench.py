"""Benchmark every engine at every book size and rebuild the CSVs in results/.

Two experiments:
  standard flow - every engine (A, B, the ablation steps C1..C4, and D)
  hard flow     - A, B, C (= C4) and D on random order ids, a wide price band and
                  prices clustered at the best price

Each measurement is its own fresh bench.exe process (2 warm-up replays, then 1 measured
run), so one engine's memory use can't leak into another's numbers.

The engines are interleaved: for each book size we do run 1 of every engine, then run 2 of
every engine, and so on, instead of all of A's runs followed by all of B's. If the machine
speeds up or slows down part-way through (heat, background programs), every engine is hit
about equally. The starting engine also rotates each run so none is always first.

    python analysis/run_bench.py
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent      # the repo root
BENCH = ROOT / "build" / "bench.exe"
RESULTS = ROOT / "results"

EXPERIMENTS = {
    "standard": ["A", "B", "C1", "C2", "C3", "C4", "D"],
    "hard": ["A", "B", "C4", "D"],
}
BOOK_SIZES = [1_000, 10_000, 100_000, 1_000_000]   # resting orders in the book before measuring
ACTIONS = 1_000_000                                # actions measured per run
RUNS = 5                                           # measured runs per engine per size


def main():
    if not BENCH.exists():
        sys.exit(f"{BENCH} not found - build it first: cmake --build build")

    RESULTS.mkdir(exist_ok=True)
    for name in ("summary.csv", "latency_hist.csv", "latency_by_type.csv"):
        (RESULTS / name).unlink(missing_ok=True)   # bench.exe appends, so start from empty files

    for flow, engines in EXPERIMENTS.items():
        for size in BOOK_SIZES:
            for run in range(1, RUNS + 1):
                shift = (run - 1) % len(engines)
                for engine in engines[shift:] + engines[:shift]:
                    subprocess.run([str(BENCH), engine, str(size), str(ACTIONS), str(run), str(RESULTS), flow],
                                   check=True)

    print(f"Results written to {RESULTS}")


if __name__ == "__main__":
    main()
