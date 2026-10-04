// bench/real_reference.cpp
//
// Re-measures the index against REAL DNA and a REAL pore model.
//
// Why this exists: every index measurement so far used synthetic data, and the two
// simplifications it made both bias the result in the same direction.
//
//   random DNA has no repeats     a mammalian genome is ~50% repetitive, and repeats
//                                 produce identical k-mer contexts and therefore
//                                 identical keys, so real effective key space is
//                                 SMALLER and occupancy HIGHER than measured
//   synthetic i.i.d. levels       a real pore model has its own structure; nearby
//                                 k-mers do not have independent levels
//
// The first of those is the stated reason 3x15 was chosen over the nominal optimum
// 3x13: 30x headroom against the occurrence cap instead of 25%. That reasoning has
// never been tested. This tests it.
//
// Not a ctest: it needs a reference FASTA and an ONT pore model, neither of which is
// in the repo or available in CI.
//
// Usage:
//   mru_real_reference --model R10_model.tsv --fasta hg38_chr20.fa [--bases N]
//                      [--bits 3] [--events 15] [--window 10]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "index/batched_probe.hpp"
#include "index/minimizer_index.hpp"
#include "index/quantise.hpp"

namespace {

// Poisson occupancy: drawing n positions from a space of K distinct keys gives
// E[distinct] = K(1 - exp(-n/K)). Measure distinct and n, solve for K.
//
// Duplicated from test/index_test.cpp rather than shared, because this is a one-off
// bench tool and the test is the authority on the method. If a third caller appears,
// it belongs in the library.
double effective_key_space(std::size_t n_positions, std::size_t distinct) {
  const auto n = static_cast<double>(n_positions);
  const auto d = static_cast<double>(distinct);
  if (d <= 0.0) return 0.0;
  if (d >= n * 0.9999) return 1e18;  // no measurable collisions; space is >> n
  double lo = d, hi = 1e18;
  for (int it = 0; it < 200; ++it) {
    const double mid = 0.5 * (lo + hi);
    if (mid * (1.0 - std::exp(-n / mid)) < d) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return 0.5 * (lo + hi);
}

// ONT 9-mer level table: "<kmer>\t<level_pA>" per line, 4^k lines in lexicographic
// ACGT order -- which is exactly the 2-bit packing order PoreModel::load_levels
// expects, so file order is index order and no k-mer parsing is needed.
bool load_model_tsv(const std::string& path, std::vector<float>& levels,
                    std::string& err) {
  std::ifstream f(path);
  if (!f) {
    err = "cannot open " + path;
    return false;
  }
  levels.clear();
  levels.reserve(262144);
  std::string line;
  std::size_t lineno = 0;
  while (std::getline(f, line)) {
    ++lineno;
    if (line.empty()) continue;
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      err = "no tab on line " + std::to_string(lineno);
      return false;
    }
    levels.push_back(std::strtof(line.c_str() + tab + 1, nullptr));
  }
  // Must be a power of four, or the k-mer order assumption is wrong.
  std::size_t p = 1;
  while (p < levels.size()) p *= 4;
  if (p != levels.size() || levels.empty()) {
    err = "level count " + std::to_string(levels.size()) + " is not a power of four";
    return false;
  }
  return true;
}

// Reads a FASTA and returns up to max_bases of ACGT, skipping headers and any
// non-ACGT (chromosome assemblies open with long telomeric N runs).
bool load_fasta_acgt(const std::string& path, std::size_t max_bases, std::string& out,
                     std::size_t& skipped_non_acgt, std::string& err) {
  std::ifstream f(path);
  if (!f) {
    err = "cannot open " + path;
    return false;
  }
  out.clear();
  out.reserve(std::min<std::size_t>(max_bases, 1u << 28));
  skipped_non_acgt = 0;
  std::string line;
  while (out.size() < max_bases && std::getline(f, line)) {
    if (!line.empty() && line[0] == '>') continue;
    for (char c : line) {
      switch (c) {
        case 'A': case 'a': case 'C': case 'c':
        case 'G': case 'g': case 'T': case 't':
          out.push_back(c);
          break;
        case '\r': case '\n':
          break;
        default:
          ++skipped_non_acgt;
          break;
      }
      if (out.size() >= max_bases) break;
    }
  }
  if (out.empty()) {
    err = "no ACGT found in " + path;
    return false;
  }
  return true;
}

[[maybe_unused]] std::size_t count_distinct(std::vector<std::uint64_t>& keys) {
  std::sort(keys.begin(), keys.end());
  return static_cast<std::size_t>(std::unique(keys.begin(), keys.end()) - keys.begin());
}

}  // namespace

int main(int argc, char** argv) {
  std::string model_path, fasta_path;
  std::size_t max_bases = 10'000'000;
  bool adaptive = false;
  mru::QuantConfig cfg;  // frozen defaults: 3 bits, 15 events, window 10

  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    const auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (k == "--model") {
      const char* v = val(); if (v == nullptr) return 2; model_path = v;
    } else if (k == "--fasta") {
      const char* v = val(); if (v == nullptr) return 2; fasta_path = v;
    } else if (k == "--bases") {
      const char* v = val(); if (v == nullptr) return 2;
      max_bases = static_cast<std::size_t>(std::atoll(v));
    } else if (k == "--bits") {
      const char* v = val(); if (v == nullptr) return 2;
      cfg.bits_per_event = static_cast<std::uint32_t>(std::atoi(v));
    } else if (k == "--events") {
      const char* v = val(); if (v == nullptr) return 2;
      cfg.events_per_key = static_cast<std::uint32_t>(std::atoi(v));
    } else if (k == "--adaptive") {
      adaptive = true;
    } else if (k == "--window") {
      const char* v = val(); if (v == nullptr) return 2;
      cfg.minimizer_window = static_cast<std::uint32_t>(std::atoi(v));
    } else {
      std::printf("usage: %s --model MODEL.tsv --fasta REF.fa [--bases N] "
                  "[--bits B] [--events E] [--window W]\n", argv[0]);
      return 2;
    }
  }
  if (model_path.empty() || fasta_path.empty()) {
    std::printf("--model and --fasta are required\n");
    return 2;
  }
  if (!cfg.valid()) {
    std::printf("invalid geometry\n");
    return 2;
  }

  std::string err;

  // --- pore model ----------------------------------------------------------
  std::vector<float> levels;
  if (!load_model_tsv(model_path, levels, err)) {
    std::printf("model: %s\n", err.c_str());
    return 1;
  }
  mru::PoreModel model;
  if (!model.load_levels(levels)) {
    std::printf("model: load_levels rejected %zu entries\n", levels.size());
    return 1;
  }
  // Equal-occupancy boundaries, fitted from the MODEL's normalised levels rather than
  // from any read: both sides must bucket identically, and the model is what they
  // share. reference_to_events normalises by the table's own mean/sd, so the same
  // transform is applied here.
  if (adaptive) {
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
    if (!mru::fit_adaptive_boundaries(zs, cfg)) {
      std::printf("fit_adaptive_boundaries failed\n");
      return 1;
    }
  }

  const auto lo = *std::min_element(levels.begin(), levels.end());
  const auto hi = *std::max_element(levels.begin(), levels.end());
  std::printf("pore model : %s\n", model_path.c_str());
  std::printf("             %zu levels, k=%u, range %.2f .. %.2f pA\n", levels.size(),
              model.k(), static_cast<double>(lo), static_cast<double>(hi));

  // --- reference -----------------------------------------------------------
  std::string dna;
  std::size_t skipped = 0;
  if (!load_fasta_acgt(fasta_path, max_bases, dna, skipped, err)) {
    std::printf("fasta: %s\n", err.c_str());
    return 1;
  }
  std::printf("reference  : %s\n", fasta_path.c_str());
  std::printf("             %zu ACGT bases used, %zu non-ACGT skipped (N runs)\n",
              dna.size(), skipped);
  std::printf("quantiser  : %s\n",
              cfg.adaptive() ? "ADAPTIVE (equal-occupancy)" : "uniform");
  std::printf("geometry   : %u bits x %u events = %u-bit keys, window %u\n\n",
              cfg.bits_per_event, cfg.events_per_key, cfg.key_bits(),
              cfg.minimizer_window);

  // --- reference DNA -> expected signal -> quantised events ----------------
  std::vector<mru::QEvent> events;
  if (!model.reference_to_events(dna, cfg, events)) {
    std::printf("reference_to_events failed (unloaded model?)\n");
    return 1;
  }
  std::printf("events     : %zu\n", events.size());
  if (events.size() < cfg.events_per_key) {
    std::printf("not enough events for one key\n");
    return 1;
  }

  // Bucket occupancy tells us whether the quantiser is using its alphabet or
  // collapsing a real model into a couple of levels, which is what wrecked the very
  // first synthetic measurement.
  std::vector<std::uint64_t> bucket_hist(cfg.levels(), 0);
  for (mru::QEvent e : events) {
    if (e < bucket_hist.size()) ++bucket_hist[e];
  }
  std::printf("bucket use : ");
  for (std::size_t b = 0; b < bucket_hist.size(); ++b) {
    std::printf("%zu:%.1f%% ", b,
                100.0 * static_cast<double>(bucket_hist[b]) /
                    static_cast<double>(events.size()));
  }
  double entropy = 0.0;
  for (std::uint64_t c : bucket_hist) {
    if (c == 0) continue;
    const double q = static_cast<double>(c) / static_cast<double>(events.size());
    entropy -= q * std::log2(q);
  }
  std::printf("\n             entropy %.2f of %u bits (%.0f%% of the alphabet used)\n",
              entropy, cfg.bits_per_event,
              100.0 * entropy / static_cast<double>(cfg.bits_per_event));

  // --- key diversity on real sequence --------------------------------------
  const std::size_t n_positions = events.size() - cfg.events_per_key + 1;
  std::vector<std::uint64_t> keys;
  keys.reserve(n_positions);
  for (std::size_t i = 0; i < n_positions; ++i) {
    keys.push_back(mru::pack_key(events.data() + i, cfg));
  }
  // Frequency distribution, measured BEFORE the sorted array is released.
  //
  // A fixed per-key cap keeps an arbitrary 8 occurrences of every key, including the
  // useless high-frequency ones. A frequency FILTER instead discards whole keys that
  // occur too often and keeps EVERY occurrence of the rest -- which is what RawHash2
  // does. Which is better depends entirely on how skewed the distribution is, so the
  // skew is measured rather than assumed.
  std::sort(keys.begin(), keys.end());
  std::vector<std::size_t> runs;
  runs.reserve(keys.size() / 4 + 1);
  for (std::size_t i = 0; i < keys.size();) {
    std::size_t j = i;
    while (j < keys.size() && keys[j] == keys[i]) ++j;
    runs.push_back(j - i);
    i = j;
  }
  const std::size_t distinct = runs.size();
  const std::size_t max_freq =
      runs.empty() ? 0 : *std::max_element(runs.begin(), runs.end());

  std::printf("\n--- key frequency distribution on real DNA ---\n");
  std::printf("most frequent key occurs %zu times\n", max_freq);
  std::printf("  %8s | %12s %8s | %12s %8s\n", "freq<=", "keys kept", "of all",
              "positions", "of all");
  for (std::size_t t : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8},
                        std::size_t{16}, std::size_t{32}, std::size_t{64},
                        std::size_t{128}, std::size_t{256}, std::size_t{1024}}) {
    std::size_t kk = 0, pp = 0;
    for (std::size_t r : runs) {
      if (r <= t) {
        ++kk;
        pp += r;
      }
    }
    std::printf("  %8zu | %12zu %7.1f%% | %12zu %7.1f%%\n", t, kk,
                100.0 * static_cast<double>(kk) / static_cast<double>(distinct), pp,
                100.0 * static_cast<double>(pp) / static_cast<double>(n_positions));
  }
  runs.clear();
  runs.shrink_to_fit();
  keys.clear();
  keys.shrink_to_fit();

  const double representable = std::pow(2.0, static_cast<double>(cfg.key_bits()));
  const double keff = effective_key_space(n_positions, distinct);

  std::printf("\n--- key diversity on real DNA ---\n");
  std::printf("positions           : %zu\n", n_positions);
  std::printf("distinct keys       : %zu (%.2f%% unique)\n", distinct,
              100.0 * static_cast<double>(distinct) / static_cast<double>(n_positions));
  std::printf("representable space : %.3e\n", representable);
  std::printf("effective space     : %.3e  (%.0fx smaller than representable)\n", keff,
              representable / std::max(1.0, keff));

  // --- index at the frozen geometry ----------------------------------------
  std::vector<mru::SeedHash> all_seeds, minimizers;
  mru::hash_all_keys(events, cfg, all_seeds);
  mru::select_minimizers(all_seeds, cfg.minimizer_window, minimizers);

  mru::MinimizerIndex idx;
  idx.build(minimizers);

  const double kept = all_seeds.empty()
                          ? 0.0
                          : static_cast<double>(minimizers.size()) /
                                static_cast<double>(all_seeds.size());

  std::printf("\n--- index at this geometry ---\n");
  std::printf("minimizers kept     : %zu of %zu (%.1f%%)\n", minimizers.size(),
              all_seeds.size(), 100.0 * kept);
  std::printf("distinct keys in idx: %llu, capped %llu, insert failures %llu\n",
              static_cast<unsigned long long>(idx.distinct_keys()),
              static_cast<unsigned long long>(idx.capped_seeds()),
              static_cast<unsigned long long>(idx.insert_failures()));
  std::printf("slots %zu, load %.2f, %.1f MiB\n", idx.capacity(), idx.load_factor(),
              static_cast<double>(idx.capacity() * sizeof(std::uint64_t)) / (1024.0 * 1024.0));

  // --- cap behaviour, measured rather than extrapolated ---------------------
  //
  // This replaces a projection that was mathematically invalid. The previous version
  // fitted an "effective key space" K from E[distinct] = K(1-exp(-n/K)) and projected
  // genome-scale occupancy from it. That model assumes every key is equally likely;
  // real DNA is wildly skewed -- 91.8% of keys occur exactly once while the most
  // frequent occurs 4211 times. The diagnostic that killed it: the fitted K tracked
  // reference size instead of converging (5.0e6, 1.2e7, 2.6e7, 3.7e7 at 1, 3, 10 and
  // 30 Mbp), and a genuine fixed key space would give one value at every size.
  //
  // The model-free quantity is the fraction of POSITIONS whose key occurs at most
  // `cap` times -- the freq<=N table above, which needs no distributional assumption.
  // Read it as a TREND across --bases rather than extrapolating one point: diversity
  // genuinely saturates, so a single reading cannot be projected.
  constexpr double kHumanBases = 3.1e9;
  const std::size_t kept_at_cap =
      minimizers.size() - static_cast<std::size_t>(idx.capped_seeds());
  const double human_gb = kHumanBases * kept / 0.5 * 8.0 / 1e9;

  std::printf("\n--- cap behaviour at THIS reference size ---\n");
  std::printf("minimizer occurrences kept : %zu of %zu (%.1f%%)\n", kept_at_cap,
              minimizers.size(),
              100.0 * static_cast<double>(kept_at_cap) /
                  static_cast<double>(std::max<std::size_t>(1, minimizers.size())));
  std::printf("index size at 3.1e9 bases  : %.1f GB (scales with kept%%, not with skew)\n",
              human_gb);
  std::printf("\nCap survival is the freq<=N table. Run several --bases values for the\n"
              "trend; one point cannot be extrapolated.\n");
  return 0;
}
