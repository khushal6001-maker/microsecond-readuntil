// src/core/tsc.hpp
//
// Calibrated cycle-counter timing for the decision path.
//
// Why not std::chrono: clock_gettime(CLOCK_MONOTONIC) through the vDSO costs
// ~20-25 ns, which is fine at the boundaries but not when stamping six points
// per chunk at PromethION rates. RDTSCP is ~8-10 ns and, critically, has stable
// cost -- it does not occasionally trap into the kernel.
//
// Three things that will invalidate your numbers if you ignore them:
//
//  1. INVARIANT TSC. On older or virtualised hosts the counter is tied to the
//     current core frequency, so every measurement scales with turbo state.
//     Call tsc_is_invariant() at startup and refuse to report latency figures
//     if it returns false. This is checked, not assumed.
//
//  2. RDTSCP is only half-serialising. It waits for earlier loads to retire but
//     does NOT stop later instructions from being hoisted above it. For a tight
//     microbenchmark put lfence() after the read; for end-to-end stamping in
//     the data plane the surrounding work dwarfs the skew, so skip it there.
//
//  3. WSL2 and most VMs virtualise the clock. Functional testing is fine, but
//     every number that goes in the paper must come from bare-metal Linux with
//     a fixed frequency governor and the measured core isolated.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#if defined(__GNUC__)
#include <cpuid.h>
#endif
#endif

namespace mru {

// ---------------------------------------------------------------------------
// Raw counter
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::uint64_t rdtscp() noexcept {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
  unsigned aux = 0;
  return __rdtscp(&aux);
#elif defined(__aarch64__)
  std::uint64_t v;
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
  return v;
#else
  // Portable fallback: still monotonic, just coarser and more expensive.
  return static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline void lfence() noexcept {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
  _mm_lfence();
#elif defined(__aarch64__)
  __asm__ __volatile__("isb" ::: "memory");
#endif
}

// Reads the counter with both sides fenced. Use in microbenchmarks, not in the
// data plane.
[[nodiscard]] inline std::uint64_t rdtscp_serialized() noexcept {
  const std::uint64_t t = rdtscp();
  lfence();
  return t;
}

// CPUID.80000007H:EDX[8] -- "TSC ticks at a constant rate across all P/C states".
[[nodiscard]] inline bool tsc_is_invariant() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  int regs[4] = {0, 0, 0, 0};
  __cpuid(regs, 0x80000000);
  if (static_cast<unsigned>(regs[0]) < 0x80000007u) return false;
  __cpuid(regs, 0x80000007);
  return (regs[3] & (1 << 8)) != 0;
#elif (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (__get_cpuid(0x80000000u, &a, &b, &c, &d) == 0 || a < 0x80000007u) return false;
  if (__get_cpuid(0x80000007u, &a, &b, &c, &d) == 0) return false;
  return (d & (1u << 8)) != 0;
#elif defined(__aarch64__)
  return true;  // CNTVCT_EL0 is architecturally fixed-frequency
#else
  return false;
#endif
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

class TscClock {
 public:
  // Calibrated once, lazily, on first use.
  static const TscClock& instance() {
    static const TscClock c = calibrate(5, std::chrono::milliseconds(10));
    return c;
  }

  // Median of `rounds` independent measurements against steady_clock. Median
  // rather than mean because a single descheduled round would otherwise poison
  // the estimate.
  [[nodiscard]] static TscClock calibrate(int rounds, std::chrono::nanoseconds per_round) {
    std::vector<double> ratios;
    ratios.reserve(static_cast<std::size_t>(rounds));

    for (int i = 0; i < rounds; ++i) {
      lfence();
      const std::uint64_t c0 = rdtscp();
      const auto m0 = std::chrono::steady_clock::now();
      lfence();

      std::this_thread::sleep_for(per_round);

      lfence();
      const std::uint64_t c1 = rdtscp();
      const auto m1 = std::chrono::steady_clock::now();
      lfence();

      const auto ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(m1 - m0).count();
      if (ns <= 0 || c1 <= c0) continue;
      ratios.push_back(static_cast<double>(c1 - c0) / static_cast<double>(ns));
    }

    TscClock clk;
    clk.invariant_ = tsc_is_invariant();
    if (ratios.empty()) {
      clk.cycles_per_ns_ = 1.0;  // fallback path: counter already in ns units
      clk.calibrated_ = false;
      return clk;
    }
    std::sort(ratios.begin(), ratios.end());
    clk.cycles_per_ns_ = ratios[ratios.size() / 2];
    clk.calibrated_ = true;
    return clk;
  }

  [[nodiscard]] double cycles_per_ns() const noexcept { return cycles_per_ns_; }
  [[nodiscard]] double mhz() const noexcept { return cycles_per_ns_ * 1000.0; }
  [[nodiscard]] bool invariant() const noexcept { return invariant_; }
  [[nodiscard]] bool calibrated() const noexcept { return calibrated_; }

  [[nodiscard]] double to_ns(std::uint64_t cycles) const noexcept {
    return static_cast<double>(cycles) / cycles_per_ns_;
  }
  [[nodiscard]] double to_us(std::uint64_t cycles) const noexcept {
    return to_ns(cycles) / 1000.0;
  }
  [[nodiscard]] std::uint64_t ns_to_cycles(double ns) const noexcept {
    return static_cast<std::uint64_t>(ns * cycles_per_ns_);
  }

 private:
  double cycles_per_ns_{1.0};
  bool invariant_{false};
  bool calibrated_{false};
};

// ---------------------------------------------------------------------------
// Minimal latency accumulator
// ---------------------------------------------------------------------------
//
// Power-of-two buckets, so reported percentiles are the UPPER BOUND of the
// containing bucket -- i.e. ~2x resolution. That is plenty for "is the core
// data plane sane today" and nowhere near good enough for the paper. Swap in
// HdrHistogram (1-2% resolution, same API shape) before producing figures.
class CycleHistogram {
 public:
  void record(std::uint64_t cycles) noexcept {
    ++count_;
    sum_ += cycles;
    if (cycles < min_) min_ = cycles;
    if (cycles > max_) max_ = cycles;
    ++buckets_[bucket_of(cycles)];
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
  [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
  [[nodiscard]] double mean() const noexcept {
    return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
  }

  // Upper bound of the bucket containing the p-th percentile (p in [0, 1]).
  [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
    if (count_ == 0) return 0;
    const auto target = static_cast<std::uint64_t>(static_cast<double>(count_) * p);
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
      seen += buckets_[i];
      if (seen >= target) return upper_bound_of(i);
    }
    return max_;
  }

  void merge(const CycleHistogram& o) noexcept {
    count_ += o.count_;
    sum_ += o.sum_;
    min_ = std::min(min_, o.min_);
    max_ = std::max(max_, o.max_);
    for (std::size_t i = 0; i < buckets_.size(); ++i) buckets_[i] += o.buckets_[i];
  }

 private:
  static constexpr std::size_t kBuckets = 65;  // bucket i holds [2^(i-1), 2^i)

  static std::size_t bucket_of(std::uint64_t v) noexcept {
    if (v == 0) return 0;
    return static_cast<std::size_t>(64 - std::countl_zero(v));
  }
  static std::uint64_t upper_bound_of(std::size_t i) noexcept {
    if (i == 0) return 0;
    if (i >= 64) return ~std::uint64_t{0};
    return (std::uint64_t{1} << i) - 1;
  }

  std::uint64_t count_{0};
  std::uint64_t sum_{0};
  std::uint64_t min_{~std::uint64_t{0}};
  std::uint64_t max_{0};
  std::array<std::uint64_t, kBuckets> buckets_{};
};

// Stamps a cycle delta into a histogram on scope exit.
class ScopedCycles {
 public:
  explicit ScopedCycles(CycleHistogram& h) noexcept : h_(h), t0_(rdtscp()) {}
  ~ScopedCycles() { h_.record(rdtscp() - t0_); }

  ScopedCycles(const ScopedCycles&) = delete;
  ScopedCycles& operator=(const ScopedCycles&) = delete;

 private:
  CycleHistogram& h_;
  std::uint64_t t0_;
};

}  // namespace mru
