// bench/symbol_agreement.cpp
//
// Per-event bucket agreement `a` between a reference symbol stream and a query symbol
// stream, measured under an explicit correspondence rather than assumed.
//
// Chapter 7 section 7.4 measures `a` by assuming query event i corresponds to reference
// event d + i for a constant d. That holds for fixed-width segmentation and does NOT hold
// for any pipeline whose event retention is data-dependent: a dropped or inserted event
// shifts the correspondence for the remainder of the read, so a constant offset
// understates agreement. Section 7.4.3 names a correspondence-aware measurement as the
// correct instrument and records that it was not implemented. This is that instrument.
//
// It is estimator-symmetric: it takes symbol dumps in one format and does not know which
// system produced them, so the same number is computed for this pipeline and for an
// external one. That matters, because an agreement figure computed by two different
// estimators is not a comparison.
//
// WHAT IT REPORTS, and why there are three numbers rather than one:
//
//   a_dp     matches / aligned-pairs under the maximising alignment. An UPPER bound on
//            agreement: the alignment is chosen to maximise matches, so a system whose
//            correspondence is genuinely ambiguous is flattered.
//   a_affine matches / length under the single best constant offset. Comparable to the
//            Chapter 7 estimator. A LOWER bound once retention is data-dependent.
//   indel    gap columns as a fraction of the alignment. The gap between a_dp and
//            a_affine is only interpretable alongside this: a_dp much higher than
//            a_affine with few gaps would indicate a bug, not a drifting correspondence.
//
// Reference dump format (one record per contig and strand):
//   >NAME<TAB>{+,-}
//   POS:SYM,POS:SYM,...
// Query dump format (one record per read), truth parsed from a squigulator read id
// NAME!CONTIG!START!END!STRAND:
//   S1_7!chr20_20Mb!6574!9719!-
//   SYM,SYM,...
//
// Usage:
//   mru_symbol_agreement <ref_symbols> <query_symbols> <contig_len> [pad] [max_reads]
//
// Build standalone (no project dependencies):
//   g++ -O2 -std=c++17 -o mru_symbol_agreement bench/symbol_agreement.cpp

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Scoring. Deliberately plain: +1 match, -1 mismatch, -1 gap. A gap penalty equal to a
// mismatch is the conservative choice here, because a cheaper gap would let the aligner
// manufacture matches by inserting gaps freely and would inflate a_dp.
constexpr int kMatch = 1;
constexpr int kMismatch = -1;
constexpr int kGap = -1;

struct RefStream {
  std::vector<std::int32_t> pos;
  std::vector<std::int8_t> sym;
};

// Reference symbol lines run to hundreds of megabytes, so the body is parsed as it is
// read rather than split into tokens first.
bool load_reference(const std::string& path, std::unordered_map<std::string, RefStream>& out) {
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return false;
  }
  std::string line, key;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (line[0] == '>') {
      key = line.substr(1);
      // trim trailing CR, then normalise the tab-separated strand into NAME:STRAND
      while (!key.empty() && (key.back() == '\r' || key.back() == '\n')) key.pop_back();
      for (char& c : key) {
        if (c == '\t') c = ':';
      }
      out[key];
      continue;
    }
    if (key.empty()) continue;
    RefStream& r = out[key];
    const char* p = line.c_str();
    const char* end = p + line.size();
    while (p < end) {
      long v = std::strtol(p, const_cast<char**>(&p), 10);
      if (p >= end || *p != ':') break;
      ++p;
      long s = std::strtol(p, const_cast<char**>(&p), 10);
      r.pos.push_back(static_cast<std::int32_t>(v));
      r.sym.push_back(static_cast<std::int8_t>(s));
      if (p < end && *p == ',') ++p;
    }
  }
  return true;
}

struct Read {
  std::string id;
  std::string contig;
  long start = 0;
  long end = 0;
  char strand = '+';
  std::vector<std::int8_t> sym;
  bool truth = false;
};

bool parse_truth(const std::string& id, Read& r) {
  // NAME!CONTIG!START!END!STRAND
  std::vector<std::string> f;
  std::string cur;
  for (char c : id) {
    if (c == '!') {
      f.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  f.push_back(cur);
  if (f.size() < 5) return false;
  r.contig = f[1];
  r.start = std::atol(f[2].c_str());
  r.end = std::atol(f[3].c_str());
  r.strand = f[4].empty() ? '+' : f[4][0];
  return r.end > r.start;
}

bool load_queries(const std::string& path, std::vector<Read>& out, std::size_t max_reads) {
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return false;
  }
  std::string line;
  Read cur;
  bool have = false;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.empty()) continue;
    // A record header is a line that is not a comma-separated numeric list. The read ids
    // this pipeline sees always contain '!', so that is the discriminator.
    if (line.find('!') != std::string::npos) {
      if (have && !cur.sym.empty()) {
        out.push_back(std::move(cur));
        if (max_reads && out.size() >= max_reads) return true;
      }
      cur = Read{};
      cur.id = line;
      cur.truth = parse_truth(line, cur);
      have = true;
      continue;
    }
    if (!have) continue;
    const char* p = line.c_str();
    const char* end = p + line.size();
    while (p < end) {
      long s = std::strtol(p, const_cast<char**>(&p), 10);
      cur.sym.push_back(static_cast<std::int8_t>(s));
      if (p < end && *p == ',') ++p;
      else if (p < end) ++p;
    }
  }
  if (have && !cur.sym.empty()) out.push_back(std::move(cur));
  return true;
}

struct Agreement {
  long matches = 0;
  long aligned = 0;   // columns where both sides carry a symbol
  long gaps = 0;      // columns where one side is a gap
  // Positions that begin a gap-free, all-matching run of E aligned columns, for the two
  // key lengths in play: 12 (this pipeline) and 8 (the external mapper's default).
  //
  // This is the quantity the a^E model approximates. Measuring it directly removes the
  // model's independence assumption -- section 7.2.3 argues a^E is a LOWER bound because
  // per-event agreements are positively correlated, and a direct count does not care.
  long keys8 = 0;
  long keys12 = 0;
  long key_windows = 0;  // alignment columns at which a key could start
};

// Exact-match run statistics over an alignment path. `path` holds one entry per column:
// 2 = both sides present and equal, 1 = both present and unequal, 0 = gap.
void score_runs(const std::vector<std::uint8_t>& path, Agreement& a) {
  long run = 0;
  for (std::uint8_t c : path) {
    if (c == 2) {
      ++run;
    } else {
      run = 0;
    }
    ++a.key_windows;
    if (run >= 8) ++a.keys8;
    if (run >= 12) ++a.keys12;
  }
}

// Semi-global: the query is aligned in full, the reference may have free prefix and
// suffix gaps, because the reference window is extracted with padding and the exact
// coordinate correspondence is not known to the event.
Agreement align(const std::int8_t* ref, std::size_t R, const std::int8_t* qry, std::size_t Q) {
  if (R == 0 || Q == 0) return Agreement{};
  const std::size_t W = Q + 1;
  std::vector<int> prev_h(W), cur_h(W);
  // Direction per cell, for traceback: 0 = diagonal, 1 = up (reference gap),
  // 2 = left (query gap). (R+1) * (Q+1) bytes; a few megabytes at these lengths.
  std::vector<std::uint8_t> dir((R + 1) * W, 0);

  prev_h[0] = 0;
  for (std::size_t j = 1; j < W; ++j) {
    prev_h[j] = prev_h[j - 1] + kGap;
    dir[j] = 2;
  }

  int best_h = prev_h[Q];
  std::size_t best_i = 0;

  for (std::size_t i = 1; i <= R; ++i) {
    cur_h[0] = 0;  // free reference prefix
    dir[i * W] = 1;
    const std::int8_t rs = ref[i - 1];
    const std::size_t row = i * W;
    for (std::size_t j = 1; j < W; ++j) {
      const bool eq = (rs == qry[j - 1]);
      const int diag = prev_h[j - 1] + (eq ? kMatch : kMismatch);
      const int up = prev_h[j] + kGap;       // reference symbol unmatched
      const int left = cur_h[j - 1] + kGap;  // query symbol unmatched
      if (diag >= up && diag >= left) {
        cur_h[j] = diag;
        dir[row + j] = 0;
      } else if (up >= left) {
        cur_h[j] = up;
        dir[row + j] = 1;
      } else {
        cur_h[j] = left;
        dir[row + j] = 2;
      }
    }
    if (cur_h[Q] > best_h) {
      best_h = cur_h[Q];
      best_i = i;
    }
    prev_h.swap(cur_h);
  }

  // Traceback from the best free-suffix row. The path is built backwards and then
  // reversed, because run length is defined along the read.
  std::vector<std::uint8_t> path;
  path.reserve(R + Q);
  std::size_t i = best_i, j = Q;
  while (j > 0) {
    const std::uint8_t d = dir[i * W + j];
    if (d == 0) {
      if (i == 0) break;
      path.push_back(ref[i - 1] == qry[j - 1] ? 2 : 1);
      --i;
      --j;
    } else if (d == 1) {
      if (i == 0) break;
      path.push_back(0);
      --i;
    } else {
      path.push_back(0);
      --j;
    }
  }
  std::reverse(path.begin(), path.end());

  Agreement a{};
  for (std::uint8_t c : path) {
    if (c == 0) ++a.gaps;
    else {
      ++a.aligned;
      if (c == 2) ++a.matches;
    }
  }
  score_runs(path, a);
  return a;
}

// Best single constant offset, which is the Chapter 7 section 7.4 estimator. Reported so
// that the DP figure is not compared against a number produced a different way.
Agreement best_affine(const std::int8_t* ref, std::size_t R, const std::int8_t* qry,
                      std::size_t Q) {
  Agreement best{};
  if (R == 0 || Q == 0) return best;
  const long span = static_cast<long>(R) - static_cast<long>(Q);
  const long lo = std::min<long>(0, span) - 64;
  const long hi = std::max<long>(0, span) + 64;
  for (long d = lo; d <= hi; ++d) {
    long m = 0, n = 0;
    for (std::size_t j = 0; j < Q; ++j) {
      const long i = static_cast<long>(j) + d;
      if (i < 0 || i >= static_cast<long>(R)) continue;
      ++n;
      if (ref[i] == qry[j]) ++m;
    }
    if (n > 0 && m * best.aligned > best.matches * n) {
      best.matches = m;
      best.aligned = n;
    } else if (best.aligned == 0 && n > 0) {
      best.matches = m;
      best.aligned = n;
    }
  }
  return best;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: %s <ref_symbols> <query_symbols> <contig_len> [pad] [max_reads]\n",
                 argv[0]);
    return 2;
  }
  const std::string ref_path = argv[1];
  const std::string qry_path = argv[2];
  const long contig_len = std::atol(argv[3]);
  const long pad = (argc > 4) ? std::atol(argv[4]) : 200;
  const std::size_t max_reads = (argc > 5) ? std::strtoul(argv[5], nullptr, 10) : 0;

  std::unordered_map<std::string, RefStream> refs;
  if (!load_reference(ref_path, refs)) return 1;
  std::fprintf(stderr, "reference streams: %zu\n", refs.size());
  for (const auto& kv : refs) {
    std::fprintf(stderr, "  %-24s events=%zu\n", kv.first.c_str(), kv.second.sym.size());
  }

  std::vector<Read> reads;
  if (!load_queries(qry_path, reads, max_reads)) return 1;
  std::fprintf(stderr, "query reads: %zu\n", reads.size());

  std::printf("read_id,contig,strand,qlen,rlen,a_dp,a_affine,indel_frac,keys8,keys12\n");

  long double sum_dp = 0, sum_af = 0, sum_gap = 0;
  long n_ok = 0;
  long long tot_m_dp = 0, tot_a_dp = 0, tot_m_af = 0, tot_a_af = 0;
  long long tot_k8 = 0, tot_k12 = 0, tot_kw = 0;

  for (const Read& r : reads) {
    if (!r.truth) continue;
    const std::string key = r.contig + ":" + std::string(1, r.strand);
    auto it = refs.find(key);
    if (it == refs.end()) continue;
    const RefStream& rs = it->second;

    // Reference positions are indices into the signal the reference encoder produced for
    // THIS strand. For the reverse strand that array runs the other way, so a forward
    // interval [start,end) maps to [L-k-end, L-k-start) there. The padding absorbs the
    // off-by-k ambiguity rather than asserting a particular convention.
    long lo, hi;
    if (r.strand == '+') {
      lo = r.start - pad;
      hi = r.end + pad;
    } else {
      const long l_eff = contig_len - 9;  // k = 9
      lo = (l_eff - r.end) - pad;
      hi = (l_eff - r.start) + pad;
    }
    const auto b = std::lower_bound(rs.pos.begin(), rs.pos.end(), static_cast<std::int32_t>(lo));
    const auto e = std::upper_bound(rs.pos.begin(), rs.pos.end(), static_cast<std::int32_t>(hi));
    const std::size_t off = static_cast<std::size_t>(b - rs.pos.begin());
    const std::size_t cnt = static_cast<std::size_t>(e - b);
    if (cnt < 32 || r.sym.size() < 32) continue;

    const Agreement dp = align(&rs.sym[off], cnt, r.sym.data(), r.sym.size());
    const Agreement af = best_affine(&rs.sym[off], cnt, r.sym.data(), r.sym.size());
    if (dp.aligned == 0 || af.aligned == 0) continue;

    const double a_dp = static_cast<double>(dp.matches) / static_cast<double>(dp.aligned);
    const double a_af = static_cast<double>(af.matches) / static_cast<double>(af.aligned);
    const double gf = static_cast<double>(dp.gaps) /
                      static_cast<double>(dp.gaps + dp.aligned);

    std::printf("%s,%s,%c,%zu,%zu,%.4f,%.4f,%.4f,%ld,%ld\n", r.id.c_str(),
                r.contig.c_str(), r.strand, r.sym.size(), cnt, a_dp, a_af, gf, dp.keys8,
                dp.keys12);

    sum_dp += a_dp;
    sum_af += a_af;
    sum_gap += gf;
    tot_m_dp += dp.matches;
    tot_a_dp += dp.aligned;
    tot_m_af += af.matches;
    tot_a_af += af.aligned;
    tot_k8 += dp.keys8;
    tot_k12 += dp.keys12;
    tot_kw += dp.key_windows;
    ++n_ok;
  }

  if (n_ok == 0) {
    std::fprintf(stderr, "no reads scored\n");
    return 1;
  }
  std::fprintf(stderr,
               "\nreads scored      : %ld\n"
               "a_dp     per-read : %.4f\n"
               "a_dp     pooled   : %.4f  (%lld / %lld)\n"
               "a_affine per-read : %.4f\n"
               "a_affine pooled   : %.4f  (%lld / %lld)\n"
               "indel fraction    : %.4f\n",
               n_ok, static_cast<double>(sum_dp / n_ok),
               static_cast<double>(tot_m_dp) / static_cast<double>(tot_a_dp), tot_m_dp,
               tot_a_dp, static_cast<double>(sum_af / n_ok),
               static_cast<double>(tot_m_af) / static_cast<double>(tot_a_af), tot_m_af,
               tot_a_af, static_cast<double>(sum_gap / n_ok));

  // Direct key-match probability, against what a^E predicts from the measured a. The
  // comparison is the whole point: if the direct count exceeds a^E, the per-event
  // agreements are positively correlated, which is what section 7.2.3 argues.
  const double a = static_cast<double>(tot_m_dp) / static_cast<double>(tot_a_dp);
  const double p8 = static_cast<double>(tot_k8) / static_cast<double>(tot_kw);
  const double p12 = static_cast<double>(tot_k12) / static_cast<double>(tot_kw);
  std::fprintf(stderr,
               "\nkey-match probability, measured directly from the alignment\n"
               "  E = 8   : %.5f   (a^8  = %.5f, ratio %.2fx)\n"
               "  E = 12  : %.5f   (a^12 = %.5f, ratio %.2fx)\n"
               "anchors per read\n"
               "  E = 8   : %.1f\n"
               "  E = 12  : %.1f\n",
               p8, std::pow(a, 8.0), p8 > 0 ? p8 / std::pow(a, 8.0) : 0.0, p12,
               std::pow(a, 12.0), p12 > 0 ? p12 / std::pow(a, 12.0) : 0.0,
               p8 * static_cast<double>(tot_kw) / static_cast<double>(n_ok),
               p12 * static_cast<double>(tot_kw) / static_cast<double>(n_ok));
  return 0;
}
