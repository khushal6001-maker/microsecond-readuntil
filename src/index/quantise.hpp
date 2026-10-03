// src/index/quantise.hpp
//
// Raw UNCALIBRATED int16 signal -> normalised events -> quantised keys.
//
// This is the step that replaces basecalling in the critical path, and it is
// also where the scientific risk lives. Read this before tuning anything:
//
// THE HARD PART IS RECALL, NOT SPEED
// Quantisation throws information away on purpose, so two observations of the
// same DNA k-mer must land in the same bucket despite noise, drift and dwell-time
// variation. Too coarse and everything collides (no specificity); too fine and
// true matches miss (no sensitivity). A fast index with poor recall is worthless,
// so every parameter here is explicit and measurable rather than baked in, and
// test/index_test.cpp measures recall directly.
//
// WHAT IS MISSING
// Matching against a genome needs the reference converted to EXPECTED current via
// ONT's k-mer -> level table (9-mers for R10.4.1). That table is an ONT data file
// this repo does not vendor. PoreModel below is the seam for it; until a real
// table is loaded, reference-side keys cannot be generated and only
// read-vs-read matching is meaningful.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace mru {

// z = (raw - shift) / scale
//
// Nanopore reads need per-read normalisation: every pore has its own offset and
// gain. Median/MAD is the standard robust choice because stalls and spikes would
// wreck a mean/stddev estimate.
//
// MinKNOW already sends `median` and `median_before` in ReadData (fields 9 and
// 8), so prefer those over recomputing: a median needs a sort, which has no
// business on the hot path.
struct SignalScaling {
  float shift = 0.0f;
  float scale = 1.0f;

  [[nodiscard]] bool valid() const noexcept {
    return std::isfinite(shift) && std::isfinite(scale) && scale > 0.0f;
  }
  [[nodiscard]] float apply(std::int16_t raw) const noexcept {
    return (static_cast<float>(raw) - shift) / scale;
  }
};

// Median and MAD-derived scale, for tests and for the case where MinKNOW's
// medians are unavailable. Sorts a copy, so NOT for the hot path.
[[nodiscard]] inline SignalScaling scaling_from_samples(std::span<const std::int16_t> s) {
  if (s.empty()) return {};
  std::vector<std::int16_t> tmp(s.begin(), s.end());
  const std::size_t mid = tmp.size() / 2;
  std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(mid), tmp.end());
  const float median = static_cast<float>(tmp[mid]);

  std::vector<float> dev;
  dev.reserve(tmp.size());
  for (std::int16_t v : s) dev.push_back(std::fabs(static_cast<float>(v) - median));
  const std::size_t dmid = dev.size() / 2;
  std::nth_element(dev.begin(), dev.begin() + static_cast<std::ptrdiff_t>(dmid), dev.end());

  SignalScaling out;
  out.shift = median;
  // 1.4826 * MAD estimates sigma for a normal distribution.
  out.scale = std::max(1e-3f, 1.4826f * dev[dmid]);
  return out;
}

// All the knobs in one place, so a sweep is a loop over configs rather than a
// recompile. Defaults are a starting point for R10.4.1 DNA at 4 kHz, NOT tuned
// values -- tuning them against measured recall is the actual research work.
struct QuantConfig {
  // ~10 samples per base at 4 kHz and ~400 b/s. Averaging this many raw samples
  // gives one event. Fixed-window downsampling, not true event segmentation:
  // segmentation is more faithful and much more expensive, and whether it is
  // worth the cycles is an experiment, not an assumption.
  std::uint32_t samples_per_event = 10;

  // Bits per quantised event. 3 bits = 8 levels.
  std::uint32_t bits_per_event = 3;

  // Events combined into one key. 9 events at 3 bits = 27 bits of signal.
  std::uint32_t events_per_key = 9;

  // z-scores are clipped to +/- this before bucketing, so outliers cannot drag
  // the whole scale.
  float z_clip = 3.0f;

  // Minimizer window, in keys. 1 disables subsampling and indexes every key.
  std::uint32_t minimizer_window = 5;

  [[nodiscard]] bool valid() const noexcept {
    return samples_per_event > 0 && bits_per_event > 0 && bits_per_event <= 8 &&
           events_per_key > 0 && bits_per_event * events_per_key <= 64 &&
           z_clip > 0.0f && minimizer_window > 0;
  }
  [[nodiscard]] std::uint32_t levels() const noexcept { return 1u << bits_per_event; }
  [[nodiscard]] std::uint32_t key_bits() const noexcept {
    return bits_per_event * events_per_key;
  }
};

// One quantised event: a small integer, so a whole key packs into a uint64.
using QEvent = std::uint8_t;

// Clip to +/-z_clip, then map linearly onto [0, levels).
[[nodiscard]] inline QEvent quantise_z(float z, const QuantConfig& cfg) noexcept {
  const float clipped = std::clamp(z, -cfg.z_clip, cfg.z_clip);
  const float unit = (clipped + cfg.z_clip) / (2.0f * cfg.z_clip);  // -> [0, 1]
  const auto levels = static_cast<float>(cfg.levels());
  auto bucket = static_cast<std::int32_t>(unit * levels);
  if (bucket >= static_cast<std::int32_t>(cfg.levels())) {
    bucket = static_cast<std::int32_t>(cfg.levels()) - 1;  // z == +z_clip exactly
  }
  if (bucket < 0) bucket = 0;
  return static_cast<QEvent>(bucket);
}

// Downsample raw signal into quantised events. Appends to `out`, which the caller
// reuses across chunks so the hot path never allocates.
inline void quantise_signal(std::span<const std::int16_t> raw, const SignalScaling& sc,
                            const QuantConfig& cfg, std::vector<QEvent>& out) {
  if (!sc.valid() || !cfg.valid()) return;
  const std::size_t w = cfg.samples_per_event;
  if (raw.size() < w) return;

  const std::size_t n_events = raw.size() / w;
  out.reserve(out.size() + n_events);
  for (std::size_t e = 0; e < n_events; ++e) {
    // Mean of the window in float. int32 accumulation would be enough for int16
    // but float keeps this identical to the reference implementation in tests.
    float sum = 0.0f;
    const std::size_t base = e * w;
    for (std::size_t i = 0; i < w; ++i) sum += sc.apply(raw[base + i]);
    out.push_back(quantise_z(sum / static_cast<float>(w), cfg));
  }
}

// Interprets int16 samples out of the raw bytes MinKNOW sends.
//
// Endianness note: protobuf `bytes` is opaque, and MinKNOW sends host-order
// little-endian int16 for UNCALIBRATED. We are little-endian on x86-64 and
// aarch64, so this is a reinterpret rather than a byte swap. On a big-endian host
// it would need swapping, which is why it is one function and not scattered casts.
[[nodiscard]] inline std::span<const std::int16_t> as_int16(
    const std::byte* data, std::size_t bytes) noexcept {
  return std::span<const std::int16_t>(reinterpret_cast<const std::int16_t*>(data),
                                       bytes / sizeof(std::int16_t));
}

// Packs events_per_key consecutive events into one integer, low event first.
[[nodiscard]] inline std::uint64_t pack_key(const QEvent* events,
                                            const QuantConfig& cfg) noexcept {
  std::uint64_t k = 0;
  for (std::uint32_t i = 0; i < cfg.events_per_key; ++i) {
    k |= static_cast<std::uint64_t>(events[i]) << (i * cfg.bits_per_event);
  }
  return k;
}

// splitmix64. Cheap, good avalanche, and critically it is DETERMINISTIC across
// platforms and compilers -- an index built on one machine must be queryable on
// another, so std::hash is unusable here.
[[nodiscard]] inline std::uint64_t hash64(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// A key plus where it came from.
struct SeedHash {
  std::uint64_t hash;
  std::uint32_t offset;  // index of the first event of this key
};

// Every key over the event stream, hashed. Use when minimizer_window == 1 or to
// feed select_minimizers.
inline void hash_all_keys(std::span<const QEvent> events, const QuantConfig& cfg,
                          std::vector<SeedHash>& out) {
  if (!cfg.valid() || events.size() < cfg.events_per_key) return;
  const std::size_t last = events.size() - cfg.events_per_key;
  out.reserve(out.size() + last + 1);
  for (std::size_t i = 0; i <= last; ++i) {
    out.push_back(SeedHash{hash64(pack_key(events.data() + i, cfg)), static_cast<std::uint32_t>(i)});
  }
}

// Minimizer subsampling: keep the smallest hash in each window of w consecutive
// keys, deduplicating consecutive repeats.
//
// This is what keeps the index small and makes query and reference agree on which
// positions to look at without coordinating. Larger w means a smaller index and
// lower sensitivity -- directly the recall/size trade-off to sweep.
inline void select_minimizers(std::span<const SeedHash> all, std::uint32_t w,
                              std::vector<SeedHash>& out) {
  if (all.empty() || w == 0) return;
  if (w == 1) {
    out.insert(out.end(), all.begin(), all.end());
    return;
  }
  const std::size_t n = all.size();
  bool have_last = false;
  SeedHash last{};
  for (std::size_t i = 0; i + w <= n; ++i) {
    std::size_t best = i;
    for (std::size_t j = i + 1; j < i + w; ++j) {
      if (all[j].hash < all[best].hash) best = j;
    }
    const SeedHash cand = all[best];
    if (have_last && cand.offset == last.offset && cand.hash == last.hash) continue;
    out.push_back(cand);
    last = cand;
    have_last = true;
  }
}

// ---------------------------------------------------------------------------
// Pore model seam
// ---------------------------------------------------------------------------
//
// Converting reference DNA to expected current needs ONT's k-mer -> level table.
// This repo does not vendor it, so PoreModel is deliberately an empty shell with
// the interface the reference-side indexer will need. Everything that depends on
// it returns false while it is unloaded, rather than silently producing keys from
// invented levels -- fabricated expected currents would yield an index that looks
// fine and matches nothing.
class PoreModel {
 public:
  // Expects one mean current per k-mer, in lexicographic k-mer order
  // (4^k entries). Returns false if the size is not a power of four.
  [[nodiscard]] bool load_levels(std::span<const float> levels_in_kmer_order);

  [[nodiscard]] bool loaded() const noexcept { return !levels_.empty(); }
  [[nodiscard]] std::uint32_t k() const noexcept { return k_; }

  // Expected current for a packed 2-bit-per-base k-mer.
  [[nodiscard]] float level(std::uint64_t kmer) const noexcept {
    return kmer < levels_.size() ? levels_[kmer] : 0.0f;
  }

  // Reference DNA -> quantised events, via the model. Returns false when no model
  // is loaded, which is the current state.
  [[nodiscard]] bool reference_to_events(std::span<const char> dna,
                                         const QuantConfig& cfg,
                                         std::vector<QEvent>& out) const;

 private:
  std::vector<float> levels_;
  std::uint32_t k_ = 0;
};

inline bool PoreModel::load_levels(std::span<const float> levels_in_kmer_order) {
  const std::size_t n = levels_in_kmer_order.size();
  if (n == 0) return false;
  std::uint32_t k = 0;
  std::size_t p = 1;
  while (p < n) {
    p *= 4;
    ++k;
  }
  if (p != n) return false;  // not 4^k
  levels_.assign(levels_in_kmer_order.begin(), levels_in_kmer_order.end());
  k_ = k;
  return true;
}

inline bool PoreModel::reference_to_events(std::span<const char> dna,
                                           const QuantConfig& cfg,
                                           std::vector<QEvent>& out) const {
  if (!loaded() || k_ == 0 || !cfg.valid()) return false;
  if (dna.size() < k_) return false;

  // Levels are in pA; normalise them the same way read signal is normalised, so
  // the two sides land in the same buckets. Using the model's own distribution
  // rather than a read's keeps reference keys independent of any single read.
  float mean = 0.0f;
  for (float v : levels_) mean += v;
  mean /= static_cast<float>(levels_.size());
  float var = 0.0f;
  for (float v : levels_) var += (v - mean) * (v - mean);
  var /= static_cast<float>(levels_.size());
  const float sd = std::max(1e-6f, std::sqrt(var));

  const auto base_code = [](char c) -> int {
    switch (c) {
      case 'A': case 'a': return 0;
      case 'C': case 'c': return 1;
      case 'G': case 'g': return 2;
      case 'T': case 't': return 3;
      default: return -1;
    }
  };

  out.reserve(out.size() + dna.size());
  std::uint64_t kmer = 0;
  std::uint32_t have = 0;
  const std::uint64_t mask = (k_ >= 32) ? ~std::uint64_t{0} : ((std::uint64_t{1} << (2 * k_)) - 1);
  for (char c : dna) {
    const int code = base_code(c);
    if (code < 0) {  // N or junk: the k-mer window restarts
      have = 0;
      kmer = 0;
      continue;
    }
    kmer = ((kmer << 2) | static_cast<std::uint64_t>(code)) & mask;
    if (++have < k_) continue;
    out.push_back(quantise_z((level(kmer) - mean) / sd, cfg));
  }
  return true;
}

}  // namespace mru
