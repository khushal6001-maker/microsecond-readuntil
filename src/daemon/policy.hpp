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

#include "daemon/diagonal_votes.hpp"
#include "index/batched_probe.hpp"
#include "index/minimizer_index.hpp"
#include "index/quantise.hpp"
#include "transport/chunk_ref.hpp"
#include "transport/live_reads_stream.hpp"

namespace mru {

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

    thread_local std::vector<std::int64_t> diags;
    if (diags.capacity() == 0) diags.reserve(kDiagCapacity);
    std::uint64_t dropped = 0;
    // Advance the read's diagonal base by the events THIS CHUNK produced, not by
    // raw.size()/samples_per_event. The two agree under fixed-width slicing and do not
    // agree under event detection, where a chunk's event count depends on how many level
    // changes were in it.
    const std::size_t cands =
        accumulate_chunk_votes(scratch, v, diags, dropped, scratch.events.size());
    stats_.candidates.fetch_add(cands, std::memory_order_relaxed);
    if (dropped != 0) {
      stats_.candidates_dropped.fetch_add(dropped, std::memory_order_relaxed);
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
