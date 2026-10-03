// test/read_state_test.cpp
//
// Unit tests for the per-channel read state, plus a throughput check that backs
// the design claim: observe() must be allocation-free and cheap enough to run on
// every chunk of every channel at PromethION scale.
//
// Framework-free, same style as ring_tsan.cpp.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/tsc.hpp"
#include "decide/read_state.hpp"
#include "transport/chunk_ref.hpp"

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

#define CHECK_EQ(a, b, what)                                                 \
  do {                                                                       \
    const std::uint64_t lhs_ = static_cast<std::uint64_t>(a);                \
    const std::uint64_t rhs_ = static_cast<std::uint64_t>(b);                \
    if (lhs_ != rhs_) {                                                      \
      std::printf("  FAIL  %s:%d  %s  (%llu != %llu)\n", __FILE__, __LINE__, \
                  (what), static_cast<unsigned long long>(lhs_),             \
                  static_cast<unsigned long long>(rhs_));                    \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

void banner(const char* name) { std::printf("[ RUN ] %s\n", name); }

// A realistic read id: 36-character UUID, which is what MinKNOW sends.
constexpr std::string_view kUuidA = "0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9";
constexpr std::string_view kUuidB = "ffffffff-0000-1111-2222-333344445555";

void test_read_id() {
  banner("read_id");
  mru::ReadId id;
  CHECK(id.empty(), "default-constructed id is empty");
  CHECK(id == std::string_view(""), "empty id equals empty view");

  CHECK(id.assign(kUuidA), "a 36-char UUID fits");
  CHECK_EQ(id.size(), kUuidA.size(), "stored length");
  CHECK(id == kUuidA, "compares equal to its own value");
  CHECK(id != kUuidB, "compares unequal to a different UUID");
  CHECK(id.view() == kUuidA, "view round-trips");
  CHECK(std::string(id.c_str()) == std::string(kUuidA), "NUL-terminated");

  // Capacity boundary: 39 chars fit (39 + NUL == 40), 40 do not.
  const std::string just_fits(mru::kReadIdCapacity - 1, 'x');
  const std::string too_long(mru::kReadIdCapacity, 'x');
  CHECK(id.assign(just_fits), "capacity-1 chars fit");
  CHECK_EQ(id.size(), mru::kReadIdCapacity - 1, "boundary length");
  CHECK(!id.assign(too_long), "capacity chars are rejected");
  CHECK(id.empty(), "a rejected assign clears rather than truncating");

  id.clear();
  CHECK(id.empty(), "clear works");
}

void test_new_read_and_continuation() {
  banner("new_read_and_continuation");
  mru::ChannelTable t(1, 512);
  CHECK_EQ(t.size(), 512u, "dense slot count");
  CHECK(t.contains(1) && t.contains(512), "range endpoints included");
  CHECK(!t.contains(0) && !t.contains(513), "outside range excluded");

  CHECK(t.observe(7, kUuidA, 1000, 400) == mru::ChunkKind::kNewRead,
        "first chunk of a read is a new read");
  const auto& s = t.at(7);
  CHECK_EQ(s.first_chunk_sample, 1000u, "first_chunk_sample recorded");
  CHECK_EQ(s.chunks_seen, 1u, "chunk count after first chunk");
  CHECK_EQ(s.samples_seen, 400u, "samples after first chunk");
  CHECK_EQ(s.reads_seen, 1u, "reads_seen after first read");

  CHECK(t.observe(7, kUuidA, 1400, 400) == mru::ChunkKind::kContinuation,
        "same id is a continuation");
  CHECK(t.observe(7, kUuidA, 1800, 400) == mru::ChunkKind::kContinuation,
        "and again");
  CHECK_EQ(t.at(7).chunks_seen, 3u, "chunks accumulate");
  CHECK_EQ(t.at(7).samples_seen, 1200u, "samples accumulate");
  CHECK_EQ(t.at(7).first_chunk_sample, 1000u, "first_chunk_sample is stable");
  CHECK_EQ(t.reads_started(), 1u, "only one read started");

  // A different id on the same channel means the previous read ended.
  CHECK(t.observe(7, kUuidB, 9000, 400) == mru::ChunkKind::kNewRead,
        "a different id starts a new read");
  CHECK_EQ(t.at(7).chunks_seen, 1u, "accumulators reset on new read");
  CHECK_EQ(t.at(7).samples_seen, 400u, "sample count reset");
  CHECK_EQ(t.at(7).first_chunk_sample, 9000u, "first_chunk_sample moved");
  CHECK_EQ(t.at(7).reads_seen, 2u, "reads_seen incremented");
  CHECK_EQ(t.reads_started(), 2u, "two reads started");

  // Channels are independent.
  CHECK(t.observe(8, kUuidA, 1000, 400) == mru::ChunkKind::kNewRead,
        "another channel is independent");
  CHECK_EQ(t.at(7).reads_seen, 2u, "channel 7 untouched by channel 8");
}

void test_error_paths() {
  banner("error_paths");
  mru::ChannelTable t(100, 200);

  CHECK(t.observe(99, kUuidA, 0, 400) == mru::ChunkKind::kUnknownChannel,
        "below range is reported, not asserted");
  CHECK(t.observe(201, kUuidA, 0, 400) == mru::ChunkKind::kUnknownChannel,
        "above range is reported");
  CHECK_EQ(t.unknown_channel(), 2u, "unknown channels counted");
  CHECK_EQ(t.reads_started(), 0u, "no reads started from bad channels");

  const std::string too_long(mru::kReadIdCapacity + 5, 'z');
  CHECK(t.observe(150, too_long, 0, 400) == mru::ChunkKind::kIdTooLong,
        "an oversized id is reported, never truncated");
  CHECK_EQ(t.id_too_long(), 1u, "oversized ids counted");
  CHECK_EQ(t.reads_started(), 0u, "a rejected id does not start a read");
  CHECK(t.at(150).read_id.empty(), "state left clean after rejection");

  // Recovery: a valid id on that same channel still works afterwards.
  CHECK(t.observe(150, kUuidA, 500, 400) == mru::ChunkKind::kNewRead,
        "channel recovers after a bad id");
}

void test_decision_latch() {
  banner("decision_latch");
  mru::ChannelTable t(1, 16);
  (void)t.observe(3, kUuidA, 1000, 400);
  mru::ChannelState& s = t.at(3);

  CHECK(!s.decided(), "undecided initially");
  CHECK(!s.decide(mru::ReadDecision::kUndecided, 1400),
        "deciding 'undecided' is rejected");
  CHECK(!s.decided(), "still undecided");

  CHECK(s.decide(mru::ReadDecision::kReject, 1400), "first decision takes");
  CHECK(s.decided(), "now decided");
  CHECK_EQ(s.decision_sample, 1400u, "decision sample recorded");
  CHECK(s.decision == mru::ReadDecision::kReject, "decision value");

  CHECK(!s.decide(mru::ReadDecision::kAccept, 1800),
        "a second decision is refused");
  CHECK(s.decision == mru::ReadDecision::kReject, "original decision preserved");
  CHECK_EQ(s.decision_sample, 1400u, "original sample preserved");

  // A new read on the channel clears the latch.
  (void)t.observe(3, kUuidB, 5000, 400);
  CHECK(!t.at(3).decided(), "new read resets the latch");
  CHECK_EQ(t.at(3).decision_sample, 0u, "decision sample reset");
}

void test_bases_wasted() {
  banner("bases_wasted");
  // 4000 samples at 10 samples/base == 400 bases.
  CHECK_EQ(mru::bases_wasted(1000, 5000), 400u, "R10 DNA conversion");
  CHECK_EQ(mru::bases_wasted(1000, 1000), 0u, "no waste when equal");
  CHECK_EQ(mru::bases_wasted(5000, 1000), 0u, "no negative waste");
  CHECK_EQ(mru::bases_wasted(0, 4000, 10.0), 400u, "explicit rate");
  // RNA at ~70 b/s with 4 kHz sampling is ~57 samples/base.
  CHECK_EQ(mru::bases_wasted(0, 5700, 57.0), 100u, "RNA-like rate");
  CHECK_EQ(mru::bases_wasted(0, 4000, 0.0), 0u, "zero rate is rejected");
}

// PromethION scale: 2675 channels, each producing chunks continuously. observe()
// runs once per chunk, so it must be nanoseconds and allocation-free.
void test_observe_throughput() {
  banner("observe_throughput");
  constexpr std::uint32_t kChannels = 2675;
  constexpr std::uint32_t kChunksPerRead = 8;
  constexpr std::uint32_t kReadsPerChannel = 40;

  mru::ChannelTable t(1, kChannels);

  // Pre-build ids so the measurement covers observe(), not string formatting.
  std::vector<std::string> ids;
  ids.reserve(kReadsPerChannel);
  for (std::uint32_t r = 0; r < kReadsPerChannel; ++r) {
    std::string id = std::string(kUuidA);
    id[0] = static_cast<char>('a' + (r % 16));
    id[1] = static_cast<char>('a' + ((r / 16) % 16));
    ids.push_back(id);
  }

  const std::uint64_t t0 = mru::rdtscp();
  std::uint64_t ops = 0;
  for (std::uint32_t r = 0; r < kReadsPerChannel; ++r) {
    for (std::uint32_t c = 1; c <= kChannels; ++c) {
      for (std::uint32_t k = 0; k < kChunksPerRead; ++k) {
        const auto kind = t.observe(c, ids[r], 400ull * k, 400);
        if (kind == mru::ChunkKind::kUnknownChannel ||
            kind == mru::ChunkKind::kIdTooLong) {
          ++g_failures;
        }
        ++ops;
      }
    }
  }
  const double ns = mru::TscClock::instance().to_ns(mru::rdtscp() - t0);

  CHECK_EQ(t.reads_started(), static_cast<std::uint64_t>(kChannels) * kReadsPerChannel,
           "every read on every channel was detected exactly once");
  CHECK_EQ(t.unknown_channel(), 0u, "no routing errors");
  CHECK_EQ(t.id_too_long(), 0u, "no id overflows");

  const double per_op = ns / static_cast<double>(ops);
  std::printf("        %llu observe() calls, %.1f ns each\n",
              static_cast<unsigned long long>(ops), per_op);
  // A 0.4 s chunk cadence over 2675 channels is ~6.7k chunks/s. Even 1 us per
  // call would be ample; anything above that means a hidden allocation.
  CHECK(per_op < 1000.0, "observe() is unexpectedly slow -- hidden allocation?");
}

void test_chunk_ref() {
  banner("chunk_ref");
  CHECK_EQ(mru::bytes_per_sample(mru::RawDataType::kCalibrated), 4u,
           "calibrated is float32");
  CHECK_EQ(mru::bytes_per_sample(mru::RawDataType::kUncalibrated), 2u,
           "uncalibrated is int16");
  CHECK_EQ(mru::bytes_per_sample(mru::RawDataType::kNone), 0u, "NONE has no width");
  CHECK_EQ(mru::bytes_per_sample(mru::RawDataType::kKeepLast), 0u,
           "KEEP_LAST has no width");

  // Exercise the real arena handoff: build a descriptor over arena memory, then
  // release it and confirm the slot recycles.
  mru::BumpArenaPool pool(2, 4096);
  mru::BumpArenaPool::Slot* slot = pool.acquire();
  CHECK(slot != nullptr, "slot acquired");

  // 400 samples of uncalibrated signal == 800 bytes.
  constexpr std::uint64_t kSamples = 400;
  auto* sig = static_cast<std::byte*>(slot->arena().Allocate(kSamples * 2, 2));
  CHECK(sig != nullptr, "signal allocated in arena");
  auto* id = static_cast<char*>(slot->arena().Allocate(kUuidA.size(), 1));
  CHECK(id != nullptr, "id allocated in arena");
  std::memcpy(id, kUuidA.data(), kUuidA.size());

  mru::TestChunkRef ref{};
  ref.channel = 42;
  ref.read_id = id;
  ref.read_id_len = static_cast<std::uint16_t>(kUuidA.size());
  ref.chunk_start_sample = 8000;
  ref.chunk_length = static_cast<std::uint32_t>(kSamples);
  ref.raw_data = sig;
  ref.raw_data_len = static_cast<std::uint32_t>(kSamples * 2);
  ref.raw_data_type = mru::RawDataType::kUncalibrated;
  ref.owner = slot;
  ref.t_recv_tsc = mru::rdtscp();

  CHECK(ref.read_id_view() == kUuidA, "read id span round-trips");
  CHECK_EQ(ref.samples_in_payload(), kSamples, "int16 payload sample count");
  CHECK(ref.payload_matches_reported_length(), "payload agrees with chunk_length");
  CHECK_EQ(ref.chunk_end_sample(), 8400u, "end sample");

  // A calibrated chunk of the same byte length holds half as many samples, so
  // the reported length should no longer agree.
  mru::TestChunkRef cal = ref;
  cal.raw_data_type = mru::RawDataType::kCalibrated;
  CHECK_EQ(cal.samples_in_payload(), kSamples / 2, "float32 payload sample count");
  CHECK(!cal.payload_matches_reported_length(),
        "a truncated or mistyped payload is detectable");

  // The descriptor was copied by value, as the rings do; both share the owner.
  CHECK_EQ(pool.recycled(), 0u, "nothing recycled while a reference is held");
  ref.release();
  CHECK_EQ(pool.recycled(), 1u, "slot recycled after the last release");
  CHECK_EQ(pool.free_count_quiesced(), 2u, "both slots back in the free list");
}

}  // namespace

int main() {
  std::printf("read_state and chunk_ref unit tests\n\n");

  test_read_id();
  test_new_read_and_continuation();
  test_error_paths();
  test_decision_latch();
  test_bases_wasted();
  test_chunk_ref();
  test_observe_throughput();

  std::printf("\n%s  (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED",
              g_failures, g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
