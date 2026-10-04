// src/daemon/main.cpp
//
// Minimal read-until daemon: connect to a flow-cell position, stream live reads,
// and act on them.
//
// Scope, deliberately: this proves the TRANSPORT against a real MinKNOW-protocol
// server. The decision policy here is a placeholder that unblocks after a fixed
// number of chunks -- it does no mapping at all. Signal-space matching needs the
// index wired in, which is the next step and a separate concern from showing that
// the stream works.
//
// The specific thing being tested is a prediction from the schema audit in
// proto_compat.hpp. Our client is generated from minknow_api 6.10.3, where
// ReadData.number and Action.read.number are `reserved`. Icarust's vendored protos
// still carry them (fields 2 and 4, the pre-6.0 schema). Protobuf compatibility is
// by field number, so the prediction is:
//
//   * every field we read has the same number in both, so ingestion works
//   * Icarust's ReadData.number arrives as an unknown field and is skipped
//   * our Action.id (field 3) is also field 3 there, so unblocks land
//
// If that is wrong, this program receives nothing or unblocks nothing, and the audit
// was wrong in a way no amount of unit testing would have caught.

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

#include "core/affinity.hpp"
#include "transport/auth.hpp"
#include "transport/live_reads_stream.hpp"

namespace {

struct Args {
  std::string target = "127.0.0.1:10001";  // Icarust's position port; MinKNOW differs
  std::string ca_path;                     // overrides discovery
  std::string token_path;
  std::string ssl_name = "localhost";  // server cert CN rarely matches an IP literal
  bool insecure = false;
  std::uint32_t first_channel = 1;
  std::uint32_t last_channel = 512;
  unsigned shards = 4;
  int seconds = 30;
  std::uint32_t unblock_after_chunks = 2;
  bool pin = false;  // off by default: pinning a dev box is rarely what you want
};

void usage() {
  std::printf(
      "mru_daemon -- minimal read-until client\n\n"
      "  --target HOST:PORT      flow-cell position (default 127.0.0.1:10001)\n"
      "  --ca PATH               TLS root certificate (else auto-discovery)\n"
      "  --token PATH            local auth token file (optional)\n"
      "  --ssl-name NAME         expected server cert name (default localhost)\n"
      "  --insecure              no TLS at all, for a simulator that allows it\n"
      "  --first-channel N       default 1\n"
      "  --last-channel N        default 512\n"
      "  --shards N              power of two, default 4\n"
      "  --seconds N             how long to stream, default 30\n"
      "  --unblock-after N       placeholder policy: unblock after N chunks\n"
      "  --pin                   pin data-plane threads to cores\n");
}

[[nodiscard]] bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    const auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::printf("missing value for %s\n", what);
        return nullptr;
      }
      return argv[++i];
    };
    if (k == "--help" || k == "-h") {
      usage();
      return false;
    } else if (k == "--insecure") {
      a.insecure = true;
    } else if (k == "--pin") {
      a.pin = true;
    } else if (k == "--target") {
      const char* v = next("--target");
      if (v == nullptr) return false;
      a.target = v;
    } else if (k == "--ca") {
      const char* v = next("--ca");
      if (v == nullptr) return false;
      a.ca_path = v;
    } else if (k == "--token") {
      const char* v = next("--token");
      if (v == nullptr) return false;
      a.token_path = v;
    } else if (k == "--ssl-name") {
      const char* v = next("--ssl-name");
      if (v == nullptr) return false;
      a.ssl_name = v;
    } else if (k == "--first-channel") {
      const char* v = next("--first-channel");
      if (v == nullptr) return false;
      a.first_channel = static_cast<std::uint32_t>(std::atoi(v));
    } else if (k == "--last-channel") {
      const char* v = next("--last-channel");
      if (v == nullptr) return false;
      a.last_channel = static_cast<std::uint32_t>(std::atoi(v));
    } else if (k == "--shards") {
      const char* v = next("--shards");
      if (v == nullptr) return false;
      a.shards = static_cast<unsigned>(std::atoi(v));
    } else if (k == "--seconds") {
      const char* v = next("--seconds");
      if (v == nullptr) return false;
      a.seconds = std::atoi(v);
    } else if (k == "--unblock-after") {
      const char* v = next("--unblock-after");
      if (v == nullptr) return false;
      a.unblock_after_chunks = static_cast<std::uint32_t>(std::atoi(v));
    } else {
      std::printf("unknown option: %s\n\n", k.c_str());
      usage();
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) return 2;

  // --- credentials -----------------------------------------------------------
  mru::Credentials creds;
  if (!args.ca_path.empty() || !args.token_path.empty()) {
    creds = mru::credentials_from_paths(args.ca_path, args.token_path);
  } else if (!args.insecure) {
    creds = mru::discover_credentials();
  }
  if (!args.insecure) {
    std::printf("%s\n", creds.describe().c_str());
    if (!creds.have_ca()) {
      std::printf("No CA certificate. Pass --ca, set MRU_MINKNOW_CA, or use "
                  "--insecure if the server permits it.\n");
      return 1;
    }
  }

  // --- channel ---------------------------------------------------------------
  grpc::ChannelArguments ch_args;
  ch_args.SetInt(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH, 64 << 20);
  ch_args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 10'000);
  ch_args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);

  std::shared_ptr<grpc::Channel> channel;
  if (args.insecure) {
    channel = grpc::CreateCustomChannel(args.target, grpc::InsecureChannelCredentials(),
                                        ch_args);
  } else {
    // A server certificate almost never matches an IP literal, so the expected name
    // is overridden explicitly rather than by disabling verification.
    ch_args.SetSslTargetNameOverride(args.ssl_name);
    grpc::SslCredentialsOptions ssl;
    ssl.pem_root_certs = creds.ca_cert_pem;
    channel = grpc::CreateCustomChannel(args.target, grpc::SslCredentials(ssl), ch_args);
  }

  // --- config ---------------------------------------------------------------
  mru::StreamConfig cfg;
  cfg.target = args.target;
  cfg.first_channel = args.first_channel;
  cfg.last_channel = args.last_channel;
  cfg.shard_count = args.shards;
  cfg.raw_data_type = mru::RawDataType::kUncalibrated;
  cfg.sample_minimum_chunk_size = 1600;  // 0.4 s at 4 kHz
  cfg.arena_slots = 32;
  cfg.arena_block_bytes = 4u * 1024 * 1024;
  cfg.writer_tick = std::chrono::microseconds(500);
  cfg.cores.worker_count = args.shards;
  if (!args.pin) {
    // Park every role on core 0 so an unpinned run does not fight the scheduler;
    // IdleWait yields, so this stays correct, just not latency-optimal.
    cfg.cores.reader_core = 0;
    cfg.cores.writer_core = 0;
    cfg.cores.first_worker_core = 0;
  }
  if (!cfg.valid()) {
    std::printf("invalid configuration: shards must be a power of two and "
                "last_channel >= first_channel\n");
    return 1;
  }

  // --- placeholder policy ----------------------------------------------------
  // Unblocks every read once it has produced N chunks. This does NO mapping: its
  // only job is to prove that actions reach the server and are accepted. Replace
  // with signal-space matching plus diagonal voting once the index is wired in.
  std::atomic<std::uint64_t> policy_calls{0};
  const std::uint32_t after = args.unblock_after_chunks;
  auto policy = [&policy_calls, after](const mru::ChunkRef& c,
                                       const mru::ChannelState& s)
      -> std::optional<mru::Decision> {
    policy_calls.fetch_add(1, std::memory_order_relaxed);
    if (s.chunks_seen < after) return std::nullopt;
    return mru::Decision::reject(c, 0, 0.1);
  };

  std::printf("connecting to %s (%s), channels %u-%u, %u shards\n", args.target.c_str(),
              args.insecure ? "insecure" : "TLS", cfg.first_channel, cfg.last_channel,
              cfg.shard_count);

  mru::LiveReadsStream stream(cfg, channel, policy, creds);
  if (!stream.start()) {
    std::printf("failed to start: %s\n", stream.last_error().c_str());
    return 1;
  }
  std::printf("streaming for %d s...\n\n", args.seconds);

  // Progress every second, so a stream that produces nothing is obvious immediately
  // rather than after the full duration.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(args.seconds);
  std::uint64_t last_chunks = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto& st = stream.stats();
    const auto chunks = st.chunks.load(std::memory_order_relaxed);
    std::printf("  resp %6llu  chunks %8llu (+%llu/s)  reads %6llu  actions %6llu  "
                "ok %6llu  late %5llu  dropped %llu\n",
                static_cast<unsigned long long>(st.responses.load()),
                static_cast<unsigned long long>(chunks),
                static_cast<unsigned long long>(chunks - last_chunks),
                static_cast<unsigned long long>(st.reads_started.load()),
                static_cast<unsigned long long>(st.actions_sent.load()),
                static_cast<unsigned long long>(st.action_success.load()),
                static_cast<unsigned long long>(st.action_failed_read_too_long.load()),
                static_cast<unsigned long long>(st.chunks_dropped.load()));
    std::fflush(stdout);
    last_chunks = chunks;
  }

  stream.stop();

  std::printf("\n%s", stream.stats().describe().c_str());
  std::printf("policy calls: %llu\n",
              static_cast<unsigned long long>(policy_calls.load()));
  if (!stream.last_error().empty()) {
    std::printf("stream ended with: %s\n", stream.last_error().c_str());
  }

  const auto* pool = stream.arena_pool();
  if (pool != nullptr) {
    std::printf("arena slots returned: %zu/%zu\n", pool->free_count_quiesced(),
                pool->slot_count());
  }

  const bool got_data = stream.stats().chunks.load() > 0;
  const bool acted = stream.stats().actions_sent.load() > 0;
  std::printf("\nVERDICT: ingestion %s, actions %s\n", got_data ? "WORKED" : "FAILED",
              acted ? "SENT" : "NONE");
  return got_data ? 0 : 1;
}
