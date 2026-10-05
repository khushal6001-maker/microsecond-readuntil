// bench/squig_sweep.cpp
//
// Dwell-sensitivity sweep on signal from SQUIGULATOR, not from our own generator.
//
// Why this exists rather than reusing bench/geometry_sweep.cpp: that bench synthesises its
// own squiggle, and a claim about evaluation methodology cannot rest on signal we wrote
// ourselves. Squigulator is the accepted community simulator, it is what reviewers will
// expect, and it exposes --dwell-std, the standard deviation of signal samples per base.
// Its DEFAULT is --dwell-mean 9.0 --dwell-std 4.0, i.e. a coefficient of variation of
// about 0.44, which is worth stating on its own: the accepted simulator's default
// time-domain noise sits exactly where fixed-width segmentation fails.
//
// Compatibility notes that decide whether the numbers mean anything.
//
//   SAME PORE MODEL ON BOTH SIDES. Squigulator's built-in R10.4 9-mer table disagrees with
//   the one Icarust ships -- 54.316 pA against 57.203 for AAAAAAAAA, a ~3 pA gap against a
//   ~1.2 pA level standard deviation. Encoding the reference with one model and generating
//   signal from the other measures model mismatch, not dwell sensitivity. Pass the model
//   extracted from squigulator's own source.
//
//   BOTH STRANDS INDEXED. Squigulator emits reads from both strands, and reference_to_events
//   encodes only the forward sequence, so half the reads would be unmatchable by
//   construction and every TPR would be halved for a reason that has nothing to do with
//   dwell. The reverse complement is appended to the reference before encoding.
//
//   CHUNKS ARE 0.4 s OF SAMPLES, matching MinKNOW's delivery granularity, which at
//   squigulator's 5 kHz dna-r10-prom profile is 2000 samples rather than the 1800 that
//   bench/geometry_sweep.cpp uses at 4 kHz.
//
// Off-target reads come from a composition-matched shuffle of the same reference, simulated
// at the same dwell-std, so the only difference between the two sets is whether the
// sequence is in the index.
//
// Usage:
//   mru_squig_sweep <model.tsv> <ref.fa> <on.slow5> <off.slow5>
//                   [chunks] [detect 0|1] [min_window] [bits] [events] [accept_probe]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "daemon/diagonal_votes.hpp"
#include "index/batched_probe.hpp"
#include "index/minimizer_index.hpp"
#include "index/quantise.hpp"

namespace {

// 0.4 s at squigulator's dna-r10-prom sampling rate of 5 kHz.
constexpr std::size_t kChunkSamples = 2000;

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
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line[0] == '>') continue;
    for (char c : line) {
      if (base_code(c) >= 0) out.push_back(c);
    }
  }
  return !out.empty();
}

// Read ASCII SLOW5. Column 7 is len_raw_signal and column 8 is the comma-separated signal.
// Only those two are needed; everything else is metadata.
std::size_t load_slow5(const std::string& path,
                       std::vector<std::vector<std::int16_t>>& reads, std::size_t limit) {
  std::ifstream f(path);
  if (!f) return 0;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#' || line[0] == '@') continue;
    // Walk to the 8th tab-separated field without allocating a split.
    const char* p = line.c_str();
    int field = 0;
    const char* sig = nullptr;
    for (const char* q = p;; ++q) {
      if (*q == '\t' || *q == '\0') {
        ++field;
        if (field == 7) {
          sig = q + 1;
          break;
        }
        if (*q == '\0') break;
      }
    }
    if (sig == nullptr) continue;
    std::vector<std::int16_t> raw;
    raw.reserve(16384);
    const char* q = sig;
    while (*q != '\0' && *q != '\t') {
      raw.push_back(static_cast<std::int16_t>(std::strtol(q, nullptr, 10)));
      while (*q != '\0' && *q != ',' && *q != '\t') ++q;
      if (*q == ',') ++q;
    }
    if (!raw.empty()) reads.push_back(std::move(raw));
    if (limit != 0 && reads.size() >= limit) break;
  }
  return reads.size();
}

std::uint32_t best_votes_for_read(const mru::MinimizerIndex& idx,
                                 const std::vector<std::int16_t>& raw,
                                 const mru::QuantConfig& cfg, int probes,
                                 mru::ProbeScratch& scratch,
                                 mru::ScalingScratch& sscratch,
                                 std::vector<std::int64_t>& diags,
                                 std::size_t max_chunks, std::size_t& candidates) {
  mru::ChannelVotes votes;
  votes.reset();
  const std::size_t need =
      static_cast<std::size_t>(cfg.samples_per_event) * cfg.events_per_key;
  mru::SignalScaling scaling{};
  bool have_scaling = false;
  std::uint64_t dropped = 0;

  for (std::size_t c = 0; c < max_chunks; ++c) {
    const std::size_t off = c * kChunkSamples;
    if (off >= raw.size()) break;
    const std::size_t len = std::min(kChunkSamples, raw.size() - off);
    if (len < need) break;
    const std::span<const std::int16_t> chunk(raw.data() + off, len);
    if (!have_scaling) {
      // Once per read, from the first chunk, exactly as the daemon does.
      scaling = mru::scaling_from_samples(chunk, sscratch);
      if (!scaling.valid()) break;
      have_scaling = true;
    }
    (void)mru::match_signal(idx, chunk, scaling, cfg, scratch, probes);
    candidates += mru::accumulate_chunk_votes(scratch, votes, diags, dropped,
                                             scratch.events.size());
  }
  std::int64_t d = 0;
  return votes.best(&d);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::printf("usage: %s <model.tsv> <ref.fa> <on.slow5> <off.slow5> [chunks] "
                "[detect 0|1] [min_window] [bits] [events]\n",
                argv[0]);
    return 2;
  }
  const std::size_t max_chunks = argc > 5 ? std::strtoul(argv[5], nullptr, 10) : 10;
  const bool detect = argc > 6 ? (std::atoi(argv[6]) != 0) : true;
  const std::uint32_t min_window =
      argc > 7 ? static_cast<std::uint32_t>(std::strtoul(argv[7], nullptr, 10)) : 1;
  const std::uint32_t bits =
      argc > 8 ? static_cast<std::uint32_t>(std::strtoul(argv[8], nullptr, 10)) : 0;
  const std::uint32_t events =
      argc > 9 ? static_cast<std::uint32_t>(std::strtoul(argv[9], nullptr, 10)) : 0;

  std::vector<float> levels;
  if (!load_model_tsv(argv[1], levels)) {
    std::printf("bad model %s\n", argv[1]);
    return 1;
  }
  std::string dna;
  if (!load_fasta_acgt(argv[2], dna)) {
    std::printf("bad reference %s\n", argv[2]);
    return 1;
  }

  // Append the reverse complement, so reads from either strand can match. Without this
  // half of squigulator's reads are unmatchable and every TPR is halved for a reason
  // unrelated to dwell.
  const std::size_t fwd = dna.size();
  dna.reserve(fwd * 2 + 1);
  for (std::size_t i = fwd; i-- > 0;) {
    switch (dna[i]) {
      case 'A': case 'a': dna.push_back('T'); break;
      case 'C': case 'c': dna.push_back('G'); break;
      case 'G': case 'g': dna.push_back('C'); break;
      default: dna.push_back('A'); break;
    }
  }

  mru::QuantConfig cfg;
  cfg.event_detection = detect;
  cfg.minimizer_window = min_window;
  if (bits != 0) cfg.bits_per_event = bits;
  if (events != 0) cfg.events_per_key = events;
  if (!cfg.valid() || cfg.key_bits() > 64) {
    std::printf("invalid geometry %u x %u\n", cfg.bits_per_event, cfg.events_per_key);
    return 1;
  }
  const int probes = mru::kDefaultProbeBudget;

  mru::PoreModel model;
  if (!model.load_levels(levels)) {
    std::printf("model load failed\n");
    return 1;
  }
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

  std::vector<std::vector<std::int16_t>> on, off;
  load_slow5(argv[3], on, 0);
  load_slow5(argv[4], off, 0);
  if (on.empty() || off.empty()) {
    std::printf("no reads loaded (on=%zu off=%zu)\n", on.size(), off.size());
    return 1;
  }

  std::printf("ref %zu bases (both strands), %u-mer model, geometry %u x %u = %u-bit, "
              "window %u, %s, %zu chunks of %zu samples\n",
              dna.size(), model.k(), cfg.bits_per_event, cfg.events_per_key,
              cfg.key_bits(), cfg.minimizer_window,
              detect ? "EVENT-DETECTED" : "fixed-width", max_chunks, kChunkSamples);
  std::printf("index %zu minimizers, %llu distinct, %llu capped | reads on=%zu off=%zu\n",
              minimizers.size(), static_cast<unsigned long long>(idx.distinct_keys()),
              static_cast<unsigned long long>(idx.capped_seeds()), on.size(), off.size());

  mru::ProbeScratch scratch;
  mru::ScalingScratch sscratch;
  scratch.reserve_for(kChunkSamples * 2, cfg, probes);
  sscratch.reserve(kChunkSamples * 2);
  std::vector<std::int64_t> diags;
  diags.reserve(mru::kDiagCapacity);

  std::vector<std::uint32_t> on_v, off_v;
  std::size_t on_cand = 0, off_cand = 0;
  on_v.reserve(on.size());
  off_v.reserve(off.size());
  for (const auto& r : on) {
    on_v.push_back(best_votes_for_read(idx, r, cfg, probes, scratch, sscratch, diags,
                                      max_chunks, on_cand));
  }
  for (const auto& r : off) {
    off_v.push_back(best_votes_for_read(idx, r, cfg, probes, scratch, sscratch, diags,
                                       max_chunks, off_cand));
  }

  std::sort(on_v.begin(), on_v.end());
  std::sort(off_v.begin(), off_v.end());
  const std::uint32_t off_max = off_v.back();
  const std::uint32_t t0 = off_max + 1;
  std::size_t tp = 0;
  for (std::uint32_t v : on_v) {
    if (v >= t0) ++tp;
  }
  // Also report at a 1% false-positive budget, because demanding ZERO false positives lets
  // one outlier off-target read dictate the threshold, and that statistic is noisy.
  const std::uint32_t t1 = off_v[static_cast<std::size_t>(0.99 * (off_v.size() - 1))] + 1;
  std::size_t tp1 = 0;
  for (std::uint32_t v : on_v) {
    if (v >= t1) ++tp1;
  }

  const auto med = [](const std::vector<std::uint32_t>& v) {
    return v.empty() ? 0u : v[v.size() / 2];
  };
  std::printf("on  votes: med %u p95 %u max %u | cand/read %.0f\n", med(on_v),
              on_v[static_cast<std::size_t>(0.95 * (on_v.size() - 1))], on_v.back(),
              static_cast<double>(on_cand) / static_cast<double>(on.size()));
  std::printf("off votes: med %u p95 %u max %u | cand/read %.0f\n", med(off_v),
              off_v[static_cast<std::size_t>(0.95 * (off_v.size() - 1))], off_max,
              static_cast<double>(off_cand) / static_cast<double>(off.size()));
  std::printf("RESULT  zero-FP: >=%u TPR %.1f%%   |   1%%-FP: >=%u TPR %.1f%%\n", t0,
              100.0 * static_cast<double>(tp) / static_cast<double>(on_v.size()), t1,
              100.0 * static_cast<double>(tp1) / static_cast<double>(on_v.size()));
  return 0;
}
