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
#include <array>
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
// recompile.
//
// THE DEFAULTS BELOW ARE FROZEN, and they are measurements rather than guesses. The
// full derivation lives in test/index_test.cpp; the short version:
//
//   3 bits/event   Per-event bucket disagreement is set almost entirely by bucket
//                  width and halves for each bit removed: 5 bits 30.9%, 4 bits
//                  15.9%, 3 bits 6.9%, 2 bits 3.3%. Fewer bits looks strictly
//                  better until key space is accounted for, and then 2 bits
//                  collapses (see below).
//
//   15 events      Key space is the binding constraint at mammalian scale. 27-bit
//                  keys give 1.34e8 distinct values, which a 3.1e9-event reference
//                  occupies 23x over, capping recall at ~35% by pigeonhole before
//                  any noise. Worse, k-mer correlation makes the EFFECTIVE key space
//                  far smaller than the representable one -- measured 1066x smaller
//                  at this geometry. 2 bits x 15 events looked optimal (89.9%) under
//                  an i.i.d. signal model and collapses to 0.8% once adjacent events
//                  are correlated as a real pore makes them. 3 x 15 was chosen over
//                  the nominal optimum 3 x 13 (76.1%) for headroom: random DNA has
//                  no repeats, a real genome is ~50% repetitive, and 3 x 13 leaves
//                  only 25% margin against the occurrence cap where 3 x 15 leaves
//                  30x.
//
//   window 10      Not a tuning choice, a memory constraint. At one entry per base
//                  and 8-byte entries at 0.5 load factor a mammalian index is
//                  ~49 GB at w=1. w=10 keeps 18.2% of positions for ~9.0 GB.
//                  It costs recall -- 68.2% of available seeds at w=1 falls to
//                  41.4% at w=10 -- which is affordable because locus
//                  identification, not seed recall, is what the decision needs:
//                  200/200 correct loci with a 29.6x diagonal vote margin.
//
// Changing any of these invalidates the off-target separation that sets the unblock
// threshold, so test/index_test.cpp asserts them.
struct QuantConfig {
  // ~10 samples per base at 4 kHz and ~400 b/s. Averaging this many raw samples
  // gives one event. Fixed-window downsampling, not true event segmentation:
  // segmentation is more faithful and much more expensive, and whether it is
  // worth the cycles is an experiment, not an assumption.
  std::uint32_t samples_per_event = 10;

  // Bits per quantised event. 3 bits = 8 levels. FROZEN.
  std::uint32_t bits_per_event = 3;

  // Events combined into one key. 15 at 3 bits = 45-bit keys. FROZEN.
  std::uint32_t events_per_key = 15;

  // z-scores are clipped to +/- this before bucketing, so outliers cannot drag
  // the whole scale.
  float z_clip = 3.0f;

  // ADAPTIVE (equal-occupancy) BUCKET BOUNDARIES.
  //
  // Uniform bucketing over +/-z_clip wastes the alphabet, because a real pore model's
  // normalised level distribution is nowhere near uniform. Measured on Icarust's R10
  // 9-mer table over human chr20, uniform bucketing into 8 levels gives occupancies
  //     0:0.0%  1:5.0%  2:28.6%  3:12.0%  4:27.3%  5:21.7%  6:5.1%  7:0.3%
  // One bucket of eight is never used and another takes 0.3%, so the quantiser
  // delivers about 2.3 bits per event instead of 3. Key space is the scarce resource
  // in this design, and that is 0.7 bits per event thrown away.
  //
  // Boundaries placed at the empirical QUANTILES of the level distribution make every
  // bucket equally likely by construction, so entropy is exactly bits_per_event. That
  // is the information-theoretic optimum for a fixed level count, and strictly better
  // than a two-piece fine/coarse split when the distribution is actually available --
  // which it is, since it comes from the pore model.
  //
  // Both sides MUST use the same boundaries. Deriving them from the pore model rather
  // than from any particular read guarantees that.
  //
  // Empty (n_boundaries == 0) means uniform bucketing, which stays the default so
  // existing measurements remain reproducible.
  static constexpr std::size_t kMaxBoundaries = 31;
  std::array<float, kMaxBoundaries> boundaries{};
  std::uint32_t n_boundaries = 0;

  [[nodiscard]] bool adaptive() const noexcept { return n_boundaries != 0; }

  // Minimizer window, in keys. 1 disables subsampling and indexes every key.
  // FROZEN at 10: this is what brings a mammalian index to ~9.0 GB.
  std::uint32_t minimizer_window = 10;

  [[nodiscard]] bool valid() const noexcept {
    if (!(samples_per_event > 0 && bits_per_event > 0 && bits_per_event <= 8 &&
          events_per_key > 0 && bits_per_event * events_per_key <= 64 &&
          z_clip > 0.0f && minimizer_window > 0)) {
      return false;
    }
    // A partial boundary table would silently mis-bucket, so require exactly
    // levels()-1 interior boundaries or none at all.
    if (n_boundaries != 0 && n_boundaries + 1 != levels()) return false;
    if (n_boundaries > kMaxBoundaries) return false;
    return true;
  }
  [[nodiscard]] std::uint32_t levels() const noexcept { return 1u << bits_per_event; }
  [[nodiscard]] std::uint32_t key_bits() const noexcept {
    return bits_per_event * events_per_key;
  }
};

// One quantised event: a small integer, so a whole key packs into a uint64.
using QEvent = std::uint8_t;

// Maps z onto [0, levels).
//
// Adaptive path: a linear scan of at most levels()-1 ascending boundaries. For 8
// levels that is 7 float compares, which is a few cycles against a per-chunk budget
// of hundreds of microseconds, and it buys back 0.7 bits per event.
[[nodiscard]] inline QEvent quantise_z(float z, const QuantConfig& cfg) noexcept {
  if (cfg.n_boundaries != 0) {
    std::uint32_t b = 0;
    while (b < cfg.n_boundaries && z >= cfg.boundaries[b]) ++b;
    return static_cast<QEvent>(b);
  }
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

// The edges of the bucket containing z, in z units.
inline void bucket_edges(float z, const QuantConfig& cfg, float& lo, float& hi) noexcept {
  if (cfg.n_boundaries != 0) {
    const auto b = static_cast<std::uint32_t>(quantise_z(z, cfg));
    lo = b == 0 ? -cfg.z_clip : cfg.boundaries[b - 1];
    hi = b >= cfg.n_boundaries ? cfg.z_clip : cfg.boundaries[b];
    return;
  }
  const float width = 2.0f * cfg.z_clip / static_cast<float>(cfg.levels());
  const auto b = static_cast<float>(quantise_z(z, cfg));
  lo = -cfg.z_clip + b * width;
  hi = lo + width;
}

// Where z sits inside its bucket: 0.0 exactly on a boundary, 1.0 dead centre.
//
// This is the quantity multi-probe is built on. Measured: 94% of per-event bucket
// disagreements occur within 0.3 of a boundary, and across 16000 events not one was
// ever off by more than a single bucket. So an event near a boundary is the one worth
// probing both ways, and an event near the centre never needs it.
//
// Normalised by the LOCAL bucket width, so it remains comparable across buckets when
// adaptive boundaries make them unequal in z.
[[nodiscard]] inline float boundary_distance(float z, const QuantConfig& cfg) noexcept {
  float lo = 0.0f, hi = 0.0f;
  bucket_edges(z, cfg, lo, hi);
  const float width = hi - lo;
  if (!(width > 0.0f)) return 1.0f;
  const float clamped = std::clamp(z, lo, hi);
  return 2.0f * std::min(clamped - lo, hi - clamped) / width;
}

// The neighbouring bucket on the side of the nearest boundary -- the only plausible
// alternative, since disagreements are never off by more than one.
[[nodiscard]] inline QEvent neighbour_bucket(float z, const QuantConfig& cfg) noexcept {
  float lo = 0.0f, hi = 0.0f;
  bucket_edges(z, cfg, lo, hi);
  const auto bucket = static_cast<int>(quantise_z(z, cfg));
  const float clamped = std::clamp(z, lo, hi);
  const int nb = (clamped - lo) < (hi - clamped) ? bucket - 1 : bucket + 1;
  return static_cast<QEvent>(std::clamp(nb, 0, static_cast<int>(cfg.levels()) - 1));
}

// Fits equal-occupancy boundaries from observed z values.
//
// Sorts a copy and takes the levels()-1 interior quantiles, so each bucket receives
// 1/levels of the mass and entropy is exactly bits_per_event. Offline only -- this is
// an index-build step, never on the decision path.
//
// Pass the pore model's own normalised levels, not a read's: both sides must bucket
// identically, and the model is the one thing both sides share.
[[nodiscard]] inline bool fit_adaptive_boundaries(std::span<const float> z_samples,
                                                  QuantConfig& cfg) {
  const std::uint32_t n_edges = cfg.levels() - 1;
  if (z_samples.size() < cfg.levels() || n_edges > QuantConfig::kMaxBoundaries) {
    return false;
  }
  std::vector<float> v(z_samples.begin(), z_samples.end());
  std::sort(v.begin(), v.end());
  for (std::uint32_t e = 0; e < n_edges; ++e) {
    const double q = static_cast<double>(e + 1) / static_cast<double>(cfg.levels());
    auto idx = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
    if (idx >= v.size()) idx = v.size() - 1;
    cfg.boundaries[e] = v[idx];
  }
  // Must be strictly ascending, or quantise_z's scan would produce empty buckets.
  for (std::uint32_t e = 1; e < n_edges; ++e) {
    if (!(cfg.boundaries[e] > cfg.boundaries[e - 1])) return false;
  }
  cfg.n_boundaries = n_edges;
  return true;
}

// Downsample raw signal into quantised events. Appends to `out`, which the caller
// reuses across chunks so the hot path never allocates.
// `out_z`, when non-null, receives the pre-quantisation z of each event. Multi-probe
// needs it to rank which events sit nearest a bucket boundary, and it cannot be
// recovered from the quantised value afterwards.
inline void quantise_signal(std::span<const std::int16_t> raw, const SignalScaling& sc,
                            const QuantConfig& cfg, std::vector<QEvent>& out,
                            std::vector<float>* out_z = nullptr) {
  if (!sc.valid() || !cfg.valid()) return;
  const std::size_t w = cfg.samples_per_event;
  if (raw.size() < w) return;

  const std::size_t n_events = raw.size() / w;
  out.reserve(out.size() + n_events);
  if (out_z != nullptr) out_z->reserve(out_z->size() + n_events);
  for (std::size_t e = 0; e < n_events; ++e) {
    // Mean of the window in float. int32 accumulation would be enough for int16
    // but float keeps this identical to the reference implementation in tests.
    float sum = 0.0f;
    const std::size_t base = e * w;
    for (std::size_t i = 0; i < w; ++i) sum += sc.apply(raw[base + i]);
    const float z = sum / static_cast<float>(w);
    if (out_z != nullptr) out_z->push_back(z);
    out.push_back(quantise_z(z, cfg));
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
