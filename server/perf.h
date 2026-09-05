// server/perf.h
//
// Tick-cost and egress instrumentation for A4 (docs/matchmaking-plan.md).
//
// A4's job is to decide how many concurrent matches this box can hold, and that
// number has to come from measurement rather than arithmetic. Two things are
// being measured, and the second is the one most likely to bind:
//
//   CPU     - how long a tick takes, split so the expensive part is identifiable
//   EGRESS  - bytes on the wire, because 8 clients x 60 Hz x ~830 B is ~400 KB/s
//             per full match, and twenty of those is ~20 TB/month against a plan
//             that includes 2 TB
//
// OFF BY DEFAULT. Set PLATFORMZ_PERF=1 to enable; the sampling is cheap but the
// point is that a production server logs nothing extra unless asked. Timing calls
// are compiled in either way - a steady_clock read is tens of nanoseconds against
// a tick budget of 16,666,000.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

//MARK: PerfStat
// A fixed window of samples, summarised on demand. Percentiles matter more than
// the mean here: a scheduler is sized by its bad ticks, not its typical ones, so
// p95 and max are the numbers that decide the cap.
class PerfStat {
public:
    explicit PerfStat(size_t window = 900) : window_(window) { samples_.reserve(window); }

    void Add(double ms) {
        if (samples_.size() < window_) samples_.push_back(ms);
        else { samples_[next_] = ms; }
        next_ = (next_ + 1) % window_;
    }

    bool   Empty() const { return samples_.empty(); }
    size_t Count() const { return samples_.size(); }

    struct Summary { double p50 = 0, p95 = 0, max = 0, mean = 0; };

    Summary Summarise() const {
        Summary s;
        if (samples_.empty()) return s;
        std::vector<double> v = samples_;
        std::sort(v.begin(), v.end());
        auto at = [&](double q) {
            size_t i = (size_t)(q * (v.size() - 1) + 0.5);
            return v[std::min(i, v.size() - 1)];
        };
        s.p50 = at(0.50);
        s.p95 = at(0.95);
        s.max = v.back();
        double total = 0; for (double d : v) total += d;
        s.mean = total / v.size();
        return s;
    }

    void Reset() { samples_.clear(); next_ = 0; }

private:
    std::vector<double> samples_;
    size_t window_;
    size_t next_ = 0;
};

//MARK: Scoped timer
// Adds its own lifetime, in milliseconds, to a PerfStat.
class ScopedTime {
public:
    explicit ScopedTime(PerfStat& into) : into_(into), t0_(std::chrono::steady_clock::now()) {}
    ~ScopedTime() {
        into_.Add(std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0_).count());
    }
private:
    PerfStat& into_;
    std::chrono::steady_clock::time_point t0_;
};

//MARK: Egress
// Bytes and datagrams actually handed to a socket. Incremented from io threads
// and the sim thread, so relaxed atomics - these are counters for a human, not
// synchronisation.
struct EgressCounters {
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> packets{0};

    void Add(size_t n) {
        bytes.fetch_add(n, std::memory_order_relaxed);
        packets.fetch_add(1, std::memory_order_relaxed);
    }
};

//MARK: Switch
// Read once at startup. Sampling is only worth its (small) cost when someone is
// actually looking at the numbers.
inline bool& PerfEnabled() { static bool on = false; return on; }
