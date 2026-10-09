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

#include "daemon/chain.hpp"
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
                       std::vector<std::vector<std::int16_t>>& reads, std::size_t limit,
                       std::vector<std::string>* ids = nullptr) {
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
    if (!raw.empty()) {
      if (ids != nullptr) ids->push_back(line.substr(0, line.find('	')));
      reads.push_back(std::move(raw));
    }
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
                                 std::size_t max_chunks, std::size_t& candidates,
                                 std::size_t& ev_total, int mode,
                                 const mru::ChainConfig& ccfg) {
  mru::ChannelVotes votes;
  mru::ChannelChains chains;
  static thread_local mru::ChannelAnchorChain ach;  // 14 KB; not a stack object
  votes.reset();
  chains.reset();
  ach.reset();
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
    if (mode == 2) {
      candidates += mru::accumulate_chunk_anchor_chain(scratch, ach, ccfg, dropped,
                                                       scratch.events.size());
    } else if (mode == 1) {
      candidates += mru::accumulate_chunk_chains(scratch, chains, ccfg, dropped,
                                                 scratch.events.size());
    } else {
      candidates += mru::accumulate_chunk_votes(scratch, votes, diags, dropped,
                                               scratch.events.size());
    }
    ev_total += scratch.events.size();
  }
  std::int64_t d = 0;
  if (mode == 2) return ach.best(&d);
  if (mode == 1) return chains.best(&d);
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
  const float det_thr = argc > 10 ? std::strtof(argv[10], nullptr) : 0.0f;
  const std::uint32_t det_min =
      argc > 11 ? static_cast<std::uint32_t>(std::strtoul(argv[11], nullptr, 10)) : 0;
  const std::uint32_t det_win =
      argc > 12 ? static_cast<std::uint32_t>(std::strtoul(argv[12], nullptr, 10)) : 0;
  // 1 selects streaming gap-tolerant chaining instead of exact-diagonal voting. band 0
  // makes chaining behave like the vote table, which is the control that shows the band
  // is what matters rather than the restructuring.
  // 0 votes, 1 greedy chains, 2 anchor-history chains
  const int mode = argc > 13 ? std::atoi(argv[13]) : 0;
  mru::ChainConfig ccfg;
  if (argc > 14) ccfg.band = static_cast<std::uint32_t>(std::strtoul(argv[14], nullptr, 10));
  if (argc > 15) {
    ccfg.max_gap = static_cast<std::uint32_t>(std::strtoul(argv[15], nullptr, 10));
  }
  if (argc > 16) {
    ccfg.lookback = static_cast<std::uint32_t>(std::strtoul(argv[16], nullptr, 10));
  }
  if (argc > 17) {
    ccfg.max_occ = static_cast<std::uint32_t>(std::strtoul(argv[17], nullptr, 10));
  }
  const bool delta = argc > 18 ? (std::atoi(argv[18]) != 0) : false;
  const float dclip = argc > 19 ? std::strtof(argv[19], nullptr) : 0.0f;

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
  if (det_thr > 0.0f) cfg.detect.threshold = det_thr;
  if (det_min != 0) cfg.detect.min_len = det_min;
  if (det_win != 0) cfg.detect.window = det_win;
  cfg.delta_keys = delta;
  if (dclip > 0.0f) cfg.delta_clip = dclip;
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
  std::vector<std::string> on_ids;
  load_slow5(argv[3], on, 0, &on_ids);
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
  if (mode == 2) {
    std::printf("decision: ANCHOR-HISTORY CHAINS (band %u, max_gap %u, lookback %u, "
                "max_occ %u, %zu anchors/channel)\n",
                ccfg.band, ccfg.max_gap, ccfg.lookback, ccfg.max_occ,
                mru::ChannelAnchorChain::kAnchors);
  } else if (mode == 1) {
    std::printf("decision: STREAMING CHAINS (band %u, max_gap %u, %zu chains/channel)\n",
                ccfg.band, ccfg.max_gap, mru::ChannelChains::kChains);
  } else {
    std::printf("decision: exact-diagonal votes (%zu slots/channel)\n",
                mru::ChannelVotes::kEntries);
  }
  std::printf("index %zu minimizers, %llu distinct, %llu capped | reads on=%zu off=%zu\n",
              minimizers.size(), static_cast<unsigned long long>(idx.distinct_keys()),
              static_cast<unsigned long long>(idx.capped_seeds()), on.size(), off.size());

  mru::ProbeScratch scratch;
  mru::ScalingScratch sscratch;
  scratch.reserve_for(kChunkSamples * 2, cfg, probes);
  sscratch.reserve(kChunkSamples * 2);
  std::vector<std::int64_t> diags;
  diags.reserve(mru::kDiagCapacity);

  // ---------------------------------------------------------------------------------
  // PER-EVENT BUCKET AGREEMENT.
  //
  // Everything downstream -- key match rate, votes, chains -- is a power of this number. A
  // key of E events matches only if all E of its buckets agree, so agreement a gives a key
  // match rate of a^E. Measuring a directly says whether the pipeline's problem is the
  // decision rule, the geometry, or the quantiser itself, and no amount of chaining can
  // recover a key that never matched.
  //
  // Forward-strand reads only, since the reverse half of the index is a separate
  // coordinate system. The read id carries the truth locus. The reference event index for a
  // read starting at base d is d up to a k-mer-convention offset, so a small offset search
  // is run and the best agreement reported -- that absorbs an off-by-k without hiding a
  // real disagreement.
  {
    std::size_t reads_scored = 0, agree_sum = 0, compared_sum = 0;
    std::vector<mru::QEvent> qev;
    std::vector<float> qz;
    mru::ScalingScratch diag_ss;
    diag_ss.reserve(kChunkSamples * 2);
    for (std::size_t i = 0; i < on.size() && reads_scored < 200; ++i) {
      const std::string id = on_ids.size() > i ? on_ids[i] : std::string();
      // name!contig!start!end!strand
      std::size_t p1 = id.find('!');
      if (p1 == std::string::npos) continue;
      std::size_t p2 = id.find('!', p1 + 1);
      std::size_t p3 = id.find('!', p2 + 1);
      std::size_t p4 = id.find('!', p3 + 1);
      if (p4 == std::string::npos) continue;
      if (id.substr(p4 + 1, 1) != "+") continue;  // forward strand only
      const long d = std::strtol(id.c_str() + p2 + 1, nullptr, 10);
      if (d <= 0) continue;

      const std::size_t len = std::min(kChunkSamples, on[i].size());
      if (len < kChunkSamples) continue;
      const std::span<const std::int16_t> chunk(on[i].data(), len);
      const mru::SignalScaling sc = mru::scaling_from_samples(chunk, diag_ss);
      if (!sc.valid()) continue;
      qev.clear();
      qz.clear();
      if (cfg.event_detection) {
        mru::EventScratch diag_es;
        std::vector<float> diag_means;
        mru::quantise_signal_detected(chunk, sc, cfg, diag_es, diag_means, qev, &qz);
      } else {
        mru::quantise_signal(chunk, sc, cfg, qev, &qz);
      }
      if (qev.size() < 32) continue;

      std::size_t best_agree = 0, best_cmp = 0;
      for (long shift = -12; shift <= 12; ++shift) {
        const long ref_off = d + shift;
        if (ref_off < 0) continue;
        std::size_t agree = 0, cmp = 0;
        for (std::size_t e = 0; e < qev.size(); ++e) {
          const std::size_t ri = static_cast<std::size_t>(ref_off) + e;
          if (ri >= ref_events.size()) break;
          ++cmp;
          if (ref_events[ri] == qev[e]) ++agree;
        }
        if (cmp > 0 && agree > best_agree) {
          best_agree = agree;
          best_cmp = cmp;
        }
      }
      if (best_cmp == 0) continue;
      agree_sum += best_agree;
      compared_sum += best_cmp;
      ++reads_scored;
    }
    if (compared_sum > 0) {
      const double a = static_cast<double>(agree_sum) / static_cast<double>(compared_sum);
      std::printf("per-event bucket agreement: %.1f%% over %zu reads (%zu events)\n",
                  100.0 * a, reads_scored, compared_sum);
      std::printf("  implied key match rate a^E for E=%u: %.3f%%   (E=8: %.3f%%)\n",
                  cfg.events_per_key,
                  100.0 * std::pow(a, static_cast<double>(cfg.events_per_key)),
                  100.0 * std::pow(a, 8.0));
    }
  }

  std::vector<std::uint32_t> on_v, off_v;
  std::size_t on_cand = 0, off_cand = 0, on_ev = 0, off_ev = 0;
  on_v.reserve(on.size());
  off_v.reserve(off.size());
  for (const auto& r : on) {
    on_v.push_back(best_votes_for_read(idx, r, cfg, probes, scratch, sscratch, diags,
                                      max_chunks, on_cand, on_ev, mode, ccfg));
  }
  for (const auto& r : off) {
    off_v.push_back(best_votes_for_read(idx, r, cfg, probes, scratch, sscratch, diags,
                                       max_chunks, off_cand, off_ev, mode, ccfg));
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
  // One event per base is the target. Over-segmenting is as fatal as not segmenting,
  // because a key counts a fixed number of CONSECUTIVE events, so one spurious event
  // shifts every later key.
  std::printf("events/read on %.0f off %.0f (ideal ~%zu at %u samples/base)\n",
              static_cast<double>(on_ev) / static_cast<double>(on.size()),
              static_cast<double>(off_ev) / static_cast<double>(off.size()),
              max_chunks * kChunkSamples / cfg.samples_per_event,
              cfg.samples_per_event);
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
