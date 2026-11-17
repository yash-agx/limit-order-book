# Limit Order Book & Matching Engine

**[Read the full report (PDF)](report.pdf)**

A limit-order matching engine in C++17, implemented four times with different data structures
behind one interface, and benchmarked to answer one question:

**How does data-structure choice affect the latency and throughput of a limit-order matching engine?**

It supports limit and market orders, cancel, modify (cancel + replace), partial fills, price-time
priority, best bid/ask and spread, market depth, and a timestamped trade history. Prices are integer
cents. Python is used only to run the benchmarks, preprocess market data and draw the charts.

The engines are benchmarked on two synthetic order flows and on a real Nasdaq trading day replayed from
TotalView-ITCH 5.0 data - with the important caveat, explained in
[Real exchange data (ITCH)](#real-exchange-data-itch), that ITCH is a feed of book events the exchange
has already matched, so it tests real order-book *update* flow rather than real matching.

## The four engines

All four derive from `OrderBookBase`, pass the same tests, and produce identical trades.

| Engine | Price levels | Queue at each price | Order-id index |
|---|---|---|---|
| **A** – baseline | `std::map` | `std::list` | `std::unordered_map` |
| **B** – flat arrays | `std::vector` indexed by price | `std::vector`, cancelled slots removed in batches | `std::deque` indexed by the id (hash-map fallback for large ids) |
| **C** – custom | `std::map` (pool-allocated) | intrusive doubly-linked list in a pre-allocated node pool, 32-byte nodes | open-addressing hash map |
| **D** – C with flat levels | array indexed by price, as in B | as C | as C |

Four intermediate versions (C1–C4) add C's changes to A one at a time, for the ablation below.

## Build and run

Requires CMake 3.15+ and a C++17 compiler.

**Windows** (tested with WinLibs MinGW-w64 GCC 16.2; `g++` and `cmake` on PATH):

```
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
.\build\tests.exe
.\build\demo.exe
```

**Linux** (GCC 9+ or Clang with a C++17 standard library that has `<memory_resource>`):

```
cmake -S . -B build
cmake --build build
./build/tests
./build/demo
```

The engines, tests and demo are standard C++17. They have only been built and run on Windows so far.
The benchmark is Windows-only (it uses the Windows API to measure memory) and CMake skips it elsewhere.

### Benchmark (Windows)

```
pip install matplotlib
python analysis\run_bench.py       # 220 runs, about 20 minutes; rewrites results\*.csv
python analysis\plot_results.py    # prints the tables, writes results\*.png
```

A single run: `.\build\bench.exe <engine> [bookSize] [actions] [run] [outDir] [standard|hard]`,
where `<engine>` is one of `A B C1 C2 C3 C4 D`.

To replay real market data instead, see [Real exchange data (ITCH)](#real-exchange-data-itch).

## Project structure

```
Order.hpp               Order, Trade, Side, OrderType
OrderBookBase.hpp/.cpp  interface for every engine + rules shared by all (validation, timestamps, trade history, modify)
OrderBook.hpp/.cpp      engine A, and C1 (A + memory pool)
FlatBook.hpp/.cpp       engine B
IntrusiveBook.hpp       engines C2, C3, C (= C4) and D: one template with pluggable index, pool and price levels
Engines.hpp/.cpp        makeBook("A" | "B" | "C1" | "C2" | "C3" | "C4" | "D")
OrderFlow.hpp/.cpp      seeded order-flow generator (standard and hard flows)
ItchReplay.hpp/.cpp     reader for the real-market action files produced from Nasdaq ITCH data
tests.cpp               40 tests per engine + a differential test + the real-data replay test
bench.cpp               benchmark executable (-O3): synthetic flows and ITCH replay
main.cpp                small demo
analysis/               run_bench.py, plot_results.py                                  (synthetic)
                        itch_download.py, itch_extract.py, run_itch.py, plot_itch.py   (real data)
results/                benchmark CSVs and charts
results_itch/           real-data CSVs and charts
results_phase3/         earlier baseline-only results
data/                   the ITCH download and the actions extracted from it (gitignored, 3.5 GB)
```

## Tests

`tests` runs the same 40 assert-based tests against each of the 7 engine variants. A differential test
then replays six seeded order flows through every engine and checks that each produces the same trades
and the same final book as engine A. Finally, if the real-market action files are present (see
[Real exchange data (ITCH)](#real-exchange-data-itch)), the same check runs on a real Nasdaq trading
day, and also asserts that no engine ever leaves the book crossed. Without that data the last test
reports that it was skipped and the rest still run.

## Benchmark method

- **Machine:** Intel Core i7-14650HX, Windows 11, GCC 16.2, `-O3 -DNDEBUG`, single thread pinned to one core.
- **Workload:** the book is filled to N resting orders (1k, 10k, 100k, 1M), then 1,000,000 generated
  actions are replayed: about 48% limit orders, 5% market orders, 37% cancels, 10% modifies. The generator
  keeps the book at N. The same seed gives the same actions for every engine.
- **Standard flow:** sequential order ids, prices within $2 of the mid (300–400 price levels in use).
- **Hard flow:** random 64-bit order ids, prices up to $50 from the mid (up to about 10,000 levels in
  use), 95% of passive orders within 20 cents of the mid.
- **Runs:** each measurement is a fresh process with 2 warm-up replays and 1 measured run; 5 runs per
  engine per size, with the engines interleaved.
- **Throughput** is timed over the whole replay. **Latency** is timed per action with the CPU cycle
  counter in a separate replay.
- **Noise rule:** two results are treated as different only if their run ranges (slowest to fastest
  run) do not overlap.

## Results

Median of 5 runs. Throughput is in million actions per second, with the slowest–fastest run in brackets.

### Throughput – standard flow

| Resting orders | A | B | C | D |
|---|---|---|---|---|
| 1k | 7.45 (6.88–8.00) | 18.50 (14.47–19.13) | 10.61 (10.24–13.52) | 25.48 (22.33–26.77) |
| 10k | 6.77 (6.42–6.95) | 17.51 (17.09–19.63) | 13.47 (12.33–13.99) | 24.02 (14.98–24.70) |
| 100k | 4.56 (3.84–4.87) | 14.27 (9.28–15.89) | 11.19 (9.65–12.06) | 18.06 (14.08–18.87) |
| 1M | 2.44 (2.38–2.46) | 8.77 (8.40–8.91) | 5.78 (5.43–5.87) | 6.84 (6.72–6.90) |

Ranking by throughput: D > B > C > A at 1k; D ~ B > C > A at 10k; D ~ B ~ C > A at 100k (the run
ranges overlap, so B and C cannot be separated there); B > D > C > A at 1M.

### Throughput – hard flow

| Resting orders | A | B | C | D |
|---|---|---|---|---|
| 1k | 8.46 (8.04–8.59) | 11.51 (11.21–11.66) | 16.61 (16.41–16.91) | 24.93 (24.55–25.37) |
| 10k | 7.47 (7.29–7.54) | 9.08 (8.88–9.27) | 13.44 (12.83–13.53) | 20.44 (20.35–20.73) |
| 100k | 3.86 (3.70–4.32) | 6.04 (5.92–6.53) | 10.56 (9.92–11.00) | 15.59 (13.87–15.83) |
| 1M | 1.89 (1.77–1.93) | 2.58 (2.32–2.60) | 5.78 (5.50–5.91) | 6.84 (5.64–6.99) |

Ranking by throughput: D > C > B > A at 1k, 10k and 100k; D ~ C > B > A at 1M. B's lead over A falls
from x2.5–x3.6 on the standard flow to x1.2–x1.6 here, so most of it depended on sequential order ids
and a narrow price band. D never gives up more than 15% of its standard-flow throughput.

![Throughput by engine on both order flows](results/throughput_vs_book_size.png)

### Latency per action (p50 / p99 / p99.9, nanoseconds)

Standard flow:

| Resting orders | A | B | C | D |
|---|---|---|---|---|
| 1k | 118 / 350 / 1129 | 42 / 207 / 443 | 66 / 261 / 477 | 37 / 177 / 297 |
| 10k | 138 / 396 / 2111 | 40 / 219 / 656 | 67 / 250 / 450 | 42 / 197 / 314 |
| 100k | 202 / 583 / 4205 | 43 / 264 / 4217 | 83 / 281 / 588 | 55 / 223 / 355 |
| 1M | 397 / 1125 / 5398 | 43 / 412 / 2688 | 186 / 514 / 3995 | 130 / 476 / 957 |

Hard flow:

| Resting orders | A | B | C | D |
|---|---|---|---|---|
| 1k | 110 / 320 / 569 | 80 / 341 / 1066 | 54 / 207 / 330 | 40 / 173 / 265 |
| 10k | 126 / 356 / 1535 | 94 / 354 / 5318 | 65 / 254 / 466 | 45 / 206 / 337 |
| 100k | 224 / 649 / 4532 | 139 / 521 / 3779 | 86 / 324 / 920 | 62 / 250 / 468 |
| 1M | 510 / 1413 / 4409 | 374 / 1289 / 3572 | 184 / 565 / 3719 | 137 / 519 / 800 |

D has the best or joint-best p99.9 at every size on both flows. B's is the worst of the four at 100k
on the standard flow (4217 ns, no better than A's) and at 10k on the hard flow (5318 ns, worse than
A's): its lazily-removed dead slots have to be cleaned up eventually, and the clean-up all lands on
one unlucky action.

Each timing includes about 6 ns of measurement overhead.

![Latency percentiles, standard flow](results/latency_percentiles_standard.png)

![Latency percentiles, hard flow](results/latency_percentiles_hard.png)

### Ablation – standard flow

Throughput after adding each change to the one before. The bracket is the change from the previous
step; "noise" means the run ranges overlap.

| Resting orders | A | + pool | + intrusive list | + hash map | + layout (= C) | + flat levels (= D) |
|---|---|---|---|---|---|---|
| 1k | 7.45 | 6.20 (noise) | 7.69 (noise) | 12.84 (+67%) | 10.61 (noise) | 25.48 (+140%) |
| 10k | 6.77 | 6.07 (−10%) | 6.99 (+15%) | 13.25 (+90%) | 13.47 (noise) | 24.02 (+78%) |
| 100k | 4.56 | 3.79 (noise) | 5.56 (+47%) | 11.14 (+100%) | 11.19 (noise) | 18.06 (+61%) |
| 1M | 2.44 | 2.72 (+12%) | 3.29 (+21%) | 5.57 (+70%) | 5.78 (noise) | 6.84 (+18%) |

The memory pool — the change I expected to matter most after Phase 3 — is the only step that never
clearly helps: noise at 1k and 100k, −10% at 10k, +12% at 1M. Replacing the hash map is the biggest
single win, and replacing `std::map` with a flat array of price levels is bigger still.

![Ablation from A to C to D](results/ablation_throughput.png)

### Book memory

MB used by the book right after filling it → after 1,000,000 more actions (the number of resting
orders is the same at both points). Measured as process growth through the Windows API; values for
the 1k book are approximate.

| Resting orders | Flow | A | B | C | D |
|---|---|---|---|---|---|
| 1k | standard | 0.15 → 0.00 | 0.27 → 14.21 | 0.24 → 0.21 | 0.25 → 0.25 |
| 10k | standard | 1.19 → 1.23 | 0.96 → 17.23 | 0.93 → 1.17 | 0.95 → 1.00 |
| 100k | standard | 11.47 → 12.16 | 8.06 → 28.71 | 7.98 → 8.39 | 8.46 → 8.43 |
| 1M | standard | 122.48 → 121.09 | 89.66 → 120.48 | 107.19 → 107.20 | 107.23 → 107.24 |
| 1k | hard | 0.14 → 0.23 | 1.33 → 1.59 | 0.22 → 0.21 | 0.36 → 0.35 |
| 10k | hard | 1.27 → 1.10 | 2.34 → 4.25 | 0.95 → 1.43 | 1.09 → 1.05 |
| 100k | hard | 12.34 → 12.64 | 13.90 → 21.17 | 8.89 → 9.14 | 8.63 → 8.60 |
| 1M | hard | 123.16 → 121.81 | 121.00 → 144.32 | 108.22 → 108.12 | 107.32 → 107.50 |

A, C and D stay flat, as they should: the book holds the same number of orders at both points. B is
the exception, and the next section explains why.

![Book memory after the flow](results/memory_after_flow.png)

More output is in `results/`: the per-run CSVs, latency by action type, and latency distribution charts
at 1M orders for both flows. `python analysis\plot_results.py` reprints every table.

### Why engine B's memory spikes

B is the only engine whose memory grows while the number of resting orders stays the same
(0.27 → 14.21 MB at a 1k book on the standard flow). Both of B's "index by the number itself instead
of hashing" tricks contribute, and only one of them was intended.

**1. The order-id array (a real design cost).** `direct_` is a `std::deque<Slot>` indexed by order
id, so finding an order is one subtraction and one load instead of a hash. It therefore has to be as
long as the *largest id ever seen*, not as long as the book. The standard flow issues sequential ids,
so 1,000,000 actions use about 527,000 ids: 527,000 × 24 bytes = **12.1 MB**, whatever the book size.
That is why the growth is almost the same at 1k and at 10k. Nothing can be freed — an array indexed
by id cannot have holes punched out of it — so this is the price of the design, not a bug. (The hard
flow uses random 64-bit ids, which skip `direct_` entirely and go to the `overflow_` hash map, so it
pays nothing here.)

**2. Price levels keeping buffers they no longer need (a bug, now fixed).** Each price level is a
`std::vector<Order>`, and `tidy()` used to empty a level with `clear()`, which keeps the allocated
buffer "ready for the next orders". That is right for the handful of levels at the best price, which
empty and refill constantly, but wrong for the book as a whole: over a long run the book touches far
more prices than it ever holds orders at, and each one of those kept a buffer sized to its own peak —
including the slack of up to `kSlack` (32) dead slots per level that the lazy-deletion scheme leaves
behind. After 1M actions on the hard flow, 9,832 *empty* levels were still holding 199,383 order
slots (**7.6 MB**) for 999 live orders.

The fix is one branch in `tidy()`: when a level empties, release the buffer
(`std::vector<Order>().swap(level.orders)`) unless it is already small (≤ `kKeepSlots` = 8 slots),
which keeps the hot levels allocation-free. Slots held by empty levels at a 1k book:

| Flow | Before | After |
|---|---|---|
| standard | 41,778 slots (1.6 MB) | 4,239 slots (0.16 MB) |
| hard | 199,383 slots (7.6 MB) | 11,615 slots (0.44 MB) |

Memory after the flow, old and new binaries run back to back (3 runs each):

| Resting orders | Flow | Before | After |
|---|---|---|---|
| 1k | standard | 15.80 MB | 14.20 MB |
| 10k | standard | 18.02 MB | 17.22 MB |
| 100k | standard | 30.02 MB | 28.75 MB |
| 1M | standard | 124.1 MB | 121.6 MB |
| 1k | hard | 9.21 MB | 1.50 MB |
| 10k | hard | 8.56 MB | 4.25 MB |
| 100k | hard | 24.30 MB | 21.20 MB |
| 1M | hard | 144.1 MB | 144.2 MB |

The hard flow at 1k improves by 84%; the standard flow barely moves, because there the 12.1 MB id
array dominates and that part is inherent. At a 1M book nothing changes, because then the book really
is holding a million orders. Throughput is unchanged: in interleaved runs every comparison has
overlapping run ranges (the noise rule), the largest median gap being −8% on the standard flow at 1k,
where levels empty and refill most often. Every table above was re-measured with the fix in place.


## Real exchange data (ITCH)

The synthetic flows above are my own invention, so the obvious objection is that they flatter whichever
engine happens to suit them. This phase replays a real trading day through the same four engines:
**30 December 2019 on Nasdaq, from the free TotalView-ITCH 5.0 sample file** (3.5 GB gzipped,
266 million messages), for five liquid symbols.

### What this tests, and what it does not

**ITCH is a feed of book events Nasdaq has already matched, not the raw order flow that arrived at the
exchange.** Every message says what *happened* to the order book — an order was added, cancelled,
deleted, replaced or executed. During continuous trading an Add Order message therefore never crosses
the book: had the incoming order been executable, Nasdaq would have matched it and published execution
messages instead.

So this measures the engines on **real order-book update flow** — real order-id patterns, real price
clustering, real book sizes, a real mix of adds, cancels and executions — and **not on real matching**.
The matching path is barely touched. Nothing here says how well an engine matches.

That claim is measured, not assumed: across all 4.4 million real actions, every engine produced
**zero trades** and the book was **never crossed, not once**. See the correctness table below.

### The data

`analysis/itch_extract.py` streams the gzipped file (it is never loaded — ~8.3 GB raw) and converts one
symbol's messages into the engine's existing actions:

| ITCH message | Engine action |
|---|---|
| `A`, `F` — add order (without / with attribution) | add a limit order |
| `D` — order delete (whole order gone) | cancel |
| `X` — order cancel (part of an order gone) | modify to the smaller quantity |
| `E`, `C` — order executed (with / without price) | modify to the smaller quantity, or cancel if nothing is left |
| `U` — order replace (new price, size and id) | modify to the new price and quantity |

Measured window: **09:30–16:00**, continuous trading only. Before the opening cross the displayed book
is legitimately *crossed* — buy and sell orders for the 09:30 auction pile up with bids above asks and
the exchange holds them rather than matching them — so replaying the pre-open would have the engines
trade orders Nasdaq was still holding. The extractor still tracks the book from 04:00, so it knows
exactly what was resting at 09:30 and writes that out as a **prefill** block, which the benchmark
applies untimed before measuring, exactly as it does with the synthetic flows.

| Symbol | ITCH messages = actions | adds | cancels | modifies | 09:30 book | peak book | price span touched |
|---|---|---|---|---|---|---|---|
| AAPL | 1,477,252 | 694,517 | 668,258 | 114,477 | 828 | 27,102 | $0.01 – $199,999.00 |
| MSFT | 1,188,106 | 573,145 | 551,280 | 63,681 | 1,075 | 22,996 | $0.01 – $199,999.99 |
| INTC | 740,571 | 351,413 | 343,027 | 46,131 | 136 | 8,605 | $0.01 – $199,999.99 |
| CSCO | 529,009 | 253,867 | 240,855 | 34,287 | 259 | 13,405 | $0.01 – $199,999.99 |
| AMZN | 477,185 | 227,054 | 191,717 | 58,414 | 1,724 | 37,063 | $0.01 – $199,999.00 |

4,412,123 actions in total. One ITCH message becomes exactly one engine action, so the two counts are
the same number. Of 266 million messages in the file, only 40 carried a price that was not a whole
cent (rounded, and counted), and 29 adds had no usable price and were skipped.

Three honest caveats about the mapping:

1. The engine's `modifyOrder` is cancel + re-add, so it sends the order to the **back** of its price
   level. For `U` (replace) that is exactly right — a real replace loses time priority too. For `X` and
   `E` it is not: shrinking an order, or partially filling it, keeps its place in the real queue. The
   engines have no "reduce in place" call and this phase does not change them. The set of resting
   orders and their quantities stays correct; only the order *within* one price level differs.
2. `U` gives the order a brand-new ITCH reference number. Rather than emit a cancel and an add, the
   extractor keeps using the original id inside the engine and remembers that later messages about the
   new reference mean that same order — so one message stays one action and the book is identical.
3. `E`, `C` and `X` carry a *delta* (shares executed or cancelled), so the extractor keeps its own copy
   of each tracked order's resting quantity to turn deltas into the absolute quantity `modifyOrder` wants.

### Getting the data and running it

The data is deliberately not in git (`data/` is gitignored — it is 3.5 GB).

```
python analysis\itch_download.py     # 3.5 GB; parallel ranged requests, resumable
python analysis\itch_extract.py      # streams it, writes data\itch\<SYMBOL>.actions
python analysis\run_itch.py          # 100 runs (5 symbols x 4 engines x 5 runs)
python analysis\plot_itch.py         # prints the tables, writes results_itch\*.png
```

A single replay: `.\build\bench.exe replay <engine> <actionFile> [run] [outDir] [maxActions]`.
Measurement is identical to the synthetic benchmark — same warm-ups, same single measured run per
process, same 5 interleaved runs, same percentiles, same noise rule — so the two are directly
comparable. **Engine B needs about 1.9 GB per process on real prices** (see below); D needs about
0.5 GB.

### Correctness

`tests.exe` replays the real data through **all seven** engine variants and checks that they finish
identical — same best bid and ask, same depth at every price, same order count, same quantity on every
individual order id, same trades — and that no engine ever leaves the book crossed.

| Symbol | Actions replayed | Resting at the end | Engines agreeing | Trades from the prefill | Trades during the replay | Times crossed |
|---|---|---|---|---|---|---|
| AAPL | 1,000,000 | 22,019 | 7 / 7 | 0 | 0 | 0 |
| MSFT | 1,000,000 | 19,642 | 7 / 7 | 0 | 0 | 0 |
| INTC | 740,571 | 8,522 | 7 / 7 | 0 | 0 | 0 |
| CSCO | 529,009 | 13,271 | 7 / 7 | 0 | 0 | 0 |
| AMZN | 477,185 | 37,061 | 7 / 7 | 0 | 0 | 0 |

(The test caps a symbol at 1,000,000 actions to stay quick; the benchmark replays the whole day.)
The zeros in the last three columns are the honesty point, measured: real ITCH adds never cross.

### Results

Run `python analysis\run_itch.py && python analysis\plot_itch.py` to produce the throughput and
latency tables and the charts in `results_itch/`. The timing numbers in this section are **from single
exploratory replays of the full AAPL day, not the 5-run medians**, and are here only to show the shape
of the result; the committed tables come from the full session.

Book memory, however, is deterministic, and it is where real data changes the answer. Full AAPL day,
one book, measured after the 09:30 prefill and again after the whole day:

| Engine | After the prefill | After the full day | vs engine C |
|---|---|---|---|
| **A** – `std::map` + `std::list` | 0.39 MB | 3.87 MB | x1.4 |
| **B** – flat arrays | 1,939.9 MB | 1,940.7 MB | **x688** |
| **C** – custom structures | 2.25 MB | 2.82 MB | – |
| **D** – C + flat price levels | 231.7 MB | 460.7 MB | **x163** |

Provisional single-run throughput on the full AAPL day: A 7.9M actions/s (p50 122 ns), B 14.1M
(p50 69 ns), C 11.9M (p50 72 ns), D 14.3M (p50 43 ns). On the synthetic flows at a comparable book
size the ranking was D > B > C > A (standard) and D > C > B > A (hard); on real data C and D stay ahead
of A by a similar margin, but **D pays a 44 ms worst-case action** and B pays 1.9 GB — neither of which
the synthetic flows ever exposed.

### Why real prices cost the flat-array engines so much

This is the one place where real data changes the ranking rather than just the numbers, and the cause
is a handful of orders.

B and D store price levels in a `std::vector` **indexed by the price itself**: `levels[price - base]`.
That is one subtraction instead of a walk down a `std::map`, and it is why they win on the synthetic
flows, where every price sits within $2 (standard) or $50 (hard) of the mid. The array has to span
every price the book ever touches.

Real books touch absurd prices. Of AAPL's 809,822 priced records, **72 — 0.009% — lie outside $1 to
$1,000**: a few orders at $0.01 and a few at $199,999.00. (These are real displayed orders; extreme
limit prices are a normal way to express "fill me at any price".) Those 72 orders force the array to
span **20 million price levels** instead of the 5,308 actually in use at the end of the day:

- **B**'s level is 48 bytes — it holds a whole `std::vector<Order>` plus a head, a live count and a
  sequence number. 20M levels x 48 bytes x 2 sides = **1.92 GB**, which matches the 1,939.9 MB measured.
  With my first benchmark harness, which built several hundred identical books to measure the memory of
  a small one, B simply died with `std::bad_alloc`; the replay now measures a single book.
- **D**'s level is 12 bytes — just the two ends of an intrusive linked list and a count, because the
  orders themselves live in a shared node pool. 20M x 12 x 2 = **0.48 GB**, matching the 460.7 MB
  measured after the full day.
- **C** keeps its levels in a `std::map`, so it only pays for the 5,308 prices actually in use: 2.8 MB.

Growing the array also costs time, not just space. When an order at $0.01 arrives while the array is
based around $290, the vector has to extend *downwards*, which moves every existing element: D's worst
single action on the full AAPL day was **44 ms**, against 102 µs for C. The synthetic hard flow, with
its 10,000-level band, never came close to triggering this.

So the honest conclusion from real data is narrower than the synthetic one: indexing price levels by
price is the single biggest win available (D has the best typical latency on both synthetic and real
flow), but it is only safe if the price range is bounded. A production engine would need to clamp or
hash the far-away prices — at which point it is no longer a plain array. This is exactly the kind of
weakness a self-made order flow cannot reveal, because I chose its price range myself.


## Notes

Built with AI coding assistance. The design decisions and analysis are my own.
