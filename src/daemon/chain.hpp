// src/daemon/chain.hpp
//
// Gap-tolerant chaining, streamed, with fixed memory per channel.
//
// WHY THIS REPLACES EXACT DIAGONAL VOTING. bench/squig_sweep.cpp measured RawHash2 across
// a dwell sweep on identical signal and it barely moves: 95.0% correctly mapped at dwell
// CV 0 and 92.0% at CV 0.60. Our exact-diagonal vote goes 49.7% to 1.3% over the same
// range. The difference is not quantisation, not the pore model and not the detector --
// RawHash chains anchors with gap tolerance, so a slipped event boundary costs ONE anchor,
// while demanding exact diagonal equality means the same slip moves every later seed onto a
// different diagonal and the read is lost. Event detection reduced how often boundaries
// slip; it could never make an exact-equality test tolerate the slips that remain.
//
// WHY NOT MINIMAP2/RAWHASH-STYLE CHAINING DIRECTLY. That is a batch algorithm: collect all
// of a read's anchors, sort, then dynamic-programming over up to --max-iterations (200)
// predecessors per anchor. It needs the whole read and memory proportional to its anchor
// count. A read-until daemon has neither: it must decide from the chunks that have arrived
// so far, for up to 2675 channels at once, without allocating on the decision path. Chains
// here are therefore extended INCREMENTALLY as anchors arrive, in O(kChains) per anchor
// with kChains fixed, and a channel's state is a small fixed array that is reset per read.
//
// THE RULE. An anchor (q, r) extends a chain whose last anchor was (q_end, r_end) when both
// gaps move forward, neither exceeds kMaxGap, and the two gaps agree to within kBand:
//
//     dq = q - q_end,  dr = r - r_end
//     0 <= dq <= kMaxGap,  0 <= dr <= kMaxGap,  |dq - dr| <= kBand
//
// |dq - dr| is the drift this anchor introduces. Exact diagonal voting is the special case
// kBand = 0 with kMaxGap unbounded, which is why it cannot absorb a boundary slip: one
// missed event makes dq and dr differ by one for the rest of the read. Allowing a few events
// of local disagreement lets a chain survive mis-segmentation while still requiring the
// anchors to march forward together, which random matches do not do.
//
// Replacement is Misra-Gries, as in the vote table it replaces: a new chain either takes the
// weakest slot or decays it, so nothing is ever permanently locked out.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "index/batched_probe.hpp"
#include "index/quantise.hpp"

namespace mru {

struct ChainConfig {
  // Largest forward step, in events, that still counts as the same chain. Wide enough to
  // bridge a dropped seed or a capped-occurrence gap, narrow enough that two unrelated
  // loci do not get stitched together.
  std::uint32_t max_gap = 160;

  // Allowed disagreement between the query gap and the reference gap for a single
  // extension. This is the whole point of the structure: 0 reproduces exact diagonal
  // voting and its cliff.
  std::uint32_t band = 12;

  // Predecessors examined per anchor by the anchor-history chainer below. Cost is
  // O(lookback) per anchor, paid once and never re-paid on a later chunk.
  std::uint32_t lookback = 48;

  // Skip a seed whose match count has reached this value, as an occurrence filter. Our
  // probe returns at most SeedMatches::kMaxPerSeed positions, so a saturated count means
  // the key is ambiguous. 0 disables filtering.
  std::uint32_t max_occ = 0;

  [[nodiscard]] bool valid() const noexcept { return max_gap > 0; }
};

// Per-channel chain state, reset at the start of each read.
struct ChannelChains {
  static constexpr std::size_t kChains = 24;

  std::int64_t q_end[kChains] = {};
  std::int64_t r_end[kChains] = {};
  std::uint32_t score[kChains] = {};  // 0 means the slot is empty
  std::uint64_t events_consumed = 0;
  std::uint32_t chunks = 0;
  SignalScaling scaling{};
  bool have_scaling = false;

  void reset() noexcept {
    std::memset(score, 0, sizeof(score));
    events_consumed = 0;
    chunks = 0;
    have_scaling = false;
  }

  // Returns the best chain's score, and its diagonal through out_diag so a caller can
  // report a locus.
  [[nodiscard]] std::uint32_t best(std::int64_t* out_diag) const noexcept {
    std::uint32_t b = 0;
    std::int64_t d = 0;
    for (std::size_t i = 0; i < kChains; ++i) {
      if (score[i] > b) {
        b = score[i];
        d = r_end[i] - q_end[i];
      }
    }
    if (out_diag != nullptr) *out_diag = d;
    return b;
  }

  void add_anchor(std::int64_t q, std::int64_t r, const ChainConfig& cfg) noexcept {
    std::size_t weakest = 0;
    std::uint32_t weakest_score = 0xFFFFFFFFu;
    // Prefer extending the HIGHEST-scoring compatible chain. Picking the first match
    // instead lets a one-anchor stub steal extensions from the real chain and both then
    // stay short.
    std::size_t best_fit = kChains;
    std::uint32_t best_fit_score = 0;

    for (std::size_t i = 0; i < kChains; ++i) {
      if (score[i] == 0) {
        if (weakest_score != 0) {
          weakest = i;
          weakest_score = 0;
        }
        continue;
      }
      const std::int64_t dq = q - q_end[i];
      const std::int64_t dr = r - r_end[i];
      if (dq >= 0 && dr >= 0 && dq <= static_cast<std::int64_t>(cfg.max_gap) &&
          dr <= static_cast<std::int64_t>(cfg.max_gap)) {
        const std::int64_t drift = dq > dr ? dq - dr : dr - dq;
        if (drift <= static_cast<std::int64_t>(cfg.band) && score[i] >= best_fit_score) {
          best_fit = i;
          best_fit_score = score[i];
        }
      }
      if (score[i] < weakest_score) {
        weakest_score = score[i];
        weakest = i;
      }
    }

    if (best_fit != kChains) {
      ++score[best_fit];
      q_end[best_fit] = q;
      r_end[best_fit] = r;
      return;
    }
    // No compatible chain: seed a new one, Misra-Gries style.
    if (weakest_score <= 1) {
      q_end[weakest] = q;
      r_end[weakest] = r;
      score[weakest] = 1;
    } else {
      --score[weakest];
    }
  }
};

// Fold one chunk's seed matches into a channel's chains. Returns candidates seen.
//
// Anchors arrive in increasing query order because seeds are generated in event order,
// which is what makes single-pass chain extension valid without sorting. chunk_events
// advances the read's event base so the next chunk's query coordinates continue this one's.
inline std::size_t accumulate_chunk_chains(const ProbeScratch& scratch,
                                           ChannelChains& chains, const ChainConfig& cfg,
                                           std::uint64_t& dropped,
                                           std::uint64_t chunk_events,
                                           std::size_t max_anchors = 8192) {
  const std::int64_t base = static_cast<std::int64_t>(chains.events_consumed);
  std::size_t seen = 0;
  for (const SeedMatches& m : scratch.matches) {
    const std::int64_t q = base + static_cast<std::int64_t>(m.seed_offset);
    for (std::uint32_t j = 0; j < m.count; ++j) {
      ++seen;
      if (seen > max_anchors) {
        ++dropped;
        continue;
      }
      chains.add_anchor(q, static_cast<std::int64_t>(m.positions[j]), cfg);
    }
  }
  chains.events_consumed += chunk_events;
  return seen;
}

// ---------------------------------------------------------------------------------------
// Anchor-history chaining: minimap2-style best-predecessor scoring, restricted to what a
// streaming decision can afford.
//
// The greedy chainer above keeps only each chain's LAST anchor, so it cannot reconsider:
// once an anchor attaches to the wrong chain that is permanent, and with a saturated index
// most anchors are noise, so 24 slots thrash. That is why it bought only 23% where RawHash
// sits 5x higher. RawHash scores every anchor against every plausible predecessor and takes
// the best, which is what actually tolerates noise.
//
// Doing that exactly needs the whole read. Doing it incrementally needs two bounds:
//
//   lookback   how many recent anchors are considered as predecessors. minimap2 uses 25-50
//              with an early exit; RawHash's --max-iterations defaults to 200. Cost is
//              O(lookback) per anchor, paid once and NEVER re-paid on a later chunk --
//              which is the point, because a batch chainer re-chains every accumulated
//              anchor on every chunk and so does quadratic work over a read.
//   kAnchors   per-channel anchor history, a ring. Older anchors fall out, which bounds
//              memory and is harmless: a predecessor further back than max_gap in query
//              space could not have been chained to anyway.
//
// max_occ is the other half. Our probe returns at most SeedMatches::kMaxPerSeed positions,
// so a seed that comes back saturated is by definition ambiguous. Dropping those is the
// cheap form of RawHash's --q-mid-occ occurrence filter, and on a capped index it removes
// most of the noise the chainer would otherwise have to out-vote.
struct ChannelAnchorChain {
  static constexpr std::size_t kAnchors = 1024;

  std::int32_t q[kAnchors] = {};
  std::int64_t r[kAnchors] = {};
  std::uint16_t sc[kAnchors] = {};  // best chain score ENDING at this anchor
  std::size_t head = 0;             // next write position in the ring
  std::size_t stored = 0;           // anchors ever written, for ring arithmetic
  std::uint32_t best_score = 0;
  std::int64_t best_diag = 0;

  std::uint64_t events_consumed = 0;
  std::uint32_t chunks = 0;
  SignalScaling scaling{};
  bool have_scaling = false;

  void reset() noexcept {
    head = 0;
    stored = 0;
    best_score = 0;
    best_diag = 0;
    events_consumed = 0;
    chunks = 0;
    have_scaling = false;
  }

  [[nodiscard]] std::uint32_t best(std::int64_t* out_diag) const noexcept {
    if (out_diag != nullptr) *out_diag = best_diag;
    return best_score;
  }

  void add_anchor(std::int64_t qq, std::int64_t rr, const ChainConfig& cfg) noexcept {
    const std::int64_t gap = static_cast<std::int64_t>(cfg.max_gap);
    const std::int64_t band = static_cast<std::int64_t>(cfg.band);
    std::uint16_t best_here = 1;  // an anchor is always a chain of one

    const std::size_t look =
        std::min<std::size_t>(cfg.lookback, std::min(stored, kAnchors));
    for (std::size_t back = 1; back <= look; ++back) {
      const std::size_t i = (head + kAnchors - back) % kAnchors;
      const std::int64_t dq = qq - static_cast<std::int64_t>(q[i]);
      if (dq < 0) continue;
      // Anchors arrive in increasing query order, so once the look-back reaches further
      // than max_gap in q, nothing older can qualify either.
      if (dq > gap) break;
      const std::int64_t dr = rr - r[i];
      if (dr < 0 || dr > gap) continue;
      const std::int64_t drift = dq > dr ? dq - dr : dr - dq;
      if (drift > band) continue;
      const std::uint16_t cand = static_cast<std::uint16_t>(sc[i] + 1);
      if (cand > best_here) best_here = cand;
    }

    q[head] = static_cast<std::int32_t>(qq);
    r[head] = rr;
    sc[head] = best_here;
    head = (head + 1) % kAnchors;
    ++stored;

    if (best_here > best_score) {
      best_score = best_here;
      best_diag = rr - qq;
    }
  }
};

// As accumulate_chunk_chains, for the anchor-history chainer. Seeds whose match count has
// saturated the per-seed cap are skipped when cfg.max_occ is set.
inline std::size_t accumulate_chunk_anchor_chain(const ProbeScratch& scratch,
                                                 ChannelAnchorChain& ch,
                                                 const ChainConfig& cfg,
                                                 std::uint64_t& dropped,
                                                 std::uint64_t chunk_events) {
  const std::int64_t base = static_cast<std::int64_t>(ch.events_consumed);
  std::size_t seen = 0;
  for (const SeedMatches& m : scratch.matches) {
    if (cfg.max_occ != 0 && m.count >= cfg.max_occ) {
      seen += m.count;
      dropped += m.count;
      continue;
    }
    const std::int64_t qq = base + static_cast<std::int64_t>(m.seed_offset);
    for (std::uint32_t j = 0; j < m.count; ++j) {
      ++seen;
      ch.add_anchor(qq, static_cast<std::int64_t>(m.positions[j]), cfg);
    }
  }
  ch.events_consumed += chunk_events;
  return seen;
}

}  // namespace mru
