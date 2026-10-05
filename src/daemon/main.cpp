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

#include <fstream>

#include "core/affinity.hpp"
#include "core/hdr_latency.hpp"
#include "core/tsc.hpp"
#include "daemon/policy.hpp"
#include "index/minimizer_index.hpp"
#include "index/quantise.hpp"
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
  std::uint32_t unblock_after_chunks = 2;  // placeholder policy only

  // Index: with both of these the daemon runs real signal-space matching. Without them
  // it falls back to the placeholder policy, which is useful for isolating transport
  // problems from decision problems.
  std::string model_path;      // ONT k-mer level table, <kmer>\t<level_pA>
  std::string reference_path;  // target reference FASTA
  // 0 means "keep the frozen default from PolicyConfig". These used to carry their own
  // copies of the defaults and assign them unconditionally, which meant PolicyConfig's
  // frozen accept_votes was silently overridden by a stale 5 here -- the daemon ran a
  // threshold that no measurement supported, accepting 53.6% of reads from a shuffled
  // control while the frozen value had been re-measured to 8. A frozen parameter must
  // have exactly one home.
  std::uint32_t accept_votes = 0;
  std::uint32_t max_chunks = 0;
  int probe_budget = 0;
  bool pin = false;  // off by default: pinning a dev box is rarely what you want
  int detect = -1;           // -1 = keep the frozen default, 0 = off, 1 = on
  std::uint32_t min_window = 0;  // 0 = keep the frozen default
  std::uint32_t bits = 0;    // 0 = keep the frozen default
  std::uint32_t events = 0;  // 0 = keep the frozen default
  std::string latency_out;  // write the full percentile distribution here
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
      "  --model PATH            ONT k-mer level table (enables real matching)\n"
      "  --reference PATH        target reference FASTA (enables real matching)\n"
      "  --accept-votes N        diagonal votes to call on-target, default 8\n"
      "  --max-chunks N          defer up to N chunks before unblocking, default 10\n"
      "  --bits N                bits per quantised event (default 3)\n"
      "  --events N              events per key (default 14)\n"
      "  --min-window N          minimizer window; 1 selects every key as a seed\n"
      "  --detect                segment the query by detected level changes rather\n"
      "                          than fixed time slices (needed for variable dwell)\n"
      "  --probes N              keys probed per seed (1, 2 or 4), default 4\n"
      "  --pin                   pin data-plane threads to cores\n"
      "  --latency-out PATH      write the full latency distribution for plotting\n");
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
    } else if (k == "--latency-out") {
      const char* v = next("--latency-out");
      if (v == nullptr) return false;
      a.latency_out = v;
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
    } else if (k == "--model") {
      const char* v = next("--model");
      if (v == nullptr) return false;
      a.model_path = v;
    } else if (k == "--reference") {
      const char* v = next("--reference");
      if (v == nullptr) return false;
      a.reference_path = v;
    } else if (k == "--accept-votes") {
      const char* v = next("--accept-votes");
      if (v == nullptr) return false;
      a.accept_votes = static_cast<std::uint32_t>(std::atoi(v));
    } else if (k == "--max-chunks") {
      const char* v = next("--max-chunks");
      if (v == nullptr) return false;
      a.max_chunks = static_cast<std::uint32_t>(std::atoi(v));
    } else if (k == "--bits") {
      const char* v = next("--bits");
      if (v == nullptr) return false;
      a.bits = static_cast<std::uint32_t>(std::atoi(v));
    } else if (k == "--events") {
      const char* v = next("--events");
      if (v == nullptr) return false;
      a.events = static_cast<std::uint32_t>(std::atoi(v));
    } else if (k == "--min-window") {
      const char* v = next("--min-window");
      if (v == nullptr) return false;
      a.min_window = static_cast<std::uint32_t>(std::atoi(v));
    } else if (k == "--detect") {
      a.detect = 1;
    } else if (k == "--no-detect") {
      a.detect = 0;
    } else if (k == "--probes") {
      const char* v = next("--probes");
      if (v == nullptr) return false;
      a.probe_budget = std::atoi(v);
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

// ONT level table: "<kmer>\t<level_pA>" per line, 4^k lines in lexicographic ACGT
// order, which is the 2-bit packing order PoreModel::load_levels expects.
bool load_model_tsv(const std::string& path, std::vector<float>& levels) {
  std::ifstream f(path);
  if (!f) return false;
  levels.clear();
  levels.reserve(262144);
  std::string line;
  while (std::getline(f, line)) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    levels.push_back(std::strtof(line.c_str() + tab + 1, nullptr));
  }
  std::size_t p = 1;
  while (p < levels.size()) p *= 4;
  return !levels.empty() && p == levels.size();
}

bool load_fasta_acgt(const std::string& path, std::string& out) {
  std::ifstream f(path);
  if (!f) return false;
  out.clear();
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line[0] == '>') continue;
    for (char c : line) {
      switch (c) {
        case 'A': case 'a': case 'C': case 'c':
        case 'G': case 'g': case 'T': case 't':
          out.push_back(c);
          break;
        default:
          break;  // N runs and anything else contribute no k-mer
      }
    }
  }
  return !out.empty();
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
  // Pin only when asked, and then to distinct cores. The previous version expressed
  // "do not pin" by pointing every role at core 0, which, because the stream pinned
  // unconditionally, crowded six threads onto one core and inflated the decision-path
  // p99 to sixteen times the median.
  cfg.pin_threads = args.pin;
  if (args.pin) {
    // Distinct PHYSICAL cores, not consecutive CPU numbers. On this machine's SMT
    // layout cpu0 and cpu1 are one core, so the previous contiguous plan put the
    // reader and writer on shared execution units and left a core entirely idle.
    const mru::CoreAssignment a = mru::assign_distinct_cores(cfg.shard_count);
    cfg.cores = a.plan;
    std::printf("pinning:   %s\n", a.plan.describe().c_str());
    std::printf("  layout:  %s\n", a.note.c_str());
    if (a.shared_siblings) {
      std::printf("  WARNING: threads share physical cores, so this run's tail cannot be\n"
                  "  attributed to the daemon rather than to SMT contention.\n");
    }
  }
  if (!cfg.valid()) {
    std::printf("invalid configuration: shards must be a power of two and "
                "last_channel >= first_channel\n");
    return 1;
  }

  // --- index, if a model and reference were given ----------------------------
  const bool use_index = !args.model_path.empty() && !args.reference_path.empty();
  mru::MinimizerIndex index;
  mru::PolicyConfig pcfg;
  // Geometry overrides, for sweeping the key-entropy question from the command line
  // instead of a rebuild. 0 keeps the frozen default.
  if (args.bits != 0) pcfg.quant.bits_per_event = args.bits;
  if (args.events != 0) pcfg.quant.events_per_key = args.events;
  if (args.detect >= 0) pcfg.quant.event_detection = (args.detect != 0);
  if (args.min_window != 0) pcfg.quant.minimizer_window = args.min_window;
  mru::PolicyStats pstats;
  std::unique_ptr<mru::SignalPolicy> signal_policy;

  if (use_index) {
    std::vector<float> levels;
    if (!load_model_tsv(args.model_path, levels)) {
      std::printf("cannot load level table %s (expects 4^k lines of "
                  "<kmer>TAB<level_pA>)\n", args.model_path.c_str());
      return 1;
    }
    mru::PoreModel model;
    if (!model.load_levels(levels)) {
      std::printf("level table has %zu entries, which is not a power of four\n",
                  levels.size());
      return 1;
    }
    std::string dna;
    if (!load_fasta_acgt(args.reference_path, dna)) {
      std::printf("cannot load reference %s\n", args.reference_path.c_str());
      return 1;
    }

    pcfg.first_channel = cfg.first_channel;
    pcfg.last_channel = cfg.last_channel;
    if (args.accept_votes != 0) pcfg.accept_votes = args.accept_votes;
    if (args.max_chunks != 0) pcfg.max_chunks = args.max_chunks;
    if (args.probe_budget != 0) pcfg.probe_budget = args.probe_budget;

    std::printf("index: %u-mer model, %zu reference bases, geometry %u x %u = %u-bit "
                "keys, window %u\n",
                model.k(), dna.size(), pcfg.quant.bits_per_event,
                pcfg.quant.events_per_key, pcfg.quant.key_bits(),
                pcfg.quant.minimizer_window);

    std::vector<mru::QEvent> ref_events;
    if (!model.reference_to_events(dna, pcfg.quant, ref_events)) {
      std::printf("reference_to_events failed\n");
      return 1;
    }
    std::vector<mru::SeedHash> all_seeds, minimizers;
    mru::hash_all_keys(ref_events, pcfg.quant, all_seeds);
    mru::select_minimizers(all_seeds, pcfg.quant.minimizer_window, minimizers);
    index.build(minimizers);

    std::printf("index: %zu events, %zu minimizers of %zu keys, %llu distinct, "
                "%llu capped, %llu insert failures, %.1f MiB table + %.1f MiB positions\n",
                ref_events.size(), minimizers.size(), all_seeds.size(),
                static_cast<unsigned long long>(index.distinct_keys()),
                static_cast<unsigned long long>(index.capped_seeds()),
                static_cast<unsigned long long>(index.insert_failures()),
                static_cast<double>(index.capacity() * 8) / (1024.0 * 1024.0),
                static_cast<double>(index.position_count() * 4) / (1024.0 * 1024.0));
    std::printf("rule: accept at >=%u votes, unblock after %u chunks without them, "
                "%d probes/seed\n",
                pcfg.accept_votes, pcfg.max_chunks, pcfg.probe_budget);
    signal_policy = std::make_unique<mru::SignalPolicy>(index, pcfg, pstats);
  } else {
    std::printf("NO INDEX: --model and --reference not both given, so the placeholder\n"
                "policy runs instead. It unblocks after %u chunks and does NO mapping;\n"
                "it exists to isolate transport problems from decision problems.\n",
                args.unblock_after_chunks);
  }

  // Decision-path latency, for comparison against other adaptive-sampling clients.
  //
  // One histogram per shard, indexed by channel. Chunks are sharded by channel, so each
  // histogram is touched by exactly one worker and needs no synchronisation -- the same
  // argument that makes the per-channel policy state safe.
  //
  // This times the POLICY ONLY: quantise, probe, vote, decide. It excludes gRPC receive
  // and protobuf parse, which is deliberate, because it is the number comparable to what
  // a basecall-free orchestration loop reports for its own per-read work.
  std::vector<mru::LatencyRecorder> policy_hist;
  policy_hist.reserve(cfg.shard_count);
  for (unsigned i = 0; i < cfg.shard_count; ++i) policy_hist.emplace_back();
  const unsigned shard_mask = cfg.shard_count - 1;

  std::atomic<std::uint64_t> policy_calls{0};
  const std::uint32_t after = args.unblock_after_chunks;
  mru::LiveReadsStream::PolicyFn policy;
  if (signal_policy) {
    policy = [&policy_calls, &signal_policy, &policy_hist, shard_mask](
                 const mru::ChunkRef& c, const mru::ChannelState& s)
        -> std::optional<mru::Decision> {
      policy_calls.fetch_add(1, std::memory_order_relaxed);
      const std::uint64_t t0 = mru::rdtscp();
      auto d = (*signal_policy)(c, s);
      policy_hist[c.channel & shard_mask].record(mru::rdtscp() - t0);
      return d;
    };
  } else {
    policy = [&policy_calls, after, &policy_hist, shard_mask](
                 const mru::ChunkRef& c, const mru::ChannelState& s)
        -> std::optional<mru::Decision> {
      policy_calls.fetch_add(1, std::memory_order_relaxed);
      const std::uint64_t t0 = mru::rdtscp();
      std::optional<mru::Decision> d;
      if (s.chunks_seen >= after) d = mru::Decision::reject(c, 0, 0.1);
      policy_hist[c.channel & shard_mask].record(mru::rdtscp() - t0);
      return d;
    };
  }

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
  if (use_index) std::printf("%s", pstats.describe().c_str());

  mru::LatencyRecorder merged;
  for (const auto& h : policy_hist) merged.merge(h);
  if (merged.count() > 0) {
    const auto& clk = mru::TscClock::instance();
    std::printf("decision path (policy only): %s\n",
                mru::format_latency_us(merged, clk).c_str());
    std::printf("  histogram: %s\n", mru::LatencyRecorder::backend());
    if (merged.out_of_range() > 0) {
      std::printf("  WARNING: %llu samples exceeded the histogram ceiling and are NOT in\n"
                  "  the percentiles above -- the real tail is worse than reported.\n",
                  static_cast<unsigned long long>(merged.out_of_range()));
    }
    if (!clk.invariant()) {
      std::printf("  WARNING: TSC is not invariant on this host, so these are indicative\n"
                  "  only. Publication numbers need an invariant TSC.\n");
    }
    if (!args.latency_out.empty()) {
      std::FILE* f = std::fopen(args.latency_out.c_str(), "w");
      if (f != nullptr) {
        // Scale cycles to microseconds so the dumped distribution is in real units.
        const bool ok = merged.print_distribution(f, clk.cycles_per_ns() * 1000.0);
        std::fclose(f);
        std::printf("  distribution %s to %s\n", ok ? "written" : "NOT written (fallback "
                    "histogram cannot produce one)", args.latency_out.c_str());
      } else {
        std::printf("  could not open %s for writing\n", args.latency_out.c_str());
      }
    }
  }
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
