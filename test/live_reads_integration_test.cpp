// test/live_reads_integration_test.cpp
//
// End-to-end test of LiveReadsStream against a mock MinKNOW DataService, over
// gRPC's in-process channel. No sequencer, no Icarust, no TCP port, no TLS, and
// fully deterministic -- so it runs in CI on every push and a reviewer can run it
// too, which is the point for reproducibility.
//
// What this actually proves, which the compile-only test could not:
//   * StreamSetup is sent first and carries the configured channel range
//   * responses are parsed, sharded by channel, and reach the workers
//   * the read-id string path works end to end (6.x has no read numbers)
//   * decisions become real Action messages with the right channel and read id,
//     and the server receives them
//   * the arena refcount survives real traffic: every slot comes back
//
// What it does NOT prove: anything about real MinKNOW's behaviour, timing on real
// hardware, or TLS/auth. Those need Icarust or a sequencer.

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "transport/live_reads_stream.hpp"

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

using Req = minknow_api::data::GetLiveReadsRequest;
using Resp = minknow_api::data::GetLiveReadsResponse;

constexpr std::uint32_t kChannels = 32;
constexpr std::uint32_t kReadsPerChannel = 4;
constexpr std::uint32_t kChunksPerRead = 3;
constexpr std::uint32_t kSamplesPerChunk = 400;  // 0.1 s at 4 kHz

// Deterministic 36-character read id, so a mixed-up id is detectable.
std::string make_read_id(std::uint32_t channel, std::uint32_t read_index) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%08x-%04x-4000-8000-%012x", channel, read_index,
                channel * 1000u + read_index);
  return std::string(buf);
}

struct ReceivedAction {
  std::string action_id;
  std::string read_id;
  std::uint32_t channel;
  bool unblock;
};

class MockDataService final : public minknow_api::data::DataService::Service {
 public:
  grpc::Status get_live_reads(grpc::ServerContext* /*ctx*/,
                             grpc::ServerReaderWriter<Resp, Req>* stream) override {
    // 1. The first message must be StreamSetup. If it is not, the client is
    //    broken in a way worth failing loudly for.
    Req first;
    if (!stream->Read(&first)) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "no setup message");
    }
    if (!first.has_setup()) {
      saw_setup_first_ = false;
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "first message was not setup");
    }
    saw_setup_first_ = true;
    setup_first_channel_ = first.setup().first_channel();
    setup_last_channel_ = first.setup().last_channel();
    setup_chunk_size_ = first.setup().sample_minimum_chunk_size();
    setup_raw_type_ = static_cast<int>(first.setup().raw_data_type());

    // 2. Write every response, then drain actions. Writing first keeps this
    //    single-threaded: ServerReaderWriter::Read blocks, and interleaving would
    //    need a second thread for no gain at this volume.
    for (std::uint32_t r = 0; r < kReadsPerChannel; ++r) {
      for (std::uint32_t k = 0; k < kChunksPerRead; ++k) {
        Resp resp;
        resp.set_samples_since_start(static_cast<std::uint64_t>(r) * 10000 + k * 400);
        resp.set_seconds_since_start(0.1 * static_cast<double>(r * kChunksPerRead + k));

        for (std::uint32_t c = 1; c <= kChannels; ++c) {
          Resp::ReadData rd;
          rd.set_id(make_read_id(c, r));
          rd.set_start_sample(static_cast<std::uint64_t>(r) * 10000);
          rd.set_chunk_start_sample(static_cast<std::uint64_t>(r) * 10000 +
                                    static_cast<std::uint64_t>(k) * kSamplesPerChunk);
          rd.set_chunk_length(kSamplesPerChunk);
          // UNCALIBRATED: int16 per sample. Byte value encodes the channel so a
          // crossed span shows up as corrupt signal.
          rd.set_raw_data(std::string(kSamplesPerChunk * 2, static_cast<char>(c)));
          (*resp.mutable_channels())[c] = rd;
        }
        if (!stream->Write(resp)) {
          return grpc::Status::OK;  // client went away; not an error for this test
        }
        ++responses_written_;
      }
    }

    // 3. Drain actions until the client half-closes.
    Req in;
    while (stream->Read(&in)) {
      if (!in.has_actions()) continue;
      std::lock_guard<std::mutex> lock(mu_);
      for (const auto& a : in.actions().actions()) {
        actions_.push_back(ReceivedAction{a.action_id(), a.id(), a.channel(),
                                         a.has_unblock()});
      }
    }
    return grpc::Status::OK;
  }

  [[nodiscard]] std::vector<ReceivedAction> actions() const {
    std::lock_guard<std::mutex> lock(mu_);
    return actions_;
  }
  [[nodiscard]] bool saw_setup_first() const noexcept { return saw_setup_first_; }
  [[nodiscard]] std::uint32_t setup_first_channel() const noexcept {
    return setup_first_channel_;
  }
  [[nodiscard]] std::uint32_t setup_last_channel() const noexcept {
    return setup_last_channel_;
  }
  [[nodiscard]] std::uint64_t setup_chunk_size() const noexcept {
    return setup_chunk_size_;
  }
  [[nodiscard]] int setup_raw_type() const noexcept { return setup_raw_type_; }
  [[nodiscard]] std::uint64_t responses_written() const noexcept {
    return responses_written_;
  }

 private:
  mutable std::mutex mu_;
  std::vector<ReceivedAction> actions_;
  std::atomic<bool> saw_setup_first_{false};
  std::atomic<std::uint32_t> setup_first_channel_{0};
  std::atomic<std::uint32_t> setup_last_channel_{0};
  std::atomic<std::uint64_t> setup_chunk_size_{0};
  std::atomic<int> setup_raw_type_{-1};
  std::atomic<std::uint64_t> responses_written_{0};
};

void test_end_to_end() {
  std::printf("[ RUN ] end_to_end_against_mock_minknow\n");

  MockDataService service;
  grpc::ServerBuilder builder;
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  CHECK(server != nullptr, "mock server started");
  if (server == nullptr) return;

  grpc::ChannelArguments args;
  auto channel = server->InProcessChannel(args);

  mru::StreamConfig cfg;
  cfg.first_channel = 1;
  cfg.last_channel = kChannels;
  cfg.shard_count = 2;
  cfg.raw_data_type = mru::RawDataType::kUncalibrated;
  cfg.sample_minimum_chunk_size = kSamplesPerChunk;
  cfg.arena_slots = 16;
  cfg.arena_block_bytes = 1u * 1024 * 1024;
  cfg.writer_tick = std::chrono::microseconds(200);
  // 2 shards means 4 data-plane threads (reader, writer, 2 workers), which fits a
  // 4-vCPU CI runner. Pinning is still attempted and fails harmlessly on a
  // restricted cpuset; IdleWait is what keeps progress safe if it does.
  cfg.cores.worker_count = cfg.shard_count;

  // Reject on the second chunk of every read, so continuation handling runs
  // before a decision is made.
  std::atomic<std::uint64_t> policy_calls{0};
  auto policy = [&policy_calls](const mru::ChunkRef& c, const mru::ChannelState& s)
      -> std::optional<mru::Decision> {
    policy_calls.fetch_add(1, std::memory_order_relaxed);
    if (s.chunks_seen < 2) return std::nullopt;
    return mru::Decision::reject(c, 0, 0.1f);
  };

  mru::LiveReadsStream stream(cfg, channel, policy);
  CHECK(stream.start(), "stream started");

  // Wait for the expected number of reads, with a hard ceiling so a hang fails
  // the test instead of hanging CI.
  constexpr std::uint64_t kExpectedReads =
      static_cast<std::uint64_t>(kChannels) * kReadsPerChannel;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto decided = stream.stats().decisions.load(std::memory_order_relaxed);
    if (decided >= kExpectedReads) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Give the writer a few ticks to flush the last batch.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  stream.stop();
  server->Shutdown();
  server->Wait();

  const auto& st = stream.stats();
  const auto g = [](const std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
  };

  std::printf("        %s", st.describe().c_str());
  std::printf("        policy calls: %llu, server responses: %llu\n",
              static_cast<unsigned long long>(policy_calls.load()),
              static_cast<unsigned long long>(service.responses_written()));

  // --- setup handshake ---
  CHECK(service.saw_setup_first(), "StreamSetup was the first message on the stream");
  CHECK_EQ(service.setup_first_channel(), 1u, "first_channel sent");
  CHECK_EQ(service.setup_last_channel(), kChannels, "last_channel sent");
  CHECK_EQ(service.setup_chunk_size(), kSamplesPerChunk, "chunk size sent");
  CHECK_EQ(service.setup_raw_type(), static_cast<int>(mru::RawDataType::kUncalibrated),
           "raw data type sent as UNCALIBRATED");

  // --- ingestion ---
  const std::uint64_t expected_chunks =
      static_cast<std::uint64_t>(kChannels) * kReadsPerChannel * kChunksPerRead;
  CHECK_EQ(g(st.chunks), expected_chunks, "every chunk reached a worker");
  CHECK_EQ(g(st.chunks_dropped), 0u, "no chunks dropped");
  CHECK_EQ(g(st.arena_exhausted), 0u, "arena pool never exhausted");
  CHECK_EQ(g(st.payload_mismatch), 0u, "payload length always matched chunk_length");
  CHECK_EQ(g(st.reads_started), kExpectedReads, "every read detected exactly once");

  // --- decisions and actions ---
  CHECK_EQ(g(st.decisions), kExpectedReads, "one decision per read");
  CHECK_EQ(g(st.decisions_dropped), 0u, "no decisions dropped");

  const auto received = service.actions();
  CHECK_EQ(received.size(), g(st.actions_sent),
           "the server received exactly the actions we counted as sent");
  CHECK(!received.empty(), "the server received actions at all");

  // Every action must name a channel in range, carry a well-formed read id that
  // matches what the server sent for that channel, and be an unblock.
  bool all_valid = true;
  for (const auto& a : received) {
    if (a.channel < 1 || a.channel > kChannels) { all_valid = false; break; }
    if (!a.unblock) { all_valid = false; break; }
    if (a.read_id.size() != 36) { all_valid = false; break; }
    if (a.action_id.empty()) { all_valid = false; break; }
    bool id_known = false;
    for (std::uint32_t r = 0; r < kReadsPerChannel; ++r) {
      if (a.read_id == make_read_id(a.channel, r)) { id_known = true; break; }
    }
    if (!id_known) { all_valid = false; break; }
  }
  CHECK(all_valid, "every action carries an in-range channel and a matching read id");

  // --- the arena invariant, under real traffic ---
  const auto* pool = stream.arena_pool();
  CHECK(pool != nullptr, "arena pool exists after start");
  if (pool != nullptr) {
    CHECK_EQ(pool->free_count_quiesced(), cfg.arena_slots,
             "every arena slot returned: no leak under real traffic");
  }
}

}  // namespace

int main() {
  std::printf("live_reads_stream integration test (mock MinKNOW, in-process gRPC)\n\n");
  test_end_to_end();
  std::printf("\n%s  (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED",
              g_failures, g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
