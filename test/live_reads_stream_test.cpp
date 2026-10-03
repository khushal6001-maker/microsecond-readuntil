// test/live_reads_stream_test.cpp
//
// Compiling this is most of the point: it instantiates LiveReadsStream and every
// template it depends on, so the transport CI job type-checks the whole header.
//
// The runtime half covers only what can be checked without a MinKNOW server:
// config validation and the guards start() applies BEFORE it touches the network.
// Anything beyond that needs Icarust, and is a separate integration step rather
// than something to fake here.

#include <cstdio>
#include <optional>
#include <string>

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

void banner(const char* name) { std::printf("[ RUN ] %s\n", name); }

mru::LiveReadsStream::PolicyFn never_decide() {
  return [](const mru::ChunkRef&,
            const mru::ChannelState&) -> std::optional<mru::Decision> {
    return std::nullopt;
  };
}

void test_config_validation() {
  banner("config_validation");
  mru::StreamConfig c;
  CHECK(c.valid(), "defaults are valid");
  CHECK(c.channel_count() == 512, "default MinION channel count");

  c.last_channel = 2675;
  CHECK(c.channel_count() == 2675, "PromethION channel count");
  CHECK(c.valid(), "PromethION range is valid");

  mru::StreamConfig bad = c;
  bad.shard_count = 3;  // not a power of two: the shard mask would be wrong
  CHECK(!bad.valid(), "non-power-of-two shard count rejected");

  bad = c;
  bad.shard_count = 0;
  CHECK(!bad.valid(), "zero shards rejected");

  bad = c;
  bad.first_channel = 100;
  bad.last_channel = 99;
  CHECK(!bad.valid(), "inverted channel range rejected");

  bad = c;
  bad.arena_block_bytes = 0;
  CHECK(!bad.valid(), "zero arena block rejected");

  bad = c;
  bad.max_actions_per_message = 0;
  CHECK(!bad.valid(), "zero action batch rejected");
}

// start() must reject bad inputs before it opens a stream, so a misconfigured
// daemon fails loudly at launch rather than connecting and going quietly inert.
void test_start_guards_before_network() {
  banner("start_guards_before_network");

  {
    mru::StreamConfig c;
    c.shard_count = 6;  // invalid
    mru::LiveReadsStream s(c, nullptr, never_decide());
    CHECK(!s.start(), "invalid config refused");
    CHECK(s.last_error().find("invalid StreamConfig") != std::string::npos,
          "error names the config");
  }
  {
    mru::StreamConfig c;  // valid
    mru::LiveReadsStream s(c, nullptr, never_decide());
    CHECK(!s.start(), "null channel refused");
    CHECK(s.last_error().find("null grpc::Channel") != std::string::npos,
          "error names the channel");
  }
  {
    // A real channel object, but no policy. gRPC channels are lazy, so creating
    // one connects to nothing; start() must bail on the missing policy before any
    // RPC is attempted.
    mru::StreamConfig c;
    auto ch = grpc::CreateChannel("127.0.0.1:1", grpc::InsecureChannelCredentials());
    mru::LiveReadsStream s(c, ch, nullptr);
    CHECK(!s.start(), "missing policy refused");
    CHECK(s.last_error().find("no policy") != std::string::npos,
          "error names the policy");
  }
}

void test_decision_helpers() {
  banner("decision_helpers");
  constexpr std::string_view kUuid = "0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9";
  std::string id(kUuid);

  mru::ChunkRef c{};
  c.channel = 77;
  c.read_id = id.data();
  c.read_id_len = static_cast<std::uint16_t>(id.size());
  c.chunk_start_sample = 4000;
  c.chunk_length = 400;
  c.raw_data_type = mru::RawDataType::kUncalibrated;

  const mru::Decision rej = mru::Decision::reject(c, 42, 0.2f);
  CHECK(rej.unblock, "reject sets unblock");
  CHECK(rej.channel == 77, "channel carried");
  CHECK(rej.action_id == 42, "action id carried");
  CHECK(rej.read_id == kUuid, "read id copied into the inline buffer");
  CHECK(rej.unblock_seconds > 0.0f, "unblock duration set");
  CHECK(rej.t_decided_tsc != 0, "decision timestamped");

  const mru::Decision acc = mru::Decision::accept(c, 43);
  CHECK(!acc.unblock, "accept means stop_further_data, not unblock");
  CHECK(acc.read_id == kUuid, "read id copied");
}

void test_stats_describe() {
  banner("stats_describe");
  mru::StreamStats st;
  st.chunks.store(10);
  st.chunks_dropped.store(2);
  st.action_failed_read_too_long.store(3);
  const std::string s = st.describe();
  CHECK(s.find("10 chunks") != std::string::npos, "chunk count reported");
  CHECK(s.find("2 dropped") != std::string::npos, "drop count reported");
  // "TOO LATE" is deliberately shouty: it is the metric that says decision
  // latency cost yield, and it should be impossible to skim past in a log.
  CHECK(s.find("TOO LATE") != std::string::npos, "late-unblock count is prominent");
}

}  // namespace

int main() {
  std::printf("live_reads_stream tests (no server required)\n\n");

  test_config_validation();
  test_start_guards_before_network();
  test_decision_helpers();
  test_stats_describe();

  std::printf("\n%s  (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED",
              g_failures, g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
