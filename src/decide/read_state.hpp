// src/decide/read_state.hpp
//
// Per-channel read state, owned by exactly one decision worker.
//
// Two facts about the MinKNOW 6.x API drive this file:
//
//  1. ReadData.number and Action.read.number were REMOVED (field 2 of ReadData
//     and field 4 of Action are gaps in 6.10.3). Every action must therefore
//     carry the read id as a string. Doing that naively means a heap allocation
//     and a string hash per chunk, on the hot path, 2675 channels at a time. So
//     the id is copied ONCE per read into a fixed inline buffer and compared
//     with memcmp thereafter.
//
//  2. Channels are dense and bounded -- 512 on MinION, 2675 on PromethION --
//     and arrive as uint32 keys. That makes a flat array indexed by
//     (channel - first_channel) strictly better than any hash map: one
//     predictable load, no hashing, no probing, no rehash. There is no reason to
//     use an unordered_map here and every reason not to.
//
// Because chunks are sharded by channel, one worker owns a disjoint subset of
// channels for the lifetime of the run, so nothing in this file needs to be
// atomic or locked. That is the whole payoff of the shard-per-worker model.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mru {

// Read ids are UUIDs: 36 characters plus a NUL. 40 keeps the struct aligned and
// leaves room for a longer id without silently truncating one.
inline constexpr std::size_t kReadIdCapacity = 40;

// A read id stored inline. No allocation, trivially copyable.
class ReadId {
 public:
  // Copies the id. Returns false if it does not fit, which the caller MUST
  // surface rather than ignore: a truncated id would be rejected by MinKNOW and
  // the unblock would be lost silently. That is the worst available failure mode
  // -- the pore keeps sequencing a molecule we already decided to reject.
  [[nodiscard]] bool assign(std::string_view id) noexcept {
    if (id.size() >= kReadIdCapacity) {
      clear();
      return false;
    }
    if (!id.empty()) std::memcpy(buf_, id.data(), id.size());
    buf_[id.size()] = '\0';
    len_ = static_cast<std::uint32_t>(id.size());
    return true;
  }

  [[nodiscard]] std::string_view view() const noexcept {
    return std::string_view(buf_, len_);
  }
  [[nodiscard]] const char* c_str() const noexcept { return buf_; }
  [[nodiscard]] std::uint32_t size() const noexcept { return len_; }
  [[nodiscard]] bool empty() const noexcept { return len_ == 0; }

  void clear() noexcept {
    len_ = 0;
    buf_[0] = '\0';
  }

  [[nodiscard]] bool operator==(std::string_view o) const noexcept {
    if (o.size() != len_) return false;
    if (len_ == 0) return true;  // memcmp with a null pointer is UB even at n=0
    return std::memcmp(buf_, o.data(), len_) == 0;
  }
  [[nodiscard]] bool operator!=(std::string_view o) const noexcept {
    return !(*this == o);
  }

 private:
  std::uint32_t len_{0};
  char buf_[kReadIdCapacity]{};
};

static_assert(std::is_trivially_copyable_v<ReadId>);

enum class ReadDecision : std::uint8_t {
  kUndecided = 0,  // still accumulating chunks
  kAccept,         // let it sequence to completion
  kReject,         // unblock dispatched
  kExhausted,      // gave up: too many chunks without a confident call
};

// What a freshly arrived chunk means for the channel it landed on.
enum class ChunkKind : std::uint8_t {
  kNewRead,        // different read id than last time: accumulators were reset
  kContinuation,   // same read, more signal
  kUnknownChannel, // outside this worker's range -- a routing bug, so count it
  kIdTooLong,      // see ReadId::assign; must be counted, never ignored
};

struct ChannelState {
  ReadId read_id;
  std::uint64_t first_chunk_sample{0};  // chunk_start_sample of the read's first chunk
  std::uint64_t samples_seen{0};
  std::uint64_t decision_sample{0};     // sample index at which we decided
  std::uint32_t chunks_seen{0};
  std::uint32_t reads_seen{0};          // how many reads this channel has produced
  ReadDecision decision{ReadDecision::kUndecided};

  void begin(std::uint64_t chunk_start_sample) noexcept {
    first_chunk_sample = chunk_start_sample;
    samples_seen = 0;
    decision_sample = 0;
    chunks_seen = 0;
    decision = ReadDecision::kUndecided;
    ++reads_seen;
  }

  [[nodiscard]] bool decided() const noexcept {
    return decision != ReadDecision::kUndecided;
  }

  // Latches. Once decided, a second call is a no-op returning false. This stops
  // two actions being sent for one read, which would waste an action slot and
  // can race with MinKNOW's own read teardown.
  [[nodiscard]] bool decide(ReadDecision d, std::uint64_t at_sample) noexcept {
    if (decided() || d == ReadDecision::kUndecided) return false;
    decision = d;
    decision_sample = at_sample;
    return true;
  }
};

// R10.4.1 at 4 kHz sampling with ~400 b/s translocation: 10 samples per base.
// RNA and other chemistries differ, so this is a parameter, not a constant.
inline constexpr double kSamplesPerBaseR10DNA = 10.0;

// The metric a biologist actually cares about: how much sequencing was spent on
// a molecule after we had already decided to reject it.
[[nodiscard]] inline std::uint64_t bases_wasted(
    std::uint64_t decision_sample, std::uint64_t unblock_sample,
    double samples_per_base = kSamplesPerBaseR10DNA) noexcept {
  if (unblock_sample <= decision_sample || samples_per_base <= 0.0) return 0;
  const double samples = static_cast<double>(unblock_sample - decision_sample);
  return static_cast<std::uint64_t>(samples / samples_per_base);
}

// Flat, dense, one worker's channels. Indexed by channel - first_channel.
class ChannelTable {
 public:
  ChannelTable(std::uint32_t first_channel, std::uint32_t last_channel)
      : first_(first_channel),
        last_(last_channel),
        slots_(last_channel >= first_channel
                   ? static_cast<std::size_t>(last_channel - first_channel) + 1
                   : 0) {
    assert(last_channel >= first_channel && "empty channel range");
  }

  [[nodiscard]] bool contains(std::uint32_t channel) const noexcept {
    return channel >= first_ && channel <= last_;
  }

  [[nodiscard]] ChannelState& at(std::uint32_t channel) noexcept {
    assert(contains(channel));
    return slots_[channel - first_];
  }
  [[nodiscard]] const ChannelState& at(std::uint32_t channel) const noexcept {
    assert(contains(channel));
    return slots_[channel - first_];
  }

  // Call once per arriving chunk. The read id is copied only when the read
  // changes, so steady state is one memcmp plus two integer adds.
  [[nodiscard]] ChunkKind observe(std::uint32_t channel, std::string_view read_id,
                                  std::uint64_t chunk_start_sample,
                                  std::uint64_t chunk_samples) noexcept {
    if (!contains(channel)) {
      ++unknown_channel_;
      return ChunkKind::kUnknownChannel;
    }
    ChannelState& s = at(channel);

    if (!s.read_id.empty() && s.read_id == read_id) {
      ++s.chunks_seen;
      s.samples_seen += chunk_samples;
      return ChunkKind::kContinuation;
    }

    if (!s.read_id.assign(read_id)) {
      ++id_too_long_;
      return ChunkKind::kIdTooLong;
    }
    s.begin(chunk_start_sample);
    s.chunks_seen = 1;
    s.samples_seen = chunk_samples;
    ++reads_started_;
    return ChunkKind::kNewRead;
  }

  [[nodiscard]] std::uint32_t first_channel() const noexcept { return first_; }
  [[nodiscard]] std::uint32_t last_channel() const noexcept { return last_; }
  [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }
  [[nodiscard]] std::uint64_t reads_started() const noexcept { return reads_started_; }
  [[nodiscard]] std::uint64_t unknown_channel() const noexcept {
    return unknown_channel_;
  }
  [[nodiscard]] std::uint64_t id_too_long() const noexcept { return id_too_long_; }

 private:
  std::uint32_t first_;
  std::uint32_t last_;
  std::vector<ChannelState> slots_;
  std::uint64_t reads_started_{0};
  std::uint64_t unknown_channel_{0};
  std::uint64_t id_too_long_{0};
};

}  // namespace mru
