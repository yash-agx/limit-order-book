// Benchmark for the order book.
//
//   bench.exe [bookSize] [actions] [repeats] [outDir]
//
// Fills a book with `bookSize` resting orders, then replays `actions` generated actions
// against it and measures throughput, per-action latency and memory. Results are appended
// to <outDir>/summary.csv and <outDir>/latency_hist.csv.
//
// One book size per run of the program, on purpose: memory numbers are only trustworthy
// in a fresh process. analysis/run_bench.py runs it once for each size.

#define NOMINMAX            // stop windows.h defining min/max macros that break std::min/std::max
#include <windows.h>
#include <psapi.h>

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>      // __rdtsc
#endif

#include "OrderBook.hpp"
#include "OrderFlow.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

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

// What the stopwatch itself costs: time "nothing" many times and take the median.
double timerOverheadNs(double nsPerTick) {
    std::vector<std::uint64_t> d(100001);
    for (std::uint64_t& x : d) {
        std::uint64_t c0 = __rdtsc();
        std::uint64_t c1 = __rdtsc();
        x = c1 - c0;
    }
    std::sort(d.begin(), d.end());
    return static_cast<double>(d[d.size() / 2]) * nsPerTick;
}

// ---------- the measured runs ----------

void fill(OrderBook& book, const std::vector<Action>& prefill) {
    for (const Action& a : prefill) apply(book, a);
}

// Throughput: replay the whole flow with a single stopwatch around it, so the timing code
// doesn't slow the thing being measured. Returns actions per second.
double runThroughput(const std::vector<Action>& prefill, const std::vector<Action>& flow) {
    OrderBook book;
    fill(book, prefill);

    auto t0 = Clock::now();
    for (const Action& a : flow) apply(book, a);
    auto t1 = Clock::now();

    return static_cast<double>(flow.size()) / std::chrono::duration<double>(t1 - t0).count();
}

// Latency: time every action on its own. ticks[i] = CPU ticks spent on flow[i].
void runLatency(const std::vector<Action>& prefill, const std::vector<Action>& flow,
                std::vector<std::uint64_t>& ticks) {
    OrderBook book;
    fill(book, prefill);

    for (std::size_t i = 0; i < flow.size(); ++i) {
        std::uint64_t c0 = __rdtsc();
        apply(book, flow[i]);
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

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::size_t countOrders(const OrderBook& book, Side side, std::size_t* levels) {
    std::size_t orders = 0;
    std::vector<DepthLevel> all = book.depth(side, static_cast<std::size_t>(-1));
    for (const DepthLevel& lvl : all) orders += lvl.orderCount;
    *levels += all.size();
    return orders;
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
    std::size_t bookSize = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000;
    std::size_t actions  = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1000000;
    int         repeats  = argc > 3 ? std::atoi(argv[3]) : 5;
    std::string outDir   = argc > 4 ? argv[4] : "results";
    if (bookSize == 0 || actions == 0 || repeats <= 0) {
        std::printf("usage: bench [bookSize] [actions] [repeats] [outDir]\n");
        return 1;
    }

    // Stay on one CPU core and ask Windows for high priority, so the scheduler interrupts us less.
    SetThreadAffinityMask(GetCurrentThread(), 1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    double nsPerTick = calibrateNsPerTick();
    std::printf("book size %zu, %zu actions, %d repeats (+%d warm-up)\n", bookSize, actions, repeats, kWarmupRuns);
    std::printf("timer: %.3f ns per tick, costs about %.0f ns per measurement\n",
                nsPerTick, timerOverheadNs(nsPerTick));

    // ---- generate the order flow (same seed -> same flow every run) ----
    FlowConfig cfg;
    cfg.targetBookSize = bookSize;

    std::vector<Action> prefill, flow;
    double      bookMemoryMb = 0;
    std::size_t finalBookSize = 0;
    {
        OrderFlowGenerator gen(cfg);
        prefill = gen.prefill();

        // Memory of a full book = how much the process grows while building one.
        // Windows hands out memory in chunks, so one small book is too small to see.
        // Build enough identical books to hold about 200,000 orders and divide.
        std::size_t copies = std::max<std::size_t>(1, 200000 / bookSize);
        double      before = readMemory().privateMb;
        {
            std::vector<OrderBook> books(copies);
            for (OrderBook& book : books) fill(book, prefill);
            bookMemoryMb = (readMemory().privateMb - before) / static_cast<double>(copies);
        }

        flow = gen.generate(actions);
        finalBookSize = gen.bookSize();
    }

    // kind[i] says which of the four kinds flow[i] is, for the per-kind latency numbers.
    std::vector<int>         kind(flow.size());
    std::vector<std::size_t> kindCount(kKinds, 0);
    for (std::size_t i = 0; i < flow.size(); ++i) {
        kind[i] = kindOf(flow[i]);
        ++kindCount[kind[i]];
    }
    double pct = 100.0 / static_cast<double>(flow.size());
    std::printf("flow mix: %.1f%% limit, %.1f%% market, %.1f%% cancel, %.1f%% modify\n",
                kindCount[0] * pct, kindCount[1] * pct, kindCount[2] * pct, kindCount[3] * pct);

    // ---- one untimed replay to count trades and price levels ----
    std::size_t trades = 0, priceLevels = 0;
    {
        OrderBook book;
        fill(book, prefill);
        for (const Action& a : flow) apply(book, a);
        trades = book.tradeHistory().size();
        std::size_t resting = countOrders(book, Side::Buy, &priceLevels) + countOrders(book, Side::Sell, &priceLevels);
        if (resting != finalBookSize) {
            std::printf("ERROR: replay ended with %zu orders, generator expected %zu\n", resting, finalBookSize);
            return 1;
        }
    }
    std::printf("after the flow: %zu resting orders on %zu price levels, %zu trades\n",
                finalBookSize, priceLevels, trades);
    std::printf("book memory: %.3f MB (%.0f bytes per resting order)\n",
                bookMemoryMb, bookMemoryMb * 1024.0 * 1024.0 / static_cast<double>(bookSize));

    // ---- warm-up: run it for real but throw the numbers away ----
    // The first runs are slower (CPU caches cold, CPU not yet at full speed, memory not yet mapped).
    for (int i = 0; i < kWarmupRuns; ++i) runThroughput(prefill, flow);

    // ---- measured repeats ----
    std::filesystem::create_directories(outDir);
    std::ofstream summary = openCsv(std::filesystem::path(outDir) / "summary.csv",
        "book_size,repeat,actions,orders_per_sec,avg_ns,p50_ns,p99_ns,p999_ns,max_ns,"
        "book_memory_mb,peak_memory_mb,final_book_size,price_levels,trades");

    std::ofstream byKind = openCsv(std::filesystem::path(outDir) / "latency_by_type.csv",
                                   "book_size,repeat,action,count,avg_ns,p50_ns,p99_ns");

    std::vector<std::uint64_t> ticks(flow.size());
    std::vector<std::uint64_t> histogram(12 * kBucketsPerDecade, 0);
    std::vector<double> allRate, allAvg, allP50, allP99, allP999, allMax;

    std::printf("\n%6s %14s %9s %9s %9s %10s %12s\n", "repeat", "orders/sec", "avg ns", "p50 ns", "p99 ns", "p99.9 ns", "worst ns");
    for (int r = 1; r <= repeats; ++r) {
        double rate = runThroughput(prefill, flow);
        runLatency(prefill, flow, ticks);

        double sum = 0;
        std::vector<std::vector<std::uint64_t>> ticksByKind(kKinds);
        for (std::size_t i = 0; i < ticks.size(); ++i) {
            double ns = static_cast<double>(ticks[i]) * nsPerTick;
            sum += ns;
            int bucket = ns < 1.0 ? 0 : static_cast<int>(std::log10(ns) * kBucketsPerDecade);
            ++histogram[std::min<std::size_t>(static_cast<std::size_t>(bucket), histogram.size() - 1)];
            ticksByKind[kind[i]].push_back(ticks[i]);
        }
        std::sort(ticks.begin(), ticks.end());

        for (int k = 0; k < kKinds; ++k) {
            std::vector<std::uint64_t>& t = ticksByKind[k];
            if (t.empty()) continue;
            std::sort(t.begin(), t.end());
            double kindSum = 0;
            for (std::uint64_t x : t) kindSum += static_cast<double>(x);
            byKind << bookSize << ',' << r << ',' << kKindNames[k] << ',' << t.size() << ','
                   << kindSum * nsPerTick / static_cast<double>(t.size()) << ','
                   << percentile(t, 0.50) * nsPerTick << ',' << percentile(t, 0.99) * nsPerTick << '\n';
        }

        double avg  = sum / static_cast<double>(ticks.size());
        double p50  = percentile(ticks, 0.50)  * nsPerTick;
        double p99  = percentile(ticks, 0.99)  * nsPerTick;
        double p999 = percentile(ticks, 0.999) * nsPerTick;
        double max  = static_cast<double>(ticks.back()) * nsPerTick;
        double peak = readMemory().peakMb;

        std::printf("%6d %14.0f %9.1f %9.1f %9.1f %10.1f %12.0f\n", r, rate, avg, p50, p99, p999, max);
        summary << bookSize << ',' << r << ',' << actions << ',' << static_cast<std::uint64_t>(rate) << ','
                << avg << ',' << p50 << ',' << p99 << ',' << p999 << ',' << max << ','
                << bookMemoryMb << ',' << peak << ',' << finalBookSize << ',' << priceLevels << ',' << trades << '\n';

        allRate.push_back(rate); allAvg.push_back(avg); allP50.push_back(p50);
        allP99.push_back(p99);   allP999.push_back(p999); allMax.push_back(max);
    }
    std::printf("%6s %14.0f %9.1f %9.1f %9.1f %10.1f %12.0f\n", "median",
                median(allRate), median(allAvg), median(allP50), median(allP99), median(allP999), median(allMax));
    std::printf("peak process memory: %.1f MB (includes the benchmark's own data)\n\n", readMemory().peakMb);

    // ---- latency histogram, all repeats pooled ----
    std::ofstream hist = openCsv(std::filesystem::path(outDir) / "latency_hist.csv",
                                 "book_size,bucket_lo_ns,bucket_hi_ns,count");
    for (std::size_t b = 0; b < histogram.size(); ++b) {
        if (histogram[b] == 0) continue;
        double lo = std::pow(10.0, static_cast<double>(b) / kBucketsPerDecade);
        double hi = std::pow(10.0, static_cast<double>(b + 1) / kBucketsPerDecade);
        hist << bookSize << ',' << lo << ',' << hi << ',' << histogram[b] << '\n';
    }
    return 0;
}
