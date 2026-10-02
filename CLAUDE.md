# High-Performance Limit Order Book & Matching Engine

A simplified exchange matching engine in C++17. Python is used only for benchmark analysis and charts.

Research question for the final write-up:
**How does data-structure choice affect the latency and throughput of a limit-order matching engine?**

## About me (Yash)
- IB DP2 student, aspiring quant developer. Comfortable with Python; **new to C++**.
- I need to be able to explain every line of this project in an interview, so:
  - Work one phase at a time, one feature at a time. Don't jump ahead.
  - After writing each piece, explain what it does and *why* it's designed that way, in simple language.
  - Point out the C++ concepts I'm meeting for the first time (templates, iterators, references, etc.).
  - Keep explanations brief (what it does + why). Don't stop to quiz me after each feature — I'll ask if I want to be tested.

## Current status
**Phases 1–3 are DONE. Phase 4 code, experiment and results are DONE**; the only thing left is the
write-up, which I (Yash) am writing myself — don't write it for me.
40 tests per engine × 7 engines, plus a differential test on both order flows, all passing.

Files:
- `Order.hpp` — `Order`, `Trade`, `Side`, `OrderType` types
- `OrderBookBase.hpp / .cpp` — the interface every engine implements, plus the rules shared by all
  (validate, timestamps, trade history, `reserveTrades`, modify, print)
- `OrderBook.hpp / .cpp` — engine **A** (`OrderBook`) and ablation step **C1** (`PoolBook` = A + memory pool)
- `FlatBook.hpp / .cpp` — engine **B**
- `IntrusiveBook.hpp` — engines **C** and **D** as one template with three pluggable parts (index, pool, price levels):
  `IntrusiveBookC2`, `IntrusiveBookC3`, `CustomBook` (= C4 = C), `HybridBook` (= D = C with B's flat price array)
- `Engines.hpp / .cpp` — `makeBook("A" | "B" | "C1" | "C2" | "C3" | "C4" | "D")`
- `OrderFlow.hpp / OrderFlow.cpp` — seeded order-flow generator (`OrderFlowGenerator`, `Action`, `apply`);
  `standardFlow(n)` and `hardFlow(n)` (random 64-bit ids, up to 10,000 price levels, orders clustered within
  20 cents of the mid with 5% far-away orders)
- `main.cpp` — demo using the example book from the spec
- `tests.cpp` — assert-based tests (a template run once per engine) + differential test
- `bench.cpp` — benchmark (`bench.exe <engine> [bookSize] [actions] [run] [outDir] [standard|hard]`), built with
  `-O3 -DNDEBUG`; 2 warm-ups + 1 measured run per process
- `analysis/run_bench.py` — standard flow: all 7 engines; hard flow: A, B, C4, D. Each × 1k/10k/100k/1M × 5 runs,
  interleaved, fresh process each (220 runs, ~20 min); rewrites `results/*.csv`
- `analysis/plot_results.py` — prints the comparison/ablation tables (with the noise rule), writes charts to `results/*.png`
- `results/` — Phase 4 CSVs and charts (committed, so the write-up can cite them)
- `results_phase3/` — the Phase 3 baseline, before the trade-history fix
- `CMakeLists.txt` — build with CMake (Windows, WinLibs g++), from this folder:
  ```
  cmake -S . -B build -G "MinGW Makefiles"
  cmake --build build
  .\build\tests.exe
  .\build\demo.exe
  python analysis\run_bench.py
  python analysis\plot_results.py
  ```

Phase 4 results (i7-14650HX, 1M actions, median of 5 interleaved runs; million actions/sec and p50).
These are from the final 220-run session; an earlier 120-run session (A, B, C only) gave the same ranking.

| Flow | Book | A | B | C | D |
|---|---|---|---|---|---|
| standard | 1k | 8.21M, 115 ns | 19.73M, 41 ns | 13.41M, 67 ns | 24.96M, 36 ns |
| standard | 10k | 6.88M, 132 ns | 17.56M, 41 ns | 12.93M, 70 ns | 22.90M, 43 ns |
| standard | 100k | 4.42M, 204 ns | 13.66M, 45 ns | 10.90M, 87 ns | 17.37M, 58 ns |
| standard | 1M | 2.35M, 403 ns | 8.29M, 45 ns | 5.50M, 195 ns | 6.51M, 135 ns |
| hard | 1k | 8.53M, 110 ns | 11.03M, 81 ns | 16.46M, 55 ns | 24.60M, 41 ns |
| hard | 10k | 7.21M, 134 ns | 9.00M, 95 ns | 13.07M, 66 ns | 20.02M, 45 ns |
| hard | 100k | 4.06M, 222 ns | 6.11M, 138 ns | 10.20M, 89 ns | 15.14M, 63 ns |
| hard | 1M | 1.66M, 567 ns | 2.26M, 411 ns | 5.36M, 193 ns | 6.17M, 146 ns |

Phase 4 findings (full tables: run `python analysis\plot_results.py`):
- Noise rule: two results differ only if their min–max run ranges don't overlap.
- Standard flow ranking by throughput: D > B > C > A at 10k and 100k (B ~ C within noise at 100k); D ~ B at 1k;
  B > D > C > A at 1M.
- Hard flow ranking: D > C > B > A at every size (C ~ B within noise at 10k). B falls from 2.4–3.5x A to
  1.25–1.5x A, so B's standard-flow lead depended on sequential ids and a narrow price band.
- Hard vs standard throughput, per engine (1k / 10k / 100k / 1M; "noise" = run ranges overlap):
  - A: +4% / noise / -8% / -29%
  - B: -44% / -49% / -55% / -73%
  - C: +23% / noise / -6% / noise
  - D: noise / -13% / -13% / -5%
  Every engine moves somewhere; B moves by far the most, D never by more than 13%.
- D vs C (flat price array instead of `std::map`, nothing else changed): +86% / +77% / +59% / +19% at
  1k / 10k / 100k / 1M on the standard flow, +49% / +53% / +48% / +15% on the hard flow.
- Ablation A → C → D (standard flow): pool is small and inconsistent (noise at 1k and 100k, -5% at 10k, +16% at 1M;
  the earlier session had it within noise at all four sizes); intrusive list +21–39% (noise at 1k);
  open-addressing hash map +53–79%; compact layout within noise at every size; flat levels as above.
- p99.9: D is best or tied for best at every size on both flows. B's p99.9 is no better than A's at 100k
  (standard) and worse than A's at 1k and 10k (hard).
- B's memory grows while the book stays the same size: 0.27 → 15.7 MB at 1k and 90 → 124 MB at 1M over 1M
  actions (standard flow); 1.3 → 9.2 MB and 121 → 144 MB (hard flow). A, C and D grow by at most 0.4 MB
  at any size (the largest is C at 10k on the hard flow, 1.03 → 1.43 MB).
- Worst-case latency is too noisy to rank engines overall: the run ranges overlap in most comparisons. The
  largest clear gap is the hard flow at 1M, where B (0.56–0.69 ms) is worse than C and D (under 0.2 ms).
- The trade-history fix removed the vector-doubling spikes and did not change throughput. A's median worst
  case is now about 0.1 ms at 10k–1M, but single runs still reach up to 2.9 ms from causes I did not identify
  (A's 1k median was 0.16 ms in the first session and 0.73 ms in the second).
- Not tried: a cheaper index for D's new orders at 1M (the remaining gap to B on the standard flow), hardware
  cache-miss counters, real exchange data.

Phase 3 baseline (implementation A, i7-14650HX, 1M actions, median of 5 repeats):

| Book size | Actions/sec | p50 | p99 | p99.9 | Worst | Book memory |
|---|---|---|---|---|---|---|
| 1k | 7.13M | 124 ns | 369 ns | 0.9 µs | 0.78 ms | 0.15 MB |
| 10k | 6.72M | 136 ns | 382 ns | 1.4 µs | 1.25 ms | 1.2 MB |
| 100k | 4.88M | 187 ns | 562 ns | 2.7 µs | 0.67 ms | 12.6 MB |
| 1M | 2.56M | 365 ns | 1.07 µs | 4.9 µs | 0.95 ms | 124 MB |

Findings to carry into Phase 4:
- Slowdown with book size comes from cache misses, not Big-O: there are only ~400 price levels at every size.
- At small sizes, memory allocation dominates (one new+delete pair costs ~110 ns here; an add does two allocations).
  **Phase 4 did not support this guess:** swapping in a memory pool had no reliable effect.
- Worst-case spikes (~1 ms) happen when the `trades_` vector doubles and copies itself.
- ~130 bytes per resting order, though `Order` itself is 40 bytes.
- Absolute numbers move with machine state (one earlier session ran ~2.5x slower); compare runs done back to back.

## Design decisions (keep these)
- Prices are **integer cents** (`int64_t`): $101.20 = `10120`. Never use float/double for prices.
- Every engine derives from `OrderBookBase` and must pass the same tests and the differential test.
- The data-structure bullets below describe engine A (the baseline):
- Bids: `std::map<Price, std::list<Order>, std::greater<Price>>` (best = highest = `begin()`).
- Asks: `std::map<Price, std::list<Order>>` (best = lowest = `begin()`).
- Each price level is a `std::list` queue → time priority (oldest fills first).
- `unordered_map<OrderId, Location>` index → O(1) cancel.
- Trades execute at the **resting** order's price.
- Leftover market-order quantity is dropped; leftover limit quantity rests in the book.

## Phases

### Phase 1 — Basic engine ✅
Buy/sell, limit/market orders, cancellation, partial fills, trade execution.

### Phase 2 — Real order-book logic ✅
- [x] Price-time priority (already works — tests 9–12 prove it explicitly)
- [x] Bid/ask spread: `bestBid()`, `bestAsk()`, `spread()`
- [x] Market depth: top-N levels on each side (`depth(side, n)`)
- [x] Reject duplicate order IDs and zero-quantity orders (`validate()`; duplicate = id currently resting)
- [x] Order timestamps (nanosecond, `std::chrono::steady_clock`; stamped by the book in `addOrder`)
- [x] Trade history with timestamps (`tradeHistory()`; a trade's time = the incoming order's arrival time)
- [x] Order modify (cancel + replace; loses time priority) — `modifyOrder(id, newPrice, newQuantity)`

### Phase 3 — Performance ✅
- [x] Random order-flow generator (seeded, reproducible) — `OrderFlow.hpp`, tests 32–34
- [x] Benchmark harness: orders/second, average latency, p50/p99/p99.9/worst-case latency, memory usage — `bench.cpp`
- [x] Measure how performance changes as book size grows (1k → 10k → 100k → 1M resting orders)
- [x] Export results to CSV — `results/summary.csv`, `latency_hist.csv`, `latency_by_type.csv`
- [x] Python script (`analysis/`) to plot results with matplotlib
- [x] Always build benchmarks with `-O2` or `-O3` — the `bench` target uses `-O3 -DNDEBUG`

### Phase 4 — CS investigation
Same interface, three implementations, benchmarked on identical order flow:
- [x] Common interface (`OrderBookBase`), trade-history fix (`reserveTrades`), differential test
- [x] **A — Standard containers**: `std::map` + `std::list` + `std::unordered_map`
- [x] **B — Optimized standard structures**: flat `std::vector` of levels indexed by price, `std::vector` queues
      with lazily removed dead slots, `std::deque` indexed by order id (hash-map fallback for huge ids)
- [x] **C — Custom/low-level**: pre-allocated node pool, intrusive doubly-linked lists, open-addressing hash map,
      32-byte aligned nodes with the timestamp split out. Levels are still a `std::map`.
- [x] **D — C with B's flat price-level array** instead of the `std::map`
- [x] Ablation A → C1 (pool) → C2 (intrusive list) → C3 (hash map) → C4 (layout) → D (flat levels)
- [x] Robustness: A, B, C, D re-run on the hard flow (random ids, wide price band, clustered prices)
- [x] Compare throughput, latency distribution, memory, with medians, spread and a noise rule
- [ ] Write-up: method, results tables/charts, analysis, limitations — **Yash writes this, not Claude**

## Rules
- Every new feature gets tests in `tests.cpp`. All tests must pass before moving on.
- Compile with `-Wall -Wextra` and fix every warning.
- Keep code readable over clever until Phase 4.
- I'm on Windows: build with CMake (commands above), not `make`.
- Git: the repo root is the parent `quant-projects/` folder. Commit after each finished feature.
