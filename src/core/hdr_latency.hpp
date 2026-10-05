// src/core/hdr_latency.hpp
//
// Latency recording with bounded relative error, for numbers that go in a paper.
//
// Why this replaces CycleHistogram for reporting
// ----------------------------------------------
// CycleHistogram (core/tsc.hpp) uses power-of-two buckets, so a reported percentile is
// the upper bound of a bucket spanning a factor of two. That is fine for "is the data
// plane sane today" and useless for a figure: a p99 of 112 us from it means somewhere in
// [56, 112). Worse, when the measured p99 moved from 224 to 112 us it was impossible to
// tell a real halving from a value crossing one bucket edge.
//
// HdrHistogram keeps constant RELATIVE error across the whole range -- 3 significant
// figures means every reported value is within 0.1% of the true one -- at fixed memory
// and with recording that is a handful of instructions. Gil Tene's design, reference C
// implementation vendored at third_party/HdrHistogram_c (0.12.0).
//
// Fallback: when the submodule is absent the class degrades to CycleHistogram so the code
// still builds and runs. backend() says which is in use, and it is printed next to any
// figure, because a percentile from the fallback must not be mistaken for a measured one.
//
// Thread safety: none, deliberately. One recorder per worker, merged at the end. Chunks
// are sharded by channel so each recorder has exactly one writer, which is the same
// argument the rest of the data plane rests on.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "core/tsc.hpp"

#if defined(MRU_WITH_HDR)
#include <hdr/hdr_histogram.h>
#endif

namespace mru {

class LatencyRecorder {
 public:
  // Range in cycles. The default spans 1 cycle to 1e9 (~0.4 s at 2.4 GHz), which covers
  // everything from a cache hit to a pathological stall. 3 significant figures gives
  // 0.1% relative error for a few hundred KB per recorder.
  explicit LatencyRecorder(std::int64_t highest_cycles = 1'000'000'000,
                           int significant_figures = 3) {
#if defined(MRU_WITH_HDR)
    if (hdr_init(1, highest_cycles, significant_figures, &h_) != 0) h_ = nullptr;
#else
    (void)highest_cycles;
    (void)significant_figures;
#endif
  }

  ~LatencyRecorder() {
#if defined(MRU_WITH_HDR)
    if (h_ != nullptr) hdr_close(h_);
#endif
  }

  LatencyRecorder(const LatencyRecorder&) = delete;
  LatencyRecorder& operator=(const LatencyRecorder&) = delete;

  LatencyRecorder(LatencyRecorder&& o) noexcept {
#if defined(MRU_WITH_HDR)
    h_ = o.h_;
    o.h_ = nullptr;
#endif
    fallback_ = o.fallback_;
    out_of_range_ = o.out_of_range_;
  }

  [[nodiscard]] bool usable() const noexcept {
#if defined(MRU_WITH_HDR)
    return h_ != nullptr;
#else
    return true;
#endif
  }

  void record(std::uint64_t cycles) noexcept {
#if defined(MRU_WITH_HDR)
    if (h_ != nullptr) {
      // A value past the configured ceiling would be silently clamped, so count it
      // instead: a run that reports out-of-range samples has an unreported tail.
      if (!hdr_record_value(h_, static_cast<std::int64_t>(cycles))) ++out_of_range_;
      return;
    }
#endif
    fallback_.record(cycles);
  }

  // p in [0, 1].
  [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
#if defined(MRU_WITH_HDR)
    if (h_ != nullptr) {
      return static_cast<std::uint64_t>(hdr_value_at_percentile(h_, p * 100.0));
    }
#endif
    return fallback_.percentile(p);
  }

  [[nodiscard]] std::uint64_t count() const noexcept {
#if defined(MRU_WITH_HDR)
    if (h_ != nullptr) return static_cast<std::uint64_t>(h_->total_count);
#endif
    return fallback_.count();
  }

  [[nodiscard]] std::uint64_t min() const noexcept {
#if defined(MRU_WITH_HDR)
    if (h_ != nullptr) return static_cast<std::uint64_t>(hdr_min(h_));
#endif
    return fallback_.min();
  }

  [[nodiscard]] std::uint64_t max() const noexcept {
#if defined(MRU_WITH_HDR)
    if (h_ != nullptr) return static_cast<std::uint64_t>(hdr_max(h_));
#endif
    return fallback_.max();
  }

  [[nodiscard]] double mean() const noexcept {
#if defined(MRU_WITH_HDR)
    if (h_ != nullptr) return hdr_mean(h_);
#endif
    return fallback_.mean();
  }

  [[nodiscard]] std::uint64_t out_of_range() const noexcept { return out_of_range_; }

  void merge(const LatencyRecorder& o) noexcept {
#if defined(MRU_WITH_HDR)
    if (h_ != nullptr && o.h_ != nullptr) {
      (void)hdr_add(h_, o.h_);
      out_of_range_ += o.out_of_range_;
      return;
    }
#endif
    fallback_.merge(o.fallback_);
    out_of_range_ += o.out_of_range_;
  }

  // Full percentile distribution, for plotting. value_scale converts cycles to the unit
  // you want printed -- pass cycles-per-microsecond to get microseconds.
  bool print_distribution(std::FILE* out, double value_scale) const {
#if defined(MRU_WITH_HDR)
    if (h_ != nullptr && out != nullptr) {
      return hdr_percentiles_print(h_, out, 5, value_scale, CLASSIC) == 0;
    }
#endif
    (void)out;
    (void)value_scale;
    return false;  // the fallback cannot produce a meaningful distribution
  }

  [[nodiscard]] static const char* backend() noexcept {
#if defined(MRU_WITH_HDR)
    return "HdrHistogram 0.12.0 (3 significant figures, 0.1% relative error)";
#else
    return "power-of-two fallback -- percentiles are UPPER BOUNDS at ~2x resolution";
#endif
  }

  [[nodiscard]] static bool exact() noexcept {
#if defined(MRU_WITH_HDR)
    return true;
#else
    return false;
#endif
  }

 private:
#if defined(MRU_WITH_HDR)
  struct hdr_histogram* h_ = nullptr;
#endif
  CycleHistogram fallback_{};
  std::uint64_t out_of_range_ = 0;
};

// One line of percentiles in microseconds, with the backend named so a fallback figure
// can never be mistaken for a measured one.
inline std::string format_latency_us(const LatencyRecorder& r, const TscClock& clk) {
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "n=%llu  p50 %.2f  p90 %.2f  p99 %.2f  p99.9 %.2f  p99.99 %.2f  "
                "max %.2f  mean %.2f us",
                static_cast<unsigned long long>(r.count()), clk.to_us(r.percentile(0.50)),
                clk.to_us(r.percentile(0.90)), clk.to_us(r.percentile(0.99)),
                clk.to_us(r.percentile(0.999)), clk.to_us(r.percentile(0.9999)),
                clk.to_us(r.max()), clk.to_ns(static_cast<std::uint64_t>(r.mean())) / 1000.0);
  return std::string(buf);
}

}  // namespace mru
