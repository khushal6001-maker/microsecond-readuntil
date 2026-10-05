// bench/rlc_feasibility.cpp
//
// Feasibility test, not production code: can run-length compression make the key
// dwell-invariant?
//
// bench/dwell_robustness.cpp showed the method dies at dwell CV 0.10 and that the
// failure is at the KEY, not the diagonal -- on-target vote maxima fall from 219 to 9,
// so seeds stop matching rather than scattering. A key packs 14 consecutive quantised
// events, and the query's events are fixed-width time slices, so the instant
// translocation speed varies the slices stop corresponding to bases and the key bits
// themselves change.
//
// The candidate fix is to stop keying on time. Quantise, then run-length compress the
// bucket sequence on BOTH reference and query, so a key is built from DISTINCT
// CONSECUTIVE LEVELS. A k-mer held for 7 samples and the same k-mer held for 30 both
// collapse to one symbol, which is dwell-invariant by construction.
//
// The known risk, and the reason this is a test rather than a patch: compression trades
// dwell sensitivity for FLICKER sensitivity. A single noise-induced bucket flip inside a
// run (A A -> A B A) inserts a symbol and shifts everything after it, which is the same
// class of damage as a dwell slip. Per-event bucket disagreement was already measured at
// roughly 20%, so it is genuinely unclear which effect dominates. That is what this
// measures.
//
// Deliberately minimal: its own open hash index and its own vote map, no minimizers, a
// reference prefix rather than all of chr20. The question is only "do true keys match
// under variable dwell", and answering it does not need the production index.
//
// Usage:
//   mru_rlc_feasibility <model.tsv> <reference.fa> [ref_Mb] [reads] [noise_pA]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "index/event_detect.hpp"

namespace {

constexpr float kPaToAdc = 8.0f;
constexpr std::uint32_t kBits = 3;       // 8 buckets, as frozen
constexpr std::uint32_t kEvents = 14;    // events per key, as frozen
constexpr std::uint32_t kSpe = 10;       // nominal samples per event
constexpr float kZClip = 3.0f;
constexpr std::size_t kMaxPos = 32;      // occurrence cap, as the real index has

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

bool load_fasta_prefix(const std::string& path, std::size_t want, std::string& out) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  while (std::getline(f, line) && out.size() < want) {
    if (!line.empty() && line[0] == '>') continue;
    for (char c : line) {
      if (base_code(c) >= 0) out.push_back(c);
    }
  }
  return !out.empty();
}

// Median and 1.4826*MAD, the same estimator both sides of the real pipeline use.
void median_mad(std::vector<float> v, float& med, float& mad) {
  if (v.empty()) { med = 0; mad = 1; return; }
  const std::size_t m = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m), v.end());
  med = v[m];
  for (float& x : v) x = std::fabs(x - med);
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m), v.end());
  mad = 1.4826f * v[m];
  if (!(mad > 0.0f)) mad = 1.0f;
}

std::uint8_t bucket_of(float level, float med, float mad) {
  float z = (level - med) / mad;
  z = std::clamp(z, -kZClip, kZClip);
  const float t = (z + kZClip) / (2.0f * kZClip);  // 0..1
  const std::uint32_t levels = 1u << kBits;
  std::uint32_t b = static_cast<std::uint32_t>(t * static_cast<float>(levels));
  if (b >= levels) b = levels - 1;
  return static_cast<std::uint8_t>(b);
}

// Collapse consecutive equal buckets, keeping a map back to the originating index so a
// diagonal can still be expressed in reference coordinates.
void run_length_compress(const std::vector<std::uint8_t>& in,
                         std::vector<std::uint8_t>& out,
                         std::vector<std::uint32_t>& origin) {
  out.clear();
  origin.clear();
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (out.empty() || in[i] != out.back()) {
      out.push_back(in[i]);
      origin.push_back(static_cast<std::uint32_t>(i));
    }
  }
}

std::uint64_t pack_key(const std::uint8_t* b) {
  std::uint64_t k = 0;
  for (std::uint32_t i = 0; i < kEvents; ++i) {
    k |= static_cast<std::uint64_t>(b[i] & ((1u << kBits) - 1)) << (kBits * i);
  }
  return k;
}

using Index = std::unordered_map<std::uint64_t, std::vector<std::uint32_t>>;

void build_index(const std::vector<std::uint8_t>& buckets,
                 const std::vector<std::uint32_t>& origin, Index& idx) {
  idx.clear();
  if (buckets.size() < kEvents) return;
  idx.reserve(buckets.size());
  for (std::size_t i = 0; i + kEvents <= buckets.size(); ++i) {
    auto& v = idx[pack_key(&buckets[i])];
    if (v.size() < kMaxPos) {
      // Report the position in ORIGINAL reference coordinates so diagonals from the
      // compressed and uncompressed variants are comparable.
      v.push_back(origin.empty() ? static_cast<std::uint32_t>(i) : origin[i]);
    }
  }
}

struct Outcome {
  std::size_t best_votes = 0;
  std::int64_t best_diag = 0;
};

Outcome vote(const Index& idx, const std::vector<std::uint8_t>& q,
             const std::vector<std::uint32_t>& q_origin) {
  Outcome o;
  if (q.size() < kEvents) return o;
  std::unordered_map<std::int64_t, std::size_t> votes;
  votes.reserve(1024);
  for (std::size_t i = 0; i + kEvents <= q.size(); ++i) {
    const auto it = idx.find(pack_key(&q[i]));
    if (it == idx.end()) continue;
    const std::int64_t qoff =
        q_origin.empty() ? static_cast<std::int64_t>(i) : static_cast<std::int64_t>(q_origin[i]);
    for (std::uint32_t p : it->second) {
      const std::int64_t d = static_cast<std::int64_t>(p) - qoff;
      const std::size_t n = ++votes[d];
      if (n > o.best_votes) {
        o.best_votes = n;
        o.best_diag = d;
      }
    }
  }
  return o;
}

// Variable-dwell squiggle, same generator as bench/dwell_robustness.cpp.
std::vector<std::int16_t> signal_with_dwell(const std::vector<float>& levels,
                                           std::uint32_t k, const char* dna,
                                           std::size_t len, float noise_pa, float cv,
                                           std::uint64_t seed) {
  std::vector<std::int16_t> raw;
  if (len < k) return raw;
  std::mt19937_64 rng(seed);
  std::normal_distribution<float> noise(0.0f, noise_pa * kPaToAdc);
  const double c = static_cast<double>(cv);
  const double s2 = std::log(1.0 + c * c);
  std::lognormal_distribution<double> dwell(std::log(static_cast<double>(kSpe)) - 0.5 * s2,
                                            std::sqrt(s2));
  const std::uint64_t mask = (k >= 32) ? ~0ULL : ((1ULL << (2 * k)) - 1);
  std::uint64_t kmer = 0;
  std::uint32_t have = 0;
  for (std::size_t i = 0; i < len; ++i) {
    const int b = base_code(dna[i]);
    if (b < 0) { have = 0; continue; }
    kmer = ((kmer << 2) | static_cast<std::uint64_t>(b)) & mask;
    if (++have < k) continue;
    std::uint32_t d = kSpe;
    if (cv > 0.0f) {
      d = static_cast<std::uint32_t>(std::lround(std::max(1.0, dwell(rng))));
      if (d > 8 * kSpe) d = 8 * kSpe;
    }
    const float lv = levels[kmer] * kPaToAdc;
    for (std::uint32_t t = 0; t < d; ++t) {
      raw.push_back(
          static_cast<std::int16_t>(std::clamp(lv + noise(rng), -32000.0f, 32000.0f)));
    }
  }
  return raw;
}

// Segment raw samples at a fixed width and bucket each slice's mean.
void query_buckets(const std::vector<std::int16_t>& raw, std::uint32_t width,
                   std::vector<std::uint8_t>& out) {
  out.clear();
  if (raw.empty() || width == 0) return;
  std::vector<float> means;
  means.reserve(raw.size() / width + 1);
  for (std::size_t i = 0; i + width <= raw.size(); i += width) {
    float s = 0;
    for (std::uint32_t j = 0; j < width; ++j) s += static_cast<float>(raw[i + j]);
    means.push_back(s / static_cast<float>(width));
  }
  float med = 0, mad = 1;
  median_mad(means, med, mad);
  out.reserve(means.size());
  for (float m : means) out.push_back(bucket_of(m, med, mad));
}

// Detected events -> buckets. The reference side is unchanged (one level per k-mer),
// because the reference has no dwell; detection exists to make the QUERY produce one
// event per k-mer however long the pore happened to hold it.
void query_buckets_detected(const std::vector<std::int16_t>& raw,
                            const mru::EventDetectConfig& ecfg, mru::EventScratch& es,
                            std::vector<float>& means, std::vector<std::uint8_t>& out) {
  out.clear();
  (void)mru::detect_events(std::span<const std::int16_t>(raw.data(), raw.size()), ecfg,
                           es, means);
  if (means.empty()) return;
  float med = 0, mad = 1;
  median_mad(means, med, mad);
  out.reserve(means.size());
  for (float m : means) out.push_back(bucket_of(m, med, mad));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: %s <model.tsv> <reference.fa> [ref_Mb] [reads] [noise_pA]\n",
                argv[0]);
    return 2;
  }
  const std::size_t ref_mb = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 5;
  const std::size_t n_reads = argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 200;
  const float noise_pa = argc > 5 ? std::strtof(argv[5], nullptr) : 1.5f;

  std::vector<float> levels;
  if (!load_model_tsv(argv[1], levels)) { std::printf("bad model\n"); return 1; }
  std::uint32_t k = 0;
  { std::size_t p = 1; while (p < levels.size()) { p *= 4; ++k; } }

  std::string dna;
  if (!load_fasta_prefix(argv[2], ref_mb * 1000000, dna)) {
    std::printf("bad reference\n");
    return 1;
  }

  // Reference bucket sequence: one level per k-mer position, normalised by the
  // reference's own realised levels.
  std::vector<float> ref_levels;
  ref_levels.reserve(dna.size());
  {
    const std::uint64_t mask = (k >= 32) ? ~0ULL : ((1ULL << (2 * k)) - 1);
    std::uint64_t kmer = 0;
    std::uint32_t have = 0;
    for (char c : dna) {
      const int b = base_code(c);
      if (b < 0) { have = 0; continue; }
      kmer = ((kmer << 2) | static_cast<std::uint64_t>(b)) & mask;
      if (++have < k) continue;
      ref_levels.push_back(levels[kmer] * kPaToAdc);
    }
  }
  float rmed = 0, rmad = 1;
  median_mad(ref_levels, rmed, rmad);
  std::vector<std::uint8_t> ref_fixed;
  ref_fixed.reserve(ref_levels.size());
  for (float lv : ref_levels) ref_fixed.push_back(bucket_of(lv, rmed, rmad));

  std::vector<std::uint8_t> ref_rlc;
  std::vector<std::uint32_t> ref_rlc_origin;
  run_length_compress(ref_fixed, ref_rlc, ref_rlc_origin);

  Index idx_fixed, idx_rlc;
  build_index(ref_fixed, {}, idx_fixed);
  // Compressed INDEX coordinates, not base coordinates. The compressed index is the
  // dwell-invariant coordinate the query can also express; mixing the two is a coordinate
  // error that makes every diagonal look wrong.
  build_index(ref_rlc, {}, idx_rlc);

  // Composition-matched negative control, so "it matched" can be compared against "it
  // matches anything". Without this arm the RLC vote counts are uninterpretable.
  std::string shuffled = dna;
  {
    std::mt19937_64 rng(20261005);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
  }

  std::printf("reference %zu bases (%zu Mb prefix), %u-mer model, %zu reads/cell, "
              "noise %.1f pA\n",
              dna.size(), ref_mb, k, n_reads, static_cast<double>(noise_pa));
  std::printf("reference events: %zu fixed -> %zu run-length compressed (%.2fx), "
              "distinct keys %zu fixed / %zu rlc\n\n",
              ref_fixed.size(), ref_rlc.size(),
              static_cast<double>(ref_fixed.size()) /
                  static_cast<double>(std::max<std::size_t>(1, ref_rlc.size())),
              idx_fixed.size(), idx_rlc.size());

  std::printf("%8s | %-21s | %-21s | %-21s\n", "dwellCV", "FIXED-WIDTH (current)",
              "RUN-LENGTH COMPRESSED", "EVENT-DETECTED");
  std::printf("%8s | %5s %4s %4s %4s | %5s %4s %4s %4s | %5s %4s %4s %4s\n", "",
              "medV", "TPR", "FPR", "ok", "medV", "TPR", "FPR", "ok", "medV", "TPR",
              "FPR", "ok");

  mru::EventDetectConfig ecfg;
  // Tunable from the command line because the detector has to be calibrated against a
  // target: events per read should land near one per k-mer. Over-segmenting is as fatal
  // as not segmenting at all, since the key packs a fixed number of consecutive events.
  if (argc > 6) ecfg.threshold = std::strtof(argv[6], nullptr);
  if (argc > 7) ecfg.min_len = static_cast<std::uint32_t>(std::strtoul(argv[7], nullptr, 10));
  mru::EventScratch es;
  std::vector<float> ev_means;

  const float cvs[] = {0.0f, 0.1f, 0.3f, 0.5f};
  const std::size_t read_bases = 2000;
  if (dna.size() < read_bases + 4 * k) { std::printf("reference too short\n"); return 1; }
  const std::size_t span = dna.size() - read_bases - 4 * k;
  es.reserve(read_bases * kSpe * 8);
  ev_means.reserve(read_bases * 4);
  std::printf("event detection: window %u, threshold %.2f, min_len %u, max_len %u\n\n",
              ecfg.window, static_cast<double>(ecfg.threshold), ecfg.min_len,
              ecfg.max_len);

  for (float cv : cvs) {
    std::size_t fx_hit = 0, fx_ok = 0, rl_hit = 0, rl_ok = 0, ed_hit = 0, ed_ok = 0;
    std::size_t fx_fp = 0, rl_fp = 0, ed_fp = 0;
    std::vector<std::size_t> fx_votes, rl_votes, fx_off, rl_off, ed_votes, ed_off;
    std::size_t ev_total = 0, ev_reads = 0;

    // Off-target arm first, so the on-target numbers are read against it.
    for (std::size_t r = 0; r < n_reads; ++r) {
      const std::size_t d = k + span * (r + 1) / (n_reads + 1);
      const auto raw = signal_with_dwell(levels, k, shuffled.data() + d, read_bases,
                                         noise_pa, cv, 44000 + r);
      if (raw.empty()) continue;
      std::vector<std::uint8_t> qf;
      query_buckets(raw, kSpe, qf);
      const Outcome of = vote(idx_fixed, qf, {});
      fx_off.push_back(of.best_votes);
      if (of.best_votes >= 5) ++fx_fp;
      std::vector<std::uint8_t> qfine, qr;
      std::vector<std::uint32_t> qr_origin;
      query_buckets(raw, 3, qfine);
      run_length_compress(qfine, qr, qr_origin);
      const Outcome orl = vote(idx_rlc, qr, {});
      rl_off.push_back(orl.best_votes);
      if (orl.best_votes >= 5) ++rl_fp;
      std::vector<std::uint8_t> qd;
      query_buckets_detected(raw, ecfg, es, ev_means, qd);
      const Outcome od = vote(idx_fixed, qd, {});
      ed_off.push_back(od.best_votes);
      if (od.best_votes >= 5) ++ed_fp;
    }

    for (std::size_t r = 0; r < n_reads; ++r) {
      const std::size_t d = k + span * (r + 1) / (n_reads + 1);
      const auto raw = signal_with_dwell(levels, k, dna.data() + d, read_bases, noise_pa,
                                         cv, 7000 + r);
      if (raw.empty()) continue;

      // Current method: segment at the nominal width, no compression.
      std::vector<std::uint8_t> qf;
      query_buckets(raw, kSpe, qf);
      const Outcome of = vote(idx_fixed, qf, {});
      fx_votes.push_back(of.best_votes);
      if (of.best_votes >= 5) ++fx_hit;
      if (of.best_votes >= 5 && std::llabs(of.best_diag - static_cast<std::int64_t>(d)) <= 4) ++fx_ok;

      // Candidate: segment FINER, then compress. Fine segmentation is what lets a short
      // dwell still produce a symbol; compression is what removes the dwell.
      std::vector<std::uint8_t> qfine, qr;
      std::vector<std::uint32_t> qr_origin;
      query_buckets(raw, 3, qfine);
      run_length_compress(qfine, qr, qr_origin);
      // Query origins are in fine-slice units; the reference is in base units, so the
      // diagonal is not directly comparable. Vote on compressed INDEX instead, which is
      // the dwell-invariant coordinate both sides share.
      const Outcome orl = vote(idx_rlc, qr, {});
      rl_votes.push_back(orl.best_votes);
      if (orl.best_votes >= 5) ++rl_hit;
      // Expected diagonal in COMPRESSED coordinates on both sides: where base position d
      // lands in the compressed reference. The query's compressed index starts at 0 for
      // the read, so the expected diagonal is just that.
      const auto it = std::lower_bound(ref_rlc_origin.begin(), ref_rlc_origin.end(),
                                       static_cast<std::uint32_t>(d));
      const std::int64_t d_rlc = static_cast<std::int64_t>(it - ref_rlc_origin.begin());
      if (orl.best_votes >= 5 && std::llabs(orl.best_diag - d_rlc) <= 16) ++rl_ok;

      // Detected events against the UNCOMPRESSED per-k-mer reference index. If detection
      // recovers one event per k-mer then query and reference event sequences line up
      // again, and the diagonal is in base coordinates directly comparable to d.
      std::vector<std::uint8_t> qd;
      query_buckets_detected(raw, ecfg, es, ev_means, qd);
      ev_total += qd.size();
      ++ev_reads;
      const Outcome od = vote(idx_fixed, qd, {});
      ed_votes.push_back(od.best_votes);
      if (od.best_votes >= 5) ++ed_hit;
      if (od.best_votes >= 5 &&
          std::llabs(od.best_diag - static_cast<std::int64_t>(d)) <= 8) {
        ++ed_ok;
      }
    }
    std::sort(fx_votes.begin(), fx_votes.end());
    std::sort(rl_votes.begin(), rl_votes.end());
    std::sort(fx_off.begin(), fx_off.end());
    std::sort(rl_off.begin(), rl_off.end());
    std::sort(ed_votes.begin(), ed_votes.end());
    std::sort(ed_off.begin(), ed_off.end());
    const auto med = [](const std::vector<std::size_t>& v) {
      return v.empty() ? 0 : v[v.size() / 2];
    };
    const double N = static_cast<double>(n_reads);
    std::printf("%8.2f | %5zu %3.0f%% %3.0f%% %4zu | %5zu %3.0f%% %3.0f%% %4zu | "
                "%5zu %3.0f%% %3.0f%% %4zu\n",
                static_cast<double>(cv), med(fx_votes),
                100.0 * static_cast<double>(fx_hit) / N,
                100.0 * static_cast<double>(fx_fp) / N, fx_ok, med(rl_votes),
                100.0 * static_cast<double>(rl_hit) / N,
                100.0 * static_cast<double>(rl_fp) / N, rl_ok, med(ed_votes),
                100.0 * static_cast<double>(ed_hit) / N,
                100.0 * static_cast<double>(ed_fp) / N, ed_ok);
    std::printf("%8s | off medV %-11zu | off medV %-11zu | off medV %-5zu "
                "events/read %.0f (ideal %zu)\n",
                "", med(fx_off), med(rl_off), med(ed_off),
                ev_reads ? static_cast<double>(ev_total) / static_cast<double>(ev_reads)
                         : 0.0,
                read_bases - k + 1);
  }
  std::printf("\n'diag ok' counts reads of %zu that reached 5 votes AND landed on the "
              "right diagonal.\nA high TPR with a low diag ok, or with a comparable FPR, "
              "means it is matching for\nthe wrong reason.\n",
              n_reads);
  std::printf("\nREAD THE FPR COLUMN BEFORE THE TPR COLUMN. This bench indexes EVERY\n"
              "position -- no minimizer selection -- so it probes roughly 10x more seeds\n"
              "than the real pipeline and both arms show an FPR near 100%%. The absolute\n"
              "TPR and FPR here are therefore NOT comparable to bench/geometry_sweep.cpp\n"
              "or to the daemon. Only 'diag ok' is interpretable, because it demands the\n"
              "correct diagonal rather than merely enough votes.\n");
  return 0;
}
