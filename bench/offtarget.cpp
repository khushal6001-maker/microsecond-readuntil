// bench/offtarget.cpp
//
// Re-derives the unblock threshold at the LOCKED geometry, on real DNA.
//
// Why this had to be redone: the >=4 vote rule was measured against the previous 3x15
// freeze, on synthetic signal, before two normalisation bugs were found. Recall moved
// by 5 points and per-event brittleness turned out to be 19.9% on real data against
// 6.89% on synthetic, so the on-target vote distribution has certainly shifted. A
// decision threshold carried over from a superseded measurement is worth nothing.
//
// The off-target control is a SHUFFLE of the reference, not random DNA. Shuffling
// preserves base composition and therefore GC content, so the off-target reads are
// statistically as similar to the target as possible while sharing no sequence. A real
// off-target organism would differ in composition and k-mer usage, which makes
// discrimination EASIER -- so this control is conservative, and deliberately so.
//
// Usage:
//   mru_offtarget --model R10_model.tsv --fasta ref.fa [--bases N] [--reads N]
//                 [--noise pA] [--probes 1|2|4]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "index/batched_probe.hpp"
#include "index/minimizer_index.hpp"
#include "index/quantise.hpp"

namespace {

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
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (out.size() >= max_bases) break;
      }
    }
  }
  return !out.empty();
}

int base_code(char c) {
  switch (c) {
    case 'C': return 1;
    case 'G': return 2;
    case 'T': return 3;
    default: return 0;
  }
}

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

struct Vote {
  std::size_t candidates = 0;
  std::size_t best = 0;
  std::size_t second = 0;
  std::int64_t best_diag = 0;
};

Vote vote(const mru::MinimizerIndex& idx, const std::vector<std::int16_t>& raw,
          const mru::QuantConfig& cfg, int probes, mru::ProbeScratch& scratch) {
  Vote v;
  const auto sc = mru::scaling_from_samples(raw);
  (void)mru::match_signal(idx, raw, sc, cfg, scratch, probes);

  std::vector<std::int64_t> diags;
  for (const mru::SeedMatches& m : scratch.matches) {
    for (std::uint32_t j = 0; j < m.count; ++j) {
      diags.push_back(static_cast<std::int64_t>(m.positions[j]) -
                      static_cast<std::int64_t>(m.seed_offset));
    }
  }
  v.candidates = diags.size();
  if (diags.empty()) return v;
  std::sort(diags.begin(), diags.end());
  std::size_t run = 1;
  for (std::size_t i = 1; i <= diags.size(); ++i) {
    if (i < diags.size() && diags[i] == diags[i - 1]) {
      ++run;
      continue;
    }
    if (run > v.best) {
      v.second = v.best;
      v.best = run;
      v.best_diag = diags[i - 1];
    } else if (run > v.second) {
      v.second = run;
    }
    run = 1;
  }
  return v;
}

double pct(std::vector<std::size_t> v, double q) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  auto i = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
  if (i >= v.size()) i = v.size() - 1;
  return static_cast<double>(v[i]);
}

}  // namespace

int main(int argc, char** argv) {
  std::string model_path, fasta_path;
  std::size_t max_bases = 5'000'000;
  std::size_t n_reads = 300;
  std::size_t window_bases = 450;
  float noise_pa = 2.0f;
  int probes = mru::kDefaultProbeBudget;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto v = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "--model") { const char* x = v(); if (!x) return 2; model_path = x; }
    else if (a == "--fasta") { const char* x = v(); if (!x) return 2; fasta_path = x; }
    else if (a == "--bases") { const char* x = v(); if (!x) return 2; max_bases = static_cast<std::size_t>(std::atoll(x)); }
    else if (a == "--reads") { const char* x = v(); if (!x) return 2; n_reads = static_cast<std::size_t>(std::atoll(x)); }
    else if (a == "--noise") { const char* x = v(); if (!x) return 2; noise_pa = std::strtof(x, nullptr); }
    else if (a == "--window-bases") { const char* x = v(); if (!x) return 2; window_bases = static_cast<std::size_t>(std::atoll(x)); }
    else if (a == "--probes") { const char* x = v(); if (!x) return 2; probes = std::atoi(x); }
    else { std::printf("usage: %s --model M.tsv --fasta R.fa [--bases N] [--reads N] [--noise pA] [--probes N]\n", argv[0]); return 2; }
  }
  if (model_path.empty() || fasta_path.empty()) {
    std::printf("--model and --fasta are required\n");
    return 2;
  }

  std::vector<float> levels;
  if (!load_model_tsv(model_path, levels)) { std::printf("bad model\n"); return 1; }
  std::uint32_t k = 0;
  for (std::size_t p = 1; p < levels.size(); p *= 4) ++k;
  std::string dna;
  if (!load_fasta_acgt(fasta_path, max_bases, dna)) { std::printf("bad fasta\n"); return 1; }

  // Off-target: a shuffle of the same sequence. Same composition, no shared sequence.
  std::string shuffled = dna;
  {
    std::mt19937_64 rng(20261005);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
  }

  const mru::QuantConfig cfg;  // LOCKED defaults
  std::printf("locked geometry: %u bits x %u events = %u-bit keys, window %u, %s\n",
              cfg.bits_per_event, cfg.events_per_key, cfg.key_bits(),
              cfg.minimizer_window, cfg.adaptive() ? "adaptive" : "uniform");
  std::printf("reference %zu bases, %zu reads of %zu bases each, noise %.1f pA, "
              "%d probes/seed\n",
              dna.size(), n_reads, window_bases, static_cast<double>(noise_pa), probes);

  mru::PoreModel model;
  if (!model.load_levels(levels)) { std::printf("model load failed\n"); return 1; }
  std::vector<mru::QEvent> ref_events;
  if (!model.reference_to_events(dna, cfg, ref_events)) {
    std::printf("reference_to_events failed\n");
    return 1;
  }
  std::vector<mru::SeedHash> all_seeds, minimizers;
  mru::hash_all_keys(ref_events, cfg, all_seeds);
  mru::select_minimizers(all_seeds, cfg.minimizer_window, minimizers);
  mru::MinimizerIndex idx;
  idx.build(minimizers);
  std::printf("index: %zu minimizers of %zu keys, %llu distinct, %llu capped, "
              "%llu insert failures\n\n",
              minimizers.size(), all_seeds.size(),
              static_cast<unsigned long long>(idx.distinct_keys()),
              static_cast<unsigned long long>(idx.capped_seeds()),
              static_cast<unsigned long long>(idx.insert_failures()));

  mru::ProbeScratch scratch;
  scratch.reserve_for(window_bases * cfg.samples_per_event, cfg, probes);

  std::vector<std::size_t> on_votes, off_votes, on_cands, off_cands;
  std::size_t diag_ok = 0;

  const std::size_t span = dna.size() - window_bases - 2 * k;
  for (std::size_t r = 0; r < n_reads; ++r) {
    const std::size_t d = k + span * (r + 1) / (n_reads + 1);
    const auto raw = signal_from_dna(levels, k, dna.data() + d, window_bases,
                                     cfg.samples_per_event, noise_pa, 5000 + r);
    if (raw.empty()) continue;
    const Vote v = vote(idx, raw, cfg, probes, scratch);
    on_votes.push_back(v.best);
    on_cands.push_back(v.candidates);
    if (v.best > 0 && std::llabs(v.best_diag - static_cast<std::int64_t>(d)) <= 2) {
      ++diag_ok;
    }
  }
  for (std::size_t r = 0; r < n_reads; ++r) {
    const std::size_t d = k + span * (r + 1) / (n_reads + 1);
    const auto raw = signal_from_dna(levels, k, shuffled.data() + d, window_bases,
                                     cfg.samples_per_event, noise_pa, 90000 + r);
    if (raw.empty()) continue;
    const Vote v = vote(idx, raw, cfg, probes, scratch);
    off_votes.push_back(v.best);
    off_cands.push_back(v.candidates);
  }

  std::printf("ON-target : cands med %.0f | votes min %.0f p05 %.0f med %.0f max %.0f | "
              "correct diagonal %zu/%zu\n",
              pct(on_cands, 0.5), pct(on_votes, 0.0), pct(on_votes, 0.05),
              pct(on_votes, 0.5), pct(on_votes, 1.0), diag_ok, on_votes.size());
  std::printf("OFF-target: cands med %.0f | votes med %.0f p95 %.0f p99 %.0f max %.0f\n",
              pct(off_cands, 0.5), pct(off_votes, 0.5), pct(off_votes, 0.95),
              pct(off_votes, 0.99), pct(off_votes, 1.0));

  // Sweep thresholds: the operating point is a choice, so show the curve rather than
  // one number. A late unblock costs yield; a wrong accept costs a pore.
  std::printf("\n%10s | %8s %8s | %s\n", "threshold", "TPR", "FPR", "note");
  const auto off_max = off_votes.empty() ? 0 : *std::max_element(off_votes.begin(), off_votes.end());
  for (std::size_t t = 2; t <= std::max<std::size_t>(off_max + 2, 10); ++t) {
    std::size_t tp = 0, fp = 0;
    for (std::size_t v : on_votes) if (v >= t) ++tp;
    for (std::size_t v : off_votes) if (v >= t) ++fp;
    const double tpr = 100.0 * static_cast<double>(tp) / static_cast<double>(std::max<std::size_t>(1, on_votes.size()));
    const double fpr = 100.0 * static_cast<double>(fp) / static_cast<double>(std::max<std::size_t>(1, off_votes.size()));
    const char* note = fp == 0 ? (t == off_max + 1 ? "<- lowest zero-FP threshold" : "") : "";
    std::printf("%10zu | %7.1f%% %7.1f%% | %s\n", t, tpr, fpr, note);
  }
  return 0;
}
