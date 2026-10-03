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
#include <cmath>
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

// A plausible-looking signal: a slow random walk with per-sample noise, which is
// closer to real squiggle than white noise and makes the recall numbers less
// flattering than they would otherwise be.
std::vector<std::int16_t> synth_signal(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<float> step(0.0f, 6.0f);
  std::normal_distribution<float> noise(0.0f, 10.0f);
  std::vector<std::int16_t> out;
  out.reserve(n);
  float level = 500.0f;
  for (std::size_t i = 0; i < n; ++i) {
    if (i % 10 == 0) level += step(rng);  // new "base" every ~10 samples
    out.push_back(static_cast<std::int16_t>(std::clamp(level + noise(rng), -30000.0f, 30000.0f)));
  }
  return out;
}

void test_quantisation() {
  banner("quantisation");
  mru::QuantConfig cfg;
  CHECK(cfg.valid(), "default config is valid");
  CHECK_EQ(cfg.levels(), 8u, "3 bits == 8 levels");
  CHECK_EQ(cfg.key_bits(), 27u, "9 events x 3 bits");

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

  std::printf("        mean probes %.3f, single-probe %.1f%%, fp rejects %llu\n",
              stats.mean_probes(), 100.0 * stats.single_probe_fraction(),
              static_cast<unsigned long long>(stats.fingerprint_rejects));
  // The design claim: one slot touch in the common case. At load factor 0.4 with
  // linear probing this should be comfortably above 60%.
  CHECK(stats.single_probe_fraction() > 0.55,
        "most queries resolve in a single slot touch");

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
  std::printf("        %zu queries compared, hits %llu, mean probes %.3f\n",
              queries.size(), static_cast<unsigned long long>(s_batched.hits),
              s_batched.mean_probes());
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

  std::printf("        scalar %.1f ns/query, batched %.1f ns/query, speedup %.2fx\n",
              scalar_ns, batched_ns, scalar_ns / std::max(1e-9, batched_ns));
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
  std::printf("        reference: %zu events, %zu seeds, %zu slots, load %.2f\n",
              ref_events.size(), ref_seeds.size(), idx.capacity(), idx.load_factor());

  // A region of the reference, as a read would present it.
  constexpr std::size_t kQueryOffset = 50'000;
  constexpr std::size_t kQueryLen = 4'000;
  const std::vector<std::int16_t> clean(reference.begin() + kQueryOffset,
                                        reference.begin() + kQueryOffset + kQueryLen);
  const std::uint32_t expected_first_event =
      static_cast<std::uint32_t>(kQueryOffset / cfg.samples_per_event);

  std::printf("        noise sd | seeds | recall\n");
  bool zero_noise_ok = false;

  for (float sd : {0.0f, 2.0f, 5.0f, 10.0f, 20.0f}) {
    std::mt19937_64 rng(1234);
    std::normal_distribution<float> noise(0.0f, sd);
    std::vector<std::int16_t> query(clean.size());
    for (std::size_t i = 0; i < clean.size(); ++i) {
      const float v = static_cast<float>(clean[i]) + (sd > 0.0f ? noise(rng) : 0.0f);
      query[i] = static_cast<std::int16_t>(std::clamp(v, -30000.0f, 30000.0f));
    }

    mru::ProbeScratch scratch;
    scratch.reserve_for(query.size(), cfg);
    mru::IndexStats stats;
    // Scale from the query itself: a real read has no access to reference stats.
    const auto q_scaling = mru::scaling_from_samples(query);
    (void)mru::match_signal(idx, query, q_scaling, cfg, scratch, &stats);

    // A seed is recovered if any returned position is the correct reference
    // position for that seed, within a one-event slop for window alignment.
    std::size_t recovered = 0;
    for (std::size_t i = 0; i < scratch.minimizers.size(); ++i) {
      const std::uint64_t want = expected_first_event + scratch.minimizers[i].offset;
      const mru::SeedMatches& m = scratch.matches[i];
      for (std::uint32_t j = 0; j < m.count; ++j) {
        const std::uint64_t got = m.positions[j];
        const std::uint64_t diff = got > want ? got - want : want - got;
        if (diff <= 1) {
          ++recovered;
          break;
        }
      }
    }
    const double recall =
        scratch.minimizers.empty()
            ? 0.0
            : static_cast<double>(recovered) / static_cast<double>(scratch.minimizers.size());
    std::printf("        %8.1f | %5zu | %6.1f%%\n", static_cast<double>(sd),
                scratch.minimizers.size(), 100.0 * recall);

    if (sd == 0.0f) {
      zero_noise_ok = recall > 0.95;
    }
  }

  // Zero noise is not a research question: an identical signal must match itself.
  // If this fails, the pipeline is broken somewhere, not merely insensitive.
  CHECK(zero_noise_ok, "recall at zero added noise is near-perfect");
}

}  // namespace

int main() {
  std::printf("quantised-event index tests\n\n");

  test_quantisation();
  test_index_basics();
  test_batched_equals_scalar();
  test_batched_throughput();
  test_recall_harness();

  std::printf("\n%s  (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED",
              g_failures, g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
