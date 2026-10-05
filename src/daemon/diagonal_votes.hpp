// src/daemon/diagonal_votes.hpp
//
// Diagonal voting: the bounded data structure and the per-chunk accumulation rule.
//
// Split out of policy.hpp so it depends on the index layer and NOTHING ELSE. policy.hpp
// pulls in transport/live_reads_stream.hpp for Decision and ChannelState, which needs
// the generated gRPC headers, which meant this logic could only be compiled in the
// transport build -- so the offline bench could not use it, so the offline bench grew
// its own copy, and that copy reported 89.3% TPR on chr20 while the daemon accepted
// nothing. Keeping this header free of transport is what makes one shared
// implementation possible.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "index/batched_probe.hpp"
#include "index/quantise.hpp"

namespace mru {

// Diagonal votes for one channel, accumulated across the chunks of one read.
//
// 64 sets x 4 ways = 256 tracked diagonals, ~3 KB per channel, ~1.5 MB across 512
// channels. That is a rounding error against the index and it buys correctness.
//
// WHY THIS REPLACED A 16-SLOT LINEAR TABLE, which is the bug that made the whole
// accept path fail on real sequence.
//
// The old table scanned 16 slots and evicted the weakest only when that weakest
// held a single vote:
//
//     if (votes[weakest] <= 1) { diag[weakest] = d; votes[weakest] = 1; }
//
// Against a 37 kb reference a chunk yields about ONE candidate diagonal, so slots
// stayed weak and the rule was harmless. Against 64 Mb of chr20 a chunk yields
// about NINETY-FIVE. Every slot reaches two or more votes inside the first chunk
// and the condition is never true again, so a diagonal that was not among the
// first 16 distinct ones observed can never be inserted at all. Not eviction
// churn -- a permanent lockout. Measured consequence: 0 accepts out of 763
// decisions on chr20, against 854 of 1366 on the toy reference.
//
// Two changes, and the first matters more than the table size.
//
//   PRE-AGGREGATION. bench/offtarget.cpp -- which reported 89.3% TPR on chr20 and
//   so was never wrong -- sorts a read's diagonals and counts the longest run of
//   equal values, keeping every candidate. The daemon fed candidates in one at a
//   time with weight 1, which throws away exactly the information that separates
//   signal from noise: within a single chunk the true diagonal repeats and noise
//   does not. Diagonals are now run-length counted per chunk and enter carrying
//   that run as weight, so the true diagonal arrives as a 3-to-8 vote entry while
//   noise arrives as 1 and loses the comparison below.
//
//   MISRA-GRIES REPLACEMENT. On a miss a newcomer either takes the weakest way in
//   its set (when at least as strong) or decays it. A persistent diagonal
//   therefore always wins eventually and nothing is ever locked out. This is the
//   standard bounded-memory frequent-items rule, which is precisely the problem:
//   find the mode of a stream of diagonals without storing the stream.
struct ChannelVotes {
  static constexpr std::size_t kSets = 64;
  static constexpr std::size_t kWays = 4;
  static constexpr std::size_t kEntries = kSets * kWays;

  std::int64_t diag[kEntries] = {};
  std::uint32_t votes[kEntries] = {};  // 0 means empty; no separate occupancy bit
  std::uint32_t chunks = 0;
  SignalScaling scaling{};
  bool have_scaling = false;

  // Events of THIS READ already consumed by earlier chunks.
  //
  // Without this the whole cross-chunk vote is incoherent. SeedMatches::seed_offset is
  // an offset into the QUERY, and the query is one chunk, so offsets restart at zero
  // every chunk. A read starting at reference position d therefore produces a true
  // diagonal of d for its first chunk, d + 180 for its second, d + 360 for its third
  // -- a different diagonal each time, so votes for the correct location could never
  // add up across chunks and only ever accumulated within a single chunk.
  //
  // The symptom was subtle and much worse than a low score: accepts still happened,
  // but on whatever diagonal happened to collect coincidences inside one chunk, and
  // the fraction of reads whose best diagonal was actually CORRECT fell as reads got
  // longer (49/300 at 2 chunks down to 7/300 at 20 for 3x13) -- more evidence
  // producing a worse answer, which is the signature of accumulating the wrong thing.
  //
  // Adding the running event base makes every chunk of a read agree on one diagonal.
  std::uint64_t events_consumed = 0;

  void reset() noexcept {
    // Only the counts need clearing: a zero count means the slot is empty, so a
    // stale diagonal is unreachable. 1 KB per read start.
    std::memset(votes, 0, sizeof(votes));
    chunks = 0;
    events_consumed = 0;
    have_scaling = false;
  }

  // Scatter nearby diagonals across sets. Adjacent diagonals are the common case
  // -- signal jitter puts true seeds on neighbouring diagonals -- and indexing by
  // the low bits would pile them into one set.
  [[nodiscard]] static std::size_t set_of(std::int64_t d) noexcept {
    std::uint64_t x = static_cast<std::uint64_t>(d) * 0x9E3779B97F4A7C15ULL;
    x ^= x >> 29;
    return static_cast<std::size_t>(x >> 33) & (kSets - 1);
  }

  // weight is the diagonal's run length within this chunk, never 0.
  void add(std::int64_t d, std::uint32_t weight) noexcept {
    const std::size_t base = set_of(d) * kWays;
    std::size_t min_way = base;
    std::uint32_t min_votes = votes[base];
    for (std::size_t i = 0; i < kWays; ++i) {
      const std::size_t w = base + i;
      if (votes[w] != 0 && diag[w] == d) {
        votes[w] += weight;
        return;
      }
      if (votes[w] < min_votes) {
        min_votes = votes[w];
        min_way = w;
      }
    }
    if (min_votes <= weight) {
      diag[min_way] = d;
      votes[min_way] = weight;
    } else {
      votes[min_way] = min_votes - weight;
    }
  }

  // Scans all 256 entries rather than tracking a running maximum. A running max
  // would have to remember the historical peak of an entry that was later decayed,
  // which can only overstate the winner and invent accepts. 256 contiguous
  // counters is 16 cachelines and costs ~0.1 us once per chunk.
  [[nodiscard]] std::uint32_t best(std::int64_t* out_diag) const noexcept {
    std::uint32_t best_votes = 0;
    std::int64_t best_d = 0;
    for (std::size_t i = 0; i < kEntries; ++i) {
      if (votes[i] > best_votes) {
        best_votes = votes[i];
        best_d = diag[i];
      }
    }
    if (out_diag != nullptr) *out_diag = best_d;
    return best_votes;
  }
};

// Per-chunk diagonal buffer. chr20 produces ~95 candidates per chunk; 16384 leaves
// two orders of magnitude of headroom so the cap is a safety net rather than a
// policy, at 128 KB per worker thread.
constexpr std::size_t kDiagCapacity = 16384;

// Fold one chunk's seed matches into a channel's diagonal votes. Returns the number
// of candidates seen, including any dropped at the cap.
//
// SHARED BY THE DAEMON AND THE BENCHES, DELIBERATELY. These were two separate
// implementations once. The bench kept every candidate in an unbounded sorted
// vector and counted the longest run; the daemon kept 16 slots and fed candidates
// in one at a time at weight 1. The bench reported 89.3% TPR on chr20 while the
// daemon accepted nothing at all, and the discrepancy went unnoticed for weeks
// because there was no single thing to diff -- they were different algorithms
// wearing the same name. One function now, so an offline number is a prediction
// about the live path rather than a statement about a program nobody runs.
//
// Within one chunk a true diagonal repeats and noise does not, so the run length
// IS the signal and it enters the table as the vote weight. `diags` is caller-owned
// so the hot path can reserve it once: a daemon that allocates here produced a
// 9.5 ms maximum. Overflow past the reserved capacity is counted, never hidden.
// chunk_events is how many events this chunk contributed; it advances the read's
// event base so the NEXT chunk lands on the same diagonal as this one.
inline std::size_t accumulate_chunk_votes(const ProbeScratch& scratch,
                                          ChannelVotes& votes,
                                          std::vector<std::int64_t>& diags,
                                          std::uint64_t& dropped,
                                          std::uint64_t chunk_events) {
  const std::int64_t base = static_cast<std::int64_t>(votes.events_consumed);
  diags.clear();
  for (const SeedMatches& m : scratch.matches) {
    for (std::uint32_t j = 0; j < m.count; ++j) {
      if (diags.size() == diags.capacity()) {
        ++dropped;
        continue;
      }
      // Read-relative, not chunk-relative. See ChannelVotes::events_consumed.
      diags.push_back(static_cast<std::int64_t>(m.positions[j]) - base -
                      static_cast<std::int64_t>(m.seed_offset));
    }
  }
  votes.events_consumed += chunk_events;
  const std::size_t seen = diags.size() + dropped;
  if (diags.empty()) return seen;
  std::sort(diags.begin(), diags.end());
  std::uint32_t run = 1;
  for (std::size_t i = 1; i <= diags.size(); ++i) {
    if (i < diags.size() && diags[i] == diags[i - 1]) {
      ++run;
      continue;
    }
    votes.add(diags[i - 1], run);
    run = 1;
  }
  return seen;
}

}  // namespace mru
