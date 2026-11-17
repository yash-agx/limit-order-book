"""Replay the real ITCH data through engines A, B, C and D and rebuild the CSVs in results_itch/.

Same method as analysis/run_bench.py, so the two sets of numbers can be compared directly:
each measurement is its own fresh bench.exe process (2 warm-up replays, then 1 measured run),
there are 5 measured runs per engine per symbol, and the engines are interleaved - run 1 of
every engine, then run 2 of every engine - with a rotating start, so machine drift hits every
engine about equally.

Needs the action files from analysis/itch_extract.py.

    python analysis/run_itch.py
    python analysis/run_itch.py --max-actions 2000000     # a shorter run while testing
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BENCH = ROOT / "build" / "bench.exe"
ACTIONS = ROOT / "data" / "itch"
RESULTS = ROOT / "results_itch"

ENGINES = ["A", "B", "C4", "D"]
RUNS = 5


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--max-actions", type=int, default=0, help="cap the actions per symbol (0 = whole day)")
    args = parser.parse_args()

    if not BENCH.exists():
        sys.exit(f"{BENCH} not found - build it first: cmake --build build")
    files = sorted(ACTIONS.glob("*.actions"))
    if not files:
        sys.exit(f"no action files in {ACTIONS} - run:\n"
                 f"  python analysis/itch_download.py\n  python analysis/itch_extract.py")

    RESULTS.mkdir(exist_ok=True)
    for name in ("itch_summary.csv", "itch_latency_hist.csv", "itch_latency_by_type.csv"):
        (RESULTS / name).unlink(missing_ok=True)       # bench.exe appends, so start from empty files

    print(f"{len(files)} symbol(s) x {len(ENGINES)} engines x {RUNS} runs = "
          f"{len(files) * len(ENGINES) * RUNS} measured runs")
    for file in files:
        for run in range(1, RUNS + 1):
            shift = (run - 1) % len(ENGINES)
            for engine in ENGINES[shift:] + ENGINES[:shift]:
                command = [str(BENCH), "replay", engine, str(file), str(run), str(RESULTS)]
                if args.max_actions:
                    command.append(str(args.max_actions))
                subprocess.run(command, check=True)

    print(f"\nResults written to {RESULTS}")


if __name__ == "__main__":
    main()
