// src/transport/live_reads_stream.hpp
//
// The bidirectional get_live_reads stream, wired into the verified data plane.
//
// Threading
// ---------
//     reader thread            one per stream, pinned
//       Read() -> arena slot -> ChunkRef per channel -> shard ring
//       also drains action_responses (MinKNOW's verdict on our unblocks)
//
//     worker threads           one per shard, pinned, own disjoint channels
//       drain shard ring -> ChannelTable.observe -> policy -> action ring
//
//     writer thread            one, pinned, SOLE owner of Write()
//       drain every worker's action ring -> one Actions message per tick
//
// Two deliberate choices worth stating:
//
//  1. Actions travel on ONE SPSC RING PER WORKER, drained round-robin by the
//     writer, rather than through a shared MPSC queue. The SPSC ring is already
//     proven race-free under ThreadSanitizer; an MPSC queue would be a new
//     concurrency primitive and a new race surface for no benefit. Reusing the
//     verified thing is worth a little round-robin bookkeeping.
//
//  2. gRPC's ClientReaderWriter permits ONE concurrent reader and ONE concurrent
//     writer, never two writers. That is why workers never call Write(); they
//     hand decisions to the single writer thread.
//
// Backpressure is always DROP, never block. Blocking the reader stalls every
// channel, and a stale signal chunk is worthless for read-until. Every drop is
// counted; a rising drop count is the signal that workers cannot keep up.
#pragma once

#if !defined(MRU_WITH_PROTOBUF)
#error "live_reads_stream.hpp requires the generated minknow_api headers (-DMRU_WITH_TRANSPORT=ON)"
#endif

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/affinity.hpp"
#include "core/arena_pool.hpp"
#include "core/cacheline.hpp"
#include "core/spsc_ring.hpp"
#include "core/tsc.hpp"
#include "decide/read_state.hpp"
#include "minknow_api/data.grpc.pb.h"
#include "minknow_api/data.pb.h"
#include "transport/auth.hpp"
#include "transport/chunk_ref.hpp"
#include "transport/proto_compat.hpp"

namespace mru {

// Idle wait for the data-plane threads.
//
// Spinning is correct when the thread sits on a dedicated isolated core: whatever
// it waits on is running elsewhere, and a pause loop reacts fastest. It is wrong
// when threads outnumber cores -- the waiter burns its whole timeslice while the
// thread it needs is descheduled. That is not hypothetical here: it is exactly
// how this project's first CI run failed, with four jobs hitting a 900 s timeout
// because 5 spinning threads shared 4 vCPUs.
//
// Adaptive serves both. On an isolated core under load the spin budget is rarely
// exhausted, so the fast path is unchanged; when oversubscribed, one yield
// unblocks the thread we are waiting for.
class IdleWait {
 public:
  void reset() noexcept { spins_ = 0; }

  void operator()() noexcept {
    if (++spins_ < kSpinBudget) {
      cpu_relax();
      return;
    }
    spins_ = 0;
    std::this_thread::yield();
  }

 private:
  static constexpr unsigned kSpinBudget = 2048;
  unsigned spins_ = 0;
};

// What a worker hands the writer. Trivially copyable so it rides an SpscRing.
struct Decision {
  std::uint32_t channel;
  bool unblock;  // false => stop_further_data
  std::uint8_t _pad[3];
  float unblock_seconds;
  std::uint64_t action_id;
  std::uint64_t t_decided_tsc;
  ReadId read_id;  // 6.x removed read numbers; the id string is all we have

  [[nodiscard]] static Decision reject(const ChunkRef& c, std::uint64_t id,
                                       float seconds = 0.1f) noexcept {
    Decision d{};
    d.channel = c.channel;
    d.unblock = true;
    d.unblock_seconds = seconds;
    d.action_id = id;
    d.t_decided_tsc = rdtscp();
    (void)d.read_id.assign(c.read_id_view());
    return d;
  }
  [[nodiscard]] static Decision accept(const ChunkRef& c, std::uint64_t id) noexcept {
    Decision d{};
    d.channel = c.channel;
    d.unblock = false;  // stop_further_data: keep sequencing, stop sending signal
    d.action_id = id;
    d.t_decided_tsc = rdtscp();
    (void)d.read_id.assign(c.read_id_view());
    return d;
  }
};

static_assert(std::is_trivially_copyable_v<Decision>);

struct StreamConfig {
  std::string target = "127.0.0.1:8000";  // host:port of the flow-cell position
  std::uint32_t first_channel = 1;
  std::uint32_t last_channel = 512;

  // 0.4 s of signal at 4 kHz. Below about this, MinKNOW sends more, smaller
  // chunks and the per-chunk overhead rises without buying earlier decisions.
  std::uint64_t sample_minimum_chunk_size = 1600;

  // UNCALIBRATED is int16, CALIBRATED is float32. Uncalibrated halves the bytes
  // on the wire and the parse cost; apply the scale/offset in the worker, which
  // has cycles to spare.
  RawDataType raw_data_type = RawDataType::kUncalibrated;

  unsigned shard_count = 4;  // power of two
  CorePlan cores{};

  std::size_t arena_slots = 32;
  std::size_t arena_block_bytes = 4u * 1024 * 1024;  // size from measurement

  // How often the writer flushes a batch. Adds at most this much latency to an
  // action, which is negligible against a ~400 ms chunk cadence; batching keeps
  // the number of gRPC messages down. 0 means spin instead of sleeping.
  std::chrono::microseconds writer_tick{500};
  std::size_t max_actions_per_message = 512;

  [[nodiscard]] bool valid() const noexcept {
    return shard_count > 0 && (shard_count & (shard_count - 1)) == 0 &&
           last_channel >= first_channel && arena_slots > 0 &&
           arena_block_bytes > 0 && max_actions_per_message > 0;
  }
  [[nodiscard]] std::uint32_t channel_count() const noexcept {
    return last_channel - first_channel + 1;
  }
};

struct StreamStats {
  // Ingestion
  std::atomic<std::uint64_t> responses{0};
  std::atomic<std::uint64_t> chunks{0};
  std::atomic<std::uint64_t> chunks_dropped{0};   // shard ring full
  std::atomic<std::uint64_t> arena_exhausted{0};  // no free slot
  std::atomic<std::uint64_t> payload_mismatch{0}; // chunk_length vs bytes disagree

  // Decisions
  std::atomic<std::uint64_t> reads_started{0};
  std::atomic<std::uint64_t> decisions{0};
  std::atomic<std::uint64_t> decisions_dropped{0};  // action ring full

  // Actions, and MinKNOW's verdict on them. The two failure counters are the
  // best latency evidence available: the server itself tells us we were late.
  std::atomic<std::uint64_t> actions_sent{0};
  std::atomic<std::uint64_t> action_success{0};
  std::atomic<std::uint64_t> action_failed_read_finished{0};
  std::atomic<std::uint64_t> action_failed_read_too_long{0};

  [[nodiscard]] std::string describe() const;
};

class LiveReadsStream {
 public:
  // Returns nullopt to leave the read alone (keep accumulating signal).
  // std::function costs an indirect call per chunk -- a couple of nanoseconds
  // against a ~150 us per-chunk budget. Make it a template parameter only if a
  // profile ever says so.
  using PolicyFn = std::function<std::optional<Decision>(const ChunkRef&,
                                                         const ChannelState&)>;

  LiveReadsStream(StreamConfig cfg, std::shared_ptr<grpc::Channel> channel,
                  PolicyFn policy, Credentials creds = {})
      : cfg_(std::move(cfg)),
        channel_(std::move(channel)),
        policy_(std::move(policy)),
        creds_(std::move(creds)) {}

  LiveReadsStream(const LiveReadsStream&) = delete;
  LiveReadsStream& operator=(const LiveReadsStream&) = delete;

  ~LiveReadsStream() { stop(); }

  [[nodiscard]] const StreamConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] const StreamStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

  // For tests: after stop(), free_count_quiesced() must equal arena_slots, which
  // proves no slot leaked while real traffic was flowing. Null before start().
  [[nodiscard]] const PbArenaPool* arena_pool() const noexcept { return arenas_.get(); }

  // Builds the stream, sends StreamSetup, and launches reader, workers, writer.
  // Returns false with last_error() set on any failure.
  [[nodiscard]] bool start();

  // Idempotent. Closes the write side, joins every thread, records the final
  // gRPC status.
  void stop();

 private:
  static constexpr std::size_t kShardRingCapacity = 2048;
  static constexpr std::size_t kActionRingCapacity = 1024;

  using ShardRing = SpscRing<ChunkRef, kShardRingCapacity>;
  using ActionRing = SpscRing<Decision, kActionRingCapacity>;
  using Req = minknow_api::data::GetLiveReadsRequest;
  using Resp = minknow_api::data::GetLiveReadsResponse;

  void reader_loop(std::stop_token st);
  void worker_loop(std::stop_token st, unsigned shard);
  void writer_loop(std::stop_token st);
  void note_action_responses(const Resp& resp);

  [[nodiscard]] unsigned shard_of(std::uint32_t channel) const noexcept {
    return channel & (cfg_.shard_count - 1);
  }

  StreamConfig cfg_;
  std::shared_ptr<grpc::Channel> channel_;
  PolicyFn policy_;
  Credentials creds_;

  std::unique_ptr<minknow_api::data::DataService::Stub> stub_;
  std::unique_ptr<grpc::ClientContext> ctx_;
  std::unique_ptr<grpc::ClientReaderWriter<Req, Resp>> stream_;

  std::unique_ptr<PbArenaPool> arenas_;
  std::vector<std::unique_ptr<ShardRing>> shards_;
  std::vector<std::unique_ptr<ActionRing>> actions_;

  std::jthread reader_;
  std::jthread writer_;
  std::vector<std::jthread> workers_;

  StreamStats stats_;
  std::atomic<std::uint64_t> next_action_id_{1};
  std::atomic<bool> running_{false};
  std::string last_error_;
};

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

inline bool LiveReadsStream::start() {
  if (running_.load(std::memory_order_acquire)) {
    last_error_ = "already started";
    return false;
  }
  if (!cfg_.valid()) {
    last_error_ = "invalid StreamConfig (shard_count must be a power of two, "
                  "last_channel >= first_channel)";
    return false;
  }
  if (channel_ == nullptr) {
    last_error_ = "null grpc::Channel";
    return false;
  }
  if (!policy_) {
    last_error_ = "no policy supplied";
    return false;
  }

  arenas_ = std::make_unique<PbArenaPool>(cfg_.arena_slots, cfg_.arena_block_bytes);

  shards_.clear();
  actions_.clear();
  shards_.reserve(cfg_.shard_count);
  actions_.reserve(cfg_.shard_count);
  for (unsigned s = 0; s < cfg_.shard_count; ++s) {
    shards_.emplace_back(std::make_unique<ShardRing>());
    actions_.emplace_back(std::make_unique<ActionRing>());
  }

  stub_ = minknow_api::data::DataService::NewStub(channel_);
  ctx_ = std::make_unique<grpc::ClientContext>();
  if (creds_.have_token()) {
    ctx_->AddMetadata(kLocalAuthMetadataKey, creds_.auth_token);
  }

  stream_ = stub_->get_live_reads(ctx_.get());
  if (stream_ == nullptr) {
    last_error_ = "get_live_reads returned no stream";
    return false;
  }

  // StreamSetup must be the first message on the stream.
  Req setup;
  auto* s = setup.mutable_setup();
  s->set_first_channel(cfg_.first_channel);
  s->set_last_channel(cfg_.last_channel);
  s->set_raw_data_type(
      static_cast<Req::RawDataType>(static_cast<int>(cfg_.raw_data_type)));
  s->set_sample_minimum_chunk_size(cfg_.sample_minimum_chunk_size);
  if (!stream_->Write(setup)) {
    last_error_ = "failed to write StreamSetup";
    return false;
  }

  running_.store(true, std::memory_order_release);

  workers_.clear();
  workers_.reserve(cfg_.shard_count);
  for (unsigned shard = 0; shard < cfg_.shard_count; ++shard) {
    workers_.emplace_back(
        [this, shard](std::stop_token st) { worker_loop(st, shard); });
  }
  writer_ = std::jthread([this](std::stop_token st) { writer_loop(st); });
  reader_ = std::jthread([this](std::stop_token st) { reader_loop(st); });
  return true;
}

inline void LiveReadsStream::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;

  // Close the write side first so the server finishes the stream and the
  // reader's Read() returns false rather than blocking forever.
  if (stream_ != nullptr) stream_->WritesDone();

  reader_.request_stop();
  writer_.request_stop();
  for (auto& w : workers_) w.request_stop();

  if (reader_.joinable()) reader_.join();
  if (writer_.joinable()) writer_.join();
  for (auto& w : workers_) {
    if (w.joinable()) w.join();
  }
  workers_.clear();

  if (stream_ != nullptr) {
    const grpc::Status status = stream_->Finish();
    if (!status.ok() && last_error_.empty()) {
      last_error_ = "stream finished: " + status.error_message();
    }
  }
}

inline void LiveReadsStream::reader_loop(std::stop_token st) {
  (void)set_thread_name("mru-reader");
  (void)pin_this_thread_to_core(cfg_.cores.reader_core);

  IdleWait idle;
  while (!st.stop_requested()) {
    PbArenaPool::Slot* slot = arenas_->acquire();
    if (slot == nullptr) {
      // Workers are behind. Drop this response rather than block: a chunk we
      // cannot act on in time is worth nothing.
      stats_.arena_exhausted.fetch_add(1, std::memory_order_relaxed);
      idle();
      continue;
    }

    auto* resp = google::protobuf::Arena::Create<Resp>(slot->arena().get());
    if (!stream_->Read(resp)) {
      slot->release();
      break;  // stream closed by the server, or shutting down
    }
    const std::uint64_t t_recv = rdtscp();
    stats_.responses.fetch_add(1, std::memory_order_relaxed);

    note_action_responses(*resp);

    for (const auto& entry : resp->channels()) {
      const std::uint32_t channel = entry.first;
      const Resp::ReadData& rd = entry.second;

      ChunkRef ref{};
      ref.channel = channel;
      ref.read_id = rd.id().data();
      ref.read_id_len = static_cast<std::uint16_t>(rd.id().size());
      ref.chunk_start_sample = rd.chunk_start_sample();
      ref.chunk_length = static_cast<std::uint32_t>(rd.chunk_length());
      ref.raw_data = reinterpret_cast<const std::byte*>(rd.raw_data().data());
      ref.raw_data_len = static_cast<std::uint32_t>(rd.raw_data().size());
      ref.raw_data_type = cfg_.raw_data_type;
      ref.owner = slot;
      ref.t_recv_tsc = t_recv;

      if (!ref.payload_matches_reported_length()) {
        stats_.payload_mismatch.fetch_add(1, std::memory_order_relaxed);
      }

      slot->retain();  // one reference per dispatched descriptor
      if (shards_[shard_of(channel)]->try_push(ref)) {
        stats_.chunks.fetch_add(1, std::memory_order_relaxed);
      } else {
        stats_.chunks_dropped.fetch_add(1, std::memory_order_relaxed);
        slot->release();
      }
    }

    slot->release();  // the reader drops its own reference; see arena_pool.hpp
  }
}

inline void LiveReadsStream::note_action_responses(const Resp& resp) {
  for (const auto& ar : resp.action_responses()) {
    switch (ar.response()) {
      case Resp::ActionResponse::SUCCESS:
        stats_.action_success.fetch_add(1, std::memory_order_relaxed);
        break;
      case Resp::ActionResponse::FAILED_READ_FINISHED:
        stats_.action_failed_read_finished.fetch_add(1, std::memory_order_relaxed);
        break;
      case Resp::ActionResponse::FAILED_READ_TOO_LONG:
        // We were too slow. This is the server telling us our decision latency
        // cost yield, and it is the honest headline metric for the paper.
        stats_.action_failed_read_too_long.fetch_add(1, std::memory_order_relaxed);
        break;
      default:
        break;
    }
  }
}

inline void LiveReadsStream::worker_loop(std::stop_token st, unsigned shard) {
  (void)set_thread_name("mru-worker");
  (void)pin_this_thread_to_core(cfg_.cores.worker_core(shard));

  // This worker owns a disjoint subset of channels for the whole run, so none of
  // this state needs synchronisation. The table covers the full range; only the
  // entries for this shard are ever touched.
  ChannelTable table(cfg_.first_channel, cfg_.last_channel);
  ActionRing& out = *actions_[shard];
  ShardRing& in = *shards_[shard];

  constexpr std::size_t kBatch = 32;
  ChunkRef batch[kBatch];
  IdleWait idle;

  while (!st.stop_requested()) {
    const std::size_t n = in.try_pop_bulk(batch, kBatch);
    if (n == 0) {
      idle();
      continue;
    }
    idle.reset();
    for (std::size_t i = 0; i < n; ++i) {
      ChunkRef& c = batch[i];
      const ChunkKind kind =
          table.observe(c.channel, c.read_id_view(), c.chunk_start_sample,
                        c.chunk_length);
      if (kind == ChunkKind::kNewRead) {
        stats_.reads_started.fetch_add(1, std::memory_order_relaxed);
      }

      if (kind != ChunkKind::kUnknownChannel && kind != ChunkKind::kIdTooLong) {
        ChannelState& cs = table.at(c.channel);
        if (!cs.decided()) {
          if (std::optional<Decision> d = policy_(c, cs); d.has_value()) {
            d->action_id = next_action_id_.fetch_add(1, std::memory_order_relaxed);
            if (cs.decide(d->unblock ? ReadDecision::kReject : ReadDecision::kAccept,
                          c.chunk_end_sample())) {
              if (out.try_push(*d)) {
                stats_.decisions.fetch_add(1, std::memory_order_relaxed);
              } else {
                stats_.decisions_dropped.fetch_add(1, std::memory_order_relaxed);
              }
            }
          }
        }
      }
      c.release();
    }
  }

  // Drain anything still queued so the arena slots are returned before exit.
  ChunkRef leftover{};
  while (in.try_pop(leftover)) leftover.release();
}

inline void LiveReadsStream::writer_loop(std::stop_token st) {
  (void)set_thread_name("mru-writer");
  (void)pin_this_thread_to_core(cfg_.cores.writer_core);

  // One reused request message: clear_actions() recycles the repeated field's
  // storage, so the action ids and read id strings do not reallocate every tick.
  Req req;
  auto* actions = req.mutable_actions();
  auto next_tick = std::chrono::steady_clock::now();
  IdleWait widle;

  const auto flush = [&]() {
    if (actions->actions_size() == 0) return;
    const auto n = static_cast<std::uint64_t>(actions->actions_size());
    if (stream_->Write(req)) {  // sole writer -- required by gRPC
      stats_.actions_sent.fetch_add(n, std::memory_order_relaxed);
    }
  };

  while (!st.stop_requested()) {
    actions->clear_actions();

    // Round-robin across every worker's ring. Each is SPSC with this thread as
    // the only consumer, so no shared queue and no new race surface.
    Decision d{};
    bool any = true;
    while (any && static_cast<std::size_t>(actions->actions_size()) <
                      cfg_.max_actions_per_message) {
      any = false;
      for (auto& ring : actions_) {
        if (static_cast<std::size_t>(actions->actions_size()) >=
            cfg_.max_actions_per_message) {
          break;
        }
        if (!ring->try_pop(d)) continue;
        any = true;

        auto* a = actions->add_actions();
        a->set_action_id(std::to_string(d.action_id));
        a->set_channel(d.channel);
        a->set_id(d.read_id.c_str(), d.read_id.size());  // 6.x: id string only
        if (d.unblock) {
          a->mutable_unblock()->set_duration(static_cast<double>(d.unblock_seconds));
        } else {
          a->mutable_stop_further_data();
        }
      }
    }

    flush();

    if (cfg_.writer_tick.count() == 0) {
      widle();
    } else {
      next_tick += cfg_.writer_tick;
      const auto now = std::chrono::steady_clock::now();
      if (next_tick > now) {
        std::this_thread::sleep_for(next_tick - now);
      } else {
        next_tick = now;  // fell behind; do not accumulate debt
      }
    }
  }

  // Final flush so decisions made just before shutdown still reach MinKNOW.
  actions->clear_actions();
  Decision d{};
  for (auto& ring : actions_) {
    while (ring->try_pop(d) && static_cast<std::size_t>(actions->actions_size()) <
                                   cfg_.max_actions_per_message) {
      auto* a = actions->add_actions();
      a->set_action_id(std::to_string(d.action_id));
      a->set_channel(d.channel);
      a->set_id(d.read_id.c_str(), d.read_id.size());
      if (d.unblock) {
        a->mutable_unblock()->set_duration(static_cast<double>(d.unblock_seconds));
      } else {
        a->mutable_stop_further_data();
      }
    }
  }
  flush();
}

inline std::string StreamStats::describe() const {
  const auto g = [](const std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
  };
  std::string s;
  s += "ingestion: " + std::to_string(g(responses)) + " responses, " +
       std::to_string(g(chunks)) + " chunks, " + std::to_string(g(chunks_dropped)) +
       " dropped, " + std::to_string(g(arena_exhausted)) + " arena exhausted, " +
       std::to_string(g(payload_mismatch)) + " payload mismatch\n";
  s += "decide:    " + std::to_string(g(reads_started)) + " reads, " +
       std::to_string(g(decisions)) + " decisions, " +
       std::to_string(g(decisions_dropped)) + " dropped\n";
  s += "actions:   " + std::to_string(g(actions_sent)) + " sent, " +
       std::to_string(g(action_success)) + " ok, " +
       std::to_string(g(action_failed_read_finished)) + " read finished, " +
       std::to_string(g(action_failed_read_too_long)) + " TOO LATE\n";
  return s;
}

}  // namespace mru
