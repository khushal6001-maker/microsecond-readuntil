// src/index/event_detect.hpp
//
// Event detection: find where the signal CHANGES LEVEL, instead of chopping it into
// fixed-width time slices.
//
// WHY THIS EXISTS. bench/dwell_robustness.cpp showed the fixed-width pipeline collapses
// once dwell time varies -- TPR 64.7% at dwell CV 0.00 down to 1.7% at CV 0.30, with zero
// reads out of 300 finding the correct diagonal. The cause is that a key packs 14
// CONSECUTIVE events, and fixed-width slices stop corresponding to k-mers the moment the
// pore changes speed, so the key bits themselves change. Run-length compressing the
// quantised buckets was the cheap alternative and it failed differently: dwell-invariant
// but carrying no signal at all (identical vote counts on target and on a shuffled
// control).
//
// Every published raw-signal mapper segments by detected events rather than by time:
// UNCALLED, Sigmap, RawHash and RawHash2 all do. This is adopted standard technique, not
// a contribution of this project.
//
// METHOD. A two-window t-test over the raw samples. For each candidate boundary, compare
// the window of w samples before it against the w samples after:
//
//     t = |mean_before - mean_after| / sqrt(var_before/w + var_after/w)
//
// and declare a boundary at local maxima of t that exceed a threshold. Each resulting
// segment contributes one event, whose value is the segment mean. A k-mer held for 7
// samples and the same k-mer held for 30 both yield ONE event, which is the property the
// fixed-width version lacked.
//
// COST. Prefix sums of x and x^2 make every window mean and variance O(1), so the whole
// pass is O(n) with two sequential reads of the sample buffer and no division inside the
// inner loop beyond the t-statistic itself. Scratch is caller-owned and reserved once,
// because this runs on the decision path where an allocation has already been measured
// at a 9.5 ms maximum.
//
// Using double for the prefix sums is deliberate. int16 samples squared and accumulated
// over a chunk overflow float's 24-bit mantissa quickly, and the whole point of the
// t-statistic is a difference of two large similar numbers, which is exactly where
// precision loss turns into spurious or missed boundaries.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace mru {

struct EventDetectConfig {
  // Samples each side of a candidate boundary. Smaller is more sensitive and noisier.
  // At ~10 samples per k-mer, 4 resolves adjacent k-mers without splitting one.
  std::uint32_t window = 4;

  // t-statistic a boundary must exceed. Lower finds more events, including spurious ones
  // from noise; higher merges genuinely distinct k-mers.
  float threshold = 1.4f;

  // A detected event shorter than this is merged into its neighbour. Guards against a
  // noise spike being read as two boundaries one sample apart.
  std::uint32_t min_len = 3;

  // Force a boundary after this many samples with no detected change. Without it a long
  // homopolymer becomes one enormous event and the read loses its place; with it the
  // segmentation degrades gracefully to fixed-width in exactly the region where there is
  // no level change to detect anyway.
  std::uint32_t max_len = 60;

  [[nodiscard]] bool valid() const noexcept {
    return window >= 2 && threshold > 0.0f && min_len >= 1 && max_len > min_len;
  }
};

// Caller-owned scratch so the detection path never allocates after the first chunk.
struct EventScratch {
  std::vector<double> csum;   // prefix sum of x,   size n+1
  std::vector<double> csum2;  // prefix sum of x^2, size n+1

  void reserve(std::size_t max_samples) {
    csum.reserve(max_samples + 1);
    csum2.reserve(max_samples + 1);
  }
};

// Segment `raw` into events and write each event's mean level into `out`.
// Returns the number of events written. `out` is cleared first and is the caller's
// buffer, reserved once by the caller.
inline std::size_t detect_events(std::span<const std::int16_t> raw,
                                const EventDetectConfig& cfg, EventScratch& s,
                                std::vector<float>& out) {
  out.clear();
  const std::size_t n = raw.size();
  const std::uint32_t w = cfg.window;
  if (!cfg.valid() || n < static_cast<std::size_t>(2) * w) return 0;

  s.csum.assign(n + 1, 0.0);
  s.csum2.assign(n + 1, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    const double x = static_cast<double>(raw[i]);
    s.csum[i + 1] = s.csum[i] + x;
    s.csum2[i + 1] = s.csum2[i] + x * x;
  }

  // Window statistics over [a, b) in O(1).
  const auto stats = [&s](std::size_t a, std::size_t b, double& mean, double& var) {
    const double cnt = static_cast<double>(b - a);
    const double sum = s.csum[b] - s.csum[a];
    const double sq = s.csum2[b] - s.csum2[a];
    mean = sum / cnt;
    var = sq / cnt - mean * mean;
    if (var < 1e-9) var = 1e-9;  // a perfectly flat window is not infinitely significant
  };

  const auto tstat = [&](std::size_t i) -> double {
    double m1 = 0, v1 = 0, m2 = 0, v2 = 0;
    stats(i - w, i, m1, v1);
    stats(i, i + w, m2, v2);
    const double d = m1 - m2;
    return std::fabs(d) / std::sqrt(v1 / w + v2 / w);
  };

  const auto emit = [&](std::size_t a, std::size_t b) {
    if (b <= a) return;
    const double sum = s.csum[b] - s.csum[a];
    out.push_back(static_cast<float>(sum / static_cast<double>(b - a)));
  };

  std::size_t seg_start = 0;
  // Only positions with a full window on both sides can be tested.
  for (std::size_t i = w; i + w <= n; ++i) {
    const bool too_long = (i - seg_start) >= cfg.max_len;
    if (!too_long) {
      if (i - seg_start < cfg.min_len) continue;
      const double t = tstat(i);
      if (t < static_cast<double>(cfg.threshold)) continue;
      // Local maximum only: a single transition spans ~w samples and would otherwise be
      // reported as a run of adjacent boundaries, splitting one event into several.
      if (i + 1 + w <= n && tstat(i + 1) > t) continue;
      if (i > w && (i - 1) - seg_start >= cfg.min_len && tstat(i - 1) > t) continue;
    }
    emit(seg_start, i);
    seg_start = i;
  }
  emit(seg_start, n);
  return out.size();
}

}  // namespace mru
