// Benchmark for the order-book engines.
//
//   bench.exe <engine> [bookSize] [actions] [run] [outDir] [standard|hard]
//
// Fills a book of the chosen engine with `bookSize` resting orders, then replays `actions`
// generated actions against it and measures throughput, per-action latency and memory.
// It does 2 warm-up replays and then ONE measured run, and appends that run's results to
// <outDir>/summary.csv, latency_hist.csv and latency_by_type.csv. `run` is just the label
// written in the CSV.
//
// One engine, one book size and one measured run per process, on purpose:
//   - memory numbers are only trustworthy in a fresh process, and
//   - analysis/run_bench.py can interleave the engines (A, B, C, A, B, C, ...) so that
//     slow drift in the machine's speed hits every engine equally.

#define NOMINMAX            // stop windows.h defining min/max macros that break std::min/std::max
#include <windows.h>
#include <psapi.h>

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>      // __rdtsc
#endif

#include "Engines.hpp"
#include "OrderFlow.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using BookPtr = std::unique_ptr<OrderBookBase>;

const int kWarmupRuns        = 2;
const int kBucketsPerDecade  = 20;    // latency histogram: 20 buckets for every 10x in latency

// The four kinds of action, for the per-kind latency breakdown.
const int   kKinds = 4;
const char* kKindNames[kKinds] = {"limit", "market", "cancel", "modify"};

int kindOf(const Action& a) {
    if (a.type == ActionType::Cancel) return 2;
    if (a.type == ActionType::Modify) return 3;
    return a.order.type == OrderType::Market ? 1 : 0;
}

// ---------- memory (Windows API) ----------

struct Memory {
    double privateMb;   // memory this process has asked the OS for, right now
    double peakMb;      // most physical RAM the process has used at any moment so far
};

Memory readMemory() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    const double mb = 1024.0 * 1024.0;
    return {pmc.PrivateUsage / mb, pmc.PeakWorkingSetSize / mb};
}

// ---------- timing ----------
//
// One action takes tens of nanoseconds, but steady_clock on Windows only ticks every 100 ns,
// so it can't time a single action. Instead we read the CPU's cycle counter (rdtsc), which
// advances a few times per nanosecond, and work out how many nanoseconds one tick is worth
// by comparing it against steady_clock over a quarter of a second.

double calibrateNsPerTick() {
    auto          t0 = Clock::now();
    std::uint64_t c0 = __rdtsc();
    while (Clock::now() - t0 < std::chrono::milliseconds(250)) {}
    auto          t1 = Clock::now();
    std::uint64_t c1 = __rdtsc();
    double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return ns / static_cast<double>(c1 - c0);
}

// ---------- the measured runs ----------

// Everything one benchmark needs: which engine, and the actions to feed it.
struct Workload {
    std::string         engine;
    std::size_t         bookSize;
    std::vector<Action> prefill, flow;
};

// A fresh book, told how many orders to expect, filled with the starting orders.
// forTiming also makes room for the trade history up front (see OrderBookBase::reserveTrades);
// the memory measurement leaves that out so it counts only the book itself.
BookPtr buildBook(const Workload& w, bool forTiming) {
    BookPtr book = makeBook(w.engine);
    book->reserveOrders(w.bookSize + w.bookSize / 8 + 1024);
    if (forTiming) book->reserveTrades(w.flow.size());
    for (const Action& a : w.prefill) apply(*book, a);
    return book;
}

// Throughput: replay the whole flow with a single stopwatch around it, so the timing code
// doesn't slow the thing being measured. Returns actions per second.
double runThroughput(const Workload& w) {
    BookPtr book = buildBook(w, true);

    auto t0 = Clock::now();
    for (const Action& a : w.flow) apply(*book, a);
    auto t1 = Clock::now();

    return static_cast<double>(w.flow.size()) / std::chrono::duration<double>(t1 - t0).count();
}

// Latency: time every action on its own. ticks[i] = CPU ticks spent on flow[i].
void runLatency(const Workload& w, std::vector<std::uint64_t>& ticks) {
    BookPtr book = buildBook(w, true);

    for (std::size_t i = 0; i < w.flow.size(); ++i) {
        std::uint64_t c0 = __rdtsc();
        apply(*book, w.flow[i]);
        std::uint64_t c1 = __rdtsc();
        ticks[i] = c1 - c0;
    }
}

// ---------- statistics ----------

// The value that `fraction` of the samples are at or below (p99 -> fraction 0.99).
double percentile(const std::vector<std::uint64_t>& sorted, double fraction) {
    std::size_t i = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size()));
    return static_cast<double>(sorted[std::min(i, sorted.size() - 1)]);
}

std::size_t countOrders(const OrderBookBase& book, Side side, std::size_t* levels) {
    std::size_t orders = 0;
    std::vector<DepthLevel> all = book.depth(side, static_cast<std::size_t>(-1));
    for (const DepthLevel& lvl : all) orders += lvl.orderCount;
    *levels += all.size();
    return orders;
}

// A fingerprint of a trade list, so the CSV can show every engine produced the same trades.
std::uint64_t checksum(const std::vector<Trade>& trades) {
    std::uint64_t h = 1469598103934665603ull;
    for (const Trade& t : trades)
        for (std::uint64_t v : {t.buyId, t.sellId, static_cast<std::uint64_t>(t.price), static_cast<std::uint64_t>(t.quantity)})
            h = (h ^ v) * 1099511628211ull;
    return h;
}

// Open a CSV for appending; write the header only if the file is new.
std::ofstream openCsv(const std::filesystem::path& path, const char* header) {
    bool isNew = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
    std::ofstream out(path, std::ios::app);
    if (isNew) out << header << '\n';
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    Workload w;
    w.engine   = argc > 1 ? argv[1] : "A";
    w.bookSize = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1000;
    std::size_t actions = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1000000;
    int         run     = argc > 4 ? std::atoi(argv[4]) : 1;
    std::string outDir  = argc > 5 ? argv[5] : "results";
    std::string flow    = argc > 6 ? argv[6] : "standard";
    if (!makeBook(w.engine) || w.bookSize == 0 || actions == 0 || (flow != "standard" && flow != "hard")) {
        std::printf("usage: bench <engine> [bookSize] [actions] [run] [outDir] [standard|hard]\nengines:");
        for (const std::string& name : engineNames()) std::printf(" %s", name.c_str());
        std::printf("\n");
        return 1;
    }

    // Stay on one CPU core and ask Windows for high priority, so the scheduler interrupts us less.
    SetThreadAffinityMask(GetCurrentThread(), 1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    double nsPerTick = calibrateNsPerTick();

    // ---- generate the order flow (same seed -> same flow, for every engine and every run) ----
    FlowConfig cfg = flow == "hard" ? hardFlow(w.bookSize) : standardFlow(w.bookSize);
    std::size_t finalBookSize = 0;
    {
        OrderFlowGenerator gen(cfg);
        w.prefill = gen.prefill();
        w.flow    = gen.generate(actions);
        finalBookSize = gen.bookSize();
    }

    // kind[i] says which of the four kinds flow[i] is, for the per-kind latency numbers.
    std::vector<int> kind(w.flow.size());
    for (std::size_t i = 0; i < w.flow.size(); ++i) kind[i] = kindOf(w.flow[i]);

    // ---- memory of a full book = how much the process grows while building one ----
    // Windows hands out memory in chunks, so one small book is too small to see.
    // Build enough identical books to hold about 200,000 orders and divide.
    double bookMemoryMb = 0, bookMemoryEndMb = 0;
    {
        std::size_t copies = std::max<std::size_t>(1, 200000 / w.bookSize);
        double      before = readMemory().privateMb;
        std::vector<BookPtr> books;
        for (std::size_t i = 0; i < copies; ++i) books.push_back(buildBook(w, false));
        bookMemoryMb = (readMemory().privateMb - before) / static_cast<double>(copies);

        // ---- memory of one book after the whole flow has run through it ----
        // Shows whether an engine keeps growing while the number of resting orders stays the
        // same. Done while the books above are still alive: nothing has been freed yet, so
        // all growth is new memory. The trade history is reserved before the first reading,
        // which keeps it out of the number. (One book only, so small books are approximate.)
        BookPtr book = makeBook(w.engine);
        book->reserveTrades(w.flow.size());
        before = readMemory().privateMb;
        book->reserveOrders(w.bookSize + w.bookSize / 8 + 1024);
        for (const Action& a : w.prefill) apply(*book, a);
        for (const Action& a : w.flow)    apply(*book, a);
        bookMemoryEndMb = readMemory().privateMb - before;
    }

    // ---- one untimed replay: count trades and price levels, and check the engine's answer ----
    std::size_t   trades = 0, priceLevels = 0;
    std::uint64_t tradeChecksum = 0;
    {
        BookPtr book = buildBook(w, true);
        for (const Action& a : w.flow) apply(*book, a);
        trades        = book->tradeHistory().size();
        tradeChecksum = checksum(book->tradeHistory());
        std::size_t resting = countOrders(*book, Side::Buy, &priceLevels) + countOrders(*book, Side::Sell, &priceLevels);
        if (resting != finalBookSize) {
            std::printf("ERROR: replay ended with %zu orders, generator expected %zu\n", resting, finalBookSize);
            return 1;
        }
    }

    // ---- warm-up: run it for real but throw the numbers away ----
    // The first runs are slower (CPU caches cold, CPU not yet at full speed, memory not yet mapped).
    for (int i = 0; i < kWarmupRuns; ++i) runThroughput(w);

    // ---- the measured run ----
    double rate = runThroughput(w);

    std::vector<std::uint64_t> ticks(w.flow.size());
    runLatency(w, ticks);

    std::vector<std::uint64_t>              histogram(12 * kBucketsPerDecade, 0);
    std::vector<std::vector<std::uint64_t>> ticksByKind(kKinds);
    double sum = 0;
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        double ns = static_cast<double>(ticks[i]) * nsPerTick;
        sum += ns;
        int bucket = ns < 1.0 ? 0 : static_cast<int>(std::log10(ns) * kBucketsPerDecade);
        ++histogram[std::min<std::size_t>(static_cast<std::size_t>(bucket), histogram.size() - 1)];
        ticksByKind[kind[i]].push_back(ticks[i]);
    }
    std::sort(ticks.begin(), ticks.end());

    double avg  = sum / static_cast<double>(ticks.size());
    double p50  = percentile(ticks, 0.50)  * nsPerTick;
    double p99  = percentile(ticks, 0.99)  * nsPerTick;
    double p999 = percentile(ticks, 0.999) * nsPerTick;
    double max  = static_cast<double>(ticks.back()) * nsPerTick;
    double peak = readMemory().peakMb;

    std::printf("%-8s %-3s book %8zu run %d: %10.0f actions/s  avg %6.1f  p50 %6.1f  p99 %7.1f  p99.9 %8.1f  worst %9.0f ns  book %8.3f MB, %8.3f MB after the flow\n",
                flow.c_str(), w.engine.c_str(), w.bookSize, run, rate, avg, p50, p99, p999, max, bookMemoryMb, bookMemoryEndMb);

    // ---- write the CSVs ----
    std::filesystem::create_directories(outDir);
    std::filesystem::path dir(outDir);

    std::ofstream summary = openCsv(dir / "summary.csv",
        "flow,engine,book_size,run,actions,orders_per_sec,avg_ns,p50_ns,p99_ns,p999_ns,max_ns,"
        "book_memory_mb,book_memory_end_mb,peak_memory_mb,final_book_size,price_levels,trades,trade_checksum");
    summary << flow << ',' << w.engine << ',' << w.bookSize << ',' << run << ',' << actions << ',' << static_cast<std::uint64_t>(rate) << ','
            << avg << ',' << p50 << ',' << p99 << ',' << p999 << ',' << max << ','
            << bookMemoryMb << ',' << bookMemoryEndMb << ',' << peak << ',' << finalBookSize << ',' << priceLevels << ',' << trades << ','
            << tradeChecksum << '\n';

    std::ofstream byKind = openCsv(dir / "latency_by_type.csv",
                                   "flow,engine,book_size,run,action,count,avg_ns,p50_ns,p99_ns");
    for (int k = 0; k < kKinds; ++k) {
        std::vector<std::uint64_t>& t = ticksByKind[k];
        if (t.empty()) continue;
        std::sort(t.begin(), t.end());
        double kindSum = 0;
        for (std::uint64_t x : t) kindSum += static_cast<double>(x);
        byKind << flow << ',' << w.engine << ',' << w.bookSize << ',' << run << ',' << kKindNames[k] << ',' << t.size() << ','
               << kindSum * nsPerTick / static_cast<double>(t.size()) << ','
               << percentile(t, 0.50) * nsPerTick << ',' << percentile(t, 0.99) * nsPerTick << '\n';
    }

    std::ofstream hist = openCsv(dir / "latency_hist.csv",
                                 "flow,engine,book_size,run,bucket_lo_ns,bucket_hi_ns,count");
    for (std::size_t b = 0; b < histogram.size(); ++b) {
        if (histogram[b] == 0) continue;
        double lo = std::pow(10.0, static_cast<double>(b) / kBucketsPerDecade);
        double hi = std::pow(10.0, static_cast<double>(b + 1) / kBucketsPerDecade);
        hist << flow << ',' << w.engine << ',' << w.bookSize << ',' << run << ',' << lo << ',' << hi << ',' << histogram[b] << '\n';
    }
    return 0;
}
