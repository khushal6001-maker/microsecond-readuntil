// test/index_test.cpp
//
// The load-bearing test here is batched_equals_scalar. The pipelined probe is an
// optimisation, so if it ever disagrees with the scalar reference it is simply
// wrong, however fast it runs.
//
// The second most important is recall_harness. It exists because the real risk in
// this layer is sensitivity, not throughput, and a number measured on day one is
// worth more than an assumption carried to the results section.

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "core/tsc.hpp"
#include "index/batched_probe.hpp"
#include "index/minimizer_index.hpp"
#include "index/quantise.hpp"

namespace {

int g_failures = 0;

void fail(const char* file, int line, const char* what, const char* expr) {
  std::printf("  FAIL  %s:%d  %s  [%s]\n", file, line, what, expr);
  ++g_failures;
}

#define CHECK(cond, what)                                 \
  do {                                                    \
    if (!(cond)) fail(__FILE__, __LINE__, (what), #cond); \
  } while (0)

#define CHECK_EQ(a, b, what)                                                 \
  do {                                                                       \
    const std::uint64_t lhs_ = static_cast<std::uint64_t>(a);                \
    const std::uint64_t rhs_ = static_cast<std::uint64_t>(b);                \
    if (lhs_ != rhs_) {                                                      \
      std::printf("  FAIL  %s:%d  %s  (%llu != %llu)\n", __FILE__, __LINE__, \
                  (what), static_cast<unsigned long long>(lhs_),             \
                  static_cast<unsigned long long>(rhs_));                    \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

void banner(const char* name) { std::printf("[ RUN ] %s\n", name); }

// Synthetic squiggle.
//
// The first version of this was an unbounded random walk, and it was WRONG in a way
// that silently wrecked every recall number. Over 400k samples the walk drifted by
// roughly +/-1200 while local variation was only +/-6, so global median/MAD
// normalisation mapped all local structure into one or two buckets. The result was
// ~370 distinct keys out of 39992 seeds, and recall that looked like an index
// defect when it was a fixture defect.
//
// Real nanopore signal does not drift like that. Each k-mer in the pore sits at its
// own level within a bounded range (roughly 60-120 pA for R10), so successive
// levels are near-independent draws from a fixed distribution, not a walk. Modelling
// it that way is both more faithful and what gives the quantiser something to work
// with.
std::vector<std::int16_t> synth_signal(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<float> level_dist(500.0f, 60.0f);  // per-k-mer level
  std::normal_distribution<float> noise(0.0f, 15.0f);         // per-sample noise
  std::vector<std::int16_t> out;
  out.reserve(n);
  float level = level_dist(rng);
  for (std::size_t i = 0; i < n; ++i) {
    if (i % 10 == 0) level = level_dist(rng);  // a new base enters the pore
    out.push_back(
        static_cast<std::int16_t>(std::clamp(level + noise(rng), -30000.0f, 30000.0f)));
  }
  return out;
}

void test_quantisation() {
  banner("quantisation");
  mru::QuantConfig cfg;
  CHECK(cfg.valid(), "default config is valid");
  CHECK_EQ(cfg.levels(), 8u, "3 bits == 8 levels");
  CHECK_EQ(cfg.key_bits(), 45u, "15 events x 3 bits (frozen geometry)");

  mru::QuantConfig bad = cfg;
  bad.bits_per_event = 9;
  CHECK(!bad.valid(), "more than 8 bits per event rejected");
  bad = cfg;
  bad.events_per_key = 40;  // 120 bits
  CHECK(!bad.valid(), "key wider than 64 bits rejected");

  // Boundaries must not fall off the end: this is the classic off-by-one that
  // silently gives one bucket half the traffic.
  CHECK_EQ(mru::quantise_z(-cfg.z_clip, cfg), 0u, "-z_clip -> lowest bucket");
  CHECK_EQ(mru::quantise_z(cfg.z_clip, cfg), cfg.levels() - 1, "+z_clip -> highest bucket");
  CHECK_EQ(mru::quantise_z(-1000.0f, cfg), 0u, "clipped below");
  CHECK_EQ(mru::quantise_z(1000.0f, cfg), cfg.levels() - 1, "clipped above");
  CHECK(mru::quantise_z(0.0f, cfg) == cfg.levels() / 2, "zero sits mid-range");

  const auto sig = synth_signal(2000, 1);
  const auto sc = mru::scaling_from_samples(sig);
  CHECK(sc.valid(), "scaling derived from samples is valid");

  std::vector<mru::QEvent> ev;
  mru::quantise_signal(sig, sc, cfg, ev);
  CHECK_EQ(ev.size(), sig.size() / cfg.samples_per_event, "one event per window");
  for (mru::QEvent e : ev) {
    if (e >= cfg.levels()) {
      CHECK(false, "event outside the level range");
      break;
    }
  }

  // Determinism: same input, same events. An index built on one host must be
  // queryable on another.
  std::vector<mru::QEvent> ev2;
  mru::quantise_signal(sig, sc, cfg, ev2);
  CHECK(ev == ev2, "quantisation is deterministic");

  // hash64 is pinned: if it ever changes, every prebuilt index is silently
  // invalidated, so a regression here must be loud.
  CHECK_EQ(mru::hash64(0), 16294208416658607535ull, "hash64(0) is pinned");
  CHECK_EQ(mru::hash64(1), 10451216379200822465ull, "hash64(1) is pinned");
}

void test_index_basics() {
  banner("index_basics");
  mru::MinimizerIndex idx;
  idx.reserve(1000);
  CHECK(idx.capacity() >= 2000, "capacity honours the 0.5 load factor");
  CHECK((idx.capacity() & (idx.capacity() - 1)) == 0, "capacity is a power of two");
  CHECK_EQ(idx.size(), 0u, "starts empty");

  std::mt19937_64 rng(42);
  std::vector<std::pair<std::uint64_t, std::uint64_t>> inserted;
  for (std::uint64_t i = 0; i < 800; ++i) {
    const std::uint64_t h = rng();
    CHECK(idx.insert(h, i), "insert succeeds below the probe bound");
    inserted.emplace_back(h, i);
  }
  CHECK_EQ(idx.size(), 800u, "all inserted");
  CHECK(idx.load_factor() <= mru::MinimizerIndex::kMaxLoadFactor,
        "load factor stays within bounds");
  CHECK_EQ(idx.insert_failures(), 0u, "no insert hit the probe bound");

  mru::IndexStats stats;
  std::uint64_t out[4];
  std::size_t found_all = 0;
  for (const auto& [h, pos] : inserted) {
    const std::size_t n = idx.query(h, out, &stats);
    bool saw = false;
    for (std::size_t i = 0; i < n; ++i) {
      if (out[i] == pos) saw = true;
    }
    if (saw) ++found_all;
  }
  CHECK_EQ(found_all, inserted.size(), "every inserted key is found at its position");

  std::printf("        mean slots %.3f, mean cachelines %.3f, single-line %.1f%%, "
              "fp rejects %llu\n",
              stats.mean_slots(), stats.mean_cachelines(),
              100.0 * stats.single_cacheline_fraction(),
              static_cast<unsigned long long>(stats.fingerprint_rejects));

  // The design claim is ONE CACHELINE, not one slot. This index is a multimap, so
  // a hit must walk to the end of its run to collect duplicates and therefore
  // always touches at least two slots -- asserting one slot would be asserting
  // something the data structure cannot do. What it can do, with 8 entries per
  // 64-byte line, is keep that short run inside a single line.
  CHECK(stats.mean_slots() >= 2.0, "a hit necessarily touches the match and a terminator");
  // Measured 69.5% at load 0.39 on x86-64. This threshold is a regression guard set
  // below the measurement with margin, not an aspiration: the first version asserted
  // 0.70 and failed by half a percentage point, which is how a healthy data
  // structure turns into a red build.
  CHECK(stats.single_cacheline_fraction() > 0.60,
        "most queries stay within one cacheline");
  CHECK(stats.mean_cachelines() < 1.5, "cacheline touches per query stay near one");

  // Duplicates: a minimizer legitimately occurs many times.
  mru::MinimizerIndex dup;
  dup.reserve(64);
  const std::uint64_t h = 0xDEADBEEFCAFEBABEull;
  for (std::uint64_t p = 0; p < 3; ++p) CHECK(dup.insert(h, 100 + p), "duplicate insert");
  const std::size_t n = dup.query(h, out);
  CHECK_EQ(n, 3u, "all three duplicates returned");

  // Absent key must miss, not wander.
  mru::MinimizerIndex sparse;
  sparse.reserve(1024);
  (void)sparse.insert(mru::hash64(7), 7);
  CHECK_EQ(sparse.query(mru::hash64(99999), out), 0u, "absent key returns nothing");
}

// The contract: the pipelined path is an optimisation, never a different answer.
void test_batched_equals_scalar() {
  banner("batched_equals_scalar");
  std::mt19937_64 rng(7);

  std::vector<mru::SeedHash> reference;
  reference.reserve(20000);
  for (std::uint32_t i = 0; i < 20000; ++i) {
    reference.push_back(mru::SeedHash{rng(), i});
  }
  mru::MinimizerIndex idx;
  idx.build(reference);

  // Half present, half absent, interleaved so batches straddle both.
  std::vector<mru::SeedHash> queries;
  queries.reserve(4000);
  for (std::size_t i = 0; i < 4000; ++i) {
    if (i % 2 == 0) {
      queries.push_back(reference[(i * 37) % reference.size()]);
    } else {
      queries.push_back(mru::SeedHash{rng(), static_cast<std::uint32_t>(i)});
    }
  }

  mru::IndexStats s_scalar, s_batched;

  std::vector<mru::SeedMatches> batched(queries.size());
  (void)mru::probe_batch(idx, queries, batched, &s_batched);

  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < queries.size(); ++i) {
    std::uint64_t scalar_out[mru::SeedMatches::kMaxPerSeed];
    const std::size_t n = idx.query(queries[i].hash, scalar_out, &s_scalar);

    if (n != batched[i].count) {
      ++mismatches;
      continue;
    }
    for (std::size_t j = 0; j < n; ++j) {
      if (scalar_out[j] != batched[i].positions[j]) {  // order must match too
        ++mismatches;
        break;
      }
    }
    if (batched[i].seed_offset != queries[i].offset) ++mismatches;
  }

  CHECK_EQ(mismatches, 0u, "batched probe agrees with the scalar reference exactly");
  CHECK_EQ(s_batched.queries, s_scalar.queries, "same query count");
  CHECK_EQ(s_batched.hits, s_scalar.hits, "same hit count");
  CHECK_EQ(s_batched.misses, s_scalar.misses, "same miss count");
  CHECK_EQ(s_batched.fingerprint_rejects, s_scalar.fingerprint_rejects,
           "same fingerprint reject count");

  // The probe histograms must match too, not just the totals: identical results
  // reached by a different number of memory touches would mean the two paths had
  // drifted apart even though the answers happened to agree.
  std::size_t hist_diffs = 0;
  for (std::size_t i = 0; i < mru::IndexStats::kMaxProbeBuckets; ++i) {
    if (s_batched.slots[i] != s_scalar.slots[i]) ++hist_diffs;
    if (s_batched.cachelines[i] != s_scalar.cachelines[i]) ++hist_diffs;
  }
  CHECK_EQ(hist_diffs, 0u, "probe and cacheline histograms match exactly");

  std::printf("        %zu queries compared, hits %llu, mean slots %.3f, "
              "mean cachelines %.3f\n",
              queries.size(), static_cast<unsigned long long>(s_batched.hits),
              s_batched.mean_slots(), s_batched.mean_cachelines());
}

// Reports rather than asserts a speedup: the margin depends on the machine, and a
// flaky perf assertion in CI is worse than no assertion. The paper's number comes
// from `perf stat` on bare metal, not from here.
void test_batched_throughput() {
  banner("batched_throughput");
  // ~32 MiB of table: comfortably past L2, so probes really do miss.
  constexpr std::size_t kSeeds = 2'000'000;
  std::mt19937_64 rng(11);

  std::vector<mru::SeedHash> reference;
  reference.reserve(kSeeds);
  for (std::uint32_t i = 0; i < kSeeds; ++i) reference.push_back(mru::SeedHash{rng(), i});

  mru::MinimizerIndex idx;
  idx.build(reference);
  std::printf("        table %zu entries (%.1f MiB), load %.2f\n", idx.capacity(),
              static_cast<double>(idx.capacity() * sizeof(std::uint64_t)) / (1024.0 * 1024.0),
              idx.load_factor());

  constexpr std::size_t kQueries = 200'000;
  std::vector<mru::SeedHash> queries;
  queries.reserve(kQueries);
  for (std::size_t i = 0; i < kQueries; ++i) {
    queries.push_back(reference[(i * 7919) % reference.size()]);
  }

  const auto& clk = mru::TscClock::instance();

  std::uint64_t sink = 0;
  const std::uint64_t t0 = mru::rdtscp();
  for (const auto& q : queries) {
    std::uint64_t out[mru::SeedMatches::kMaxPerSeed];
    sink += idx.query(q.hash, out);
  }
  const double scalar_ns = clk.to_ns(mru::rdtscp() - t0) / static_cast<double>(kQueries);

  std::vector<mru::SeedMatches> matches(queries.size());
  const std::uint64_t t1 = mru::rdtscp();
  sink += mru::probe_batch(idx, queries, matches);
  const double batched_ns = clk.to_ns(mru::rdtscp() - t1) / static_cast<double>(kQueries);

  const double speedup = scalar_ns / std::max(1e-9, batched_ns);
  std::printf("        scalar %.1f ns/query, batched %.1f ns/query, speedup %.2fx\n",
              scalar_ns, batched_ns, speedup);

  // MEASURED RESULT, recorded rather than asserted.
  //
  // On this hardware software prefetching LOSES: repeated runs give 0.76-0.89x. The
  // reason is that the scalar loop's iterations are independent, so the
  // out-of-order engine already overlaps several outstanding misses on its own --
  // several hundred instructions of reorder window is a bigger prefetch distance
  // than anything expressible here. Explicit prefetching pays off when a dependency
  // chain PREVENTS that overlap (pointer chasing, or a long loop body), which is
  // not this loop.
  //
  // So the "batched software prefetch" claim is NOT currently supported by
  // measurement, and the paper must not make it on this evidence. Conditions where
  // it might still win, and which are worth measuring before deciding: a table far
  // larger than LLC where TLB pressure dominates, several worker threads competing
  // for memory bandwidth, and a server part with a smaller per-core reorder window.
  //
  // Deliberately no assertion on speedup: a perf assertion that depends on the host
  // would be flaky in CI, and the honest number belongs in a `perf stat` run on bare
  // metal anyway.
  if (speedup < 1.0) {
    std::printf("        NOTE: prefetching is not a win here (%.2fx). See the comment\n"
                "        in this test and in batched_probe.hpp before claiming it is.\n",
                speedup);
  }
  CHECK(sink > 0, "queries actually matched something");
  CHECK(batched_ns > 0.0 && scalar_ns > 0.0, "timings are sane");
}

// The number that decides whether this layer is worth anything.
//
// Build an index from a synthetic "reference" signal, then query a noisy copy of a
// region of it and measure what fraction of seeds recover a correct position. At
// zero noise recall must be essentially perfect -- that is a real correctness
// assertion. The noisy rows are reported, not asserted: improving them IS the
// research, and a threshold invented today would be meaningless.
void test_recall_harness() {
  banner("recall_harness");
  mru::QuantConfig cfg;
  cfg.minimizer_window = 1;  // isolate quantisation robustness from minimizer agreement

  const auto reference = synth_signal(400'000, 99);
  const auto ref_scaling = mru::scaling_from_samples(reference);

  std::vector<mru::QEvent> ref_events;
  mru::quantise_signal(reference, ref_scaling, cfg, ref_events);
  std::vector<mru::SeedHash> ref_seeds;
  mru::hash_all_keys(ref_events, cfg, ref_seeds);

  mru::MinimizerIndex idx;
  idx.build(ref_seeds);
  std::printf("        reference: %zu events, %zu seeds, %llu distinct keys, "
              "%llu capped, %zu slots, load %.2f\n",
              ref_events.size(), ref_seeds.size(),
              static_cast<unsigned long long>(idx.distinct_keys()),
              static_cast<unsigned long long>(idx.capped_seeds()), idx.capacity(),
              idx.load_factor());

  // Guards the failure mode that cost the most time here: before build() capped
  // occurrences, inserts silently failed on over-long clusters and the index threw
  // away 90% of itself with nothing reported. Never let that be quiet again.
  CHECK_EQ(idx.insert_failures(), 0u, "no insert hit the probe bound");

  // Key diversity is a precondition for recall. If the quantiser emits only a
  // handful of distinct keys then no index can recover position, and the problem is
  // upstream in quantise.hpp rather than here. This is exactly what caught the
  // random-walk fixture bug: ~370 distinct keys out of 39992 seeds.
  CHECK(idx.distinct_keys() > ref_seeds.size() / 20,
        "quantisation produces enough distinct keys to be informative");

  // A region of the reference, as a read would present it.
  constexpr std::size_t kQueryOffset = 50'000;
  constexpr std::size_t kQueryLen = 4'000;
  const std::vector<std::int16_t> clean(reference.begin() + kQueryOffset,
                                        reference.begin() + kQueryOffset + kQueryLen);
  const std::uint32_t expected_first_event =
      static_cast<std::uint32_t>(kQueryOffset / cfg.samples_per_event);

  // Two normalisations per noise level, because they answer different questions.
  //
  //   oracle    : the reference's own scaling applied to the query. Isolates how
  //               much the QUANTISATION tolerates noise.
  //   per-read  : scaling derived from the query alone, which is all a real read
  //               has. Includes the cost of estimating shift/scale from 4000
  //               samples instead of 400000.
  //
  // The gap between the two columns is the price of per-read normalisation, and it
  // is a real result: if it is large, normalisation is a bigger sensitivity problem
  // than quantisation and should be attacked first.
  const auto measure = [&](const std::vector<std::int16_t>& query,
                           const mru::SignalScaling& sc) {
    mru::ProbeScratch scratch;
    scratch.reserve_for(query.size(), cfg);
    mru::IndexStats stats;
    (void)mru::match_signal(idx, query, sc, cfg, scratch,
                            mru::kDefaultProbeBudget, &stats);

    // A seed is recovered if ANY of its probed keys returns its correct reference
    // position, within one event of slop for window alignment.
    //
    // Keyed by seed_offset rather than by index: match_signal now returns one entry
    // per PROBE, not per minimizer, so with a budget of 4 there are 4x as many
    // entries and positional indexing would silently read the wrong seed's results.
    // That is exactly how this broke when multi-probe landed.
    std::vector<char> recovered_at(scratch.events.size() + 1, 0);
    for (const mru::SeedMatches& m : scratch.matches) {
      const std::uint64_t want = expected_first_event + m.seed_offset;
      for (std::uint32_t j = 0; j < m.count; ++j) {
        const std::uint64_t got = m.positions[j];
        const std::uint64_t diff = got > want ? got - want : want - got;
        if (diff <= 1 && m.seed_offset < recovered_at.size()) {
          recovered_at[m.seed_offset] = 1;
          break;
        }
      }
    }
    std::size_t recovered = 0;
    for (const mru::SeedHash& mm : scratch.minimizers) {
      if (mm.offset < recovered_at.size() && recovered_at[mm.offset] != 0) ++recovered;
    }
    const std::size_t n = scratch.minimizers.size();
    return n == 0 ? 0.0 : static_cast<double>(recovered) / static_cast<double>(n);
  };

  std::printf("        noise sd | recall (oracle norm) | recall (per-read norm)\n");
  double zero_noise_oracle = 0.0;

  for (float sd : {0.0f, 2.0f, 5.0f, 10.0f, 20.0f}) {
    std::mt19937_64 rng(1234);
    std::normal_distribution<float> noise(0.0f, sd);
    std::vector<std::int16_t> query(clean.size());
    for (std::size_t i = 0; i < clean.size(); ++i) {
      const float v = static_cast<float>(clean[i]) + (sd > 0.0f ? noise(rng) : 0.0f);
      query[i] = static_cast<std::int16_t>(std::clamp(v, -30000.0f, 30000.0f));
    }

    const double r_oracle = measure(query, ref_scaling);
    const double r_read = measure(query, mru::scaling_from_samples(query));
    std::printf("        %8.1f | %18.1f%% | %20.1f%%\n", static_cast<double>(sd),
                100.0 * r_oracle, 100.0 * r_read);
    if (sd == 0.0f) zero_noise_oracle = r_oracle;
  }

  // Zero noise under the reference's own scaling is not a research question: an
  // identical signal, normalised identically, must match itself. If this fails the
  // pipeline is broken rather than merely insensitive.
  //
  // Note deliberately NOT asserted: the per-read column at zero noise. That one
  // can legitimately be poor, because estimating median/MAD from 4000 samples does
  // not reproduce the estimate from 400000, and keys shift as a result. Measuring
  // that honestly is the point.
  CHECK(zero_noise_oracle > 0.95,
        "zero noise with matched normalisation recovers nearly every seed");
}

// ---------------------------------------------------------------------------
// Per-event bucket disagreement: decomposing the recall loss
// ---------------------------------------------------------------------------
//
// Key survival measured 50.5% for 9-event keys, which implies per-event agreement
// of 0.505^(1/9) = 92.7%. So a ~7% per-event error is being amplified into a 50%
// key loss, and the question is where those errors sit:
//
//   clustered near bucket boundaries -> the estimator is fine; fix the boundaries
//                                       (adaptive quantisation, query-side
//                                       multi-probe)
//   spread across the bucket         -> the shift/scale estimate itself is wrong
//                                       and boundary tricks will not help
//
// It also measures what multi-probe would buy, by flipping the most marginal
// event(s) of each key to the neighbouring bucket and re-checking. That turns
// "multi-probe should help" into a number with a probe cost attached.

// Event z-values before quantisation, under a given scaling.
std::vector<float> event_z(const std::vector<std::int16_t>& raw,
                           const mru::SignalScaling& sc, const mru::QuantConfig& cfg) {
  std::vector<float> out;
  const std::size_t w = cfg.samples_per_event;
  if (w == 0 || raw.size() < w) return out;
  const std::size_t n = raw.size() / w;
  out.reserve(n);
  for (std::size_t e = 0; e < n; ++e) {
    float sum = 0.0f;
    for (std::size_t i = 0; i < w; ++i) sum += sc.apply(raw[e * w + i]);
    out.push_back(sum / static_cast<float>(w));
  }
  return out;
}

// boundary_distance() and neighbour_bucket() now live in index/quantise.hpp,
// since multi-probe in the library needs them too. Unqualified calls below
// resolve to mru:: by ADL.

void test_bucket_disagreement() {
  banner("bucket_disagreement");
  mru::QuantConfig cfg;
  cfg.minimizer_window = 1;

  const auto reference = synth_signal(400000, 99);
  const auto ref_scaling = mru::scaling_from_samples(reference);

  // 4000 samples is ~2.5 chunks at 0.4 s and 4 kHz: roughly all the signal
  // available when a decision must be made. Pool over many such windows rather
  // than using one long one, because a longer window gives a better shift/scale
  // estimate than a real read ever has and would flatter the result.
  constexpr std::size_t kQueryLen = 4000;
  constexpr std::size_t kWindows = 40;
  constexpr std::size_t kFirstOffset = 20000;
  constexpr std::size_t kStride = 8000;
  constexpr std::size_t kBins = 10;

  std::size_t total_in_bin[kBins] = {};
  std::size_t disagree_in_bin[kBins] = {};
  std::size_t disagreements = 0;
  std::size_t n_events_total = 0;
  std::size_t delta_one = 0;  // off by exactly one bucket
  std::size_t delta_big = 0;  // off by more
  std::size_t n_keys = 0, exact = 0, with_one_flip = 0, with_two_flips = 0;
  double shift_err_sum = 0.0, scale_err_pct_sum = 0.0;
  std::size_t windows_used = 0;

  std::vector<mru::QEvent> trial(cfg.events_per_key);

  for (std::size_t wi = 0; wi < kWindows; ++wi) {
    const std::size_t off = kFirstOffset + wi * kStride;
    if (off + kQueryLen > reference.size()) break;
    ++windows_used;

    const std::vector<std::int16_t> query(
        reference.begin() + static_cast<std::ptrdiff_t>(off),
        reference.begin() + static_cast<std::ptrdiff_t>(off + kQueryLen));
    const auto q_scaling = mru::scaling_from_samples(query);
    shift_err_sum += std::fabs(static_cast<double>(q_scaling.shift - ref_scaling.shift));
    scale_err_pct_sum +=
        100.0 * std::fabs(static_cast<double>(q_scaling.scale - ref_scaling.scale)) /
        static_cast<double>(ref_scaling.scale);

    // Identical raw samples; the ONLY difference is which scaling is applied.
    const auto z_ref = event_z(query, ref_scaling, cfg);
    const auto z_qry = event_z(query, q_scaling, cfg);
    const std::size_t n_events = std::min(z_ref.size(), z_qry.size());
    n_events_total += n_events;

    std::vector<mru::QEvent> b_ref(n_events), b_qry(n_events);
    for (std::size_t i = 0; i < n_events; ++i) {
      b_ref[i] = mru::quantise_z(z_ref[i], cfg);
      b_qry[i] = mru::quantise_z(z_qry[i], cfg);

      const float d = boundary_distance(z_qry[i], cfg);
      auto bin = static_cast<std::size_t>(d * static_cast<float>(kBins));
      if (bin >= kBins) bin = kBins - 1;
      ++total_in_bin[bin];

      if (b_ref[i] != b_qry[i]) {
        ++disagreements;
        ++disagree_in_bin[bin];
        const int diff = static_cast<int>(b_ref[i]) - static_cast<int>(b_qry[i]);
        if (diff == 1 || diff == -1) {
          ++delta_one;
        } else {
          ++delta_big;
        }
      }
    }

    // Multi-probe payoff. Marginality is ranked using ONLY query-side
    // information, which is all a real read has.
    const std::size_t keys_here =
        n_events >= cfg.events_per_key ? n_events - cfg.events_per_key + 1 : 0;
    n_keys += keys_here;

    for (std::size_t i = 0; i < keys_here; ++i) {
      const std::uint64_t want = mru::pack_key(b_ref.data() + i, cfg);
      if (mru::pack_key(b_qry.data() + i, cfg) == want) {
        ++exact;
        ++with_one_flip;
        ++with_two_flips;
        continue;
      }

      std::array<std::pair<float, std::uint32_t>, 32> marg{};
      for (std::uint32_t e = 0; e < cfg.events_per_key; ++e) {
        marg[e] = std::pair<float, std::uint32_t>(boundary_distance(z_qry[i + e], cfg), e);
      }
      std::sort(marg.begin(), marg.begin() + cfg.events_per_key);

      const std::uint32_t m0 = marg[0].second;
      const std::uint32_t m1 = marg[1].second;

      for (std::uint32_t e = 0; e < cfg.events_per_key; ++e) trial[e] = b_qry[i + e];
      trial[m0] = mru::QEvent(neighbour_bucket(z_qry[i + m0], cfg));
      if (mru::pack_key(trial.data(), cfg) == want) {
        ++with_one_flip;
        ++with_two_flips;
        continue;
      }

      bool ok2 = false;
      for (int fa = 0; fa < 2 && !ok2; ++fa) {
        for (int fb = 0; fb < 2 && !ok2; ++fb) {
          if (fa == 0 && fb == 0) continue;
          for (std::uint32_t e = 0; e < cfg.events_per_key; ++e) trial[e] = b_qry[i + e];
          if (fa == 1) {
            trial[m0] = mru::QEvent(neighbour_bucket(z_qry[i + m0], cfg));
          }
          if (fb == 1) {
            trial[m1] = mru::QEvent(neighbour_bucket(z_qry[i + m1], cfg));
          }
          if (mru::pack_key(trial.data(), cfg) == want) ok2 = true;
        }
      }
      if (ok2) ++with_two_flips;
    }
  }

  CHECK(windows_used > 10, "enough windows pooled");
  CHECK(n_events_total > 10000, "enough events pooled to measure");

  std::printf("        reference scaling: shift %.2f scale %.2f  (from %zu samples)\n",
              static_cast<double>(ref_scaling.shift),
              static_cast<double>(ref_scaling.scale), reference.size());
  std::printf("        per-read estimate over %zu windows of %zu samples:\n"
              "          mean |shift error| %.2f raw units, mean |scale error| %.2f%%\n",
              windows_used, kQueryLen, shift_err_sum / static_cast<double>(windows_used),
              scale_err_pct_sum / static_cast<double>(windows_used));

  const double rate =
      static_cast<double>(disagreements) / static_cast<double>(n_events_total);
  std::printf("        per-event disagreement: %zu / %zu = %.2f%%\n", disagreements,
              n_events_total, 100.0 * rate);
  const auto denom = static_cast<double>(std::max<std::size_t>(1, disagreements));
  std::printf("        of those, off-by-one: %.1f%%, off-by-more: %.1f%%\n",
              100.0 * static_cast<double>(delta_one) / denom,
              100.0 * static_cast<double>(delta_big) / denom);

  // Validates the amplification model against the measured exact-key match rate.
  const double predicted = std::pow(1.0 - rate, static_cast<double>(cfg.events_per_key));
  std::printf("        predicted %u-event key survival: (1-%.4f)^%u = %.1f%%\n",
              cfg.events_per_key, rate, cfg.events_per_key, 100.0 * predicted);

  std::printf("\n        distance from bucket boundary (0.0 = on boundary, 1.0 = centre)\n");
  std::printf("        bin        | events | disagree | rate\n");
  for (std::size_t bi = 0; bi < kBins; ++bi) {
    const double r = total_in_bin[bi] == 0
                         ? 0.0
                         : static_cast<double>(disagree_in_bin[bi]) /
                               static_cast<double>(total_in_bin[bi]);
    std::printf("        %.1f - %.1f  | %6zu | %8zu | %6.2f%%\n",
                static_cast<double>(bi) / kBins, static_cast<double>(bi + 1) / kBins,
                total_in_bin[bi], disagree_in_bin[bi], 100.0 * r);
  }

  const auto pct_keys = [&](std::size_t v) {
    return n_keys == 0 ? 0.0 : 100.0 * static_cast<double>(v) / static_cast<double>(n_keys);
  };
  std::printf("\n        multi-probe payoff over %zu keys\n", n_keys);
  std::printf("        exact key match            : %6.1f%%   (1 probe/key)\n",
              pct_keys(exact));
  std::printf("        + flip most marginal event : %6.1f%%   (2 probes/key)\n",
              pct_keys(with_one_flip));
  std::printf("        + flip two most marginal   : %6.1f%%   (4 probes/key)\n",
              pct_keys(with_two_flips));

  CHECK(with_one_flip >= exact, "flipping cannot lose matches");
  CHECK(with_two_flips >= with_one_flip, "more flips cannot lose matches");
}

// ---------------------------------------------------------------------------
// Specificity, and how it scales with reference size
// ---------------------------------------------------------------------------
//
// Multi-probe buys recall (51.5% -> 84.2% at 4 probes/key) by querying variant
// keys. Each extra probe also returns extra candidate positions, so the question
// is whether chaining can reject them cheaply or becomes the new bottleneck.
//
// The thing that must not be measured on a toy reference: with 3-bit events and
// 9 events per key the key space is 2^27 = 1.34e8. A 40k-event reference occupies
// 0.03% of it and essentially every key is unique, which makes any specificity
// number look wonderful. A 3.1e9-event human reference would occupy it ~23x over,
// so every key collides ~23 times BY PIGEONHOLE, before any noise. Measuring at
// one size would therefore be self-deception; this sweeps reference size and
// reports the trend so the extrapolation is explicit.
//
// Chaining model: for a true locus, every seed satisfies
// ref_position - query_offset = constant (the diagonal). So bin candidates by
// diagonal and take the mode. That is O(n) with a hash, needs no alignment, and is
// the cheapest useful consensus test -- exactly the "monotonically increasing
// positions in a narrow window" idea, expressed in the form that is one pass.

struct Candidate {
  std::uint32_t key_offset;
  std::uint64_t ref_position;
};

// Builds the variant keys for one key position under a probe budget of 1, 2 or 4,
// ranking marginality from query-side information only.
std::size_t variant_keys(const std::vector<mru::QEvent>& b_qry,
                         const std::vector<float>& z_qry, std::size_t i,
                         const mru::QuantConfig& cfg, int budget,
                         std::array<std::uint64_t, 4>& keys_out) {
  std::vector<mru::QEvent> t(cfg.events_per_key);
  for (std::uint32_t e = 0; e < cfg.events_per_key; ++e) t[e] = b_qry[i + e];
  keys_out[0] = mru::pack_key(t.data(), cfg);
  if (budget <= 1) return 1;

  std::array<std::pair<float, std::uint32_t>, 32> marg{};
  for (std::uint32_t e = 0; e < cfg.events_per_key; ++e) {
    marg[e] = std::pair<float, std::uint32_t>(boundary_distance(z_qry[i + e], cfg), e);
  }
  std::sort(marg.begin(), marg.begin() + cfg.events_per_key);
  const std::uint32_t m0 = marg[0].second;
  const std::uint32_t m1 = marg[1].second;
  const auto nb0 = mru::QEvent(neighbour_bucket(z_qry[i + m0], cfg));
  const auto nb1 = mru::QEvent(neighbour_bucket(z_qry[i + m1], cfg));

  t[m0] = nb0;
  keys_out[1] = mru::pack_key(t.data(), cfg);
  if (budget <= 2) return 2;

  t[m0] = b_qry[i + m0];
  t[m1] = nb1;
  keys_out[2] = mru::pack_key(t.data(), cfg);
  t[m0] = nb0;
  keys_out[3] = mru::pack_key(t.data(), cfg);
  return 4;
}

void test_specificity_scaling() {
  banner("specificity_scaling");
  mru::QuantConfig cfg;
  cfg.minimizer_window = 1;

  const double key_space = std::pow(2.0, static_cast<double>(cfg.key_bits()));
  std::printf("        key space: %u bits = %.3g distinct keys\n", cfg.key_bits(),
              key_space);
  std::printf("        a 3.1e9-event (human-scale) reference would occupy it %.1fx over\n\n",
              3.1e9 / key_space);

  constexpr std::size_t kQueryLen = 4000;   // ~2.5 chunks
  constexpr std::size_t kWindows = 12;

  std::printf("        ref events | probes | cand/key | true%% | top-diag correct | "
              "margin\n");

  for (std::size_t ref_events : {40000u, 320000u, 2560000u}) {
    const std::size_t ref_samples = ref_events * cfg.samples_per_event + kQueryLen;
    const auto reference = synth_signal(ref_samples, 99);
    const auto ref_scaling = mru::scaling_from_samples(reference);

    std::vector<mru::QEvent> ref_ev;
    mru::quantise_signal(reference, ref_scaling, cfg, ref_ev);
    std::vector<mru::SeedHash> ref_seeds;
    mru::hash_all_keys(ref_ev, cfg, ref_seeds);

    mru::MinimizerIndex idx;
    idx.build(ref_seeds);

    for (int budget : {1, 2, 4}) {
      std::size_t cand_total = 0, cand_true = 0, keys_total = 0;
      std::size_t diag_correct = 0, windows_scored = 0;
      double margin_sum = 0.0;

      for (std::size_t wi = 0; wi < kWindows; ++wi) {
        const std::size_t off =
            (reference.size() - kQueryLen) * (wi + 1) / (kWindows + 1);
        const std::size_t aligned = (off / cfg.samples_per_event) * cfg.samples_per_event;
        const std::vector<std::int16_t> query(
            reference.begin() + static_cast<std::ptrdiff_t>(aligned),
            reference.begin() + static_cast<std::ptrdiff_t>(aligned + kQueryLen));
        const auto q_scaling = mru::scaling_from_samples(query);
        const auto z_qry = event_z(query, q_scaling, cfg);
        if (z_qry.size() < cfg.events_per_key) continue;

        std::vector<mru::QEvent> b_qry(z_qry.size());
        for (std::size_t i = 0; i < z_qry.size(); ++i) {
          b_qry[i] = mru::quantise_z(z_qry[i], cfg);
        }
        const auto true_diag =
            static_cast<std::int64_t>(aligned / cfg.samples_per_event);

        std::vector<Candidate> cands;
        const std::size_t n_keys = z_qry.size() - cfg.events_per_key + 1;
        for (std::size_t i = 0; i < n_keys; ++i) {
          std::array<std::uint64_t, 4> keys{};
          const std::size_t nk = variant_keys(b_qry, z_qry, i, cfg, budget, keys);
          ++keys_total;
          for (std::size_t k = 0; k < nk; ++k) {
            std::uint64_t pos[16];
            const std::size_t n = idx.query(mru::hash64(keys[k]), pos);
            for (std::size_t j = 0; j < n; ++j) {
              cands.push_back(Candidate{static_cast<std::uint32_t>(i), pos[j]});
            }
          }
        }

        cand_total += cands.size();
        for (const Candidate& c : cands) {
          const auto want = static_cast<std::int64_t>(true_diag) +
                            static_cast<std::int64_t>(c.key_offset);
          const auto got = static_cast<std::int64_t>(c.ref_position);
          if (std::llabs(got - want) <= 1) ++cand_true;
        }

        // Diagonal voting: the cheapest useful chaining test.
        if (!cands.empty()) {
          std::vector<std::int64_t> diags;
          diags.reserve(cands.size());
          for (const Candidate& c : cands) {
            diags.push_back(static_cast<std::int64_t>(c.ref_position) -
                            static_cast<std::int64_t>(c.key_offset));
          }
          std::sort(diags.begin(), diags.end());
          std::int64_t best_diag = diags[0];
          std::size_t best_votes = 0, second_votes = 0;
          std::size_t run = 1;
          for (std::size_t i = 1; i <= diags.size(); ++i) {
            if (i < diags.size() && diags[i] == diags[i - 1]) {
              ++run;
              continue;
            }
            if (run > best_votes) {
              second_votes = best_votes;
              best_votes = run;
              best_diag = diags[i - 1];
            } else if (run > second_votes) {
              second_votes = run;
            }
            run = 1;
          }
          ++windows_scored;
          if (std::llabs(best_diag - true_diag) <= 1) ++diag_correct;
          margin_sum += static_cast<double>(best_votes) /
                        static_cast<double>(std::max<std::size_t>(1, second_votes));
        }
      }

      const double cand_per_key =
          keys_total == 0 ? 0.0
                          : static_cast<double>(cand_total) / static_cast<double>(keys_total);
      const double true_pct =
          cand_total == 0 ? 0.0
                          : 100.0 * static_cast<double>(cand_true) /
                                static_cast<double>(cand_total);
      std::printf("        %10zu | %6d | %8.2f | %5.1f | %10zu/%-5zu | %6.1fx\n",
                  ref_events, budget, cand_per_key, true_pct, diag_correct,
                  windows_scored,
                  windows_scored == 0 ? 0.0
                                      : margin_sum / static_cast<double>(windows_scored));
    }
    std::printf("        %10zu | index: %llu distinct keys, %llu capped, load %.2f\n",
                ref_events, static_cast<unsigned long long>(idx.distinct_keys()),
                static_cast<unsigned long long>(idx.capped_seeds()), idx.load_factor());
  }

  std::printf("\n        Chaining cost: candidates/chunk = cand/key x ~152 keys.\n"
              "        Diagonal voting is one pass plus a sort, so chaining stays\n"
              "        linear in candidates; the number above is what bounds it.\n");
}

// ---------------------------------------------------------------------------
// Key geometry sweep
// ---------------------------------------------------------------------------
//
// 27-bit keys cap human-scale recall at ~35% by pigeonhole alone, so the geometry
// has to grow. Both ways of growing it cost recall, through different mechanisms:
//
//   more bits per event  -> larger key space, but narrower buckets, so a fixed
//                           normalisation error flips a bucket more often
//   more events per key  -> larger key space, bucket width unchanged, but the
//                           per-event error is amplified over more events
//
// This measures both decays at once so the trade can be read off rather than
// argued about.
//
// MEASURED vs DERIVED -- the columns are deliberately separated because a
// 3.1e9-event index is ~25 GB and cannot be built here:
//   measured: per-event disagreement, exact / 2-probe / 4-probe key recall
//   derived : human-scale occupancy, cap survival, projected recall
// The projection also assumes the true position is as likely to be retained as any
// other when the occurrence cap bites. build() currently keeps the LOWEST offsets
// after sorting, which would systematically drop positions late in the reference --
// that needs fixing before the projection is trustworthy as more than a bound.

// Reference signal with REALISTIC EVENT CORRELATION.
//
// The i.i.d. fixture draws each event's level independently, which is wrong in a way
// that matters: a nanopore reads overlapping k-mers, so consecutive events share
// k-1 bases and their levels are strongly correlated. Independence inflates the
// effective entropy of a key, and inflates it most for long keys over few levels --
// exactly the geometries the first sweep selected.
//
// This builds the real structure: one random level per k-mer drawn once (a synthetic
// stand-in for ONT's table, which this repo does not vendor), random DNA, then slide
// the k-mer window. Level spread and per-sample noise are identical to the i.i.d.
// fixture so the comparison isolates CORRELATION and not signal-to-noise.
struct KmerRef {
  std::vector<std::int16_t> raw;
  std::vector<float> levels;  // 4^k, in lexicographic k-mer order
  std::string dna;
  std::uint32_t k = 0;
};

KmerRef synth_signal_kmer(std::size_t n_events, std::uint32_t k, std::uint64_t seed,
                          std::uint32_t samples_per_event) {
  KmerRef out;
  out.k = k;
  std::mt19937_64 rng(seed);

  // One level per k-mer, drawn once. Same distribution as the i.i.d. fixture.
  const std::size_t n_kmers = static_cast<std::size_t>(1) << (2 * k);
  std::normal_distribution<float> level_dist(500.0f, 60.0f);
  out.levels.resize(n_kmers);
  for (float& v : out.levels) v = level_dist(rng);

  // Random DNA. n_events positions need n_events + k - 1 bases.
  static const char kBases[] = "ACGT";
  std::uniform_int_distribution<int> base_pick(0, 3);
  out.dna.resize(n_events + k - 1);
  for (char& c : out.dna) c = kBases[base_pick(rng)];

  // Slide the k-mer window; emit samples_per_event noisy samples per position.
  std::normal_distribution<float> noise(0.0f, 15.0f);
  out.raw.reserve(n_events * samples_per_event);
  std::uint64_t kmer = 0;
  std::uint32_t have = 0;
  const std::uint64_t mask = (std::uint64_t{1} << (2 * k)) - 1;
  for (char c : out.dna) {
    int code = 0;
    switch (c) {
      case 'C': code = 1; break;
      case 'G': code = 2; break;
      case 'T': code = 3; break;
      default: code = 0; break;
    }
    kmer = ((kmer << 2) | static_cast<std::uint64_t>(code)) & mask;
    if (++have < k) continue;
    const float level = out.levels[kmer];
    for (std::uint32_t s = 0; s < samples_per_event; ++s) {
      out.raw.push_back(
          static_cast<std::int16_t>(std::clamp(level + noise(rng), -30000.0f, 30000.0f)));
    }
  }
  return out;
}

// Distinct packed keys over a reference, for one geometry. This is the empirical
// replacement for the theoretical 2^key_bits: correlation means the reachable key
// space is smaller than the representable one, and only measurement knows by how
// much.
std::size_t distinct_keys_in(const std::vector<std::int16_t>& reference,
                             const mru::SignalScaling& sc, const mru::QuantConfig& cfg) {
  std::vector<mru::QEvent> ev;
  mru::quantise_signal(reference, sc, cfg, ev);
  if (ev.size() < cfg.events_per_key) return 0;
  const std::size_t n = ev.size() - cfg.events_per_key + 1;
  std::vector<std::uint64_t> keys;
  keys.reserve(n);
  for (std::size_t i = 0; i < n; ++i) keys.push_back(mru::pack_key(ev.data() + i, cfg));
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys.size();
}

// Poisson occupancy: drawing n positions from a space of K distinct keys yields
// E[distinct] = K(1 - exp(-n/K)). Measuring distinct and n, solve for K to get the
// EFFECTIVE key space. Monotone in K, so bisection is enough.
double effective_key_space(std::size_t n_positions, std::size_t distinct) {
  const auto n = static_cast<double>(n_positions);
  const auto d = static_cast<double>(distinct);
  if (d <= 0.0) return 0.0;
  if (d >= n * 0.9999) return 1e18;  // no measurable collisions; space is >> n
  double lo = d, hi = 1e18;
  for (int it = 0; it < 200; ++it) {
    const double mid = 0.5 * (lo + hi);
    const double pred = mid * (1.0 - std::exp(-n / mid));
    if (pred < d) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return 0.5 * (lo + hi);
}

struct GeometryResult {
  double disagree_rate = 0.0;
  double exact_pct = 0.0;
  double p2_pct = 0.0;
  double p4_pct = 0.0;
  std::size_t n_events = 0;
  std::size_t n_keys = 0;
};

// Index-free: recall at the key level depends only on whether the two bucket
// sequences agree, so no table is needed and the geometry effect is isolated.
GeometryResult measure_geometry(const std::vector<std::int16_t>& reference,
                                const mru::SignalScaling& ref_scaling,
                                const mru::QuantConfig& cfg) {
  constexpr std::size_t kQueryLen = 4000;
  constexpr std::size_t kWindows = 40;
  constexpr std::size_t kFirstOffset = 20000;
  constexpr std::size_t kStride = 8000;

  GeometryResult r;
  std::size_t disagreements = 0, exact = 0, p2 = 0, p4 = 0;

  for (std::size_t wi = 0; wi < kWindows; ++wi) {
    const std::size_t off = kFirstOffset + wi * kStride;
    if (off + kQueryLen > reference.size()) break;

    const std::vector<std::int16_t> query(
        reference.begin() + static_cast<std::ptrdiff_t>(off),
        reference.begin() + static_cast<std::ptrdiff_t>(off + kQueryLen));
    const auto q_scaling = mru::scaling_from_samples(query);

    const auto z_ref = event_z(query, ref_scaling, cfg);
    const auto z_qry = event_z(query, q_scaling, cfg);
    const std::size_t n_events = std::min(z_ref.size(), z_qry.size());
    if (n_events < cfg.events_per_key) continue;
    r.n_events += n_events;

    std::vector<mru::QEvent> b_ref(n_events), b_qry(n_events);
    for (std::size_t i = 0; i < n_events; ++i) {
      b_ref[i] = mru::quantise_z(z_ref[i], cfg);
      b_qry[i] = mru::quantise_z(z_qry[i], cfg);
      if (b_ref[i] != b_qry[i]) ++disagreements;
    }

    const std::size_t keys_here = n_events - cfg.events_per_key + 1;
    r.n_keys += keys_here;
    for (std::size_t i = 0; i < keys_here; ++i) {
      const std::uint64_t want = mru::pack_key(b_ref.data() + i, cfg);
      std::array<std::uint64_t, 4> keys{};
      const std::size_t nk = variant_keys(b_qry, z_qry, i, cfg, 4, keys);
      if (keys[0] == want) {
        ++exact;
        ++p2;
        ++p4;
        continue;
      }
      if (nk > 1 && keys[1] == want) {
        ++p2;
        ++p4;
        continue;
      }
      for (std::size_t k = 2; k < nk; ++k) {
        if (keys[k] == want) {
          ++p4;
          break;
        }
      }
    }
  }

  if (r.n_events > 0) {
    r.disagree_rate = static_cast<double>(disagreements) / static_cast<double>(r.n_events);
  }
  if (r.n_keys > 0) {
    const auto n = static_cast<double>(r.n_keys);
    r.exact_pct = 100.0 * static_cast<double>(exact) / n;
    r.p2_pct = 100.0 * static_cast<double>(p2) / n;
    r.p4_pct = 100.0 * static_cast<double>(p4) / n;
  }
  return r;
}

void test_geometry_sweep() {
  banner("geometry_sweep");

  constexpr std::size_t kRefEvents = 40000;
  constexpr std::uint32_t kSpe = 10;   // samples per event, matches QuantConfig default
  constexpr std::uint32_t kPoreK = 9;  // R10-like 9-mer
  constexpr double kHumanEvents = 3.1e9;
  constexpr double kCap = 8.0;

  // Control: independent levels (the old, wrong fixture).
  const auto ref_iid = synth_signal(kRefEvents * kSpe, 99);
  const auto sc_iid = mru::scaling_from_samples(ref_iid);

  // Treatment: overlapping k-mers, identical level spread and noise.
  const KmerRef km = synth_signal_kmer(kRefEvents, kPoreK, 99, kSpe);
  const auto sc_km = mru::scaling_from_samples(km.raw);

  // A larger correlated reference, for the effective-key-space fit. More positions
  // make the collision count, and therefore the fit, far better determined.
  // 3.2M positions, not 320k: at 320k the longer geometries showed only ~90
  // collisions, so the fitted key space had ~10%% counting error and the
  // geometries with zero collisions could not be bounded at all. Ten times the
  // positions makes the fit for the candidate winners actually load-bearing.
  constexpr std::size_t kBigEvents = 3200000;
  const KmerRef km_big = synth_signal_kmer(kBigEvents, kPoreK, 7, kSpe);
  const auto sc_km_big = mru::scaling_from_samples(km_big.raw);

  std::printf("        pore model: %u-mer, %zu levels; DNA %zu bases; "
              "reference %zu events (fit reference %zu)\n",
              kPoreK, km.levels.size(), km.dna.size(), kRefEvents, kBigEvents);

  // Exercise the real PoreModel path on the same data, so the seam is not dead code.
  mru::PoreModel model;
  CHECK(model.load_levels(km.levels), "synthetic level table loads (4^k entries)");
  CHECK(model.k() == kPoreK, "pore model k inferred from table size");
  {
    mru::QuantConfig probe_cfg;
    std::vector<mru::QEvent> model_ev;
    CHECK(model.reference_to_events(km.dna, probe_cfg, model_ev),
          "PoreModel converts reference DNA to events");
    CHECK(model_ev.size() >= kRefEvents - 1, "one event per k-mer position");
  }
  {
    mru::PoreModel empty;
    std::vector<mru::QEvent> ev;
    mru::QuantConfig c;
    CHECK(!empty.reference_to_events(km.dna, c, ev),
          "an unloaded model refuses rather than inventing levels");
  }

  std::printf("\n        TABLE A -- measured recall: independent levels vs correlated k-mers\n");
  std::printf("        bits ev kbits |   iid disagr  iid 4pr |   kmer disagr  kmer 4pr | 4pr delta\n");

  struct Row {
    std::uint32_t bits = 0, events = 0, kbits = 0;
    double km_disagr = 0.0, km_exact = 0.0, km_p2 = 0.0, km_p4 = 0.0;
    double iid_p4 = 0.0;
    std::size_t distinct = 0;
    double keff = 0.0, occ = 0.0, capsurv = 0.0, proj = 0.0;
  };
  std::vector<Row> rows;

  for (std::uint32_t bits : {2u, 3u, 4u, 5u}) {
    for (std::uint32_t events : {8u, 10u, 11u, 12u, 13u, 15u, 18u, 20u}) {
      mru::QuantConfig cfg;
      cfg.bits_per_event = bits;
      cfg.events_per_key = events;
      cfg.minimizer_window = 1;
      if (!cfg.valid()) continue;
      if (cfg.key_bits() < 27) continue;

      const GeometryResult g_iid = measure_geometry(ref_iid, sc_iid, cfg);
      const GeometryResult g_km = measure_geometry(km.raw, sc_km, cfg);

      Row r;
      r.bits = bits;
      r.events = events;
      r.kbits = cfg.key_bits();
      r.km_disagr = g_km.disagree_rate;
      r.km_exact = g_km.exact_pct;
      r.km_p2 = g_km.p2_pct;
      r.km_p4 = g_km.p4_pct;
      r.iid_p4 = g_iid.p4_pct;

      const std::size_t fit_positions =
          km_big.raw.size() / kSpe >= events ? km_big.raw.size() / kSpe - events + 1 : 0;
      r.distinct = distinct_keys_in(km_big.raw, sc_km_big, cfg);
      r.keff = effective_key_space(fit_positions, r.distinct);
      r.occ = r.keff > 0.0 ? kHumanEvents / r.keff : 0.0;
      r.capsurv = r.occ <= kCap ? 1.0 : kCap / r.occ;
      r.proj = r.km_p4 * r.capsurv;
      rows.push_back(r);

      std::printf("        %4u %2u %5u | %10.2f%% %7.1f%% | %11.2f%% %8.1f%% | %+8.1f\n",
                  bits, events, cfg.key_bits(), 100.0 * g_iid.disagree_rate, g_iid.p4_pct,
                  100.0 * g_km.disagree_rate, g_km.p4_pct, g_km.p4_pct - g_iid.p4_pct);
    }
  }

  std::printf("\n        TABLE B -- effective key space from the correlated reference,\n");
  std::printf("        and the human-scale projection that follows from it\n");
  std::printf("        bits ev kbits | representable  distinct@%zu   effective | occup  capsurv  proj4pr\n",
              kBigEvents);
  for (const Row& r : rows) {
    const double representable = std::pow(2.0, static_cast<double>(r.kbits));
    std::printf("        %4u %2u %5u | %13.2e %12zu %11.2e | %5.2f %7.1f%% %8.1f%%\n",
                r.bits, r.events, r.kbits, representable, r.distinct, r.keff, r.occ,
                100.0 * r.capsurv, r.proj);
  }

  const auto best = std::max_element(rows.begin(), rows.end(),
                                     [](const Row& a, const Row& b) { return a.proj < b.proj; });
  if (best != rows.end()) {
    std::printf("\n        best projected human-scale 4-probe recall on CORRELATED signal:\n"
                "          %u bits x %u events = %u-bit keys -> %.1f%% "
                "(exact %.1f%%, 2-probe %.1f%%)\n",
                best->bits, best->events, best->kbits, best->proj, best->km_exact,
                best->km_p2);
    std::printf("          effective key space %.2e vs %.2e representable (%.1fx smaller)\n",
                best->keff, std::pow(2.0, static_cast<double>(best->kbits)),
                std::pow(2.0, static_cast<double>(best->kbits)) / std::max(1.0, best->keff));
  }

  CHECK(!rows.empty(), "sweep produced rows");
}

// ---------------------------------------------------------------------------
// Minimizer window penalty
// ---------------------------------------------------------------------------
//
// Subsampling is not optional: at one entry per base, 8-byte entries and a 0.5 load
// factor, a human-scale index is ~69 GB at w=1. It has to come under ~10 GB, which
// means w of about 10 or more.
//
// The cost is a COMPOUND failure that the earlier measurements could not see. A seed
// survives only if BOTH hold:
//   1. the query's key equals the reference's key at that position (6.89% per-event
//      disagreement, amplified over the key), and
//   2. that position wins its minimizer window on BOTH sides
// A single bucket flip changes the key's HASH, which can hand the window to a
// different position entirely -- so the seed is lost before the key match is ever
// evaluated. Condition 2 is the unmeasured one.
//
// Also measured: whether diagonal voting still has enough votes once subsampling
// thins the true seeds. Chaining margins of 40x+ were measured with hundreds of
// candidates; at w=20 a 400-event read offers only a few dozen reference minimizers,
// and consensus could become fragile.
void test_minimizer_window_penalty() {
  banner("minimizer_window_penalty");

  mru::QuantConfig base;
  base.bits_per_event = 3;
  base.events_per_key = 15;  // the candidate geometry from the corrected sweep

  constexpr std::size_t kRefEvents = 3200000;
  constexpr std::uint32_t kPoreK = 9;
  constexpr std::size_t kQueryLen = 4000;  // ~2.5 chunks
  constexpr std::size_t kWindows = 20;
  constexpr double kHumanEvents = 3.1e9;

  const KmerRef km = synth_signal_kmer(kRefEvents, kPoreK, 7, base.samples_per_event);
  const auto sc_ref = mru::scaling_from_samples(km.raw);

  std::vector<mru::QEvent> ref_ev;
  mru::quantise_signal(km.raw, sc_ref, base, ref_ev);
  std::vector<mru::SeedHash> ref_all;
  mru::hash_all_keys(ref_ev, base, ref_all);

  std::printf("        geometry %u bits x %u events = %u-bit keys, correlated %u-mer signal\n",
              base.bits_per_event, base.events_per_key, base.key_bits(), kPoreK);
  std::printf("        reference %zu events, %zu keys at w=1\n\n", ref_ev.size(),
              ref_all.size());

  std::printf("        w | idx entries  kept%%  human idx | ref mins q mins  rec@1  @2   @4 "
              "| recall@4  diag ok  margin\n");

  for (std::uint32_t w : {1u, 5u, 10u, 20u}) {
    mru::QuantConfig cfg = base;
    cfg.minimizer_window = w;

    std::vector<mru::SeedHash> ref_min;
    mru::select_minimizers(ref_all, w, ref_min);
    std::vector<std::uint32_t> ref_off;
    ref_off.reserve(ref_min.size());
    for (const mru::SeedHash& sh : ref_min) ref_off.push_back(sh.offset);

    mru::MinimizerIndex idx;
    idx.build(ref_min);

    const double kept = ref_all.empty() ? 0.0
                                        : static_cast<double>(ref_min.size()) /
                                              static_cast<double>(ref_all.size());
    // Human-scale index: entries x 8 bytes, at 0.5 load factor.
    const double human_gb = kHumanEvents * kept / 0.5 * 8.0 / 1e9;

    std::size_t avail_total = 0, rec1 = 0, rec2 = 0, rec4 = 0, qmins_total = 0;
    std::size_t diag_ok = 0, scored = 0;
    double margin_sum = 0.0;

    for (std::size_t wi = 0; wi < kWindows; ++wi) {
      const std::size_t raw_off =
          (km.raw.size() - kQueryLen) * (wi + 1) / (kWindows + 1);
      const std::size_t aligned =
          (raw_off / base.samples_per_event) * base.samples_per_event;
      const std::vector<std::int16_t> query(
          km.raw.begin() + static_cast<std::ptrdiff_t>(aligned),
          km.raw.begin() + static_cast<std::ptrdiff_t>(aligned + kQueryLen));
      const auto q_scaling = mru::scaling_from_samples(query);
      const auto z_qry = event_z(query, q_scaling, cfg);
      if (z_qry.size() < cfg.events_per_key) continue;

      std::vector<mru::QEvent> b_qry(z_qry.size());
      for (std::size_t j = 0; j < z_qry.size(); ++j) {
        b_qry[j] = mru::quantise_z(z_qry[j], cfg);
      }
      const auto true_diag =
          static_cast<std::int64_t>(aligned / base.samples_per_event);
      const std::size_t n_pos = z_qry.size() - cfg.events_per_key + 1;

      // How many reference minimizers exist in this region at all? That is the
      // ceiling on what any query can recover.
      const auto lo = std::lower_bound(ref_off.begin(), ref_off.end(),
                                       static_cast<std::uint32_t>(true_diag));
      const auto hi = std::upper_bound(ref_off.begin(), ref_off.end(),
                                       static_cast<std::uint32_t>(true_diag + static_cast<std::int64_t>(n_pos)));
      avail_total += static_cast<std::size_t>(hi - lo);

      // Query side selects its own minimizers from its own noisy keys.
      std::vector<mru::SeedHash> q_all;
      q_all.reserve(n_pos);
      for (std::size_t j = 0; j < n_pos; ++j) {
        q_all.push_back(mru::SeedHash{mru::hash64(mru::pack_key(b_qry.data() + j, cfg)),
                                      static_cast<std::uint32_t>(j)});
      }
      std::vector<mru::SeedHash> q_min;
      mru::select_minimizers(q_all, w, q_min);
      qmins_total += q_min.size();

      std::vector<Candidate> cands;
      for (const mru::SeedHash& qm : q_min) {
        const std::size_t j = qm.offset;
        std::array<std::uint64_t, 4> keys{};
        const std::size_t nk = variant_keys(b_qry, z_qry, j, cfg, 4, keys);
        const auto want = true_diag + static_cast<std::int64_t>(j);

        bool hit1 = false, hit2 = false, hit4 = false;
        for (std::size_t k = 0; k < nk; ++k) {
          std::uint64_t pos[16];
          const std::size_t n = idx.query(mru::hash64(keys[k]), pos);
          for (std::size_t q = 0; q < n; ++q) {
            cands.push_back(Candidate{static_cast<std::uint32_t>(j), pos[q]});
            if (std::llabs(static_cast<std::int64_t>(pos[q]) - want) <= 1) {
              if (k == 0) hit1 = true;
              if (k <= 1) hit2 = true;
              hit4 = true;
            }
          }
        }
        if (hit1) ++rec1;
        if (hit2) ++rec2;
        if (hit4) ++rec4;
      }

      if (!cands.empty()) {
        std::vector<std::int64_t> diags;
        diags.reserve(cands.size());
        for (const Candidate& c : cands) {
          diags.push_back(static_cast<std::int64_t>(c.ref_position) -
                          static_cast<std::int64_t>(c.key_offset));
        }
        std::sort(diags.begin(), diags.end());
        std::int64_t best_diag = diags[0];
        std::size_t best_votes = 0, second_votes = 0, run = 1;
        for (std::size_t j = 1; j <= diags.size(); ++j) {
          if (j < diags.size() && diags[j] == diags[j - 1]) {
            ++run;
            continue;
          }
          if (run > best_votes) {
            second_votes = best_votes;
            best_votes = run;
            best_diag = diags[j - 1];
          } else if (run > second_votes) {
            second_votes = run;
          }
          run = 1;
        }
        ++scored;
        if (std::llabs(best_diag - true_diag) <= 1) ++diag_ok;
        margin_sum += static_cast<double>(best_votes) /
                      static_cast<double>(std::max<std::size_t>(1, second_votes));
      }
    }

    const double recall4 = avail_total == 0
                               ? 0.0
                               : 100.0 * static_cast<double>(rec4) /
                                     static_cast<double>(avail_total);
    // q mins is reported, not merely accumulated: the gap between the reference's
    // minimizers in a region and the query's own selections is the SELECTION
    // mismatch, which is the failure multi-probe cannot repair.
    std::printf("        %2u | %11zu %5.1f%% %7.1f GB | %8.1f %6.1f %6.1f %5.1f %5.1f | "
                "%7.1f%% %6zu/%-3zu %6.1fx\n",
                w, ref_min.size(), 100.0 * kept, human_gb,
                static_cast<double>(avail_total) / static_cast<double>(kWindows),
                static_cast<double>(qmins_total) / static_cast<double>(kWindows),
                static_cast<double>(rec1) / static_cast<double>(kWindows),
                static_cast<double>(rec2) / static_cast<double>(kWindows),
                static_cast<double>(rec4) / static_cast<double>(kWindows), recall4,
                diag_ok, scored,
                scored == 0 ? 0.0 : margin_sum / static_cast<double>(scored));
  }

  std::printf("\n        'ref mins' and 'recovered' are per query window (%zu events).\n"
              "        recall@4 is recovered/available: the ceiling is the reference\n"
              "        minimizers that exist in the region, not the query's own count.\n",
              kQueryLen / base.samples_per_event);

  CHECK(true, "measurement completed");
}

// Signal from an EXISTING level table but different DNA. Needed for the off-target
// control: a real off-target read comes off the same pore chemistry, so only the
// sequence may differ. Regenerating the level table too would make the control
// trivially easy and the result meaningless.
std::vector<std::int16_t> synth_signal_from_levels(const std::vector<float>& levels,
                                                   std::uint32_t k, std::size_t n_events,
                                                   std::uint64_t dna_seed,
                                                   std::uint32_t samples_per_event) {
  std::vector<std::int16_t> raw;
  std::mt19937_64 rng(dna_seed);
  static const char kBases[] = "ACGT";
  std::uniform_int_distribution<int> base_pick(0, 3);
  std::normal_distribution<float> noise(0.0f, 15.0f);

  std::string dna(n_events + k - 1, 'A');
  for (char& c : dna) c = kBases[base_pick(rng)];

  raw.reserve(n_events * samples_per_event);
  std::uint64_t kmer = 0;
  std::uint32_t have = 0;
  const std::uint64_t mask = (std::uint64_t{1} << (2 * k)) - 1;
  for (char c : dna) {
    int code = 0;
    switch (c) {
      case 'C': code = 1; break;
      case 'G': code = 2; break;
      case 'T': code = 3; break;
      default: code = 0; break;
    }
    kmer = ((kmer << 2) | static_cast<std::uint64_t>(code)) & mask;
    if (++have < k) continue;
    const float level = levels[kmer];
    for (std::uint32_t t = 0; t < samples_per_event; ++t) {
      raw.push_back(
          static_cast<std::int16_t>(std::clamp(level + noise(rng), -30000.0f, 30000.0f)));
    }
  }
  return raw;
}

struct VoteResult {
  std::size_t candidates = 0;
  std::size_t best_votes = 0;
  std::size_t second_votes = 0;
  std::int64_t best_diag = 0;
};

// One read through the index: select query minimizers, probe with the given budget,
// bin candidates by diagonal, return the winner and runner-up.
VoteResult vote_for_read(const mru::MinimizerIndex& idx,
                         const std::vector<std::int16_t>& query,
                         const mru::QuantConfig& cfg, int budget) {
  VoteResult r;
  const auto q_scaling = mru::scaling_from_samples(query);
  const auto z_qry = event_z(query, q_scaling, cfg);
  if (z_qry.size() < cfg.events_per_key) return r;

  std::vector<mru::QEvent> b_qry(z_qry.size());
  for (std::size_t j = 0; j < z_qry.size(); ++j) b_qry[j] = mru::quantise_z(z_qry[j], cfg);

  const std::size_t n_pos = z_qry.size() - cfg.events_per_key + 1;
  std::vector<mru::SeedHash> q_all;
  q_all.reserve(n_pos);
  for (std::size_t j = 0; j < n_pos; ++j) {
    q_all.push_back(mru::SeedHash{mru::hash64(mru::pack_key(b_qry.data() + j, cfg)),
                                  static_cast<std::uint32_t>(j)});
  }
  std::vector<mru::SeedHash> q_min;
  mru::select_minimizers(q_all, cfg.minimizer_window, q_min);

  std::vector<std::int64_t> diags;
  for (const mru::SeedHash& qm : q_min) {
    std::array<std::uint64_t, 4> keys{};
    const std::size_t nk = variant_keys(b_qry, z_qry, qm.offset, cfg, budget, keys);
    for (std::size_t k = 0; k < nk; ++k) {
      std::uint64_t pos[16];
      const std::size_t n = idx.query(mru::hash64(keys[k]), pos);
      for (std::size_t q = 0; q < n; ++q) {
        diags.push_back(static_cast<std::int64_t>(pos[q]) -
                        static_cast<std::int64_t>(qm.offset));
      }
    }
  }
  r.candidates = diags.size();
  if (diags.empty()) return r;

  std::sort(diags.begin(), diags.end());
  std::size_t run = 1;
  for (std::size_t j = 1; j <= diags.size(); ++j) {
    if (j < diags.size() && diags[j] == diags[j - 1]) {
      ++run;
      continue;
    }
    if (run > r.best_votes) {
      r.second_votes = r.best_votes;
      r.best_votes = run;
      r.best_diag = diags[j - 1];
    } else if (run > r.second_votes) {
      r.second_votes = run;
    }
    run = 1;
  }
  return r;
}

double percentile_of(std::vector<std::size_t> v, double q) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const auto idx = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
  return static_cast<double>(v[idx]);
}

// ---------------------------------------------------------------------------
// Off-target false positives: the measurement the decision actually rests on
// ---------------------------------------------------------------------------
//
// Every earlier measurement queried a read drawn FROM the reference, so all of them
// measured sensitivity and none measured specificity of the DECISION. Adaptive
// sampling unblocks when a read is not from the target, so the false-positive rate
// is the whole game: 41% seed recall is irrelevant if an off-target read also
// produces a dominant diagonal.
//
// Controls, both deliberately unflattering:
//   * off-target reads use the SAME pore model and the same noise, differing only in
//     DNA. A white-noise control would separate trivially and prove nothing.
//   * the 3.2M-event reference is ~1000x smaller than a genome, so chance collisions
//     are ~1000x rarer here than they would really be. The second configuration has
//     its occupancy matched to human scale so the collision regime is realistic,
//     which is the best that can be done without a 69 GB index.
void test_offtarget_false_positives() {
  banner("offtarget_false_positives");

  constexpr std::size_t kRefEvents = 3200000;
  constexpr std::uint32_t kPoreK = 9;
  constexpr std::size_t kQueryLen = 4000;
  constexpr std::size_t kReads = 200;
  constexpr double kHumanEvents = 3.1e9;

  mru::QuantConfig base;
  base.samples_per_event = 10;
  const KmerRef km = synth_signal_kmer(kRefEvents, kPoreK, 7, base.samples_per_event);
  const auto sc_ref = mru::scaling_from_samples(km.raw);

  // Off-target: same pore model, different DNA.
  const auto off_raw = synth_signal_from_levels(km.levels, kPoreK, 120000, 424242,
                                                base.samples_per_event);

  struct Cfg {
    std::uint32_t bits, events, window;
    const char* note;
  };
  const Cfg configs[] = {
      {3, 15, 10, "chosen geometry (optimistic: ref 1000x smaller than genome)"},
      {3, 11, 10, "occupancy-matched analogue of human scale"},
      {3, 10, 10, "occupancy-matched, slightly pessimistic"},
  };

  for (const Cfg& c : configs) {
    mru::QuantConfig cfg = base;
    cfg.bits_per_event = c.bits;
    cfg.events_per_key = c.events;
    cfg.minimizer_window = c.window;
    if (!cfg.valid()) continue;

    std::vector<mru::QEvent> ref_ev;
    mru::quantise_signal(km.raw, sc_ref, cfg, ref_ev);
    std::vector<mru::SeedHash> ref_all;
    mru::hash_all_keys(ref_ev, cfg, ref_all);
    std::vector<mru::SeedHash> ref_min;
    mru::select_minimizers(ref_all, cfg.minimizer_window, ref_min);
    mru::MinimizerIndex idx;
    idx.build(ref_min);

    const std::size_t fit_positions = ref_all.size();
    const std::size_t distinct = distinct_keys_in(km.raw, sc_ref, cfg);
    const double keff = effective_key_space(fit_positions, distinct);
    const double this_occ = static_cast<double>(ref_min.size()) / keff;
    const double human_occ = kHumanEvents * (static_cast<double>(ref_min.size()) /
                                             static_cast<double>(ref_all.size())) / keff;

    std::vector<std::size_t> on_votes, off_votes, on_cands, off_cands;
    std::size_t on_diag_ok = 0;

    for (std::size_t r = 0; r < kReads; ++r) {
      const std::size_t raw_off = (km.raw.size() - kQueryLen) * (r + 1) / (kReads + 1);
      const std::size_t aligned = (raw_off / cfg.samples_per_event) * cfg.samples_per_event;
      const std::vector<std::int16_t> q(
          km.raw.begin() + static_cast<std::ptrdiff_t>(aligned),
          km.raw.begin() + static_cast<std::ptrdiff_t>(aligned + kQueryLen));
      const VoteResult v = vote_for_read(idx, q, cfg, 4);
      on_votes.push_back(v.best_votes);
      on_cands.push_back(v.candidates);
      const auto true_diag = static_cast<std::int64_t>(aligned / cfg.samples_per_event);
      if (v.best_votes > 0 && std::llabs(v.best_diag - true_diag) <= 1) ++on_diag_ok;
    }

    for (std::size_t r = 0; r < kReads; ++r) {
      const std::size_t raw_off = (off_raw.size() - kQueryLen) * (r + 1) / (kReads + 1);
      const std::size_t aligned = (raw_off / cfg.samples_per_event) * cfg.samples_per_event;
      const std::vector<std::int16_t> q(
          off_raw.begin() + static_cast<std::ptrdiff_t>(aligned),
          off_raw.begin() + static_cast<std::ptrdiff_t>(aligned + kQueryLen));
      const VoteResult v = vote_for_read(idx, q, cfg, 4);
      off_votes.push_back(v.best_votes);
      off_cands.push_back(v.candidates);
    }

    // Smallest vote threshold with zero false positives, and the TPR it gives.
    const std::size_t off_max = *std::max_element(off_votes.begin(), off_votes.end());
    const std::size_t thresh = off_max + 1;
    std::size_t tp = 0;
    for (std::size_t v : on_votes) {
      if (v >= thresh) ++tp;
    }
    const double tpr = 100.0 * static_cast<double>(tp) / static_cast<double>(on_votes.size());

    std::printf("\n        %u bits x %u ev, w=%u  --  %s\n", c.bits, c.events, c.window,
                c.note);
    std::printf("          effective key space %.2e | this ref occupancy %.4f | "
                "human-scale occupancy %.3f\n",
                keff, this_occ, human_occ);
    std::printf("          ON-target : cands med %.0f | best votes min %.0f med %.0f max %.0f "
                "| correct diag %zu/%zu\n",
                percentile_of(on_cands, 0.5), percentile_of(on_votes, 0.0),
                percentile_of(on_votes, 0.5), percentile_of(on_votes, 1.0), on_diag_ok,
                on_votes.size());
    std::printf("          OFF-target: cands med %.0f | best votes med %.0f p95 %.0f max %.0f\n",
                percentile_of(off_cands, 0.5), percentile_of(off_votes, 0.5),
                percentile_of(off_votes, 0.95), percentile_of(off_votes, 1.0));
    std::printf("          threshold >=%zu votes -> FPR 0/%zu, TPR %.1f%%  (separation gap "
                "%.0f -> %.0f)\n",
                thresh, off_votes.size(), tpr, percentile_of(off_votes, 1.0),
                percentile_of(on_votes, 0.0));
  }

  CHECK(true, "measurement completed");
}

// ---------------------------------------------------------------------------
// Frozen geometry tripwire, and multi-probe through the library API
// ---------------------------------------------------------------------------
//
// The defaults are not preferences, they are the conclusion of a measurement chain:
// key space vs pigeonhole, k-mer correlation vs effective key space, bucket width vs
// per-event disagreement, window size vs index memory, and finally the off-target
// vote separation that sets the unblock threshold at >= 4 votes.
//
// Changing any default silently invalidates that threshold. This test exists to make
// that impossible to do by accident: if it fails, the off-target separation must be
// re-measured before the engine can be trusted to decide anything.
void test_frozen_geometry() {
  banner("frozen_geometry");
  const mru::QuantConfig cfg;  // defaults

  CHECK_EQ(cfg.bits_per_event, 3u, "FROZEN bits_per_event");
  CHECK_EQ(cfg.events_per_key, 15u, "FROZEN events_per_key");
  CHECK_EQ(cfg.minimizer_window, 10u, "FROZEN minimizer_window");
  CHECK_EQ(cfg.key_bits(), 45u, "FROZEN 45-bit keys");
  CHECK_EQ(cfg.levels(), 8u, "8 quantisation levels");
  CHECK_EQ(cfg.samples_per_event, 10u, "10 samples/event at 4 kHz and ~400 b/s");
  CHECK(cfg.valid(), "frozen config is valid");

  CHECK_EQ(mru::kDefaultProbeBudget, 4, "default probe budget is 4 keys/minimizer");
  CHECK_EQ(static_cast<int>(mru::ProbeBudget::kExact), 1, "kExact == 1 probe");
  CHECK_EQ(static_cast<int>(mru::ProbeBudget::kOneFlip), 2, "kOneFlip == 2 probes");
  CHECK_EQ(static_cast<int>(mru::ProbeBudget::kTwoFlips), 4, "kTwoFlips == 4 probes");

  std::printf("        %u bits x %u events = %u-bit keys, window %u, %u levels\n",
              cfg.bits_per_event, cfg.events_per_key, cfg.key_bits(),
              cfg.minimizer_window, cfg.levels());
}

void test_multiprobe_api() {
  banner("multiprobe_api");
  const mru::QuantConfig cfg;  // frozen defaults, window 10

  // Correlated reference, as the real thing will be.
  const KmerRef km = synth_signal_kmer(200000, 9, 11, cfg.samples_per_event);
  const auto sc_ref = mru::scaling_from_samples(km.raw);

  std::vector<mru::QEvent> ref_ev;
  mru::quantise_signal(km.raw, sc_ref, cfg, ref_ev);
  std::vector<mru::SeedHash> ref_all, ref_min;
  mru::hash_all_keys(ref_ev, cfg, ref_all);
  mru::select_minimizers(ref_all, cfg.minimizer_window, ref_min);
  mru::MinimizerIndex idx;
  idx.build(ref_min);
  CHECK_EQ(idx.insert_failures(), 0u, "index built without hitting the probe bound");

  // quantise_signal must now be able to hand back the pre-quantisation z values,
  // because expand_multiprobe cannot recover them from the buckets.
  constexpr std::size_t kOff = 40000, kLen = 4000;
  const std::vector<std::int16_t> query(km.raw.begin() + kOff,
                                        km.raw.begin() + kOff + kLen);
  const auto q_sc = mru::scaling_from_samples(query);
  std::vector<mru::QEvent> q_ev;
  std::vector<float> q_z;
  mru::quantise_signal(query, q_sc, cfg, q_ev, &q_z);
  CHECK_EQ(q_ev.size(), q_z.size(), "one z value per quantised event");
  CHECK_EQ(q_ev.size(), kLen / cfg.samples_per_event, "event count");

  std::vector<mru::SeedHash> q_all, q_min;
  mru::hash_all_keys(q_ev, cfg, q_all);
  mru::select_minimizers(q_all, cfg.minimizer_window, q_min);
  CHECK(!q_min.empty(), "query selected minimizers");

  // Expansion arity, and that variants inherit the original seed's offset so chaining
  // sees one seed position however many keys were probed for it.
  for (int budget : {1, 2, 4}) {
    std::vector<mru::SeedHash> probes;
    mru::expand_multiprobe(q_ev, q_z, q_min, cfg, budget, probes);
    CHECK_EQ(probes.size(), q_min.size() * static_cast<std::size_t>(budget),
             "one group of `budget` keys per minimizer");

    bool offsets_ok = true, exact_first = true;
    for (std::size_t j = 0; j < q_min.size(); ++j) {
      const std::size_t base = j * static_cast<std::size_t>(budget);
      if (probes[base].hash != q_min[j].hash) exact_first = false;
      for (int k = 0; k < budget; ++k) {
        if (probes[base + static_cast<std::size_t>(k)].offset != q_min[j].offset) {
          offsets_ok = false;
        }
      }
    }
    CHECK(exact_first, "the first key of each group is the unmodified key");
    CHECK(offsets_ok, "every variant carries the original minimizer's offset");
  }

  // End to end: more probes must never find fewer matches.
  mru::ProbeScratch scratch;
  scratch.reserve_for(kLen, cfg);
  mru::IndexStats s1, s4;
  const std::size_t m1 = mru::match_signal(idx, query, q_sc, cfg, scratch, 1, &s1);
  const std::size_t probes1 = scratch.probes.size();
  const std::size_t m4 = mru::match_signal(idx, query, q_sc, cfg, scratch, 4, &s4);
  const std::size_t probes4 = scratch.probes.size();

  CHECK_EQ(probes4, probes1 * 4, "4x the probes for budget 4");
  CHECK(m4 >= m1, "more probes cannot find fewer matches");
  CHECK(m1 > 0, "the exact-key path finds something on an on-target read");

  std::printf("        %zu minimizers | budget 1: %zu probes -> %zu hits | "
              "budget 4: %zu probes -> %zu hits (+%.0f%%)\n",
              q_min.size(), probes1, m1, probes4, m4,
              m1 == 0 ? 0.0 : 100.0 * (static_cast<double>(m4) - static_cast<double>(m1)) /
                                  static_cast<double>(m1));
  std::printf("        reminder: at the frozen w=10 this gain is ~+5 points of seed\n"
              "        recall, not the +74%% seen at w=1. See batched_probe.hpp.\n");
}

}  // namespace

int main() {
  std::printf("quantised-event index tests\n\n");

  test_frozen_geometry();
  test_multiprobe_api();
  test_quantisation();
  test_index_basics();
  test_batched_equals_scalar();
  test_batched_throughput();
  test_recall_harness();
  test_bucket_disagreement();
  test_specificity_scaling();
  test_geometry_sweep();
  test_minimizer_window_penalty();
  test_offtarget_false_positives();

  std::printf("\n%s  (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED",
              g_failures, g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
