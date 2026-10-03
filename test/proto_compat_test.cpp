// test/proto_compat_test.cpp
//
// Two jobs:
//
//  1. Force proto_compat.hpp to be compiled, so every field-number assertion in
//     it actually runs. Most of this file's value is at compile time.
//
//  2. Prove the zero-copy path for real, against genuine protobuf arenas: build a
//     GetLiveReadsResponse in an arena slot, construct ChunkRefs holding spans
//     into that arena's string storage, read the signal back through the spans,
//     then release the slot and confirm it recycles. This is the integration the
//     design rests on, and it needs no sequencer and no server to test.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/arena_pool.hpp"
#include "core/spsc_ring.hpp"
#include "transport/chunk_ref.hpp"
#include "transport/proto_compat.hpp"

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

using Req = minknow_api::data::GetLiveReadsRequest;
using Resp = minknow_api::data::GetLiveReadsResponse;

// Arena::CreateMessage was the 3.x spelling and is gone in 5.x; Arena::Create is
// the current one. Shim so the build does not depend on which protobuf the host
// distro ships.
template <class T>
[[nodiscard]] T* arena_new(google::protobuf::Arena* a) {
#if defined(GOOGLE_PROTOBUF_VERSION) && GOOGLE_PROTOBUF_VERSION >= 4022000
  return google::protobuf::Arena::Create<T>(a);
#else
  return google::protobuf::Arena::CreateMessage<T>(a);
#endif
}

constexpr std::string_view kUuid = "0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9";

void test_schema_is_pinned() {
  banner("schema_is_pinned");
  // The real assertions are static; reaching here means they all held. Print the
  // protobuf version so a CI log records what the guarantee was made against.
  std::printf("        protobuf %d, minknow_api pinned to 6.10.3\n",
              GOOGLE_PROTOBUF_VERSION);
  // Spot-check a couple at runtime too, so the numbers appear in the log.
  CHECK_EQ(Resp::kChannelsFieldNumber, 4u, "channels is field 4");
  CHECK_EQ(Req::Action::kIdFieldNumber, 3u, "Action.read.id is field 3");
  CHECK_EQ(Req::Actions::kActionsFieldNumber, 2u, "Actions.actions is field 2");
}

// Builds a response the way MinKNOW would, then walks it the way the reader
// thread does: no copies of the signal, only spans.
void test_zero_copy_spans() {
  banner("zero_copy_spans");
  constexpr std::uint32_t kChannels = 8;
  constexpr std::uint32_t kSamples = 400;  // 0.1 s at 4 kHz
  constexpr std::uint32_t kBytes = kSamples * 2;  // UNCALIBRATED == int16

  // 64 KiB initial block per slot; sized from measurement in the daemon.
  mru::PbArenaPool pool(4, std::size_t{64} * 1024);
  mru::PbArenaPool::Slot* slot = pool.acquire();
  CHECK(slot != nullptr, "slot acquired");
  if (slot == nullptr) return;

  auto* resp = arena_new<Resp>(slot->arena().get());
  CHECK(resp != nullptr, "response allocated in the arena");

  resp->set_samples_since_start(123456);
  resp->set_seconds_since_start(30.5);

  // Distinct signal per channel so a mixed-up span is detectable.
  for (std::uint32_t c = 1; c <= kChannels; ++c) {
    Resp::ReadData rd;
    rd.set_id(std::string(kUuid));
    rd.set_chunk_start_sample(1000ull * c);
    rd.set_chunk_length(kSamples);
    std::string sig(kBytes, static_cast<char>(c));
    rd.set_raw_data(std::move(sig));
    (*resp->mutable_channels())[c] = rd;
  }
  CHECK_EQ(resp->channels_size(), kChannels, "all channels present");

  // --- the reader-thread path ---------------------------------------------
  slot->retain(static_cast<std::uint32_t>(resp->channels_size()));

  mru::SpscRing<mru::ChunkRef, 64> ring;
  for (const auto& entry : resp->channels()) {
    const std::uint32_t channel = entry.first;
    const Resp::ReadData& rd = entry.second;

    mru::ChunkRef ref{};
    ref.channel = channel;
    ref.read_id = rd.id().data();
    ref.read_id_len = static_cast<std::uint16_t>(rd.id().size());
    ref.chunk_start_sample = rd.chunk_start_sample();
    ref.chunk_length = static_cast<std::uint32_t>(rd.chunk_length());
    ref.raw_data = reinterpret_cast<const std::byte*>(rd.raw_data().data());
    ref.raw_data_len = static_cast<std::uint32_t>(rd.raw_data().size());
    ref.raw_data_type = mru::RawDataType::kUncalibrated;
    ref.owner = slot;
    ref.t_recv_tsc = 0;

    CHECK(ring.try_push(ref), "descriptor queued");
  }
  slot->release();  // the reader drops its own reference

  // --- the worker path ----------------------------------------------------
  std::uint32_t seen = 0;
  mru::ChunkRef ref{};
  while (ring.try_pop(ref)) {
    ++seen;
    CHECK(ref.read_id_view() == kUuid, "read id span points at arena storage");
    CHECK_EQ(ref.samples_in_payload(), kSamples, "sample count from payload bytes");
    CHECK(ref.payload_matches_reported_length(), "payload agrees with chunk_length");
    CHECK_EQ(ref.chunk_start_sample, 1000ull * ref.channel, "per-channel position");

    // The signal must still be the bytes MinKNOW put there, read through the span
    // with no intervening copy.
    bool signal_intact = ref.raw_data_len == kBytes;
    for (std::uint32_t i = 0; i < ref.raw_data_len && signal_intact; ++i) {
      if (ref.raw_data[i] != static_cast<std::byte>(ref.channel)) signal_intact = false;
    }
    CHECK(signal_intact, "signal read back intact through the span");

    ref.release();
  }
  CHECK_EQ(seen, kChannels, "every descriptor consumed");
  CHECK_EQ(pool.recycled(), 1u, "slot recycled exactly once");
  CHECK_EQ(pool.free_count_quiesced(), 4u, "all slots back in the free list");
}

// The action path: one reused request message per writer tick, as the real writer
// does, so protobuf recycles the repeated field's storage instead of reallocating.
void test_action_batching() {
  banner("action_batching");
  Req req;
  auto* actions = req.mutable_actions();

  for (int tick = 0; tick < 3; ++tick) {
    actions->clear_actions();
    for (std::uint32_t c = 1; c <= 4; ++c) {
      auto* a = actions->add_actions();
      a->set_action_id("a" + std::to_string(tick * 10 + c));
      a->set_channel(c);
      a->set_id(std::string(kUuid));  // 6.x: id string only, no read number
      if (c % 2 == 0) {
        a->mutable_unblock()->set_duration(0.1);
      } else {
        a->mutable_stop_further_data();
      }
    }
    CHECK_EQ(actions->actions_size(), 4u, "four actions per tick");
  }

  // Serialise the way the stream would, and confirm it round-trips.
  std::string wire;
  CHECK(req.SerializeToString(&wire), "request serialises");
  Req parsed;
  CHECK(parsed.ParseFromString(wire), "request parses");
  CHECK_EQ(parsed.actions().actions_size(), 4u, "actions survive the round trip");
  const auto& a0 = parsed.actions().actions(0);
  CHECK(a0.id() == kUuid, "read id survives");
  CHECK(a0.has_stop_further_data(), "odd channel got stop_further_data");
  const auto& a1 = parsed.actions().actions(1);
  CHECK(a1.has_unblock(), "even channel got unblock");
  CHECK(a1.unblock().duration() > 0.0, "unblock duration set");
}

}  // namespace

int main() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  std::printf("transport proto compatibility and zero-copy tests\n\n");

  test_schema_is_pinned();
  test_zero_copy_spans();
  test_action_batching();

  std::printf("\n%s  (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED",
              g_failures, g_failures == 1 ? "" : "s");
  google::protobuf::ShutdownProtobufLibrary();
  return g_failures == 0 ? 0 : 1;
}
