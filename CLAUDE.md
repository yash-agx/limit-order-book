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
**Phases 1, 2 and 3 are DONE** and tested (34 passing tests in `tests.cpp`). Next: Phase 4.

Files:
- `Order.hpp` — `Order`, `Trade`, `Side`, `OrderType` types
- `OrderBook.hpp / OrderBook.cpp` — the engine
- `OrderFlow.hpp / OrderFlow.cpp` — seeded order-flow generator (`OrderFlowGenerator`, `Action`, `apply`)
- `main.cpp` — demo using the example book from the spec
- `tests.cpp` — assert-based tests
- `bench.cpp` — benchmark (`bench.exe [bookSize] [actions] [repeats] [outDir]`), built with `-O3 -DNDEBUG`
- `analysis/run_bench.py` — runs `bench.exe` for 1k/10k/100k/1M (fresh process each), rewrites `results/*.csv`
- `analysis/plot_results.py` — prints the summary table, writes charts to `results/*.png`
- `results/` — benchmark CSVs and charts (committed, so the write-up can cite them)
- `CMakeLists.txt` — build with CMake (Windows, WinLibs g++), from this folder:
  ```
  cmake -S . -B build -G "MinGW Makefiles"
  cmake --build build
  .\build\tests.exe
  .\build\demo.exe
  python analysis\run_bench.py
  python analysis\plot_results.py
  ```

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
- [ ] **A — Standard containers** (current: `std::map` + `std::list`)
- [ ] **B — Optimized data structures** (e.g. sorted `std::vector` of levels, `std::deque`, flat arrays indexed by price tick)
- [ ] **C — Custom/low-level** (memory pool / pre-allocated orders, intrusive linked lists, cache-friendly layout)
- [ ] Compare throughput, latency distribution, memory; explain *why* each wins or loses (cache misses, allocations, Big-O)
- [ ] Write-up: method, results tables/charts, analysis, limitations

## Rules
- Every new feature gets tests in `tests.cpp`. All tests must pass before moving on.
- Compile with `-Wall -Wextra` and fix every warning.
- Keep code readable over clever until Phase 4.
- I'm on Windows: build with CMake (commands above), not `make`.
- Git: the repo root is the parent `quant-projects/` folder. Commit after each finished feature.
