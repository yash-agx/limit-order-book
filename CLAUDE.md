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
39 tests per engine × 6 engines, plus a differential test, all passing.

Files:
- `Order.hpp` — `Order`, `Trade`, `Side`, `OrderType` types
- `OrderBookBase.hpp / .cpp` — the interface every engine implements, plus the rules shared by all
  (validate, timestamps, trade history, `reserveTrades`, modify, print)
- `OrderBook.hpp / .cpp` — engine **A** (`OrderBook`) and ablation step **C1** (`PoolBook` = A + memory pool)
- `FlatBook.hpp / .cpp` — engine **B**
- `IntrusiveBook.hpp` — engine **C** as a template: `IntrusiveBookC2`, `IntrusiveBookC3`, `CustomBook` (= C4 = C)
- `Engines.hpp / .cpp` — `makeBook("A" | "B" | "C1" | "C2" | "C3" | "C4")`
- `OrderFlow.hpp / OrderFlow.cpp` — seeded order-flow generator (`OrderFlowGenerator`, `Action`, `apply`)
- `main.cpp` — demo using the example book from the spec
- `tests.cpp` — assert-based tests (a template run once per engine) + differential test
- `bench.cpp` — benchmark (`bench.exe <engine> [bookSize] [actions] [run] [outDir]`), built with `-O3 -DNDEBUG`;
  2 warm-ups + 1 measured run per process
- `analysis/run_bench.py` — runs every engine × 1k/10k/100k/1M × 5 runs, interleaved, fresh process each; rewrites `results/*.csv`
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

Phase 4 results (i7-14650HX, 1M actions, median of 5 interleaved runs; million actions/sec, p50, p99.9, book memory):

| Book | A | B | C |
|---|---|---|---|
| 1k | 8.14M, 113 ns, 754 ns, 0.15 MB | 19.97M, 41 ns, 402 ns, 0.27 MB | 13.90M, 65 ns, 379 ns, 0.24 MB |
| 10k | 7.28M, 128 ns, 1.3 µs, 1.19 MB | 19.26M, 40 ns, 680 ns, 0.97 MB | 13.81M, 67 ns, 408 ns, 0.95 MB |
| 100k | 4.72M, 199 ns, 2.7 µs, 12.2 MB | 15.02M, 42 ns, 3.3 µs, 8.9 MB | 11.28M, 84 ns, 494 ns, 8.5 MB |
| 1M | 2.53M, 389 ns, 5.2 µs, 122 MB | 8.46M, 43 ns, 2.7 µs, 90 MB | 5.95M, 186 ns, 1.0 µs, 107 MB |

Phase 4 findings (full tables: run `python analysis\plot_results.py`):
- B is fastest on throughput and typical latency at every size (2.5–3.4x A). C is 1.7–2.4x A.
- C has the best median p99.9 from 10k up, but it only clears the run spread against B at 10k and 100k
  (at 1k and 1M the runs overlap). B's p99.9 is no better than A's at 100k. Worst-case latency: no
  engine differs from another beyond noise.
- Ablation A → C: pool = within noise at every size; intrusive list +11–32%; open-addressing hash map
  +47–81% (the biggest step); compact layout = within noise except +7% at 1M.
- Noise rule: two results differ only if their min–max run ranges don't overlap.
- The trade-history fix cut A's worst case from ~1 ms to ~0.1–0.2 ms without changing throughput.
- Not tried: C with B's flat price array instead of `std::map`.

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
- [x] Ablation A → C1 (pool) → C2 (intrusive list) → C3 (hash map) → C4 (layout)
- [x] Compare throughput, latency distribution, memory, with medians, spread and a noise rule
- [ ] Write-up: method, results tables/charts, analysis, limitations — **Yash writes this, not Claude**

## Rules
- Every new feature gets tests in `tests.cpp`. All tests must pass before moving on.
- Compile with `-Wall -Wextra` and fix every warning.
- Keep code readable over clever until Phase 4.
- I'm on Windows: build with CMake (commands above), not `make`.
- Git: the repo root is the parent `quant-projects/` folder. Commit after each finished feature.
