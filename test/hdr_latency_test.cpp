// test/hdr_latency_test.cpp
//
// Why this test exists, specifically.
//
// Before HdrHistogram, latency went into CycleHistogram's power-of-two buckets. On that
// instrument a measured p99 could only ever be one of 54.19, 108.38, 216.76 or 433.53 us
// on this host, and it duly reported 111.93 and 112.12 us on two consecutive pinned runs.
// That was read -- by me -- as "pinning made the tail reproducible to 0.2%". It meant
// nothing of the sort. It meant both runs landed in the same octave, somewhere in
// [54, 108] us, and the agreement was the bucket edge, not the data. Real percentiles
// later put those same runs 3x apart.
//
// So the assertions below are not about hdr_init returning non-null. They are about the
// one property the project now depends on: a reported percentile must track the true
// percentile closely enough that a change in the number means a change in the system.
// test_resolves_within_one_octave encodes the exact failure above -- two distributions
// that differ by 1.7x inside one octave must come back as different numbers.
//
// The test is built in both configurations. With HdrHistogram it enforces the 0.1%
// relative-error guarantee; with the fallback it enforces only that a percentile is a
// valid upper bound, and prints what the coarse instrument does to the same input. The
// fallback's collapse is reported rather than asserted: it is a limitation to see, not a
// behaviour to lock in.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/hdr_latency.hpp"

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

void banner(const char* name) { std::printf("[ RUN ] %s\n", name); }

// Nearest-rank percentile, which is the definition HdrHistogram uses: the smallest value
// whose cumulative count reaches ceil(p * n). Computing it the same way here means a
// disagreement is the histogram's error and not a convention mismatch.
std::uint64_t true_percentile(std::vector<std::uint64_t> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const double exact = p * static_cast<double>(v.size());
  std::size_t rank = static_cast<std::size_t>(exact);
  if (exact > static_cast<double>(rank)) ++rank;  // ceil
  if (rank == 0) rank = 1;
  if (rank > v.size()) rank = v.size();
  return v[rank - 1];
}

double rel_error(std::uint64_t got, std::uint64_t want) {
  if (want == 0) return got == 0 ? 0.0 : 1.0;
  const double d = static_cast<double>(got) - static_cast<double>(want);
  return (d < 0 ? -d : d) / static_cast<double>(want);
}

// 3 significant figures promises 0.1%. Allowing 0.3% leaves room for the one-rank step
// between adjacent recorded values without letting a real 2x bucketing error through.
constexpr double kTolerance = 0.003;

// A deterministic heavy-tailed shape: a dense body with a thin decade-spanning tail, which
// is what the decision path actually produces. No RNG, so a failure reproduces exactly.
std::vector<std::uint64_t> synthetic_latencies(std::uint64_t body, std::uint64_t tail_mul) {
  std::vector<std::uint64_t> v;
  v.reserve(100000);
  for (int i = 0; i < 99000; ++i) {
    v.push_back(body + static_cast<std::uint64_t>(i % 400));
  }
  for (int i = 0; i < 1000; ++i) {
    v.push_back(body * tail_mul + static_cast<std::uint64_t>(i * 37 % 5000));
  }
  return v;
}

void test_percentiles_track_truth() {
  banner("percentiles track the true distribution");
  const auto v = synthetic_latencies(27000, 12);

  mru::LatencyRecorder r;
  CHECK(r.usable(), "recorder must be usable in either configuration");
  for (std::uint64_t x : v) r.record(x);

  CHECK(r.count() == v.size(), "every recorded sample must be counted");
  CHECK(r.out_of_range() == 0, "nothing here exceeds the default ceiling");

  const double ps[] = {0.50, 0.90, 0.99, 0.999};
  for (double p : ps) {
    const std::uint64_t want = true_percentile(v, p);
    const std::uint64_t got = r.percentile(p);
    const double err = rel_error(got, want);
    std::printf("    p%-6g true %-10llu reported %-10llu rel.err %7.4f%%\n", p * 100.0,
                static_cast<unsigned long long>(want),
                static_cast<unsigned long long>(got), err * 100.0);
    if (mru::LatencyRecorder::exact()) {
      CHECK(err <= kTolerance, "HdrHistogram must stay inside its stated relative error");
    } else {
      // The coarse instrument is only ever allowed to over-report. A percentile BELOW
      // the truth would make the tail look better than it is, which is the one direction
      // that must never happen.
      CHECK(got >= want,
            "a bucketed percentile must be an upper bound, never an under-report");
    }
  }

  // max and mean are read off the same structure and are quoted alongside percentiles,
  // so they get the same treatment.
  const std::uint64_t want_max = *std::max_element(v.begin(), v.end());
  if (mru::LatencyRecorder::exact()) {
    CHECK(rel_error(r.max(), want_max) <= kTolerance, "max must be accurate");
  } else {
    CHECK(r.max() >= want_max, "max must not under-report");
  }
  CHECK(r.min() <= true_percentile(v, 0.0), "min must not over-report");
}

void test_resolves_within_one_octave() {
  banner("two tails inside one octave must not collapse to one number");
  // 1.7x apart, deliberately chosen to sit inside a single power-of-two bucket. This is
  // the comparison the old histogram could not make, and the reason a 2x "improvement"
  // in p99 was indistinguishable from a value drifting across a bucket edge.
  const std::vector<std::uint64_t> a(20000, 1100);
  const std::vector<std::uint64_t> b(20000, 1900);

  mru::LatencyRecorder ra;
  mru::LatencyRecorder rb;
  for (std::uint64_t x : a) ra.record(x);
  for (std::uint64_t x : b) rb.record(x);

  const std::uint64_t pa = ra.percentile(0.99);
  const std::uint64_t pb = rb.percentile(0.99);
  std::printf("    1100 -> %llu, 1900 -> %llu\n", static_cast<unsigned long long>(pa),
              static_cast<unsigned long long>(pb));

  if (mru::LatencyRecorder::exact()) {
    CHECK(pa != pb, "a 1.7x difference in the tail must be visible as a different number");
    CHECK(rel_error(pa, 1100) <= kTolerance, "1100 must read back as 1100");
    CHECK(rel_error(pb, 1900) <= kTolerance, "1900 must read back as 1900");
  } else if (pa == pb) {
    std::printf("    NOTE: the fallback reports both as %llu -- this is precisely the\n"
                "    ambiguity that made the pre-HdrHistogram p99 figures unusable.\n",
                static_cast<unsigned long long>(pa));
  }
}

void test_ceiling_is_counted_not_clamped() {
  banner("samples past the ceiling are counted, not silently clamped");
  // A tight ceiling so the overflow path is reachable without recording huge values.
  mru::LatencyRecorder r(/*highest_cycles=*/10000, /*significant_figures=*/3);
  CHECK(r.usable(), "a 10k ceiling must still initialise");

  for (int i = 0; i < 500; ++i) r.record(5000);
  const std::uint64_t in_range = r.count();
  for (int i = 0; i < 7; ++i) r.record(50000000);

  std::printf("    in-range %llu, out-of-range %llu\n",
              static_cast<unsigned long long>(r.count()),
              static_cast<unsigned long long>(r.out_of_range()));

  if (mru::LatencyRecorder::exact()) {
    CHECK(r.out_of_range() == 7, "every over-ceiling sample must be counted");
    // The point of counting them: they must NOT quietly inflate the reported body into
    // looking like a measured tail. A clamped sample would land at the ceiling and be
    // indistinguishable from a real 10k-cycle observation.
    CHECK(r.count() == in_range, "over-ceiling samples must not enter the percentiles");
    CHECK(r.percentile(0.99) <= 10000, "percentiles must stay within the configured range");
  }
}

void test_merge_is_additive() {
  banner("merge across shards equals recording into one");
  const auto a = synthetic_latencies(20000, 9);
  const auto b = synthetic_latencies(31000, 15);

  mru::LatencyRecorder ra;
  mru::LatencyRecorder rb;
  for (std::uint64_t x : a) ra.record(x);
  for (std::uint64_t x : b) rb.record(x);
  ra.merge(rb);

  std::vector<std::uint64_t> both = a;
  both.insert(both.end(), b.begin(), b.end());

  CHECK(ra.count() == both.size(), "merged count must be the sum");
  for (double p : {0.50, 0.99, 0.999}) {
    const std::uint64_t want = true_percentile(both, p);
    const std::uint64_t got = ra.percentile(p);
    if (mru::LatencyRecorder::exact()) {
      CHECK(rel_error(got, want) <= kTolerance, "merged percentile must match the union");
    } else {
      CHECK(got >= want, "merged percentile must not under-report");
    }
  }
}

void test_move_survives_vector_growth() {
  banner("move construction through vector reallocation");
  // main.cpp holds one recorder per shard in a vector. Without a correct move the
  // underlying histogram would be closed twice on reallocation; this gives ASan
  // something to catch if that ever regresses.
  std::vector<mru::LatencyRecorder> v;
  for (int i = 0; i < 16; ++i) {  // no reserve: force reallocation
    v.emplace_back();
    v.back().record(1000 + static_cast<std::uint64_t>(i));
  }
  std::uint64_t total = 0;
  for (const auto& r : v) {
    CHECK(r.usable(), "a moved recorder must still be usable");
    total += r.count();
  }
  CHECK(total == 16, "no sample may be lost to reallocation");

  mru::LatencyRecorder sink;
  for (const auto& r : v) sink.merge(r);
  CHECK(sink.count() == 16, "merging the moved recorders must preserve every sample");
}

}  // namespace

int main() {
  std::printf("backend: %s\n", mru::LatencyRecorder::backend());
  std::printf("exact:   %s\n\n", mru::LatencyRecorder::exact() ? "yes" : "no");

  test_percentiles_track_truth();
  test_resolves_within_one_octave();
  test_ceiling_is_counted_not_clamped();
  test_merge_is_additive();
  test_move_survives_vector_growth();

  if (g_failures != 0) {
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
