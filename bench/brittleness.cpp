// bench/brittleness.cpp
//
// Per-event bucket disagreement and multi-probe payoff, measured on REAL DNA with a
// REAL pore model, under both uniform and adaptive quantisation.
//
// This is the measurement that decides the key geometry. Cap survival turned out to be
// nearly geometry-independent on real sequence (84.1% to 88.6% across every candidate),
// so the choice rests entirely on recall -- and recall has only ever been measured under
// uniform bucketing on synthetic signal.
//
// The specific worry: adaptive boundaries are placed at quantiles, so they are NARROWER
// where the level distribution is dense, which is where most events live. Narrower
// buckets mean a fixed normalisation error flips a bucket more often. So adaptive
// quantisation buys key space (entropy 2.34 -> 2.99 bits) and may give some of it back
// as brittleness. Only measurement settles the net.
//
// Mechanism being reproduced: the reference is normalised by the pore model's own
// statistics, while a read can only normalise by its own samples. That mismatch is the
// dominant error, and it is why query and reference disagree at all.
//
// Usage:
//   mru_brittleness --model R10_model.tsv --fasta ref.fa [--bases N] [--noise pA]

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "index/batched_probe.hpp"
#include "index/quantise.hpp"

namespace {

// Levels are scaled into int16 before noise is added, so the synthetic raw signal has
// the same shape a sequencer would deliver. The absolute scale is irrelevant because
// everything downstream normalises.
constexpr float kPaToAdc = 100.0f;

bool load_model_tsv(const std::string& path, std::vector<float>& levels) {
  std::ifstream f(path);
  if (!f) return false;
  levels.clear();
  levels.reserve(262144);
  std::string line;
  while (std::getline(f, line)) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    levels.push_back(std::strtof(line.c_str() + tab + 1, nullptr));
  }
  std::size_t p = 1;
  while (p < levels.size()) p *= 4;
  return !levels.empty() && p == levels.size();
}

bool load_fasta_acgt(const std::string& path, std::size_t max_bases, std::string& out) {
  std::ifstream f(path);
  if (!f) return false;
  out.clear();
  out.reserve(std::min<std::size_t>(max_bases, 1u << 28));
  std::string line;
  while (out.size() < max_bases && std::getline(f, line)) {
    if (!line.empty() && line[0] == '>') continue;
    for (char c : line) {
      if (c == 'A' || c == 'C' || c == 'G' || c == 'T' || c == 'a' || c == 'c' ||
          c == 'g' || c == 't') {
        out.push_back(c);
        if (out.size() >= max_bases) break;
      }
    }
  }
  return !out.empty();
}

int base_code(char c) {
  switch (c) {
    case 'C': case 'c': return 1;
    case 'G': case 'g': return 2;
    case 'T': case 't': return 3;
    default: return 0;
  }
}

// Raw signal for a DNA region, via the model: slide the k-mer window, hold each level
// for samples_per_event samples, add Gaussian noise.
std::vector<std::int16_t> signal_from_dna(const std::vector<float>& levels,
                                          std::uint32_t k, const char* dna,
                                          std::size_t len, std::uint32_t spe,
                                          float noise_pa, std::uint64_t seed) {
  std::vector<std::int16_t> raw;
  if (len < k) return raw;
  raw.reserve((len - k + 1) * spe);
  std::mt19937_64 rng(seed);
  std::normal_distribution<float> noise(0.0f, noise_pa * kPaToAdc);
  std::uint64_t kmer = 0;
  std::uint32_t have = 0;
  const std::uint64_t mask = (std::uint64_t{1} << (2 * k)) - 1;
  for (std::size_t i = 0; i < len; ++i) {
    kmer = ((kmer << 2) | static_cast<std::uint64_t>(base_code(dna[i]))) & mask;
    if (++have < k) continue;
    const float level = levels[kmer] * kPaToAdc;
    for (std::uint32_t t = 0; t < spe; ++t) {
      raw.push_back(
          static_cast<std::int16_t>(std::clamp(level + noise(rng), -32000.0f, 32000.0f)));
    }
  }
  return raw;
}

struct Result {
  double disagree = 0.0;
  double off_by_one = 0.0;
  double exact = 0.0;
  double p2 = 0.0;
  double p4 = 0.0;
  std::size_t events = 0;
  std::size_t keys = 0;
};

// Compares reference-side events (normalised by the model) against query-side events
// (normalised per-read) over many short windows, which is the realistic mismatch.
Result measure(const std::vector<float>& levels, std::uint32_t k, const std::string& dna,
               const std::vector<mru::QEvent>& ref_events, const mru::QuantConfig& cfg,
               std::size_t window_bases, std::size_t n_windows, float noise_pa) {
  Result r;
  std::size_t disagreements = 0, off1 = 0, exact = 0, p2 = 0, p4 = 0;
  std::vector<mru::QEvent> trial(cfg.events_per_key);

  for (std::size_t w = 0; w < n_windows; ++w) {
    // Spread windows across the reference; keep clear of both ends.
    const std::size_t span = dna.size() - window_bases - 2 * k;
    const std::size_t d = k + span * (w + 1) / (n_windows + 1);

    const auto raw = signal_from_dna(levels, k, dna.data() + d, window_bases,
                                     cfg.samples_per_event, noise_pa, 9001 + w);
    if (raw.empty()) continue;

    // A read has only its own samples to estimate shift and scale from.
    const auto sc = mru::scaling_from_samples(raw);
    std::vector<mru::QEvent> q_ev;
    std::vector<float> q_z;
    mru::quantise_signal(raw, sc, cfg, q_ev, &q_z);
    if (q_ev.size() < cfg.events_per_key) continue;

    // Query event i corresponds to reference event d+i: both windows start their k-mer
    // at the same base.
    const std::size_t n = std::min(q_ev.size(), ref_events.size() - d);
    if (n < cfg.events_per_key) continue;
    r.events += n;

    for (std::size_t i = 0; i < n; ++i) {
      if (ref_events[d + i] == q_ev[i]) continue;
      ++disagreements;
      const int diff = static_cast<int>(ref_events[d + i]) - static_cast<int>(q_ev[i]);
      if (diff == 1 || diff == -1) ++off1;
    }

    const std::size_t n_keys = n - cfg.events_per_key + 1;
    r.keys += n_keys;
    for (std::size_t i = 0; i < n_keys; ++i) {
      const std::uint64_t want = mru::pack_key(ref_events.data() + d + i, cfg);
      if (mru::pack_key(q_ev.data() + i, cfg) == want) {
        ++exact;
        ++p2;
        ++p4;
        continue;
      }
      // Rank marginality from query-side information only, as a real read must.
      std::uint32_t m0 = 0, m1 = 0;
      float d0 = 2.0f, d1 = 2.0f;
      for (std::uint32_t e = 0; e < cfg.events_per_key; ++e) {
        const float bd = mru::boundary_distance(q_z[i + e], cfg);
        if (bd < d0) { d1 = d0; m1 = m0; d0 = bd; m0 = e; }
        else if (bd < d1) { d1 = bd; m1 = e; }
      }
      for (std::uint32_t e = 0; e < cfg.events_per_key; ++e) trial[e] = q_ev[i + e];
      const mru::QEvent nb0 = mru::neighbour_bucket(q_z[i + m0], cfg);
      trial[m0] = nb0;
      if (mru::pack_key(trial.data(), cfg) == want) {
        ++p2;
        ++p4;
        continue;
      }
      bool ok = false;
      const mru::QEvent nb1 = mru::neighbour_bucket(q_z[i + m1], cfg);
      for (int fa = 0; fa < 2 && !ok; ++fa) {
        for (int fb = 0; fb < 2 && !ok; ++fb) {
          if (fa == 0 && fb == 0) continue;
          for (std::uint32_t e = 0; e < cfg.events_per_key; ++e) trial[e] = q_ev[i + e];
          if (fa == 1) trial[m0] = nb0;
          if (fb == 1) trial[m1] = nb1;
          if (mru::pack_key(trial.data(), cfg) == want) ok = true;
        }
      }
      if (ok) ++p4;
    }
  }

  const auto ev = static_cast<double>(std::max<std::size_t>(1, r.events));
  const auto ky = static_cast<double>(std::max<std::size_t>(1, r.keys));
  r.disagree = static_cast<double>(disagreements) / ev;
  r.off_by_one =
      disagreements == 0 ? 0.0
                         : static_cast<double>(off1) / static_cast<double>(disagreements);
  r.exact = 100.0 * static_cast<double>(exact) / ky;
  r.p2 = 100.0 * static_cast<double>(p2) / ky;
  r.p4 = 100.0 * static_cast<double>(p4) / ky;
  return r;
}

void fit_boundaries_from_model(const std::vector<float>& levels, mru::QuantConfig& cfg) {
  double mean = 0.0;
  for (float v : levels) mean += static_cast<double>(v);
  mean /= static_cast<double>(levels.size());
  double var = 0.0;
  for (float v : levels) {
    const double d = static_cast<double>(v) - mean;
    var += d * d;
  }
  var /= static_cast<double>(levels.size());
  const double sd = std::max(1e-6, std::sqrt(var));
  std::vector<float> zs;
  zs.reserve(levels.size());
  for (float v : levels) {
    zs.push_back(static_cast<float>((static_cast<double>(v) - mean) / sd));
  }
  (void)mru::fit_adaptive_boundaries(zs, cfg);
}

}  // namespace

int main(int argc, char** argv) {
  std::string model_path, fasta_path;
  std::size_t max_bases = 5'000'000;
  float noise_pa = 2.0f;
  std::size_t window_bases = 450;  // ~2.5 chunks of signal at 400 b/s
  std::size_t n_windows = 400;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto v = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "--model") { const char* x = v(); if (!x) return 2; model_path = x; }
    else if (a == "--fasta") { const char* x = v(); if (!x) return 2; fasta_path = x; }
    else if (a == "--bases") { const char* x = v(); if (!x) return 2; max_bases = static_cast<std::size_t>(std::atoll(x)); }
    else if (a == "--noise") { const char* x = v(); if (!x) return 2; noise_pa = std::strtof(x, nullptr); }
    else if (a == "--windows") { const char* x = v(); if (!x) return 2; n_windows = static_cast<std::size_t>(std::atoll(x)); }
    else { std::printf("usage: %s --model M.tsv --fasta R.fa [--bases N] [--noise pA] [--windows N]\n", argv[0]); return 2; }
  }
  if (model_path.empty() || fasta_path.empty()) {
    std::printf("--model and --fasta are required\n");
    return 2;
  }

  std::vector<float> levels;
  if (!load_model_tsv(model_path, levels)) {
    std::printf("cannot load model %s\n", model_path.c_str());
    return 1;
  }
  std::uint32_t k = 0;
  for (std::size_t p = 1; p < levels.size(); p *= 4) ++k;
  std::string dna;
  if (!load_fasta_acgt(fasta_path, max_bases, dna)) {
    std::printf("cannot load fasta %s\n", fasta_path.c_str());
    return 1;
  }

  std::printf("model %zu levels (k=%u), reference %zu ACGT bases\n", levels.size(), k,
              dna.size());
  std::printf("read window %zu bases, %zu windows, per-sample noise %.1f pA\n\n",
              window_bases, n_windows, static_cast<double>(noise_pa));

  std::printf("%4s %3s %6s %9s | %8s %9s | %7s %7s %7s | %s\n", "bits", "ev", "kbits",
              "quantiser", "disagr", "off-by-1", "exact", "2-probe", "4-probe", "p4 gain");

  struct Cand { std::uint32_t bits, events; };
  const Cand cands[] = {{3, 13}, {3, 15}, {3, 18}, {4, 13}, {4, 15}};

  for (const Cand& c : cands) {
    for (int adaptive = 0; adaptive < 2; ++adaptive) {
      mru::QuantConfig cfg;
      cfg.bits_per_event = c.bits;
      cfg.events_per_key = c.events;
      cfg.minimizer_window = 1;  // isolate brittleness from minimizer selection
      if (adaptive != 0) fit_boundaries_from_model(levels, cfg);
      if (!cfg.valid()) continue;

      mru::PoreModel model;
      if (!model.load_levels(levels)) continue;
      std::vector<mru::QEvent> ref_events;
      if (!model.reference_to_events(dna, cfg, ref_events)) continue;

      const Result r =
          measure(levels, k, dna, ref_events, cfg, window_bases, n_windows, noise_pa);
      std::printf("%4u %3u %6u %9s | %7.2f%% %8.1f%% | %6.1f%% %6.1f%% %6.1f%% | %+6.1f\n",
                  c.bits, c.events, cfg.key_bits(), adaptive != 0 ? "adaptive" : "uniform",
                  100.0 * r.disagree, 100.0 * r.off_by_one, r.exact, r.p2, r.p4,
                  r.p4 - r.exact);
    }
  }
  return 0;
}
