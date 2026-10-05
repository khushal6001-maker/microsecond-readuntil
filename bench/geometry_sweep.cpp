// bench/geometry_sweep.cpp
//
// The geometry decision, made on data, with the SAME voting code the daemon runs.
//
// This supersedes bench/offtarget.cpp, which answered the same question with its own
// private vote() -- an unbounded sorted vector of every candidate. That is why it
// could report 89.3% TPR on chr20 while the live daemon accepted nothing: the two
// were different algorithms. Everything here goes through
// mru::accumulate_chunk_votes and mru::ChannelVotes, so a number printed below is a
// prediction about the daemon and not about a program nobody runs.
//
// It is also chunked. The daemon never sees a whole read; it sees ~180-base chunks
// and accumulates votes across them, estimating shift and scale ONCE from the first
// chunk and reusing it. A bench that votes over a whole read in one call measures a
// different estimator on a different input, so this one chunks and reuses scaling
// exactly as policy.hpp does.
//
// Usage:
//   mru_geometry_sweep <model.tsv> <reference.fa> [reads_per_cell] [noise_pA]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "daemon/diagonal_votes.hpp"
#include "index/batched_probe.hpp"
#include "index/minimizer_index.hpp"
#include "index/quantise.hpp"

namespace {

constexpr float kPaToAdc = 8.0f;
// 400 bases/s at 0.4 s per chunk. The README's "10 chunks is ~1800 bases" assumes
// this, and so does PolicyConfig::max_chunks.
constexpr std::size_t kChunkBases = 180;

int base_code(char c) {
  switch (c) {
    case 'A': case 'a': return 0;
    case 'C': case 'c': return 1;
    case 'G': case 'g': return 2;
    case 'T': case 't': return 3;
    default: return -1;
  }
}

bool load_model_tsv(const std::string& path, std::vector<float>& levels) {
  std::ifstream f(path);
  if (!f) return false;
  levels.clear();
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

bool load_fasta_acgt(const std::string& path, std::string& out) {
  std::ifstream f(path);
  if (!f) return false;
  out.clear();
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line[0] == '>') continue;
    for (char c : line) {
      if (base_code(c) >= 0) out.push_back(c);
    }
  }
  return !out.empty();
}

// Synthetic squiggle, with variable dwell.
//
// dwell_cv is the coefficient of variation of a lognormal dwell about spe. 0 reproduces
// the fixed-count generator that every earlier geometry decision in this project was made
// against -- which is the constant-rate condition no pore satisfies, and under which
// fixed-width segmentation is exactly correct by construction. Any geometry frozen at
// cv=0 was frozen on the wrong rig.
std::vector<std::int16_t> signal_from_dna(const std::vector<float>& levels,
                                         std::uint32_t k, const char* dna,
                                         std::size_t len, std::uint32_t spe,
                                         float noise_pa, std::uint64_t seed,
                                         float dwell_cv = 0.0f) {
  std::vector<std::int16_t> raw;
  if (len < k) return raw;
  raw.reserve((len - k + 1) * spe * 2);
  std::mt19937_64 rng(seed);
  std::normal_distribution<float> noise(0.0f, noise_pa * kPaToAdc);
  const double cv = static_cast<double>(dwell_cv);
  const double s2 = std::log(1.0 + cv * cv);
  std::lognormal_distribution<double> dwell(std::log(static_cast<double>(spe)) - 0.5 * s2,
                                            std::sqrt(s2));
  const std::uint64_t mask = (k >= 32) ? ~0ULL : ((1ULL << (2 * k)) - 1);
  std::uint64_t kmer = 0;
  std::uint32_t have = 0;
  for (std::size_t i = 0; i < len; ++i) {
    const int c = base_code(dna[i]);
    if (c < 0) { have = 0; continue; }
    kmer = ((kmer << 2) | static_cast<std::uint64_t>(c)) & mask;
    if (++have < k) continue;
    std::uint32_t hold = spe;
    if (dwell_cv > 0.0f) {
      hold = static_cast<std::uint32_t>(std::lround(std::max(1.0, dwell(rng))));
      if (hold > 8 * spe) hold = 8 * spe;
    }
    const float level = levels[kmer] * kPaToAdc;
    for (std::uint32_t t = 0; t < hold; ++t) {
      raw.push_back(
          static_cast<std::int16_t>(std::clamp(level + noise(rng), -32000.0f, 32000.0f)));
    }
  }
  return raw;
}

// Run one read through the daemon's path: chunk it, scale from the first chunk,
// accumulate votes with the shared routine, return the best vote count.
struct ReadResult {
  std::uint32_t best_votes = 0;
  std::int64_t best_diag = 0;
  std::size_t candidates = 0;
};

ReadResult vote_like_the_daemon(const mru::MinimizerIndex& idx,
                               const std::vector<std::int16_t>& raw,
                               const mru::QuantConfig& cfg, int probes,
                               mru::ProbeScratch& scratch,
                               mru::ScalingScratch& scaling_scratch,
                               std::vector<std::int64_t>& diags,
                               std::size_t max_chunks) {
  ReadResult r;
  mru::ChannelVotes votes;
  votes.reset();
  const std::size_t chunk_samples = kChunkBases * cfg.samples_per_event;
  const std::size_t need = cfg.samples_per_event * cfg.events_per_key;

  mru::SignalScaling scaling{};
  bool have_scaling = false;
  std::uint64_t dropped = 0;

  for (std::size_t c = 0; c < max_chunks; ++c) {
    const std::size_t off = c * chunk_samples;
    if (off >= raw.size()) break;
    const std::size_t len = std::min(chunk_samples, raw.size() - off);
    if (len < need) break;
    const std::span<const std::int16_t> chunk(raw.data() + off, len);

    if (!have_scaling) {
      scaling = mru::scaling_from_samples(chunk, scaling_scratch);
      if (!scaling.valid()) break;
      have_scaling = true;
    }
    (void)mru::match_signal(idx, chunk, scaling, cfg, scratch, probes);
    r.candidates += mru::accumulate_chunk_votes(scratch, votes, diags, dropped,
                                                len / cfg.samples_per_event);
  }
  r.best_votes = votes.best(&r.best_diag);
  return r;
}

double pct(std::vector<std::size_t> v, double q) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const std::size_t i = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
  return static_cast<double>(v[i]);
}

struct Cell {
  std::uint32_t bits;
  std::uint32_t events;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: %s <model.tsv> <reference.fa> [reads] [noise_pA] [dwell_cv] "
                "[detect 0|1] [det_thr] [det_min_len] [det_window]\n",
                argv[0]);
    return 2;
  }
  const std::size_t n_reads = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 400;
  const float noise_pa = argc > 4 ? std::strtof(argv[4], nullptr) : 1.5f;
  const float dwell_cv = argc > 5 ? std::strtof(argv[5], nullptr) : 0.0f;
  const bool detect = argc > 6 ? (std::atoi(argv[6]) != 0) : false;
  const float det_thr = argc > 7 ? std::strtof(argv[7], nullptr) : 2.0f;
  const std::uint32_t det_min =
      argc > 8 ? static_cast<std::uint32_t>(std::strtoul(argv[8], nullptr, 10)) : 7;
  const std::uint32_t det_win =
      argc > 9 ? static_cast<std::uint32_t>(std::strtoul(argv[9], nullptr, 10)) : 4;
  // Minimizer window. 1 means every key is a seed. Worth sweeping because minimizer
  // selection picks a key by rank within a window, so a shifted event boundary can make
  // the reference and the query select DIFFERENT keys even where both keys are correct --
  // a failure mode that does not exist when dwell is constant.
  const std::uint32_t min_win =
      argc > 10 ? static_cast<std::uint32_t>(std::strtoul(argv[10], nullptr, 10)) : 10;

  std::vector<float> levels;
  if (!load_model_tsv(argv[1], levels)) {
    std::printf("could not load model %s\n", argv[1]);
    return 1;
  }
  std::string dna;
  if (!load_fasta_acgt(argv[2], dna)) {
    std::printf("could not load reference %s\n", argv[2]);
    return 1;
  }

  mru::PoreModel model;
  if (!model.load_levels(levels)) {
    std::printf("model load failed\n");
    return 1;
  }
  const std::uint32_t k = model.k();

  // Composition-matched negative control. Shuffling preserves base composition, so a
  // false positive cannot be explained away as a GC artefact.
  std::string shuffled = dna;
  {
    std::mt19937_64 rng(20261005);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
  }

  // Shorter keys are now in the grid, which they were not before. Under detection every
  // event boundary is an estimate, and a key is only correct if ALL of its events are, so
  // key length trades specificity against the probability that one boundary inside it is
  // wrong. At cv=0 that probability is zero and longer is strictly better, which is
  // exactly why the previous sweep chose 14 and why it cannot be trusted here.
  const Cell cells[] = {{3, 8}, {3, 10}, {3, 12}, {3, 14}, {4, 8}, {4, 10}, {4, 12}};
  const std::size_t lengths[] = {2, 5, 10, 20};  // chunks; 2 stands in for "2.5"
  const int probes = mru::kDefaultProbeBudget;

  std::printf("reference %zu bases, %u-mer model, %zu reads per cell, noise %.1f pA, "
              "%d probes/seed, %zu bases/chunk\n",
              dna.size(), k, n_reads, static_cast<double>(noise_pa), probes, kChunkBases);
  std::printf("dwell CV %.2f, segmentation %s", static_cast<double>(dwell_cv),
              detect ? "EVENT-DETECTED" : "fixed-width");
  if (detect) {
    std::printf(" (window %u, threshold %.2f, min_len %u)", det_win,
                static_cast<double>(det_thr), det_min);
  }
  std::printf("\n\n");

  for (const Cell& cell : cells) {
    mru::QuantConfig cfg;
    cfg.bits_per_event = cell.bits;
    cfg.events_per_key = cell.events;
    cfg.event_detection = detect;
    cfg.detect.threshold = det_thr;
    cfg.detect.min_len = det_min;
    cfg.detect.window = det_win;
    cfg.minimizer_window = min_win;
    if (!cfg.valid() || cfg.key_bits() > 64) {
      std::printf("=== %u x %u : REJECTED, %u-bit key does not fit a packed 64-bit "
                  "key ===\n\n",
                  cell.bits, cell.events, cfg.key_bits());
      continue;
    }

    std::vector<mru::QEvent> ref_events;
    if (!model.reference_to_events(dna, cfg, ref_events)) {
      std::printf("=== %u x %u : reference_to_events failed ===\n\n", cell.bits,
                  cell.events);
      continue;
    }
    std::vector<mru::SeedHash> all_seeds, minimizers;
    mru::hash_all_keys(ref_events, cfg, all_seeds);
    mru::select_minimizers(all_seeds, cfg.minimizer_window, minimizers);
    mru::MinimizerIndex idx;
    idx.build(minimizers);
    if (idx.distinct_keys() == 0) {
      std::printf("=== %u x %u : index produced no keys ===\n\n", cell.bits,
                  cell.events);
      continue;
    }

    const double cap_sat = minimizers.empty()
                               ? 0.0
                               : 100.0 * static_cast<double>(idx.capped_seeds()) /
                                     static_cast<double>(minimizers.size());
    std::printf("=== %u bits x %u events = %u-bit keys, window %u ===\n", cell.bits,
                cell.events, cfg.key_bits(), cfg.minimizer_window);
    std::printf("  index: %zu minimizers, %llu distinct keys (%.1f%% of minimizers), "
                "%llu capped (%.1f%% saturation), %llu insert failures\n",
                minimizers.size(),
                static_cast<unsigned long long>(idx.distinct_keys()),
                minimizers.empty() ? 0.0
                                   : 100.0 * static_cast<double>(idx.distinct_keys()) /
                                         static_cast<double>(minimizers.size()),
                static_cast<unsigned long long>(idx.capped_seeds()), cap_sat,
                static_cast<unsigned long long>(idx.insert_failures()));

    mru::ProbeScratch scratch;
    mru::ScalingScratch scaling_scratch;
    const std::size_t biggest =
        lengths[std::size(lengths) - 1] * kChunkBases * cfg.samples_per_event * 3;
    scratch.reserve_for(biggest, cfg, probes);
    scaling_scratch.reserve(biggest);
    std::vector<std::int64_t> diags;
    diags.reserve(mru::kDiagCapacity);

    std::printf("  %7s | %-28s | %-22s | %s\n", "chunks", "ON-target votes",
                "OFF-target votes", "lowest zero-FP threshold");
    for (std::size_t max_chunks : lengths) {
      const std::size_t read_bases = max_chunks * kChunkBases + k + 1;
      if (dna.size() < read_bases + 2 * k) continue;
      const std::size_t span = dna.size() - read_bases - 2 * k;

      std::vector<std::size_t> on_votes, off_votes, on_cands;
      std::size_t diag_ok = 0;
      for (std::size_t r = 0; r < n_reads; ++r) {
        const std::size_t d = k + span * (r + 1) / (n_reads + 1);
        const auto raw = signal_from_dna(levels, k, dna.data() + d, read_bases,
                                         cfg.samples_per_event, noise_pa, 5000 + r,
                                         dwell_cv);
        if (raw.empty()) continue;
        const ReadResult v = vote_like_the_daemon(idx, raw, cfg, probes, scratch,
                                                  scaling_scratch, diags, max_chunks);
        on_votes.push_back(v.best_votes);
        on_cands.push_back(v.candidates);
        if (v.best_votes > 0 &&
            std::llabs(v.best_diag - static_cast<std::int64_t>(d)) <= 2) {
          ++diag_ok;
        }
      }
      for (std::size_t r = 0; r < n_reads; ++r) {
        const std::size_t d = k + span * (r + 1) / (n_reads + 1);
        const auto raw = signal_from_dna(levels, k, shuffled.data() + d, read_bases,
                                         cfg.samples_per_event, noise_pa, 90000 + r,
                                         dwell_cv);
        if (raw.empty()) continue;
        const ReadResult v = vote_like_the_daemon(idx, raw, cfg, probes, scratch,
                                                  scaling_scratch, diags, max_chunks);
        off_votes.push_back(v.best_votes);
      }
      if (on_votes.empty() || off_votes.empty()) continue;

      const std::size_t off_max =
          *std::max_element(off_votes.begin(), off_votes.end());
      // The lowest threshold at which no off-target read is accepted. Reporting TPR
      // at that threshold is the only honest single number: a threshold with any
      // false positives costs pores on real sequence.
      const std::size_t t0 = off_max + 1;
      std::size_t tp = 0;
      for (std::size_t v : on_votes) {
        if (v >= t0) ++tp;
      }
      const double tpr =
          100.0 * static_cast<double>(tp) / static_cast<double>(on_votes.size());

      std::printf("  %7zu | med %3.0f p05 %3.0f max %3.0f cand %5.0f | med %3.0f "
                  "p99 %3.0f max %3.0f | >=%2zu gives TPR %5.1f%%  (diag ok %zu/%zu)\n",
                  max_chunks, pct(on_votes, 0.5), pct(on_votes, 0.05),
                  pct(on_votes, 1.0), pct(on_cands, 0.5), pct(off_votes, 0.5),
                  pct(off_votes, 0.99), pct(off_votes, 1.0), t0, tpr, diag_ok,
                  on_votes.size());
    }
    std::printf("\n");
  }
  return 0;
}
