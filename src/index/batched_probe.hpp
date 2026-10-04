// src/index/batched_probe.hpp
//
// Batched software pipelining over MinimizerIndex.
//
// The problem being solved
// -----------------------
// A multi-gigabyte table means every probe is an LLC miss or worse: ~80-100 ns of
// pure memory latency during which the core does nothing. Querying seeds one at a
// time SERIALISES those stalls, so N seeds cost N x latency even though the memory
// system could have had all N in flight at once. Modern cores sustain ~10-16
// outstanding L1 misses, so the parallelism is there for free; it just has to be
// expressed.
//
// The fix is not SIMD. Minimizer hashing and probing are pointer-chasing, not
// arithmetic, and an AVX2 rewrite of the hash buys almost nothing because the
// bottleneck is latency, not ALU throughput. What works is overlapping the misses:
//
//     phase 1: for all K seeds, compute slot, issue __builtin_prefetch, do nothing
//     phase 2: for all K seeds, resolve the probe -- the line has arrived
//
// One stall for the whole batch instead of K stalls. This is the claim the paper
// should make about cache behaviour, and it is measurable two ways: probe-length
// telemetry in-process, and `perf stat -e LLC-load-misses,cycles,instructions`
// around the two paths.
//
// CORRECTNESS CONTRACT
// probe_batch() must return exactly what MinimizerIndex::query() returns for every
// seed, in the same order. It is an optimisation, not a different algorithm.
// test/index_test.cpp asserts equality against the scalar path, and that test is
// the only reason to trust anything here.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "index/minimizer_index.hpp"
#include "index/quantise.hpp"

namespace mru {

// How many seeds to keep in flight. Chosen to sit under the core's limit on
// outstanding misses: too few leaves memory parallelism unused, too many overflows
// the miss queue and the prefetches start evicting each other before use. 16 is a
// reasonable default for current x86-64; it is a tunable, and sweeping it is a
// figure in the paper.
inline constexpr std::size_t kProbeBatch = 16;

// Results for one seed.
struct SeedMatches {
  std::uint32_t seed_offset = 0;  // which query event the seed came from
  std::uint32_t count = 0;        // positions written
  static constexpr std::size_t kMaxPerSeed = 4;
  std::uint64_t positions[kMaxPerSeed] = {};
};

// Prefetch hint: read, high temporal locality. The line is wanted almost
// immediately, so it belongs in L1 rather than being streamed.
inline void prefetch_for_read(const void* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(p, 0 /* read */, 3 /* high locality */);
#else
  (void)p;
#endif
}

// Two-phase batched probe.
//
// `out` must have room for seeds.size() entries. Returns the number of seeds that
// matched at least one position.
inline std::size_t probe_batch(const MinimizerIndex& index,
                               std::span<const SeedHash> seeds,
                               std::span<SeedMatches> out,
                               IndexStats* stats = nullptr) noexcept {
  if (seeds.empty() || out.size() < seeds.size()) return 0;

  const MinimizerIndex::Entry* table = index.data();
  const std::size_t mask = index.mask();
  if (table == nullptr || index.capacity() == 0) return 0;

  std::size_t matched_seeds = 0;

  // PREFETCH DISTANCE, and why the obvious version did not work.
  //
  // The first cut of this issued all 16 prefetches and then immediately resolved
  // the same 16. That gives each prefetch only the handful of instructions that
  // follow it as cover -- tens of cycles against the ~80-100 ns it needs -- so the
  // loads still stalled, and the extra bookkeeping made it a net LOSS. Measured:
  // 60.8 ns/query scalar versus 69.3 ns/query batched, a 0.88x "speedup".
  //
  // Two ping-ponged buffers fix it: stage and prefetch batch i+1, THEN resolve
  // batch i. Now each prefetch has a whole batch of resolution work to hide behind.
  std::size_t slot[2][kProbeBatch];
  std::uint64_t fp[2][kProbeBatch];
  std::size_t staged[2] = {0, 0};
  int cur = 0;

  // Computes slots and issues prefetches for one batch. Deliberately touches
  // nothing in the table: that is what keeps the misses outstanding together.
  const auto stage = [&](std::size_t base, int buf) -> std::size_t {
    const std::size_t n = std::min(kProbeBatch, seeds.size() - base);
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint64_t h = seeds[base + i].hash;
      slot[buf][i] = index.slot_of(h);
      fp[buf][i] = MinimizerIndex::fingerprint_of(h);
      prefetch_for_read(&table[slot[buf][i]]);
    }
    return n;
  };

  std::size_t base = 0;
  staged[cur] = stage(base, cur);

  while (staged[cur] > 0) {
    const std::size_t n = staged[cur];
    const std::size_t next_base = base + n;
    const int nxt = cur ^ 1;

    // Stage the NEXT batch first, so its prefetches are in flight while this one
    // is resolved.
    staged[nxt] = next_base < seeds.size() ? stage(next_base, nxt) : 0;

    for (std::size_t i = 0; i < n; ++i) {
      SeedMatches& m = out[base + i];
      m.seed_offset = seeds[base + i].offset;
      m.count = 0;

      std::size_t s = slot[cur][i];
      std::size_t probes = 0;
      std::size_t lines = 0;
      std::size_t last_line = ~std::size_t{0};
      std::uint64_t rejects = 0;

      // Identical walk to MinimizerIndex::query(); see the correctness contract.
      for (std::size_t p = 0; p < MinimizerIndex::kMaxProbe; ++p) {
        ++probes;
        const std::size_t line = s / kEntriesPerLine;
        if (line != last_line) {
          ++lines;
          last_line = line;
        }
        const MinimizerIndex::Entry e = table[s];
        if (e == MinimizerIndex::kEmpty) break;
        if (MinimizerIndex::entry_fingerprint(e) == fp[cur][i]) {
          m.positions[m.count++] = MinimizerIndex::entry_position(e);
          if (m.count == SeedMatches::kMaxPerSeed) break;
        } else {
          ++rejects;
        }
        s = (s + 1) & mask;
      }

      if (m.count > 0) ++matched_seeds;
      if (stats != nullptr) {
        ++stats->queries;
        if (m.count > 0) {
          ++stats->hits;
        } else {
          ++stats->misses;
        }
        stats->fingerprint_rejects += rejects;
        stats->note_probe(probes, lines);
      }
    }

    base = next_base;
    cur = nxt;
  }
  return matched_seeds;
}

// Reusable scratch so the hot path never allocates. One per worker; workers own
// disjoint channels, so no sharing and no synchronisation.
struct ProbeScratch {
  std::vector<QEvent> events;
  std::vector<SeedHash> all_seeds;
  std::vector<SeedHash> minimizers;
  std::vector<SeedMatches> matches;

  void clear() noexcept {
    events.clear();
    all_seeds.clear();
    minimizers.clear();
    matches.clear();
  }

  // Preallocate for the largest chunk expected, so steady state does no malloc.
  void reserve_for(std::size_t max_samples, const QuantConfig& cfg) {
    const std::size_t max_events = max_samples / std::max(1u, cfg.samples_per_event) + 1;
    events.reserve(max_events);
    all_seeds.reserve(max_events);
    minimizers.reserve(max_events);
    matches.reserve(max_events);
  }
};

// Signal in, seed matches out: the whole query side in one call.
//
// Returns the number of seeds that matched. The caller decides what a decision
// requires -- a single seed hit is NOT evidence of anything. Specificity has to
// come from several seeds agreeing on a consistent reference position, which is
// chaining, and chaining is not implemented yet.
inline std::size_t match_signal(const MinimizerIndex& index,
                                std::span<const std::int16_t> raw,
                                const SignalScaling& scaling, const QuantConfig& cfg,
                                ProbeScratch& scratch, IndexStats* stats = nullptr) {
  scratch.clear();
  quantise_signal(raw, scaling, cfg, scratch.events);
  if (scratch.events.size() < cfg.events_per_key) return 0;

  hash_all_keys(scratch.events, cfg, scratch.all_seeds);
  select_minimizers(scratch.all_seeds, cfg.minimizer_window, scratch.minimizers);
  if (scratch.minimizers.empty()) return 0;

  scratch.matches.resize(scratch.minimizers.size());
  return probe_batch(index, scratch.minimizers, scratch.matches, stats);
}

}  // namespace mru
