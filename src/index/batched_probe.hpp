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
#include <cstring>
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
  const MinimizerIndex::Position* positions = index.positions();
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

      // The SAME probe function the scalar path calls. The two used to duplicate this
      // walk, kept honest by a test asserting they agreed; sharing it makes them unable
      // to disagree, which is better than detecting a divergence afterwards.
      const MinimizerIndex::Probe pr =
          MinimizerIndex::probe_at(table, mask, positions, slot[cur][i], fp[cur][i]);

      const auto take =
          static_cast<std::uint32_t>(std::min<std::size_t>(pr.count, SeedMatches::kMaxPerSeed));
      for (std::uint32_t j = 0; j < take; ++j) m.positions[j] = pr.positions[j];
      m.count = take;

      if (m.count > 0) ++matched_seeds;
      if (stats != nullptr) {
        ++stats->queries;
        if (pr.count > 0) {
          ++stats->hits;
        } else {
          ++stats->misses;
        }
        stats->fingerprint_rejects += pr.rejects;
        stats->positions_returned += take;
        if (pr.count > SeedMatches::kMaxPerSeed) {
          stats->positions_truncated += pr.count - SeedMatches::kMaxPerSeed;
        }
        stats->note_probe(pr.slots, pr.lines);
      }
    }

    base = next_base;
    cur = nxt;
  }
  return matched_seeds;
}

// ---------------------------------------------------------------------------
// Multi-probe
// ---------------------------------------------------------------------------
//
// Per-event bucket disagreement is 6.9% at the frozen geometry, and because a key
// requires ALL 15 events to agree, that compounds into roughly half of keys failing.
// The measured distribution says this is a boundary problem, not an estimator
// problem: 94% of disagreements occur within 0.3 of a bucket boundary, and across
// 16000 events not one was ever off by more than a single bucket. So probing an
// event's neighbouring bucket is a targeted repair rather than a shotgun.
//
// WHAT IT IS ACTUALLY WORTH -- stated here because this is where the claim would be
// made, and the honest number is much smaller than the first measurement suggested:
//
//   window   1 probe -> 4 probes      relative gain
//     w=1    151.9 -> 263.8 seeds          +74%
//     w=5     48.8 ->  61.6               +26%
//     w=10    25.6 ->  29.2               +14%     <- the frozen window
//     w=20    12.0 ->  13.3               +11%
//
// At w=1 multi-probe is transformative. At w=10, which the 9 GB memory budget
// forces, it is worth about +5 points of seed recall. The reason is a compound
// failure: flipping a marginal event repairs the KEY but changes its HASH, which can
// hand the minimizer window to a different position on the query side, or mean the
// reference never selected that position at all. Multi-probe fixes key mismatches;
// it cannot fix selection mismatches, and selection dominates as the window grows.
//
// It is still worth doing. The extra true seeds land on the CORRECT diagonal while
// false hits scatter, so chaining margins improve with probe count (measured 41.8x
// -> 62.1x going from 1 to 4 probes), and the cost is a few microseconds against a
// ~600 us per-chunk budget. But it is a refinement, not the headline.
//
// BIOLOGICAL LIMITATIONS of every number above, for the paper's discussion:
//   * Repeats. The synthetic reference is random DNA, which has none. A mammalian
//     genome is ~50% repetitive, and repeats create identical k-mer contexts and so
//     identical keys. Effective key space on real DNA will be smaller and occupancy
//     higher than measured, which is exactly why 3x15 was chosen over the nominal
//     optimum 3x13: 30x headroom instead of 25%.
//   * Homopolymers. A run of identical bases keeps the pore at one level for an
//     unpredictable number of samples, so the fixed samples_per_event windowing
//     drifts out of register with the reference. Fixed-window downsampling cannot
//     represent that; true event segmentation can, at a cost not yet measured.
//   * Translocation speed variance. Real DNA does not move at a constant 400 b/s --
//     it varies within and between reads, and stalls outright. Both break the
//     assumption that query event i corresponds to reference event i + constant,
//     which is the assumption diagonal voting rests on. Chaining with a tolerance
//     band rather than an exact diagonal would absorb it; the current exact-diagonal
//     match is optimistic.
//   * Signal realism. The synthetic model has Gaussian per-sample noise and no
//     drift, no blockages and no channel-specific gain changes over a run.

// 1, 2 or 4 keys per selected minimizer. 2 flips the single most marginal event;
// 4 additionally flips the second most marginal, and both together.
enum class ProbeBudget : int {
  kExact = 1,
  kOneFlip = 2,
  kTwoFlips = 4,
};

inline constexpr int kDefaultProbeBudget = static_cast<int>(ProbeBudget::kTwoFlips);

// Expands each selected minimizer into up to `budget` keys, ranking marginality from
// QUERY-SIDE information only -- a real read has nothing else.
//
// Every variant keeps the ORIGINAL minimizer's offset, so chaining sees one seed
// position regardless of how many keys were probed for it. Note that if two variants
// both hit the same reference position, the diagonal receives two votes. The measured
// off-target separation was obtained with that behaviour, so it is kept rather than
// quietly changed; deduplicating by (offset, diagonal) in the chaining stage is a
// one-line change if strict one-vote-per-seed is wanted later.
//
// Allocation-free apart from `out`, which the caller reuses across chunks.
inline void expand_multiprobe(std::span<const QEvent> events, std::span<const float> zs,
                              std::span<const SeedHash> minimizers,
                              const QuantConfig& cfg, int budget,
                              std::vector<SeedHash>& out) {
  out.clear();
  if (minimizers.empty() || budget < 1 || !cfg.valid()) return;

  const std::uint32_t n = cfg.events_per_key;
  // cfg.valid() guarantees bits_per_event >= 1 and bits*events <= 64.
  QEvent trial[64];
  out.reserve(minimizers.size() * static_cast<std::size_t>(budget));

  for (const SeedHash& m : minimizers) {
    const std::size_t i = m.offset;
    if (i + n > events.size() || i + n > zs.size()) continue;

    // The exact key: reuse the hash already computed during selection.
    out.push_back(SeedHash{m.hash, m.offset});
    if (budget < 2 || n < 1) continue;

    // Two smallest boundary distances in one linear scan; no sort, no allocation.
    std::uint32_t m0 = 0, m1 = 0;
    float d0 = 2.0f, d1 = 2.0f;
    for (std::uint32_t e = 0; e < n; ++e) {
      const float d = boundary_distance(zs[i + e], cfg);
      if (d < d0) {
        d1 = d0;
        m1 = m0;
        d0 = d;
        m0 = e;
      } else if (d < d1) {
        d1 = d;
        m1 = e;
      }
    }

    std::memcpy(trial, events.data() + i, n * sizeof(QEvent));
    const QEvent nb0 = neighbour_bucket(zs[i + m0], cfg);
    trial[m0] = nb0;
    out.push_back(SeedHash{hash64(pack_key(trial, cfg)), m.offset});

    if (budget >= 4 && n >= 2) {
      const QEvent nb1 = neighbour_bucket(zs[i + m1], cfg);
      trial[m0] = events[i + m0];
      trial[m1] = nb1;
      out.push_back(SeedHash{hash64(pack_key(trial, cfg)), m.offset});
      trial[m0] = nb0;
      out.push_back(SeedHash{hash64(pack_key(trial, cfg)), m.offset});
    }
  }
}

// Reusable scratch so the hot path never allocates. One per worker; workers own
// disjoint channels, so no sharing and no synchronisation.
struct ProbeScratch {
  std::vector<QEvent> events;
  std::vector<float> event_z;  // pre-quantisation z, needed to rank marginality
  std::vector<SeedHash> all_seeds;
  std::vector<SeedHash> minimizers;
  std::vector<SeedHash> probes;  // minimizers expanded by multi-probe
  std::vector<SeedMatches> matches;

  // Event-detection working set. Untouched when cfg.event_detection is false, and
  // reserved only then, so the fixed-width path pays nothing for it.
  EventScratch escratch;
  std::vector<float> event_means;

  void clear() noexcept {
    events.clear();
    event_z.clear();
    all_seeds.clear();
    minimizers.clear();
    probes.clear();
    matches.clear();
  }

  // Preallocate for the largest chunk expected, so steady state does no malloc.
  void reserve_for(std::size_t max_samples, const QuantConfig& cfg,
                   int budget = kDefaultProbeBudget) {
    // Event detection can emit more events than fixed-width slicing for the same
    // samples, because a short dwell still yields one event. Reserve against the
    // detector's floor (min_len samples per event) so the detected path cannot allocate
    // on the decision path either.
    const std::size_t fixed_events =
        max_samples / std::max(1u, cfg.samples_per_event) + 1;
    const std::size_t detected_events =
        cfg.event_detection ? max_samples / std::max(1u, cfg.detect.min_len) + 1 : 0;
    const std::size_t max_events = std::max(fixed_events, detected_events);
    escratch.reserve(max_samples);
    event_means.reserve(max_events);
    events.reserve(max_events);
    event_z.reserve(max_events);
    all_seeds.reserve(max_events);
    minimizers.reserve(max_events);
    probes.reserve(max_events * static_cast<std::size_t>(std::max(1, budget)));
    matches.reserve(max_events * static_cast<std::size_t>(std::max(1, budget)));
  }
};

// Signal in, seed matches out: the whole query side in one call.
//
// Returns the number of probes that matched. The caller decides what a decision
// requires -- a single seed hit is NOT evidence of anything. Specificity comes from
// several seeds agreeing on a consistent reference position, i.e. chaining. Measured
// decision rule at the frozen geometry: accept at >= 4 votes on the dominant
// diagonal, unblock below that, and defer one chunk when marginal, since votes scale
// with read length.
inline std::size_t match_signal(const MinimizerIndex& index,
                                std::span<const std::int16_t> raw,
                                const SignalScaling& scaling, const QuantConfig& cfg,
                                ProbeScratch& scratch,
                                int budget = kDefaultProbeBudget,
                                IndexStats* stats = nullptr) {
  scratch.clear();
  if (cfg.event_detection) {
    quantise_signal_detected(raw, scaling, cfg, scratch.escratch, scratch.event_means,
                             scratch.events, &scratch.event_z);
  } else {
    quantise_signal(raw, scaling, cfg, scratch.events, &scratch.event_z);
  }
  if (scratch.events.size() < cfg.events_per_key) return 0;

  hash_all_keys(scratch.events, cfg, scratch.all_seeds);
  select_minimizers(scratch.all_seeds, cfg.minimizer_window, scratch.minimizers);
  if (scratch.minimizers.empty()) return 0;

  expand_multiprobe(scratch.events, scratch.event_z, scratch.minimizers, cfg, budget,
                    scratch.probes);
  if (scratch.probes.empty()) return 0;

  scratch.matches.resize(scratch.probes.size());
  return probe_batch(index, scratch.probes, scratch.matches, stats);
}

}  // namespace mru
