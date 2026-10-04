// src/index/minimizer_index.hpp
//
// Open-addressed hash index over quantised-event seeds, built so the common case
// is ONE cacheline touch.
//
// Layout and why
// --------------
// One 8-byte entry: a 24-bit fingerprint and a 40-bit position, packed. Rationale:
//
//   * 8 bytes means 8 entries per 64-byte cacheline, so a linear probe that spills
//     past its slot usually stays inside the line it already paid for.
//   * The fingerprint lets a probe reject a wrong slot without a second memory
//     reference. Storing the full 64-bit hash would double the table and halve the
//     entries per line, which costs more than the extra discrimination buys.
//   * 40 bits of position addresses 1.1e12 events, far past any reference.
//   * Entry value 0 means empty, so stored positions are biased by one.
//
// A 24-bit fingerprint collides at roughly 1 in 16.7 million per probe. Those are
// false seed hits, not wrong answers: downstream chaining discards a seed that
// does not extend. Specificity comes from requiring several consistent seeds, not
// from fingerprint width.
//
// Load factor is capped at 0.5. Linear probing degrades badly above ~0.7, and the
// whole point is a single probe, so memory is the cheaper thing to spend.
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "core/cacheline.hpp"
#include "index/quantise.hpp"

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace mru {

// Telemetry. Probe-length counts are the in-process evidence that the table is
// single-probe in practice; actual LLC miss counts come from `perf stat`, which
// bench/perf/index_probe.sh wraps. Both belong in the paper: the histogram shows
// the data structure behaves as designed, perf shows it translates to hardware.
// Telemetry.
//
// A note on what "single probe" can honestly mean here. This index is a MULTIMAP:
// a minimizer legitimately occurs many times, so a lookup cannot stop at the first
// match -- it has to walk to the end of the run to find the duplicates. That means
// any HIT necessarily touches at least two slots: the matching one and the empty
// terminator. "One slot per query" is therefore not achievable by construction,
// and measuring it would be measuring the wrong thing.
//
// The claim that is both true and useful is ONE CACHELINE. Eight 8-byte entries
// per 64-byte line means a short probe run usually stays inside the line already
// paid for, so the cost is one memory reference even though it is two or three
// slot touches. cachelines[] measures exactly that, and it is the number that
// belongs in the paper next to `perf stat`'s LLC-load-misses.
struct IndexStats {
  std::uint64_t queries = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t fingerprint_rejects = 0;  // right slot, wrong fingerprint

  static constexpr std::size_t kMaxProbeBuckets = 16;
  // slots[i]      = queries that touched i+1 slots
  // cachelines[i] = queries that touched i+1 distinct cachelines
  std::uint64_t slots[kMaxProbeBuckets] = {};
  std::uint64_t cachelines[kMaxProbeBuckets] = {};

  void note_probe(std::size_t slot_touches, std::size_t line_touches) noexcept {
    const auto bucket = [](std::size_t n) {
      return std::min(n == 0 ? std::size_t{0} : n - 1, kMaxProbeBuckets - 1);
    };
    ++slots[bucket(slot_touches)];
    ++cachelines[bucket(line_touches)];
  }

  [[nodiscard]] static double mean_of(const std::uint64_t (&h)[kMaxProbeBuckets]) noexcept {
    std::uint64_t n = 0, total = 0;
    for (std::size_t i = 0; i < kMaxProbeBuckets; ++i) {
      n += h[i];
      total += h[i] * (i + 1);
    }
    return n ? static_cast<double>(total) / static_cast<double>(n) : 0.0;
  }
  [[nodiscard]] static double first_bucket_fraction(
      const std::uint64_t (&h)[kMaxProbeBuckets]) noexcept {
    std::uint64_t n = 0;
    for (std::size_t i = 0; i < kMaxProbeBuckets; ++i) n += h[i];
    return n ? static_cast<double>(h[0]) / static_cast<double>(n) : 0.0;
  }

  [[nodiscard]] double mean_slots() const noexcept { return mean_of(slots); }
  [[nodiscard]] double mean_cachelines() const noexcept { return mean_of(cachelines); }
  // THE design claim: the probe run fits in one cacheline.
  [[nodiscard]] double single_cacheline_fraction() const noexcept {
    return first_bucket_fraction(cachelines);
  }

  void merge(const IndexStats& o) noexcept {
    queries += o.queries;
    hits += o.hits;
    misses += o.misses;
    fingerprint_rejects += o.fingerprint_rejects;
    for (std::size_t i = 0; i < kMaxProbeBuckets; ++i) {
      slots[i] += o.slots[i];
      cachelines[i] += o.cachelines[i];
    }
  }
};

// Entries per 64-byte cacheline, given 8-byte entries.
inline constexpr std::size_t kEntriesPerLine = kCacheline / sizeof(std::uint64_t);

class MinimizerIndex {
 public:
  using Entry = std::uint64_t;

  static constexpr std::uint32_t kFingerprintBits = 24;
  static constexpr std::uint64_t kFingerprintMask = (std::uint64_t{1} << kFingerprintBits) - 1;
  static constexpr std::uint64_t kPositionMask =
      (std::uint64_t{1} << (64 - kFingerprintBits)) - 1;
  static constexpr Entry kEmpty = 0;
  static constexpr double kMaxLoadFactor = 0.5;

  // Probing never walks more than this before giving up. Bounds worst-case query
  // latency, which matters more here than finding every last duplicate: an
  // unbounded walk through a degenerate bucket would blow the latency budget.
  static constexpr std::size_t kMaxProbe = 32;

  MinimizerIndex() = default;

  // `expected_seeds` sizes the table; capacity is the next power of two at or
  // above expected_seeds / kMaxLoadFactor.
  void reserve(std::size_t expected_seeds);

  [[nodiscard]] std::size_t capacity() const noexcept { return table_.size(); }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] double load_factor() const noexcept {
    return table_.empty() ? 0.0
                          : static_cast<double>(count_) / static_cast<double>(table_.size());
  }
  [[nodiscard]] std::uint64_t insert_failures() const noexcept { return insert_failures_; }

  // Duplicate hashes are allowed: a minimizer legitimately occurs many times, and
  // they sit in adjacent slots so a query walks them in one or two cachelines.
  // Returns false if the probe bound was hit (counted in insert_failures()).
  [[nodiscard]] bool insert(std::uint64_t hash, std::uint64_t position);

  // Builds the table, capping how many times any one key may be stored.
  //
  // The cap is NOT tuning, it is required for the structure to work at all.
  // Quantised signal keys are violently skewed: a slowly varying signal emits long
  // runs of identical keys, they all hash to one slot, the cluster grows past
  // kMaxProbe, and inserts start failing. Measured before the cap existed: 39992
  // seeds in a 131072-slot table stored only ~3900 of them, load 0.03, and recall
  // at zero noise collapsed from the expected ~100% to 20.9%. The index had
  // silently discarded 90% of itself and nothing complained.
  //
  // Capping is also the right thing on the merits. A key occurring thousands of
  // times carries almost no positional information; it costs a long probe walk to
  // return matches that chaining will discard anyway. Real minimizer indexes
  // discard high-occurrence seeds for exactly this reason.
  //
  // Check insert_failures() after building. It should be zero; anything else means
  // the cap is still too loose or the table too small for this key distribution.
  void build(std::span<const SeedHash> seeds, std::uint32_t max_occurrences = 8);

  // Keys dropped by the occurrence cap during the last build().
  [[nodiscard]] std::uint64_t capped_seeds() const noexcept { return capped_seeds_; }
  [[nodiscard]] std::uint64_t distinct_keys() const noexcept { return distinct_keys_; }

  // Up to out.size() positions for `hash`. Returns how many were written.
  // This is the scalar reference implementation: batched_probe.hpp must agree
  // with it exactly, and the test asserts that.
  [[nodiscard]] std::size_t query(std::uint64_t hash, std::span<std::uint64_t> out,
                                  IndexStats* stats = nullptr) const noexcept;

  // Pieces the batched probe needs. Exposed deliberately rather than friending,
  // so the pipelined path cannot drift from the scalar one.
  [[nodiscard]] static std::uint64_t fingerprint_of(std::uint64_t hash) noexcept {
    // Top bits: the low bits already select the slot, so reusing them as the
    // fingerprint would make it nearly constant within a probe sequence.
    return (hash >> (64 - kFingerprintBits)) & kFingerprintMask;
  }
  [[nodiscard]] std::size_t slot_of(std::uint64_t hash) const noexcept {
    return static_cast<std::size_t>(hash) & mask_;
  }
  [[nodiscard]] const Entry* data() const noexcept { return table_.data(); }
  [[nodiscard]] std::size_t mask() const noexcept { return mask_; }

  [[nodiscard]] static Entry pack(std::uint64_t fingerprint, std::uint64_t position) noexcept {
    return (fingerprint << (64 - kFingerprintBits)) | ((position + 1) & kPositionMask);
  }
  [[nodiscard]] static std::uint64_t entry_fingerprint(Entry e) noexcept {
    return (e >> (64 - kFingerprintBits)) & kFingerprintMask;
  }
  [[nodiscard]] static std::uint64_t entry_position(Entry e) noexcept {
    return (e & kPositionMask) - 1;  // undo the +1 bias
  }

 private:
  void apply_hugepage_hint() noexcept;

  std::vector<Entry> table_;
  std::size_t mask_ = 0;
  std::size_t count_ = 0;
  std::uint64_t insert_failures_ = 0;
  std::uint64_t capped_seeds_ = 0;
  std::uint64_t distinct_keys_ = 0;
};

inline void MinimizerIndex::reserve(std::size_t expected_seeds) {
  std::size_t want = static_cast<std::size_t>(
      static_cast<double>(expected_seeds) / kMaxLoadFactor);
  if (want < 16) want = 16;
  std::size_t cap = std::bit_ceil(want);
  table_.assign(cap, kEmpty);
  mask_ = cap - 1;
  count_ = 0;
  insert_failures_ = 0;
  apply_hugepage_hint();
}

inline void MinimizerIndex::apply_hugepage_hint() noexcept {
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  // A multi-gigabyte table on 4 KiB pages thrashes the TLB, and a TLB miss costs
  // as much as the cache miss we are working to avoid. Advisory: if THP is off,
  // this quietly does nothing.
  if (!table_.empty()) {
    const std::size_t bytes = table_.size() * sizeof(Entry);
    (void)madvise(static_cast<void*>(table_.data()), bytes, MADV_HUGEPAGE);
  }
#endif
}

inline bool MinimizerIndex::insert(std::uint64_t hash, std::uint64_t position) {
  if (table_.empty()) return false;
  const std::uint64_t fp = fingerprint_of(hash);
  std::size_t slot = slot_of(hash);
  for (std::size_t p = 0; p < kMaxProbe; ++p) {
    if (table_[slot] == kEmpty) {
      table_[slot] = pack(fp, position);
      ++count_;
      return true;
    }
    slot = (slot + 1) & mask_;
  }
  ++insert_failures_;
  return false;
}

inline void MinimizerIndex::build(std::span<const SeedHash> seeds,
                                  std::uint32_t max_occurrences) {
  capped_seeds_ = 0;
  distinct_keys_ = 0;
  if (seeds.empty() || max_occurrences == 0) {
    reserve(1);
    return;
  }

  // Sort by hash so runs of one key are contiguous and can be counted in a single
  // pass. Sorting the build input is fine: build is offline, and only query latency
  // is on the critical path.
  std::vector<SeedHash> sorted(seeds.begin(), seeds.end());
  std::sort(sorted.begin(), sorted.end(), [](const SeedHash& a, const SeedHash& b) {
    return a.hash != b.hash ? a.hash < b.hash : a.offset < b.offset;
  });

  // Pass 1: how many entries survive the cap, so the table is sized for what is
  // actually stored rather than for the raw seed count.
  std::size_t keep = 0;
  for (std::size_t i = 0; i < sorted.size();) {
    std::size_t j = i;
    while (j < sorted.size() && sorted[j].hash == sorted[i].hash) ++j;
    const std::size_t run = j - i;
    ++distinct_keys_;
    const std::size_t take = std::min<std::size_t>(run, max_occurrences);
    keep += take;
    capped_seeds_ += run - take;
    i = j;
  }

  reserve(keep);

  // Pass 2: insert, keeping at most max_occurrences per key.
  for (std::size_t i = 0; i < sorted.size();) {
    std::size_t j = i;
    while (j < sorted.size() && sorted[j].hash == sorted[i].hash) ++j;
    const std::size_t take = std::min<std::size_t>(j - i, max_occurrences);
    for (std::size_t k = 0; k < take; ++k) {
      (void)insert(sorted[i + k].hash, sorted[i + k].offset);
    }
    i = j;
  }
}

inline std::size_t MinimizerIndex::query(std::uint64_t hash, std::span<std::uint64_t> out,
                                         IndexStats* stats) const noexcept {
  if (table_.empty() || out.empty()) return 0;
  const std::uint64_t fp = fingerprint_of(hash);
  std::size_t slot = slot_of(hash);
  std::size_t found = 0;
  std::size_t probes = 0;
  std::size_t lines = 0;
  std::size_t last_line = ~std::size_t{0};
  std::uint64_t rejects = 0;

  for (std::size_t p = 0; p < kMaxProbe; ++p) {
    ++probes;
    const std::size_t line = slot / kEntriesPerLine;
    if (line != last_line) {
      ++lines;
      last_line = line;
    }
    const Entry e = table_[slot];
    if (e == kEmpty) break;  // open addressing: empty slot ends the run
    if (entry_fingerprint(e) == fp) {
      out[found++] = entry_position(e);
      if (found == out.size()) break;
    } else {
      ++rejects;
    }
    slot = (slot + 1) & mask_;
  }

  if (stats != nullptr) {
    ++stats->queries;
    if (found > 0) {
      ++stats->hits;
    } else {
      ++stats->misses;
    }
    stats->fingerprint_rejects += rejects;
    stats->note_probe(probes, lines);
  }
  return found;
}

}  // namespace mru
