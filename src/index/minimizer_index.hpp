// src/index/minimizer_index.hpp
//
// Open-addressed index over quantised-event seeds: one entry per DISTINCT key, with the
// positions themselves in a side array.
//
// Why not one entry per occurrence
// -------------------------------
// The first version stored every occurrence as its own table entry, so a key kept 8
// times occupied 8 CONSECUTIVE slots, and neighbouring keys chained into each other.
// On synthetic data that was invisible. On real human chr20 it produced 436 insert
// failures and 13.4% of minimizer occurrences discarded, because runs regularly
// exceeded the 32-slot probe bound. The index was silently lossy exactly where the
// data is hardest -- repeat-rich regions.
//
// Raising the bound would have traded that for worse worst-case query latency while
// leaving the structure wrong. One entry per distinct key fixes it properly:
//
//   * the load factor is now over DISTINCT keys, not occurrences, so runs are short
//   * a hit resolves at the FIRST fingerprint match, so the common case really is one
//     slot and one cacheline rather than a walk to the end of a duplicate run
//   * a key's positions are contiguous in the side array, so reading them is one
//     sequential burst instead of a strided probe
//   * insert can no longer fail from duplicate clustering
//
// Entry layout, 8 bytes:
//   bits 63..40  24-bit fingerprint  rejects a wrong slot without a second reference
//   bits 39..32   8-bit count        occurrences of this key, capped at 255
//   bits 31..0   32-bit offset       start of this key's positions in positions_
//
// 32-bit offsets address 4.3e9 positions; a mammalian index at minimizer window 10
// holds ~3.1e8, so there is two orders of magnitude of headroom. Entry 0 means empty,
// which is unambiguous because a real entry always has count >= 1.
//
// A 24-bit fingerprint collides at roughly 1 in 16.7 million. Those are false seed
// hits, not wrong answers: chaining discards a seed that does not extend. Specificity
// comes from several seeds agreeing on a diagonal, never from fingerprint width.
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

// Telemetry.
//
// "Single probe" is now an achievable claim rather than an aspiration. With one entry
// per distinct key a hit stops at the first fingerprint match, so slots touched is
// usually 1. The cacheline figure remains the one that matters for hardware, since 8
// entries share a 64-byte line.
struct IndexStats {
  std::uint64_t queries = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t fingerprint_rejects = 0;  // right slot, wrong fingerprint
  std::uint64_t positions_returned = 0;
  std::uint64_t positions_truncated = 0;  // key had more positions than out could hold

  static constexpr std::size_t kMaxProbeBuckets = 16;
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
  [[nodiscard]] double single_slot_fraction() const noexcept {
    return first_bucket_fraction(slots);
  }
  [[nodiscard]] double single_cacheline_fraction() const noexcept {
    return first_bucket_fraction(cachelines);
  }

  void merge(const IndexStats& o) noexcept {
    queries += o.queries;
    hits += o.hits;
    misses += o.misses;
    fingerprint_rejects += o.fingerprint_rejects;
    positions_returned += o.positions_returned;
    positions_truncated += o.positions_truncated;
    for (std::size_t i = 0; i < kMaxProbeBuckets; ++i) {
      slots[i] += o.slots[i];
      cachelines[i] += o.cachelines[i];
    }
  }
};

inline constexpr std::size_t kEntriesPerLine = kCacheline / sizeof(std::uint64_t);

class MinimizerIndex {
 public:
  using Entry = std::uint64_t;
  using Position = std::uint32_t;

  static constexpr std::uint32_t kFingerprintBits = 24;
  static constexpr std::uint32_t kCountBits = 8;
  static constexpr std::uint32_t kOffsetBits = 32;
  static constexpr std::uint64_t kFingerprintMask = (std::uint64_t{1} << kFingerprintBits) - 1;
  static constexpr std::uint64_t kCountMask = (std::uint64_t{1} << kCountBits) - 1;
  static constexpr std::uint64_t kOffsetMask = (std::uint64_t{1} << kOffsetBits) - 1;
  static constexpr std::uint32_t kMaxCount = static_cast<std::uint32_t>(kCountMask);
  static constexpr Entry kEmpty = 0;
  static constexpr double kMaxLoadFactor = 0.5;

  // Bounded so worst-case query latency is bounded. With one entry per distinct key at
  // load 0.5 the expected run is ~1.5 slots, so this is now a generous safety margin
  // rather than a limit the data actually reaches.
  static constexpr std::size_t kMaxProbe = 32;

  MinimizerIndex() = default;

  [[nodiscard]] std::size_t capacity() const noexcept { return table_.size(); }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::size_t position_count() const noexcept { return positions_.size(); }
  [[nodiscard]] double load_factor() const noexcept {
    return table_.empty() ? 0.0
                          : static_cast<double>(count_) / static_cast<double>(table_.size());
  }
  [[nodiscard]] std::uint64_t insert_failures() const noexcept { return insert_failures_; }
  [[nodiscard]] std::uint64_t capped_seeds() const noexcept { return capped_seeds_; }
  [[nodiscard]] std::uint64_t distinct_keys() const noexcept { return distinct_keys_; }

  // `max_occurrences` discards positions beyond the cap for a given key. A key occurring
  // thousands of times carries almost no positional information and costs a long
  // sequential read to return matches chaining will discard, which is why real minimizer
  // indexes filter high-occurrence seeds. Capped above by kMaxCount, since the count has
  // 8 bits.
  void build(std::span<const SeedHash> seeds, std::uint32_t max_occurrences = 8);

  // Up to out.size() positions for `hash`. Returns how many were written. Stops at the
  // first fingerprint match: one entry per key means there is nothing further to find.
  [[nodiscard]] std::size_t query(std::uint64_t hash, std::span<std::uint64_t> out,
                                  IndexStats* stats = nullptr) const noexcept;

  // ---- shared probe, used by BOTH the scalar and batched paths ----------------
  //
  // The two paths previously duplicated this walk and were kept honest by a test that
  // asserted they agreed. Sharing the code makes them unable to disagree, which is
  // strictly better than detecting it afterwards.
  struct Probe {
    const Position* positions = nullptr;
    std::uint32_t count = 0;
    std::size_t slots = 0;
    std::size_t lines = 0;
    std::uint64_t rejects = 0;
  };

  [[nodiscard]] static Probe probe_at(const Entry* table, std::size_t mask,
                                      const Position* positions, std::size_t slot,
                                      std::uint64_t fingerprint) noexcept {
    Probe p;
    std::size_t last_line = ~std::size_t{0};
    for (std::size_t i = 0; i < kMaxProbe; ++i) {
      ++p.slots;
      const std::size_t line = slot / kEntriesPerLine;
      if (line != last_line) {
        ++p.lines;
        last_line = line;
      }
      const Entry e = table[slot];
      if (e == kEmpty) break;  // open addressing: empty slot ends the run
      if (entry_fingerprint(e) == fingerprint) {
        p.count = entry_count(e);
        p.positions = positions + entry_offset(e);
        break;
      }
      ++p.rejects;
      slot = (slot + 1) & mask;
    }
    return p;
  }

  [[nodiscard]] static std::uint64_t fingerprint_of(std::uint64_t hash) noexcept {
    // Top bits: the low bits already select the slot, so reusing them would make the
    // fingerprint nearly constant within a probe sequence.
    return (hash >> (64 - kFingerprintBits)) & kFingerprintMask;
  }
  [[nodiscard]] std::size_t slot_of(std::uint64_t hash) const noexcept {
    return static_cast<std::size_t>(hash) & mask_;
  }
  [[nodiscard]] const Entry* data() const noexcept { return table_.data(); }
  [[nodiscard]] const Position* positions() const noexcept { return positions_.data(); }
  [[nodiscard]] std::size_t mask() const noexcept { return mask_; }

  [[nodiscard]] static Entry pack(std::uint64_t fingerprint, std::uint32_t count,
                                  std::uint32_t offset) noexcept {
    return ((fingerprint & kFingerprintMask) << (kCountBits + kOffsetBits)) |
           ((static_cast<std::uint64_t>(count) & kCountMask) << kOffsetBits) |
           (static_cast<std::uint64_t>(offset) & kOffsetMask);
  }
  [[nodiscard]] static std::uint64_t entry_fingerprint(Entry e) noexcept {
    return (e >> (kCountBits + kOffsetBits)) & kFingerprintMask;
  }
  [[nodiscard]] static std::uint32_t entry_count(Entry e) noexcept {
    return static_cast<std::uint32_t>((e >> kOffsetBits) & kCountMask);
  }
  [[nodiscard]] static std::uint32_t entry_offset(Entry e) noexcept {
    return static_cast<std::uint32_t>(e & kOffsetMask);
  }

 private:
  void reserve_distinct(std::size_t distinct);
  void apply_hugepage_hint() noexcept;
  [[nodiscard]] bool insert_entry(std::uint64_t hash, std::uint32_t count,
                                  std::uint32_t offset);

  std::vector<Entry> table_;
  std::vector<Position> positions_;
  std::size_t mask_ = 0;
  std::size_t count_ = 0;
  std::uint64_t insert_failures_ = 0;
  std::uint64_t capped_seeds_ = 0;
  std::uint64_t distinct_keys_ = 0;
};

inline void MinimizerIndex::reserve_distinct(std::size_t distinct) {
  std::size_t want = static_cast<std::size_t>(static_cast<double>(distinct) / kMaxLoadFactor);
  if (want < 16) want = 16;
  table_.assign(std::bit_ceil(want), kEmpty);
  mask_ = table_.size() - 1;
  count_ = 0;
  insert_failures_ = 0;
  apply_hugepage_hint();
}

inline void MinimizerIndex::apply_hugepage_hint() noexcept {
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  // A multi-gigabyte table on 4 KiB pages thrashes the TLB, and a TLB miss costs about
  // as much as the cache miss being avoided. Advisory: a no-op when THP is off.
  if (!table_.empty()) {
    (void)madvise(static_cast<void*>(table_.data()), table_.size() * sizeof(Entry),
                  MADV_HUGEPAGE);
  }
#endif
}

inline bool MinimizerIndex::insert_entry(std::uint64_t hash, std::uint32_t count,
                                        std::uint32_t offset) {
  if (table_.empty() || count == 0) return false;
  const std::uint64_t fp = fingerprint_of(hash);
  std::size_t slot = slot_of(hash);
  for (std::size_t p = 0; p < kMaxProbe; ++p) {
    if (table_[slot] == kEmpty) {
      table_[slot] = pack(fp, count, offset);
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
  positions_.clear();
  if (seeds.empty() || max_occurrences == 0) {
    reserve_distinct(1);
    return;
  }
  const std::uint32_t cap = std::min(max_occurrences, kMaxCount);

  // Sort by hash so one key's occurrences are contiguous. Build is offline; only query
  // latency is on the critical path.
  std::vector<SeedHash> sorted(seeds.begin(), seeds.end());
  std::sort(sorted.begin(), sorted.end(), [](const SeedHash& a, const SeedHash& b) {
    return a.hash != b.hash ? a.hash < b.hash : a.offset < b.offset;
  });

  // Pass 1: size both structures from what will actually be stored.
  std::size_t total_positions = 0;
  for (std::size_t i = 0; i < sorted.size();) {
    std::size_t j = i;
    while (j < sorted.size() && sorted[j].hash == sorted[i].hash) ++j;
    const std::size_t run = j - i;
    ++distinct_keys_;
    const std::size_t take = std::min<std::size_t>(run, cap);
    total_positions += take;
    capped_seeds_ += run - take;
    i = j;
  }

  reserve_distinct(distinct_keys_);
  positions_.reserve(total_positions);

  // Pass 2: one entry per distinct key, its positions contiguous.
  for (std::size_t i = 0; i < sorted.size();) {
    std::size_t j = i;
    while (j < sorted.size() && sorted[j].hash == sorted[i].hash) ++j;
    const std::size_t take = std::min<std::size_t>(j - i, cap);
    const auto offset = static_cast<std::uint32_t>(positions_.size());
    for (std::size_t t = 0; t < take; ++t) {
      positions_.push_back(static_cast<Position>(sorted[i + t].offset));
    }
    (void)insert_entry(sorted[i].hash, static_cast<std::uint32_t>(take), offset);
    i = j;
  }
}

inline std::size_t MinimizerIndex::query(std::uint64_t hash, std::span<std::uint64_t> out,
                                         IndexStats* stats) const noexcept {
  if (table_.empty() || out.empty()) return 0;
  const Probe p = probe_at(table_.data(), mask_, positions_.data(), slot_of(hash),
                           fingerprint_of(hash));
  const std::size_t n = std::min<std::size_t>(p.count, out.size());
  for (std::size_t i = 0; i < n; ++i) out[i] = p.positions[i];

  if (stats != nullptr) {
    ++stats->queries;
    if (p.count > 0) {
      ++stats->hits;
    } else {
      ++stats->misses;
    }
    stats->fingerprint_rejects += p.rejects;
    stats->positions_returned += n;
    if (p.count > out.size()) stats->positions_truncated += p.count - out.size();
    stats->note_probe(p.slots, p.lines);
  }
  return n;
}

}  // namespace mru
