// src/transport/chunk_ref.hpp
//
// The POD descriptor that travels through the shard rings. This is the whole
// point of the arena design: the gRPC reader parses one response into an arena,
// then hands workers spans into that arena. Nothing is copied again.
//
// Field layout below is pinned to minknow_api 6.10.3, verified against
// third_party/minknow_api/proto/minknow_api/data.proto:
//
//   GetLiveReadsResponse.ReadData
//     string id                     = 1
//     reserved                        2   <- was `number`, removed since 6.0
//     uint64 start_sample           = 3
//     uint64 chunk_start_sample     = 4
//     uint64 chunk_length           = 5
//     repeated int32 chunk_classifications = 6
//     bytes  raw_data               = 7
//     float  median_before          = 8
//     float  median                 = 9
//     int32  previous_read_classification  = 10
//     ReadEndReason previous_read_end_reason = 11
//
// Templated on the arena type so the descriptor, and the tests that exercise it,
// build with no protobuf dependency. `ChunkRef` is the protobuf instantiation;
// `TestChunkRef` over BumpArena is always available.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "core/arena_pool.hpp"

namespace mru {

// Mirrors GetLiveReadsRequest.RawDataType. Kept as our own enum so the decide
// layer does not have to include generated protobuf headers; proto_compat.hpp
// static_asserts these against the generated values.
enum class RawDataType : std::uint8_t {
  kKeepLast = 0,
  kNone = 1,
  kCalibrated = 2,    // float32 per sample
  kUncalibrated = 3,  // int16 per sample
};

[[nodiscard]] constexpr std::size_t bytes_per_sample(RawDataType t) noexcept {
  switch (t) {
    case RawDataType::kCalibrated:
      return sizeof(float);
    case RawDataType::kUncalibrated:
      return sizeof(std::int16_t);
    case RawDataType::kKeepLast:
    case RawDataType::kNone:
      break;
  }
  return 0;
}

// Field order here is chosen for size, not readability: 8-byte members first,
// then the narrow ones packed into the tail. Declared in the "natural" reading
// order this struct is 72 bytes; packed it is 56. The static_assert below is
// what caught that, and it is worth keeping for exactly that reason.
//
// Two deliberate narrowings, both guarded at ingest in live_reads_stream:
//   chunk_length  uint64 -> uint32   a chunk is sub-second; 2^32 samples is
//                                    ~12 days at 4 kHz, so this cannot overflow
//                                    for a single chunk
//   read_id_len   size_t -> uint16   read ids are 36-char UUIDs and
//                                    ReadId::assign rejects anything >= 40
// chunk_start_sample stays 64-bit: it is absolute across the run, and a 72 h
// PromethION run at 4 kHz already reaches ~1.0e9, uncomfortably close to 2^32.
template <ResettableArena ArenaT>
struct ChunkRefT {
  // --- position ----------------------------------------------------------
  std::uint64_t chunk_start_sample;  // absolute, across the experiment

  // --- read identity: a span into arena-owned protobuf string storage ----
  // 6.x has no read number, so the id string is the only identifier. We do NOT
  // copy it here; ChannelTable copies it once per read into its inline buffer.
  const char* read_id;

  // --- signal: a span into arena-owned protobuf bytes storage ------------
  const std::byte* raw_data;

  // --- bookkeeping -------------------------------------------------------
  ArenaSlot<ArenaT>* owner;  // release() exactly once when done
  std::uint64_t t_recv_tsc;  // stamped right after stream->Read() returned

  // --- narrow tail -------------------------------------------------------
  std::uint32_t channel;
  std::uint32_t chunk_length;   // in samples, as reported by MinKNOW
  std::uint32_t raw_data_len;   // BYTES, not samples
  std::uint16_t read_id_len;
  RawDataType raw_data_type;
  std::uint8_t _pad;

  [[nodiscard]] std::string_view read_id_view() const noexcept {
    return std::string_view(read_id, read_id_len);
  }

  // Samples actually present in this chunk's payload. MinKNOW reports
  // chunk_length separately; if the two disagree, trust the payload and count
  // the discrepancy -- a short payload means a truncated or dropped frame.
  [[nodiscard]] std::uint64_t samples_in_payload() const noexcept {
    const std::size_t w = bytes_per_sample(raw_data_type);
    if (w == 0) return 0;
    return raw_data_len / w;
  }

  [[nodiscard]] bool payload_matches_reported_length() const noexcept {
    return samples_in_payload() == chunk_length;
  }

  // Absolute sample index one past the end of this chunk.
  [[nodiscard]] std::uint64_t chunk_end_sample() const noexcept {
    return chunk_start_sample + chunk_length;
  }

  void release() noexcept {
    if (owner != nullptr) owner->release();
  }
};

using TestChunkRef = ChunkRefT<BumpArena>;

static_assert(std::is_trivially_copyable_v<TestChunkRef>,
              "ChunkRef must be trivially copyable: it is memcpy'd through the rings");
static_assert(std::is_default_constructible_v<TestChunkRef>,
              "SpscRing preallocates its slots");
// Two per cacheline on x86-64. Worth keeping an eye on: this struct is copied
// once per chunk per shard, so growth here is growth on the hot path.
static_assert(sizeof(TestChunkRef) <= 64,
              "ChunkRef outgrew a cacheline -- re-check what was added");

#if defined(MRU_WITH_PROTOBUF)
using ChunkRef = ChunkRefT<BlockBackedArena>;
static_assert(std::is_trivially_copyable_v<ChunkRef>);
static_assert(sizeof(ChunkRef) <= 64);
#endif

}  // namespace mru
