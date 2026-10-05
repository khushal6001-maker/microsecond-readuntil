// src/daemon/policy.hpp
//
// The decision policy: signal-space matching, diagonal voting across chunks, and the
// accept / unblock / defer rule measured in bench/offtarget.cpp.
//
// THREAD SAFETY, which is the whole reason this looks the way it does.
//
// LiveReadsStream takes ONE policy callable shared by every worker, so anything it
// touches is touched concurrently -- unless the data is partitioned so that it is not.
// Two mechanisms do that here, and neither needs a lock:
//
//   per-channel state   a flat array indexed by channel. Chunks are sharded by
//                       channel, so each worker owns a disjoint subset for the whole
//                       run and no two workers ever touch the same entry. Same
//                       argument as ChannelTable in read_state.hpp.
//   per-worker scratch  thread_local. Scratch is genuinely per-thread rather than
//                       per-channel, and thread_local expresses exactly that.
//
// THE RULE, from measurement on real human chr20 against a composition-matched
// (shuffled) off-target control:
//
//   votes >= accept_votes          -> on target: stop_further_data, keep sequencing
//   chunks >= max_chunks, votes <  -> off target: unblock
//   otherwise                      -> defer, because waiting strictly improves the call
//
// Deferral is sound rather than merely hopeful, and that rests on a measured asymmetry:
// off-target votes stay flat at 3-4 however long the read runs, because random diagonal
// coincidences do not concentrate on one diagonal, while on-target votes grow roughly
// linearly (8 -> 23 -> 60 -> 174 at 2.5, 5, 10, 20 chunks). So TPR at zero false
// positives goes 66.3% -> 74.0% -> 89.3% -> 97.0% with read length.
//
// max_chunks is therefore an economic choice, not a constant, and it belongs to the
// operator: deciding at 2.5 chunks keeps 66% of on-target reads and wastes 450 bases on
// each off-target one; deciding at 10 chunks keeps 89% and wastes 1800. Which wins
// depends on how abundant the target is.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "index/batched_probe.hpp"
#include "index/minimizer_index.hpp"
#include "index/quantise.hpp"
#include "transport/chunk_ref.hpp"
#include "transport/live_reads_stream.hpp"

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

  void reset() noexcept {
    // Only the counts need clearing: a zero count means the slot is empty, so a
    // stale diagonal is unreachable. 1 KB per read start.
    std::memset(votes, 0, sizeof(votes));
    chunks = 0;
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

struct PolicyConfig {
  std::uint32_t first_channel = 1;
  std::uint32_t last_channel = 512;

  // Measured: >=4 separates with zero false positives at 2.5 chunks, >=5 at every longer
  // read length, against a shuffled (composition-matched) off-target control.
  std::uint32_t accept_votes = 5;

  // How long to defer before giving up and unblocking. 10 chunks of 0.4 s is ~1800
  // bases, which measured 89.3% TPR at zero FPR.
  std::uint32_t max_chunks = 10;

  int probe_budget = kDefaultProbeBudget;
  float unblock_seconds = 0.1f;
  QuantConfig quant{};  // frozen defaults
};

// Per-chunk diagonal buffer. chr20 produces ~95 candidates per chunk; 16384 leaves
// two orders of magnitude of headroom so the cap is a safety net rather than a
// policy, at 128 KB per worker thread.
constexpr std::size_t kDiagCapacity = 16384;

struct PolicyStats {
  std::atomic<std::uint64_t> chunks_matched{0};
  std::atomic<std::uint64_t> accepted{0};
  std::atomic<std::uint64_t> unblocked{0};
  std::atomic<std::uint64_t> deferred{0};
  std::atomic<std::uint64_t> seeds_probed{0};
  std::atomic<std::uint64_t> candidates{0};
  std::atomic<std::uint64_t> too_short{0};  // chunk held fewer samples than one key needs
  std::atomic<std::uint64_t> candidates_dropped{0};  // exceeded the per-chunk cap

  [[nodiscard]] std::string describe() const {
    const auto g = [](const std::atomic<std::uint64_t>& a) {
      return std::to_string(a.load(std::memory_order_relaxed));
    };
    return "policy:    " + g(chunks_matched) + " chunks matched, " + g(seeds_probed) +
           " seeds probed, " + g(candidates) + " candidates\n" + "decisions: " +
           g(accepted) + " accepted (on target), " + g(unblocked) + " unblocked, " +
           g(deferred) + " deferred, " + g(too_short) + " chunks too short\n" +
           (candidates_dropped.load(std::memory_order_relaxed) != 0
                ? "WARNING:   " + g(candidates_dropped) +
                      " candidates exceeded the per-chunk cap and were not voted\n"
                : std::string());
  }
};

// Owns the per-channel vote state. One instance, shared by every worker; see the note at
// the top of this file for why that is safe without a lock.
class SignalPolicy {
 public:
  SignalPolicy(const MinimizerIndex& index, PolicyConfig cfg, PolicyStats& stats)
      : index_(index),
        cfg_(cfg),
        stats_(stats),
        channels_(cfg.last_channel >= cfg.first_channel
                      ? static_cast<std::size_t>(cfg.last_channel - cfg.first_channel) + 1
                      : 0) {}

  // Signature required by LiveReadsStream::PolicyFn.
  std::optional<Decision> operator()(const ChunkRef& c, const ChannelState& s) {
    if (c.channel < cfg_.first_channel || c.channel > cfg_.last_channel) {
      return std::nullopt;
    }
    ChannelVotes& v = channels_[c.channel - cfg_.first_channel];

    // A new read on this channel starts fresh. ChannelState::chunks_seen is maintained
    // by the worker's ChannelTable and is 1 on a read's first chunk.
    if (s.chunks_seen <= 1) v.reset();

    const auto raw = as_int16(c.raw_data, c.raw_data_len);
    if (raw.size() < cfg_.quant.samples_per_event * cfg_.quant.events_per_key) {
      stats_.too_short.fetch_add(1, std::memory_order_relaxed);
      return std::nullopt;
    }

    // Estimate shift and scale ONCE per read, from its first chunk, and reuse it. Two
    // reasons: a median costs a sort, which has no business running on every chunk; and
    // a scale that drifts between chunks would move bucket boundaries mid-read and
    // invent disagreements that are not in the signal.
    //
    // Better still would be MinKNOW's own median and median_before, which arrive in
    // ReadData and would remove the sort entirely. They are not currently carried in
    // ChunkRef; adding them costs 8 bytes and the struct has exactly that much room
    // before it outgrows a cacheline.
    // Per-worker scratch, reserved once. Chunks are sharded by channel, so each thread
    // owns its own buffers and nothing here allocates after the first chunk -- which is
    // the whole point: allocation on this path produced a p99 of 223 us and a 9.5 ms
    // maximum against a 27.8 us median.
    thread_local ProbeScratch scratch;
    thread_local ScalingScratch scaling_scratch;
    thread_local bool reserved = false;
    if (!reserved) {
      scratch.reserve_for(raw.size() * 2, cfg_.quant, cfg_.probe_budget);
      scaling_scratch.reserve(raw.size() * 2);
      reserved = true;
    }

    if (!v.have_scaling) {
      v.scaling = scaling_from_samples(raw, scaling_scratch);
      v.have_scaling = v.scaling.valid();
      if (!v.have_scaling) return std::nullopt;
    }

    const std::size_t matched =
        match_signal(index_, raw, v.scaling, cfg_.quant, scratch, cfg_.probe_budget);
    (void)matched;

    ++v.chunks;
    stats_.chunks_matched.fetch_add(1, std::memory_order_relaxed);
    stats_.seeds_probed.fetch_add(scratch.probes.size(), std::memory_order_relaxed);

    // Run-length count this chunk's diagonals before they enter the vote table,
    // which is what bench/offtarget.cpp does and what the old per-candidate
    // weight-1 path threw away. Within one chunk a true diagonal repeats and
    // noise does not, so the run length IS the signal.
    //
    // Capacity is reserved once and never exceeded: a hot path that allocates
    // produced a 9.5 ms maximum once already. Overflow is counted, not hidden.
    thread_local std::vector<std::int64_t> diags;
    if (diags.capacity() == 0) diags.reserve(kDiagCapacity);
    diags.clear();
    std::uint64_t dropped = 0;
    for (const SeedMatches& m : scratch.matches) {
      for (std::uint32_t j = 0; j < m.count; ++j) {
        if (diags.size() == diags.capacity()) {
          ++dropped;
          continue;
        }
        diags.push_back(static_cast<std::int64_t>(m.positions[j]) -
                        static_cast<std::int64_t>(m.seed_offset));
      }
    }
    stats_.candidates.fetch_add(diags.size() + dropped, std::memory_order_relaxed);
    if (dropped != 0) {
      stats_.candidates_dropped.fetch_add(dropped, std::memory_order_relaxed);
    }
    if (!diags.empty()) {
      std::sort(diags.begin(), diags.end());
      std::uint32_t run = 1;
      for (std::size_t i = 1; i <= diags.size(); ++i) {
        if (i < diags.size() && diags[i] == diags[i - 1]) {
          ++run;
          continue;
        }
        v.add(diags[i - 1], run);
        run = 1;
      }
    }

    std::int64_t best_diag = 0;
    const std::uint32_t best = v.best(&best_diag);

    if (best >= cfg_.accept_votes) {
      // On target. stop_further_data rather than unblock: keep sequencing the molecule,
      // just stop paying to stream its signal.
      stats_.accepted.fetch_add(1, std::memory_order_relaxed);
      return Decision::accept(c, 0);
    }
    if (v.chunks >= cfg_.max_chunks) {
      stats_.unblocked.fetch_add(1, std::memory_order_relaxed);
      return Decision::reject(c, 0, cfg_.unblock_seconds);
    }
    stats_.deferred.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
  }

 private:
  const MinimizerIndex& index_;
  PolicyConfig cfg_;
  PolicyStats& stats_;
  std::vector<ChannelVotes> channels_;
};

}  // namespace mru
